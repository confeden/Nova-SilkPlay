"""Analytic scene generator for the Nova SilkPlay quality harness.

Renders synthetic scenes in CLOSED FORM at any real-valued time `t`, together
with the exact motion field and the exact occlusion mask.  This is the only
reference in the harness that is exact at a non-integer multiplier, which is the
case the product actually runs at (165 Hz out of 24000/1001 fps in is
6.881875 generated frames per source frame, not 6.875 and not 7).

Why closed form
---------------
Every scene is a back-to-front stack of axis-aligned rectangles carrying
textures whose integral over an axis-aligned box has a closed form.  A pixel's
value is therefore the exact integral of the scene over that pixel's unit
square -- exact analytic box coverage, not a nearest-neighbour stamp and not a
supersampled approximation.  A jaggy reference would charge the engine for the
reference's own aliasing, and a supersampled one would charge it for the
reference's own noise floor.

Colour
------
The engine blends in LINEAR LIGHT (it reads through _SRGB views and writes
through an _SRGB render target), so every reference frame here is composed in
linear light and encoded to sRGB only at the very end, with the exact piecewise
transfer function (0.0031308 / 12.92 / 1.055 / 2.4).  A gamma-2.2 shortcut
would bias every metric in the harness by a fixed amount that nobody would ever
find; `--selftest` asserts the two functions are measurably different so the
shortcut cannot creep back in.

PNGs are written with no gAMA / sRGB / iCCP / cHRM chunk, because any of those
lets a decoder re-transform the pixels and silently move the reference.

Determinism
-----------
No wall-clock time, no unseeded RNG.  All variation comes from the explicit
`seed` argument, which only jitters texture phases -- never structure, never
velocities, so the assertions below stay meaningful for every seed.

Run `python analytic.py --selftest` to make the module prove itself.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import os
import sys
import tempfile
from dataclasses import dataclass, field
from fractions import Fraction

import numpy as np
from PIL import Image

FORMAT_VERSION = 1
DEFAULT_SIZE = (640, 360)

# ---------------------------------------------------------------------------
# sRGB transfer function
# ---------------------------------------------------------------------------
# The exact IEC 61966-2-1 piecewise form.  The constants are named so that a
# future reader can see at a glance that this is NOT the 2.2 power law.
SRGB_LINEAR_THRESHOLD = 0.0031308      # linear value where the two branches meet
SRGB_ENCODED_THRESHOLD = 0.04045       # 12.92 * 0.0031308, the encoded-side threshold
SRGB_SLOPE = 12.92                     # slope of the linear toe
SRGB_ALPHA = 1.055
SRGB_GAMMA = 2.4


def linear_to_srgb(x):
    """Linear light -> sRGB-encoded, in [0,1] floats.  Exact piecewise form."""
    x = np.asarray(x, dtype=np.float64)
    # np.where evaluates both branches, so the power branch gets a clamped
    # argument; negatives would otherwise produce NaN and poison the whole frame.
    safe = np.maximum(x, 0.0)
    hi = SRGB_ALPHA * np.power(safe, 1.0 / SRGB_GAMMA) - (SRGB_ALPHA - 1.0)
    lo = x * SRGB_SLOPE
    return np.where(x <= SRGB_LINEAR_THRESHOLD, lo, hi)


def srgb_to_linear(s):
    """sRGB-encoded -> linear light.  Exact inverse of `linear_to_srgb`."""
    s = np.asarray(s, dtype=np.float64)
    safe = np.maximum(s, 0.0)
    hi = np.power((safe + (SRGB_ALPHA - 1.0)) / SRGB_ALPHA, SRGB_GAMMA)
    lo = s / SRGB_SLOPE
    return np.where(s <= SRGB_ENCODED_THRESHOLD, lo, hi)


def quantize_u8(encoded):
    """Encoded [0,1] -> uint8 with round-half-up.

    Round-half-up (not numpy's default round-half-even) because the decode
    direction is u/255 exactly, and half-up is what makes decode->encode->
    quantize an identity for all 256 codes.  --selftest asserts that.
    """
    v = np.asarray(encoded, dtype=np.float64)
    return np.clip(np.floor(v * 255.0 + 0.5), 0.0, 255.0).astype(np.uint8)


def encode_linear_u8(lin):
    """Linear-light float array -> 8-bit sRGB, the only place we leave linear."""
    return quantize_u8(linear_to_srgb(lin))


# ---------------------------------------------------------------------------
# Geometry primitives
# ---------------------------------------------------------------------------
# Pixel (i, j) covers the continuous square [j, j+1) x [i, i+1); its centre is
# at (j + 0.5, i + 0.5).  Every coordinate below is in that continuous frame.

_BIG = 1.0e6  # stands in for "unbounded" for full-frame layers


def _q(v):
    """Snap a coordinate to 1/64 px.

    Chosen so every rect edge and velocity component is an exact binary
    fraction: that is what makes an integer translation reproduce bit-exactly
    (the clipped-domain subtractions stay exact), which --selftest asserts.
    1/64 px is far finer than any sub-pixel effect the harness measures.
    """
    return math.floor(float(v) * 64.0 + 0.5) / 64.0


@dataclass(frozen=True)
class Rect:
    x0: float
    y0: float
    x1: float
    y1: float

    def translated(self, dx, dy):
        return Rect(self.x0 + dx, self.y0 + dy, self.x1 + dx, self.y1 + dy)

    @property
    def area(self):
        return max(0.0, self.x1 - self.x0) * max(0.0, self.y1 - self.y0)


def rect_intersect(a: Rect, b: Rect):
    x0 = a.x0 if a.x0 > b.x0 else b.x0
    y0 = a.y0 if a.y0 > b.y0 else b.y0
    x1 = a.x1 if a.x1 < b.x1 else b.x1
    y1 = a.y1 if a.y1 < b.y1 else b.y1
    if x1 <= x0 or y1 <= y0:
        return None
    return Rect(x0, y0, x1, y1)


# ---------------------------------------------------------------------------
# Textures with closed-form box integrals
# ---------------------------------------------------------------------------
# A texture is a tuple of components, summed.  Every component is separable in
# x and y, so its integral over an axis-aligned box is an outer product of two
# 1-D integrals -- which is what keeps exact antialiasing cheap.


@dataclass(frozen=True)
class TexDC:
    """Constant linear-light colour."""
    rgb: tuple


@dataclass(frozen=True)
class TexSin:
    """amp * cos(2*pi*(fx*x + fy*y) + phase), frequencies in cycles per pixel.

    Band-limited on purpose: a sinusoid's box integral is exact, so the texture
    detail is antialiased by construction rather than by supersampling.
    """
    fx: float
    fy: float
    phase: float
    rgb: tuple


@dataclass(frozen=True)
class TexSquare:
    """Hard-edged periodic bar pattern: `rgb` where the wave is on, 0 elsewhere.

    axis 0 = bars vary along x (vertical bars), axis 1 = along y.  Duty 1/2.
    Its box integral is exact too, so a hard edge in a texture costs no
    aliasing -- that is what lets A3's disoccluded background carry hard detail.
    """
    axis: int
    period: float
    phase: float
    rgb: tuple


def texture_value(tex, x, y):
    """Pointwise texture value (linear light).  Used for range checks and tests."""
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    out = np.zeros(np.broadcast(x, y).shape + (3,), dtype=np.float64)
    for c in tex:
        if isinstance(c, TexDC):
            out += np.asarray(c.rgb, dtype=np.float64)
        elif isinstance(c, TexSin):
            v = np.cos(2.0 * math.pi * (c.fx * x + c.fy * y) + c.phase)
            out += v[..., None] * np.asarray(c.rgb, dtype=np.float64)
        elif isinstance(c, TexSquare):
            u = ((x if c.axis == 0 else y) - c.phase) / c.period
            v = (u - np.floor(u) < 0.5).astype(np.float64)
            out += v[..., None] * np.asarray(c.rgb, dtype=np.float64)
        else:
            raise TypeError(f"unknown texture component {c!r}")
    return out


def _osc_integral(f, a, b):
    """int_a^b exp(2*pi*i*f*x) dx, elementwise over the 1-D arrays a, b."""
    if f == 0.0:
        return (b - a).astype(np.complex128)
    w = 2.0 * math.pi * f
    return (np.exp(1j * w * b) - np.exp(1j * w * a)) / (1j * w)


def _sq_antiderivative(u):
    """int_0^u sq(v) dv for the unit-period square wave (1 on [0,0.5), else 0)."""
    fl = np.floor(u)
    return fl * 0.5 + np.minimum(u - fl, 0.5)


def _square_integral(period, phase, a, b):
    """int_a^b sq((x - phase)/period) dx, exact."""
    return period * (_sq_antiderivative((b - phase) / period)
                     - _sq_antiderivative((a - phase) / period))


def texture_box_integral(tex, x0, x1, y0, y1):
    """Exact integral of `tex` over the separable box grid [x0,x1] x [y0,y1].

    x0/x1 are 1-D arrays of length nw, y0/y1 of length nh; the result is
    (nh, nw, 3).  Every component is separable, so each is one outer product.
    """
    lx = x1 - x0
    ly = y1 - y0
    nh = y0.shape[0]
    nw = x0.shape[0]
    out = np.zeros((nh, nw, 3), dtype=np.float64)
    for c in tex:
        if isinstance(c, TexDC):
            val = ly[:, None] * lx[None, :]
        elif isinstance(c, TexSin):
            ix = _osc_integral(c.fx, x0, x1)
            iy = _osc_integral(c.fy, y0, y1)
            val = np.real(np.exp(1j * c.phase) * iy[:, None] * ix[None, :])
        elif isinstance(c, TexSquare):
            if c.axis == 0:
                s = _square_integral(c.period, c.phase, x0, x1)
                val = ly[:, None] * s[None, :]
            else:
                s = _square_integral(c.period, c.phase, y0, y1)
                val = s[:, None] * lx[None, :]
        else:
            raise TypeError(f"unknown texture component {c!r}")
        out += val[:, :, None] * np.asarray(c.rgb, dtype=np.float64)
    return out


# ---------------------------------------------------------------------------
# Motion
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Motion:
    """Offset from the s=0 pose: p(s) = v*s + 0.5*a*s^2, s in source-frame units."""
    v: tuple = (0.0, 0.0)
    a: tuple = (0.0, 0.0)

    @property
    def model(self):
        if self.v == (0.0, 0.0) and self.a == (0.0, 0.0):
            return "static"
        return "linear" if self.a == (0.0, 0.0) else "quadratic"

    @property
    def is_static(self):
        return self.model == "static"

    def offset(self, s):
        s = float(s)
        return (self.v[0] * s + 0.5 * self.a[0] * s * s,
                self.v[1] * s + 0.5 * self.a[1] * s * s)

    def disp(self, s0, s1):
        p0 = self.offset(s0)
        p1 = self.offset(s1)
        return (p1[0] - p0[0], p1[1] - p0[1])

    def chord_error(self, s_a, t):
        """(chord - true) displacement for a chord drawn across [s_a, s_a+1].

        A translational interpolator can only assume the object moved along the
        straight line between the two source frames.  With constant
        acceleration a the true path is a parabola, and the gap is

            chord(t) - true(t) = 0.5 * a * u * (1 - u),   u = t - s_a

        peaking at a/8 at the midpoint.  This is the part of the residual no
        translational interpolator can ever remove, which is why the harness
        reports it instead of blaming the engine for it.
        """
        u = float(t) - float(s_a)
        k = 0.5 * u * (1.0 - u)
        return (self.a[0] * k, self.a[1] * k)

    @property
    def chord_error_max(self):
        return (self.a[0] / 8.0, self.a[1] / 8.0)


# ---------------------------------------------------------------------------
# Layers and scenes
# ---------------------------------------------------------------------------


@dataclass
class Group:
    """One layer: a set of PAIRWISE DISJOINT rects sharing a texture and motion.

    Disjointness inside a group is what keeps exact compositing linear rather
    than exponential: intersecting k groups yields a product of choices whose
    terms are themselves disjoint, so they can simply be summed.
    """
    name: str
    rects: tuple
    texture: tuple
    motion: Motion

    def rects_at(self, s):
        dx, dy = self.motion.offset(s)
        return [r.translated(dx, dy) for r in self.rects]

    def bbox_at(self, s):
        rs = self.rects_at(s)
        return Rect(min(r.x0 for r in rs), min(r.y0 for r in rs),
                    max(r.x1 for r in rs), max(r.y1 for r in rs))


@dataclass
class Scene:
    scene_id: str
    title: str
    size: tuple           # (W, H)
    seed: int
    groups: list          # back (index 0) to front
    adversarial: bool = False
    notes: dict = field(default_factory=dict)

    @property
    def static_group_ids(self):
        return tuple(i for i, g in enumerate(self.groups) if g.motion.is_static)


# ---------------------------------------------------------------------------
# 5x7 font -> disjoint rectangles (A4's "text")
# ---------------------------------------------------------------------------
# No font file and no PIL text rendering: those would antialias with their own
# filter, and the whole point of A4 is that the overlay's edges are exact.
# Glyph strokes become axis-aligned rects, which the compositor already handles
# exactly.

_FONT5x7 = {
    " ": ("00000", "00000", "00000", "00000", "00000", "00000", "00000"),
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01110", "10001", "10000", "10000", "10000", "10001", "01110"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "11011", "10001"),
    "X": ("10001", "01010", "00100", "00100", "00100", "01010", "10001"),
    "Y": ("10001", "01010", "00100", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11111", "00010", "00100", "00010", "00001", "10001", "01110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
    ":": ("00000", "00100", "00100", "00000", "00100", "00100", "00000"),
    ".": ("00000", "00000", "00000", "00000", "00000", "00110", "00110"),
    "-": ("00000", "00000", "00000", "01110", "00000", "00000", "00000"),
    "!": ("00100", "00100", "00100", "00100", "00100", "00000", "00100"),
    "?": ("01110", "10001", "00001", "00010", "00100", "00000", "00100"),
}
_FONT_UNKNOWN = ("11111", "10001", "10001", "10001", "10001", "10001", "11111")


def _mask_to_rects(mask):
    """Decompose a boolean grid into DISJOINT rects (row runs merged vertically)."""
    rows, cols = mask.shape
    runs_per_row = []
    for r in range(rows):
        runs = []
        c = 0
        while c < cols:
            if mask[r, c]:
                c2 = c
                while c2 < cols and mask[r, c2]:
                    c2 += 1
                runs.append((c, c2))
                c = c2
            else:
                c += 1
        runs_per_row.append(runs)
    taken = [set() for _ in range(rows)]
    out = []
    for r in range(rows):
        for run in runs_per_row[r]:
            if run in taken[r]:
                continue
            r2 = r + 1
            while r2 < rows and run in runs_per_row[r2] and run not in taken[r2]:
                taken[r2].add(run)
                r2 += 1
            out.append((run[0], r, run[1], r2))
    return out


def text_rects(text, x, y, scale, tracking=1):
    """Axis-aligned, pairwise disjoint rects for `text` at (x, y), `scale` px/cell."""
    glyphs = [_FONT5x7.get(ch.upper(), _FONT_UNKNOWN) for ch in text]
    cols = len(glyphs) * (5 + tracking) - tracking
    mask = np.zeros((7, max(cols, 1)), dtype=bool)
    for gi, g in enumerate(glyphs):
        c0 = gi * (5 + tracking)
        for r in range(7):
            for c in range(5):
                if g[r][c] == "1":
                    mask[r, c0 + c] = True
    return [Rect(_q(x + c0 * scale), _q(y + r0 * scale),
                 _q(x + c1 * scale), _q(y + r1 * scale))
            for (c0, r0, c1, r1) in _mask_to_rects(mask)]


# ---------------------------------------------------------------------------
# Scene builders
# ---------------------------------------------------------------------------


def _bg_texture(rng, dc, amp, freqs):
    """Static-detail texture: a DC level plus a few band-limited sinusoids.

    Amplitudes are budgeted so the pointwise linear value can never leave
    [0,1]; --selftest asserts that no clamping ever fires, because a clamp
    would silently make the reference non-analytic.
    """
    comps = [TexDC((dc, dc, dc))]
    for (fx, fy, a, tint) in freqs:
        ph = float(rng.uniform(0.0, 2.0 * math.pi))
        comps.append(TexSin(fx, fy, ph, (a * amp * tint[0], a * amp * tint[1],
                                         a * amp * tint[2])))
    return tuple(comps)


def _full_frame_rect():
    return Rect(-_BIG, -_BIG, _BIG, _BIG)


def _a1a2_common(size, seed, accel):
    """A1 and A2 share their geometry; only the acceleration differs.

    Keeping them identical apart from `a` is deliberate: the difference between
    the two scenes' residuals is then attributable to the chord-vs-curve term
    alone, with nothing else moving.
    """
    w, h = size
    rng = np.random.default_rng(seed)

    bg = Group(
        name="background",
        rects=(_full_frame_rect(),),
        texture=_bg_texture(rng, 0.20, 0.16, [
            (1.0 / 37.0, 1.0 / 53.0, 0.45, (1.0, 0.95, 0.85)),
            (1.0 / 13.0, -1.0 / 19.0, 0.30, (0.85, 1.0, 0.95)),
            (-1.0 / 71.0, 1.0 / 11.0, 0.25, (0.95, 0.90, 1.0)),
        ]),
        motion=Motion(),
    )

    # Both foreground objects share one velocity so they can never collide;
    # A1/A2 are baselines and must not smuggle in an occlusion event.
    v = (7.375, 2.125)
    mot = Motion(v=v, a=accel)

    rx = _q(0.09 * w)
    ry = _q(0.10 * h)
    rw = _q(0.14 * w)
    rh = _q(0.18 * h)
    rect = Group(
        name="hard_rect",
        rects=(Rect(rx, ry, _q(rx + rw), _q(ry + rh)),),
        texture=(TexDC((0.85, 0.83, 0.80)),),
        motion=mot,
    )

    # The patch sits far below the rect in y (0.34*h of clear space) so that a
    # background point can never be hidden by the rect in frame A and by the
    # patch in frame B: that combination has no label in the three-class mask.
    # Both objects share one Motion, so their separation never changes; the
    # combination would need the ONE-INTERVAL y displacement to exceed 0.34*h,
    # which A1 (constant velocity, 2.125 px) never reaches at any t, and which
    # A2 (accelerating) would only reach around interval 27 at h=128 -- by which
    # point both objects left the frame ten intervals earlier, measured, so the
    # case is unreachable rather than merely unlikely.  render() still refuses
    # rather than emitting a fourth class if a future scene ever gets there.
    px = _q(0.09 * w)
    py = _q(0.62 * h)
    pw = _q(0.18 * w)
    ph_ = _q(0.20 * h)
    patch_rng = np.random.default_rng(seed + 1)
    patch = Group(
        name="textured_patch",
        rects=(Rect(px, py, _q(px + pw), _q(py + ph_)),),
        texture=_bg_texture(patch_rng, 0.45, 0.35, [
            (1.0 / 7.0, 1.0 / 9.0, 0.50, (1.0, 0.7, 0.6)),
            (-1.0 / 23.0, 1.0 / 5.0, 0.30, (0.6, 1.0, 0.8)),
            (1.0 / 17.0, 1.0 / 31.0, 0.20, (0.8, 0.8, 1.0)),
        ]),
        motion=mot,
    )
    return [bg, rect, patch], v, mot


def _build_a1(size, seed):
    groups, v, _ = _a1a2_common(size, seed, (0.0, 0.0))
    return Scene(
        scene_id="A1",
        title="translate-linear",
        size=size,
        seed=seed,
        groups=groups,
        adversarial=False,
        notes={
            "description": "hard-edged rectangle and textured patch at constant "
                           "velocity over a static textured background",
            "velocity_px_per_interval": [v[0], v[1]],
            "purpose": "baseline sub-pixel placement",
        },
    )


def _build_a2(size, seed):
    # a is chosen with the SAME sign as v so the motion stays monotone for all
    # s >= 0: a reversal would let a background point be hidden in both source
    # frames yet visible in between, which has no label in the 3-class mask.
    # |a|/8 = (0.594, 0.188) px is the peak chord-vs-curve error -- big enough
    # to be visible on a hard edge, small enough to stay a sub-pixel effect.
    accel = (4.75, 1.5)
    groups, v, mot = _a1a2_common(size, seed, accel)
    return Scene(
        scene_id="A2",
        title="translate-quadratic",
        size=size,
        seed=seed,
        groups=groups,
        adversarial=False,
        notes={
            "description": "same objects as A1 with constant acceleration, so a "
                           "linear (chord) motion model is provably wrong",
            "velocity_px_per_interval": [v[0], v[1]],
            "accel_px_per_interval2": [accel[0], accel[1]],
            "chord_error_formula": "chord - true = 0.5 * a * u * (1 - u), u = t - floor(t)",
            "chord_error_max_px": list(mot.chord_error_max),
            "purpose": "the residual no translational interpolator can remove",
        },
    )


def _build_a3(size, seed):
    w, h = size
    rng = np.random.default_rng(seed)

    # Hard-edged stripes in the BACKGROUND, not a separate layer: disoccluded
    # background must carry high-frequency, hard-edged detail, otherwise a
    # disocclusion error hides in a smooth gradient and the scene proves nothing.
    stripe_period = _q(max(6.0, 0.055 * h))
    bg_tex = _bg_texture(rng, 0.30, 0.18, [
        (1.0 / 29.0, 1.0 / 41.0, 0.5, (1.0, 0.9, 0.8)),
        (1.0 / 9.0, 1.0 / 47.0, 0.5, (0.8, 0.95, 1.0)),
    ]) + (TexSquare(1, stripe_period, 0.3137, (0.33, 0.30, 0.26)),)

    bg = Group(name="background", rects=(_full_frame_rect(),),
               texture=bg_tex, motion=Motion())

    # Purely horizontal so "disoccluded" and "about to be occluded" fall cleanly
    # on opposite sides of the object; --selftest asserts exactly that.
    fx = _q(0.10 * w)
    fy = _q(0.28 * h)
    fw = _q(0.22 * w)
    fh = _q(0.42 * h)
    fg = Group(
        name="foreground_object",
        rects=(Rect(fx, fy, _q(fx + fw), _q(fy + fh)),),
        texture=(TexDC((0.72, 0.20, 0.16)),),
        motion=Motion(v=(11.25, 0.0)),
    )
    return Scene(
        scene_id="A3",
        title="occlusion-passover",
        size=size,
        seed=seed,
        groups=[bg, fg],
        adversarial=False,
        notes={
            "description": "two layers, known z-order, HARD alpha; foreground "
                           "passes over hard-edged background detail",
            "alpha": "hard (binary coverage); a soft matte would make the "
                     "'exact' motion field a lie at the boundary",
            "foreground_velocity_px_per_interval": [11.25, 0.0],
            "background_stripe_period_px": stripe_period,
            "purpose": "disoccluded background exists in exactly one source frame",
        },
    )


_A4_LABELS = ("SILKPLAY 00:12", "SILKPLAY", "SILK", "SP")


def _min_scanline_gap(rects, w, h):
    """Smallest horizontal gap between occluders (and to the frame border) on any
    pixel-centre scanline.  inf when no scanline carries an occluder.

    This is the quantity that decides whether a HORIZONTALLY moving layer can be
    hidden by one piece of a non-convex overlay in frame A and by a different
    piece in frame B while being visible in between.
    """
    best = float("inf")
    for y in (np.arange(h, dtype=np.float64) + 0.5):
        spans = sorted((r.x0, r.x1) for r in rects if r.y0 <= y < r.y1)
        if not spans:
            continue
        merged = []
        for a, b in spans:
            if merged and a <= merged[-1][1] + 1e-12:
                merged[-1][1] = max(merged[-1][1], b)
            else:
                merged.append([a, b])
        best = min(best, merged[0][0], float(w) - merged[-1][1])
        for k in range(1, len(merged)):
            best = min(best, merged[k][0] - merged[k - 1][1])
    return best


def _build_a4(size, seed):
    """Static overlay over moving content.

    The motion here is deliberately HORIZONTAL ONLY, and that is a correctness
    constraint rather than a stylistic one.  The three-class occlusion mask has
    no label for a point visible in NEITHER source frame, and a full-frame layer
    panning DIAGONALLY always produces some: a pixel in the frame corner leaves
    through the top edge going back to A and through the side edge going forward
    to B.  A4 is the only scene whose background moves, so A4 is the only one
    that has to give that up; A1-A3 carry the diagonal motion instead.

    With horizontal motion the remaining hazard is the overlay's NON-CONVEXITY:
    a panning pixel could hide behind glyph stroke i in A and stroke j in B
    while sitting in the gap between them at t.  That cannot happen when every
    horizontal gap in the overlay -- and the distance from the overlay to the
    frame border -- exceeds the background's per-interval step, which
    `_min_scanline_gap` measures on the finished layout instead of trusting the
    font metrics.
    """
    w, h = size
    rng = np.random.default_rng(seed)

    v_bg = (-4.75, 0.0)
    d_bg = abs(v_bg[0])
    if w <= d_bg + 2.0:
        raise ValueError(f"A4 needs a frame wider than {d_bg + 2.0:.0f} px")

    # One font cell is the narrowest gap a 5x7 glyph can have, so the cell size
    # is driven by the pan speed first and legibility second.
    scale = float(max(int(math.ceil(d_bg)) + 2, 4))
    text = None
    for cand in _A4_LABELS:
        if (len(cand) * 6 - 1) * scale <= 0.86 * w:
            text = cand
            break
    if text is None:
        raise ValueError(f"A4 needs a frame at least {int((2 * 6 - 1) * scale / 0.86)} px wide")

    tw = (len(text) * 6 - 1) * scale
    tx = _q((w - tw) * 0.5)
    margin_b = max(math.ceil(d_bg) + 2.0, round(0.05 * h))
    ty = _q(h - 7 * scale - margin_b)
    if ty <= 0.0:
        raise ValueError(f"A4 needs a taller frame than {h} px for a {scale:.0f} px cell")
    rects = text_rects(text, tx, ty, scale)
    text_box = Rect(tx, ty, _q(tx + tw), _q(ty + 7 * scale))

    # A hollow corner mark with a solid core -- the "logo", the other thing that
    # must never smear.  Its interior gap is one cell for the same reason.
    m = max(math.ceil(d_bg) + 2.0, round(0.025 * w))
    side = max(6.0 * scale, 0.10 * w)
    bx1 = _q(w - m)
    bx0 = _q(bx1 - side)
    by0 = _q(m)
    by1 = _q(by0 + side)
    tkn = _q(max(2.0, side / 8.0))
    gap = _q(scale)
    core0x, core0y = _q(bx0 + tkn + gap), _q(by0 + tkn + gap)
    core1x, core1y = _q(bx1 - tkn - gap), _q(by1 - tkn - gap)
    logo = [
        Rect(bx0, by0, bx1, _q(by0 + tkn)),
        Rect(bx0, _q(by1 - tkn), bx1, by1),
        Rect(bx0, _q(by0 + tkn), _q(bx0 + tkn), _q(by1 - tkn)),
        Rect(_q(bx1 - tkn), _q(by0 + tkn), bx1, _q(by1 - tkn)),
    ]
    if core1x - core0x >= 2.0 and core1y - core0y >= 2.0:
        logo.append(Rect(core0x, core0y, core1x, core1y))
    logo_box = Rect(bx0, by0, bx1, by1)
    if logo_box.y1 >= text_box.y0:
        raise ValueError(f"A4 logo and subtitle overlap vertically at {w}x{h}")
    rects = [r for r in rects + logo if r.x1 > r.x0 and r.y1 > r.y0]

    # Hard vertical edges in the pan, so the content sliding under the subtitle
    # carries the high frequencies a warp would visibly drag; a purely smooth
    # background would let a smear hide in a gradient.
    bg = Group(
        name="moving_background",
        rects=(_full_frame_rect(),),
        texture=_bg_texture(rng, 0.26, 0.15, [
            (1.0 / 43.0, 1.0 / 29.0, 0.5, (1.0, 0.9, 0.85)),
            (1.0 / 11.0, 1.0 / 17.0, 0.3, (0.85, 1.0, 0.9)),
            (-1.0 / 23.0, -1.0 / 7.0, 0.2, (0.9, 0.9, 1.0)),
        ]) + (TexSquare(0, _q(max(9.0, 0.045 * w)), 0.3137, (0.22, 0.20, 0.18)),
              TexSquare(1, _q(max(13.0, 0.11 * h)), 1.1137, (0.10, 0.12, 0.14))),
        motion=Motion(v=v_bg),
    )

    overlay = Group(
        name="static_overlay",
        rects=tuple(rects),
        texture=(TexDC((0.94, 0.94, 0.94)),),
        motion=Motion(),  # exactly zero velocity, by construction
    )

    # --- the layout invariant, measured on the finished layout ----------------
    gap_min = _min_scanline_gap(overlay.rects, w, h)
    if gap_min <= d_bg:
        raise AssertionError(
            f"A4 layout is unsound at {w}x{h}: smallest horizontal gap in the "
            f"overlay is {gap_min:.2f} px but the background steps {d_bg:.2f} px "
            "per interval, so some pixel would be visible in neither source frame")

    return Scene(
        scene_id="A4",
        title="text-over-motion",
        size=size,
        seed=seed,
        groups=[bg, overlay],
        adversarial=False,
        notes={
            "description": "static high-contrast text/graphic overlay composited on "
                           "top of horizontally panning, hard-edged content",
            "overlay_text": text,
            "overlay_scale_px_per_cell": scale,
            "overlay_rect_count": len(rects),
            "overlay_velocity_px_per_interval": [0.0, 0.0],
            "background_velocity_px_per_interval": list(v_bg),
            "min_overlay_scanline_gap_px": gap_min,
            "layout_invariant": "every horizontal gap in the overlay, and its "
                                "distance to the frame border, exceeds the "
                                "background's per-interval step; otherwise a pixel "
                                "could be visible in neither source frame",
            "motion_note": "horizontal only by construction -- a diagonally panning "
                           "full-frame layer always leaves the frame corner visible "
                           "in neither source frame",
            "purpose": "catches a warp smearing subtitles or a logo",
        },
    )


def _build_a5(size, seed):
    w, h = size
    rng = np.random.default_rng(seed)
    period = 16.0
    # Advance defaults to EXACTLY one period: the strongest form of the
    # ambiguity.  |A - B| is then zero everywhere, so anything downstream that
    # derives "what moved" from a frame difference sees a static frame while the
    # true field is 16 px/interval.
    advance = period
    # The phase is the ONLY thing the seed varies in this scene, so it has to
    # vary properly.  Jittering by 0.05 px and then snapping to the 1/64 lattice
    # produced exactly FOUR distinct phases, and 21 of the first 63 consecutive
    # seed pairs then rendered bit-identically -- the "seed is live" assertion
    # below passed only because it happened to be written on seeds 3 and 4.
    # Draw from the whole period on the same lattice instead: 1024 positions,
    # every one an exact binary fraction, so the one-period identity stays
    # bit-exact (a shift by exactly `period` changes the square-wave
    # antiderivative's argument by exactly 1.0, whatever the phase).
    # Half-pixel positions are still skipped: an edge pixel sitting exactly on
    # an 8-bit rounding tie makes the frame depend on the last ULP.
    lattice = int(period * 64.0)
    m = int(rng.integers(0, lattice))
    if m % 32 == 0:                     # m/64 would be a multiple of 0.5 px
        m += 1
    phase = m / 64.0
    grating = Group(
        name="grating",
        rects=(_full_frame_rect(),),
        texture=(TexDC((0.05, 0.05, 0.05)),
                 TexSquare(0, period, phase, (0.85, 0.85, 0.85))),
        motion=Motion(v=(advance, 0.0)),
    )
    return Scene(
        scene_id="A5",
        title="grating-aliasing",
        size=size,
        seed=seed,
        groups=[grating],
        adversarial=True,
        notes={
            "description": "periodic grating advancing one whole period per source interval",
            "period_px": period,
            "advance_px_per_interval": advance,
            "periods_per_interval": advance / period,
            "phase_px": phase,
            "ambiguity": "exact-period",
            "warning": "correspondence is genuinely ambiguous; with an exact-period "
                       "advance |A - B| is ZERO everywhere, so any estimator that "
                       "derives motion from a frame difference sees a static frame. "
                       "The emitted flow is the TRUE field, not a recoverable one.",
            "purpose": "adversarial; no interpolator can be scored as wrong here",
        },
    )


def _build_selftest_translate(size, seed):
    """Selftest fixture: solid background + one solid rect, INTEGER velocity.

    A uniform background is what makes np.roll the right oracle: the region the
    roll wraps in is background on both sides, so the whole frame must match
    bit-for-bit, not just the interior.
    """
    w, h = size
    rect = Rect(_q(0.20 * w), _q(0.18 * h), _q(0.52 * w), _q(0.56 * h))
    bg = Group("background", (_full_frame_rect(),), (TexDC((0.25, 0.25, 0.25)),), Motion())
    fg = Group("rect", (rect,), (TexDC((0.85, 0.85, 0.85)),), Motion(v=(5.0, 3.0)))
    return Scene("_translate", "selftest integer translation", size, seed,
                 [bg, fg], False, {"velocity_px_per_interval": [5.0, 3.0],
                                   "internal": True})


def _build_selftest_warp(size, seed):
    """Selftest fixture: TEXTURED layers whose every displacement is an integer.

    At t = 0.5 the two motions below give integral displacements in all three
    directions the module reports:

        linear    v = (6, -4)              -> to A (-3, 2), to B (3, -2), t+1 (6, -4)
        quadratic v = (4, 0), a = (8, 0)   -> to A (-3, 0), to B (5, 0),  t+1 (12, 0)

    That is what turns "the content really moved where the flow says" into a
    BIT-EXACT statement about the encoded image rather than an approximate one,
    and it is why the layers carry texture: over a flat layer the comparison
    would pass under any displacement at all.

    The two objects are separated in y by more than either one-interval step, so
    no background point can hide behind one of them in A and the other in B.
    """
    w, h = size
    tex_bg = (TexDC((0.30, 0.28, 0.26)),
              TexSin(1.0 / 9.0, 1.0 / 13.0, 0.4, (0.12, 0.10, 0.08)),
              TexSquare(0, 7.0, 0.3137, (0.20, 0.18, 0.16)))
    tex_lin = (TexDC((0.50, 0.22, 0.30)),
               TexSin(1.0 / 6.0, 1.0 / 8.0, 0.9, (0.10, 0.10, 0.10)),
               TexSquare(1, 5.0, 1.1137, (0.15, 0.30, 0.20)))
    tex_acc = (TexDC((0.20, 0.40, 0.48)),
               TexSin(-1.0 / 5.0, 1.0 / 7.0, 2.1, (0.12, 0.12, 0.12)),
               TexSquare(0, 6.0, 0.6137, (0.18, 0.14, 0.22)))
    bg = Group("background", (_full_frame_rect(),), tex_bg, Motion())
    lin = Group("linear_obj",
                (Rect(_q(0.12 * w), _q(0.10 * h), _q(0.46 * w), _q(0.34 * h)),),
                tex_lin, Motion(v=(6.0, -4.0)))
    acc = Group("accel_obj",
                (Rect(_q(0.12 * w), _q(0.60 * h), _q(0.46 * w), _q(0.88 * h)),),
                tex_acc, Motion(v=(4.0, 0.0), a=(8.0, 0.0)))
    return Scene("_warp", "selftest integral displacements", size, seed,
                 [bg, lin, acc], False, {"internal": True})


SCENE_BUILDERS = {
    "A1": _build_a1,
    "A2": _build_a2,
    "A3": _build_a3,
    "A4": _build_a4,
    "A5": _build_a5,
    "_translate": _build_selftest_translate,
    "_warp": _build_selftest_warp,
}


def list_scenes(include_private=False):
    return [k for k in SCENE_BUILDERS if include_private or not k.startswith("_")]


def build_scene(scene_id, size=DEFAULT_SIZE, seed=0):
    try:
        builder = SCENE_BUILDERS[scene_id]
    except KeyError:
        raise KeyError(f"unknown scene {scene_id!r}; known: {sorted(SCENE_BUILDERS)}")
    w, h = int(size[0]), int(size[1])
    if w < 16 or h < 16:
        raise ValueError("size must be at least 16x16")
    return builder((w, h), int(seed))


def _resolve_scene(scene, size, seed):
    """A scene id builds at `size`; a prebuilt Scene carries its own.

    `size` defaults to None rather than to DEFAULT_SIZE so that passing a Scene
    of any size just works -- a literal default would make render(scene_obj, t)
    raise for every scene that is not 640x360.
    """
    if isinstance(scene, Scene):
        if size is not None and tuple(size) != tuple(scene.size):
            raise ValueError(f"Scene built for {scene.size}, render asked for {tuple(size)}")
        return scene
    return build_scene(scene, DEFAULT_SIZE if size is None else size, seed)


# ---------------------------------------------------------------------------
# Exact compositing
# ---------------------------------------------------------------------------


def _accumulate(out_lin, out_cov, tex, offset, rect, sign, w, h):
    """Add sign * (integral of `tex` over pixel-cap-`rect`) into the accumulators.

    Only the pixel bounding box of `rect` is touched, which is what keeps a
    120-rect text overlay cheap despite the inclusion-exclusion below.
    """
    x0 = rect.x0 if rect.x0 > 0.0 else 0.0
    y0 = rect.y0 if rect.y0 > 0.0 else 0.0
    x1 = rect.x1 if rect.x1 < float(w) else float(w)
    y1 = rect.y1 if rect.y1 < float(h) else float(h)
    if x1 <= x0 or y1 <= y0:
        return
    j0 = max(int(math.floor(x0)), 0)
    j1 = min(int(math.ceil(x1)), w)
    i0 = max(int(math.floor(y0)), 0)
    i1 = min(int(math.ceil(y1)), h)
    if j1 <= j0 or i1 <= i0:
        return

    je = np.arange(j0, j1, dtype=np.float64)
    ie = np.arange(i0, i1, dtype=np.float64)
    cx0 = np.clip(je, x0, x1)
    cx1 = np.clip(je + 1.0, x0, x1)
    cy0 = np.clip(ie, y0, y1)
    cy1 = np.clip(ie + 1.0, y0, y1)

    area = (cy1 - cy0)[:, None] * (cx1 - cx0)[None, :]
    if sign > 0:
        out_cov[i0:i1, j0:j1] += area
    else:
        out_cov[i0:i1, j0:j1] -= area

    # The texture travels with its layer, so integrate it in the layer's own
    # frame: shifting the DOMAIN is exact, whereas resampling the result is not.
    val = texture_box_integral(tex, cx0 - offset[0], cx1 - offset[0],
                               cy0 - offset[1], cy1 - offset[1])
    if sign > 0:
        out_lin[i0:i1, j0:j1, :] += val
    else:
        out_lin[i0:i1, j0:j1, :] -= val


def _composite(scene, t, want_coverage=False):
    """Exact back-to-front composite of the layer stack at time `t`.

    Layer k's visible region inside a pixel is  P cap L_k \\ union_{j>k} L_j.
    Inclusion-exclusion over the GROUPS above turns that into signed integrals
    over plain rect intersections, and because each group's own rects are
    disjoint, the product of rect choices inside one term is disjoint too and
    simply sums.  Everything stays exact: no coverage-as-alpha approximation,
    which would be wrong wherever two layers' edges share a pixel.
    """
    w, h = scene.size
    n = len(scene.groups)
    rects_at = [g.rects_at(t) for g in scene.groups]
    offs = [g.motion.offset(t) for g in scene.groups]

    lin = np.zeros((h, w, 3), dtype=np.float64)
    cov_total = np.zeros((h, w), dtype=np.float64)
    cov_per = np.zeros((n, h, w), dtype=np.float64) if want_coverage else None

    for k in range(n):
        cov_k = np.zeros((h, w), dtype=np.float64)
        above = list(range(k + 1, n))
        for size_s in range(0, len(above) + 1):
            sign = -1.0 if (size_s & 1) else 1.0
            for combo_groups in itertools.combinations(above, size_s):
                choice_lists = [rects_at[j] for j in combo_groups]
                for base in rects_at[k]:
                    for combo in itertools.product(*choice_lists):
                        inter = base
                        for rr in combo:
                            inter = rect_intersect(inter, rr)
                            if inter is None:
                                break
                        if inter is None:
                            continue
                        _accumulate(lin, cov_k, scene.groups[k].texture,
                                    offs[k], inter, sign, w, h)
        cov_total += cov_k
        if want_coverage:
            cov_per[k] = cov_k
    return lin, cov_total, cov_per


# ---------------------------------------------------------------------------
# Visibility, flow, occlusion
# ---------------------------------------------------------------------------


def _frontmost_at(scene, s, px, py):
    """Index of the frontmost group covering each (px, py) at time `s`.

    Group 0 must be full-frame, which every scene here guarantees, so the
    result is defined everywhere -- including outside the frame, where the
    caller masks it off anyway.
    """
    idx = np.zeros(px.shape, dtype=np.int16)
    for k in range(1, len(scene.groups)):
        g = scene.groups[k]
        bb = g.bbox_at(s)
        # Group bbox pre-test: a 120-rect overlay would otherwise cost 120 full
        # comparisons over every pixel of the frame.
        in_bb = (px >= bb.x0) & (px < bb.x1) & (py >= bb.y0) & (py < bb.y1)
        if not in_bb.any():
            continue
        sx = px[in_bb]
        sy = py[in_bb]
        hit = np.zeros(sx.shape, dtype=bool)
        for r in g.rects_at(s):
            hit |= (sx >= r.x0) & (sx < r.x1) & (sy >= r.y0) & (sy < r.y1)
        if hit.any():
            sub = idx[in_bb]
            sub[hit] = k
            idx[in_bb] = sub
    return idx


def _visibility(scene, t, idx, xc, yc, s):
    """Is the material point seen at (xc, yc) at time `t` also visible at time `s`?"""
    w, h = scene.size
    dx = np.zeros(idx.shape, dtype=np.float64)
    dy = np.zeros(idx.shape, dtype=np.float64)
    for k, g in enumerate(scene.groups):
        d = g.motion.disp(t, s)
        m = idx == k
        if m.any():
            dx[m] = d[0]
            dy[m] = d[1]
    xs = xc + dx
    ys = yc + dy
    in_frame = (xs >= 0.0) & (xs < float(w)) & (ys >= 0.0) & (ys < float(h))
    front = _frontmost_at(scene, s, xs, ys)
    return in_frame & (front == idx.astype(np.int16))


# ---------------------------------------------------------------------------
# render
# ---------------------------------------------------------------------------

OCCL_BOTH = 0
OCCL_ONLY_A = 1
OCCL_ONLY_B = 2

FLOW_CONVENTION = (
    "flow[y,x] = displacement in pixels of the scene point visible at pixel "
    "centre (x+0.5, y+0.5) over one full source interval, from t to t+1. "
    "flow_to_a / flow_to_b give the partial displacements from t back to source "
    "frame A = floor(t) and forward to B = floor(t)+1, which is what a "
    "bidirectional interpolator actually needs. At an antialiased edge the "
    "field is discontinuous, so the value reported is that of the frontmost "
    "layer at the PIXEL CENTRE."
)

OCCL_CONVENTION = {
    "0": "visible in both source frames A = floor(t) and B = floor(t)+1",
    "1": "visible only in A (about to be occluded or to leave the frame)",
    "2": "visible only in B (disoccluded; absent from A)",
}


def render(scene, t, size=None, seed=0, return_coverage=False):
    """Render `scene` at real-valued time `t` with its exact ground truth.

    Time is in SOURCE-FRAME units: t = 0 is source frame 0, t = 1 is source
    frame 1, and any real value in between is a frame the product would have to
    generate.  The bracketing source frames are A = floor(t) and B = A + 1.

    Returns a dict with
        'rgb'         uint8  (H, W, 3)  sRGB-encoded, composed in linear light
        'flow'        float32(H, W, 2)  true displacement over [t, t+1]
        'flow_to_a'   float32(H, W, 2)  true displacement from t to A
        'flow_to_b'   float32(H, W, 2)  true displacement from t to B
        'occl'        uint8  (H, W)     0 both / 1 only-in-A / 2 only-in-B
        'layer'       uint8  (H, W)     frontmost group index at the pixel centre
        'static_mask' uint8  (H, W)     1 where the visible layer has zero velocity
        'meta'        dict              JSON-safe scene/time/motion description
        'coverage'    float64(G, H, W)  per-group exact area coverage (opt-in)

    `size` is only used when `scene` is a scene id; a prebuilt Scene carries
    its own.  It defaults to DEFAULT_SIZE = (640, 360).
    """
    sc = _resolve_scene(scene, size, seed)
    t_exact = t
    t = float(t)
    if not math.isfinite(t):
        raise ValueError("t must be finite")
    w, h = sc.size

    lin, cov_total, cov_per = _composite(sc, t, want_coverage=return_coverage)

    # The stack is exhaustive and non-overlapping by construction; if coverage
    # ever drifts from 1 the inclusion-exclusion is wrong and every metric built
    # on this frame would be wrong with it.
    cov_err = float(np.max(np.abs(cov_total - 1.0)))
    if cov_err > 1e-9:
        raise AssertionError(f"coverage does not partition the pixel (max err {cov_err:.3e})")

    lo = float(lin.min())
    hi = float(lin.max())
    if lo < -1e-9 or hi > 1.0 + 1e-9:
        raise AssertionError(
            f"linear radiance left [0,1] ({lo:.6f}..{hi:.6f}); a clamp here would "
            "silently make the reference non-analytic -- fix the texture budget")
    rgb = encode_linear_u8(np.clip(lin, 0.0, 1.0))

    xc = (np.arange(w, dtype=np.float64) + 0.5)[None, :].repeat(h, axis=0)
    yc = (np.arange(h, dtype=np.float64) + 0.5)[:, None].repeat(w, axis=1)
    idx = _frontmost_at(sc, t, xc, yc)

    t_a = math.floor(t)
    t_b = t_a + 1.0

    flow = np.zeros((h, w, 2), dtype=np.float64)
    flow_a = np.zeros((h, w, 2), dtype=np.float64)
    flow_b = np.zeros((h, w, 2), dtype=np.float64)
    for k, g in enumerate(sc.groups):
        m = idx == k
        if not m.any():
            continue
        d = g.motion.disp(t, t + 1.0)
        da = g.motion.disp(t, t_a)
        db = g.motion.disp(t, t_b)
        flow[m] = d
        flow_a[m] = da
        flow_b[m] = db

    vis_a = _visibility(sc, t, idx, xc, yc, t_a)
    vis_b = _visibility(sc, t, idx, xc, yc, t_b)
    occl = np.where(vis_a & vis_b, OCCL_BOTH,
                    np.where(vis_a, OCCL_ONLY_A,
                             np.where(vis_b, OCCL_ONLY_B, 3))).astype(np.uint8)
    n_neither = int(np.count_nonzero(occl == 3))
    if n_neither:
        raise AssertionError(
            f"{n_neither} pixels are visible in NEITHER source frame at t={t}; the "
            "three-class mask no longer partitions the frame. Non-monotone motion "
            "or a per-interval displacement wider than the frame will do this.")

    static_ids = sc.static_group_ids
    static_mask = np.isin(idx, np.array(static_ids, dtype=np.int16)).astype(np.uint8) \
        if static_ids else np.zeros((h, w), dtype=np.uint8)

    meta = {
        "scene": sc.scene_id,
        "title": sc.title,
        "adversarial": bool(sc.adversarial),
        "t": t,
        "t_exact": str(t_exact) if isinstance(t_exact, Fraction) else repr(t),
        "source_frame_a": t_a,
        "source_frame_b": t_b,
        "size": [w, h],
        "seed": int(sc.seed),
        "velocity_model": {
            g.name: {
                "layer": k,
                "model": g.motion.model,
                "v_px_per_interval": [g.motion.v[0], g.motion.v[1]],
                "a_px_per_interval2": [g.motion.a[0], g.motion.a[1]],
                "displacement_t_to_t1_px": list(g.motion.disp(t, t + 1.0)),
            } for k, g in enumerate(sc.groups)
        },
        "static_layers": [sc.groups[i].name for i in static_ids],
        "colour": {
            "compose_space": "linear",
            "transfer": "sRGB piecewise IEC 61966-2-1",
            "constants": {
                "linear_threshold": SRGB_LINEAR_THRESHOLD,
                "slope": SRGB_SLOPE,
                "alpha": SRGB_ALPHA,
                "gamma": SRGB_GAMMA,
            },
            "note": "never a 2.2 power approximation",
        },
        "conventions": {"flow": FLOW_CONVENTION, "occl": OCCL_CONVENTION},
        "occl_counts": {
            "both": int(np.count_nonzero(occl == OCCL_BOTH)),
            "only_a": int(np.count_nonzero(occl == OCCL_ONLY_A)),
            "only_b": int(np.count_nonzero(occl == OCCL_ONLY_B)),
        },
        "notes": dict(sc.notes),
    }

    # The chord-vs-curve term is the analytic part of the residual that no
    # translational interpolator can remove; report it so scoring can subtract it
    # instead of blaming the engine for physics.
    chord = {}
    for k, g in enumerate(sc.groups):
        if g.motion.a != (0.0, 0.0):
            e = g.motion.chord_error(t_a, t)
            chord[g.name] = {
                "layer": k,
                "error_px": [e[0], e[1]],
                "error_norm_px": math.hypot(e[0], e[1]),
                "max_error_px": list(g.motion.chord_error_max),
                "formula": "chord - true = 0.5 * a * u * (1 - u), u = t - floor(t)",
                "sign": "positive means the chord model places the object AHEAD of truth",
            }
    if chord:
        meta["chord_vs_curve"] = chord
        # The scalar shorthand is emitted ONLY when every accelerating layer
        # agrees.  Scoring subtracts this term from a residual, so a scene whose
        # layers accelerate differently would otherwise put one layer's number
        # in the manifest and quietly mis-correct all the others; a consumer
        # that finds no scalar is forced to read the per-layer dict.  A1-A5 all
        # move their accelerating layers together, so today it is always emitted.
        distinct = {tuple(v["error_px"]) for v in chord.values()}
        if len(distinct) == 1:
            first = next(iter(chord.values()))
            meta["chord_error_px"] = first["error_px"]
            meta["chord_error_norm_px"] = first["error_norm_px"]
        else:
            meta["chord_error_px_omitted"] = (
                f"{len(distinct)} accelerating layers disagree; "
                "read chord_vs_curve per layer")

    out = {
        "rgb": np.ascontiguousarray(rgb),
        "flow": flow.astype(np.float32),
        "flow_to_a": flow_a.astype(np.float32),
        "flow_to_b": flow_b.astype(np.float32),
        "occl": occl,
        "layer": idx.astype(np.uint8),
        "static_mask": static_mask,
        "meta": meta,
    }
    if return_coverage:
        # float64 on purpose: coverage is the exactness witness for the whole
        # module, and a float32 sum over 30k pixels loses ~5e-4 of an area --
        # enough to make a genuinely exact result look wrong.
        out["coverage"] = cov_per
    return out


# ---------------------------------------------------------------------------
# Cadence -- exact rationals, never a rounded literal
# ---------------------------------------------------------------------------


def parse_rational(text):
    """'165', '24000/1001' or '23.976' -> an exact Fraction."""
    text = str(text).strip()
    if "/" in text:
        num, den = text.split("/", 1)
        return Fraction(int(num), int(den))
    return Fraction(text)


@dataclass(frozen=True)
class Cadence:
    """Exact display/source rate pair.

    165 / (24000/1001) = 11011/1600 = 6.881875 generated frames per source
    frame.  Carrying the rounded literal 6.875 instead slips a full OUTPUT
    frame after 146 source intervals (~6.1 s of 24p) and a full SOURCE frame
    after 1001 intervals (~41.7 s) -- a quality cliff at the end of every clip
    that would be read as an engine defect.  Hence Fractions everywhere and
    `float()` only at the point of rendering.
    """
    display_hz: Fraction
    source_fps: Fraction

    @staticmethod
    def of(display_hz, source_fps):
        return Cadence(parse_rational(display_hz), parse_rational(source_fps))

    @property
    def ratio(self):
        return Fraction(self.display_hz, 1) / Fraction(self.source_fps, 1)

    def ticks(self, n_intervals):
        """[(output_tick_k, interval_n, t as an exact Fraction in [0,1)), ...]."""
        r = self.ratio
        out = []
        for n in range(int(n_intervals)):
            k0 = math.ceil(n * r)
            k1 = math.ceil((n + 1) * r)
            for k in range(k0, k1):
                out.append((k, n, Fraction(k, 1) / r - n))
        return out

    def as_json(self):
        r = self.ratio
        return {
            "kind": "rational",
            "display_hz": str(self.display_hz),
            "source_fps": str(self.source_fps),
            "ratio": f"{r.numerator}/{r.denominator}",
            "ratio_float": float(r),
            "note": "exact rational; a rounded literal drifts one output frame "
                    "per ~146 source intervals at 165 Hz / 24000-1001 fps",
        }


# ---------------------------------------------------------------------------
# Writing clips
# ---------------------------------------------------------------------------


def _sha256(path):
    hsh = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            hsh.update(chunk)
    return hsh.hexdigest()


def save_png(path, rgb):
    """Write 8-bit sRGB with NO colour chunk that a decoder could act on.

    icc_profile=None is explicit rather than implied: an iCCP / gAMA / sRGB
    chunk lets a viewer or a loader re-transform the pixels, and a reference
    that moves under the reader is not a reference.
    """
    arr = np.ascontiguousarray(np.asarray(rgb, dtype=np.uint8))
    if arr.ndim != 3 or arr.shape[2] != 3:
        raise ValueError("expected an (H, W, 3) uint8 array")
    img = Image.fromarray(arr, "RGB")
    img.info.pop("icc_profile", None)
    img.save(path, format="PNG", optimize=False, icc_profile=None)


def png_chunk_names(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    names = []
    i = 8
    while i + 8 <= len(data):
        length = int.from_bytes(data[i:i + 4], "big")
        name = data[i + 4:i + 8].decode("ascii", "replace")
        names.append(name)
        i += 12 + length
        if name == "IEND":
            break
    return names


def read_png(path):
    with Image.open(path) as img:
        return np.asarray(img.convert("RGB"), dtype=np.uint8)


def _resolve_ts(stride_or_ts, n_intervals):
    """-> (list of (tick_k, interval_n, Fraction t), cadence-json dict)."""
    if isinstance(stride_or_ts, Cadence):
        return stride_or_ts.ticks(n_intervals), stride_or_ts.as_json()
    if isinstance(stride_or_ts, int):
        if stride_or_ts < 1:
            raise ValueError("stride must be >= 1")
        ticks = []
        k = 0
        for n in range(n_intervals):
            for m in range(stride_or_ts):
                ticks.append((k, n, Fraction(m, stride_or_ts)))
                k += 1
        return ticks, {"kind": "stride", "stride": int(stride_or_ts),
                       "ratio": f"{stride_or_ts}/1", "ratio_float": float(stride_or_ts),
                       "note": "uniform stride; not a real display cadence"}
    ts = [Fraction(x) if not isinstance(x, Fraction) else x for x in stride_or_ts]
    for t in ts:
        if not (0 <= t < 1):
            raise ValueError(f"explicit t values must lie in [0,1), got {t}")
    ticks = []
    k = 0
    for n in range(n_intervals):
        for t in ts:
            ticks.append((k, n, t))
            k += 1
    return ticks, {"kind": "explicit", "ts": [str(t) for t in ts],
                   "note": "explicit t list, repeated per interval"}


def write_clip(scene, out_dir, n_source_frames, stride_or_ts, *,
               size=None, seed=0, write_npy=True, quiet=True):
    """Write a clip: source PNGs, ground-truth PNGs at the intermediate t values,
    flow/occl .npy, and a clip.json manifest with a SHA-256 for every file.

    `stride_or_ts` is a Cadence (exact rational, the real case), an int stride
    (uniform t = m/stride), or an explicit iterable of t values in [0,1).
    """
    sc = _resolve_scene(scene, size, seed)
    n_source_frames = int(n_source_frames)
    if n_source_frames < 2:
        raise ValueError("need at least 2 source frames")
    n_intervals = n_source_frames - 1
    ticks, cadence_json = _resolve_ts(stride_or_ts, n_intervals)

    out_dir = os.path.abspath(out_dir)
    src_dir = os.path.join(out_dir, "src")
    gt_dir = os.path.join(out_dir, "gt")
    os.makedirs(src_dir, exist_ok=True)
    os.makedirs(gt_dir, exist_ok=True)

    files = []

    def emit(rel, arr_or_rgb, kind, entry):
        path = os.path.join(out_dir, rel)
        if kind == "png":
            save_png(path, arr_or_rgb)
        else:
            np.save(path, arr_or_rgb, allow_pickle=False)
        entry = dict(entry)
        entry["path"] = rel.replace("\\", "/")
        entry["sha256"] = _sha256(path)
        entry["bytes"] = os.path.getsize(path)
        files.append(entry)

    def emit_frame(prefix, res, base_entry):
        emit(prefix + ".png", res["rgb"], "png", dict(base_entry, kind="rgb"))
        if write_npy:
            emit(prefix + ".flow.npy", res["flow"], "npy", dict(base_entry, kind="flow"))
            emit(prefix + ".flow_to_a.npy", res["flow_to_a"], "npy",
                 dict(base_entry, kind="flow_to_a"))
            emit(prefix + ".flow_to_b.npy", res["flow_to_b"], "npy",
                 dict(base_entry, kind="flow_to_b"))
            emit(prefix + ".occl.npy", res["occl"], "npy", dict(base_entry, kind="occl"))
            if sc.static_group_ids:
                emit(prefix + ".static.npy", res["static_mask"], "npy",
                     dict(base_entry, kind="static_mask"))

    sample_meta = None
    for n in range(n_source_frames):
        res = render(sc, float(n), size=None, seed=sc.seed)
        if sample_meta is None:
            sample_meta = res["meta"]
        emit_frame(f"src/src_{n:04d}", res,
                   {"role": "source", "source_index": n, "t": str(n), "t_float": float(n)})
        if not quiet:
            print(f"  source {n}")

    gt_count = 0
    for (k, n, t) in ticks:
        if t == 0:
            # Coincides with source frame n; the source PNG already is that
            # frame, so writing it again would only invite the two to diverge.
            files.append({"path": f"src/src_{n:04d}.png", "role": "source_aligned",
                          "kind": "rgb", "output_tick": k, "interval": n,
                          "t": "0", "t_float": 0.0, "sha256": None, "bytes": None,
                          "note": "output tick lands exactly on a source frame"})
            continue
        abs_t = Fraction(n, 1) + t
        res = render(sc, abs_t, size=None, seed=sc.seed)
        base = {"role": "ground_truth", "output_tick": k, "interval": n,
                "t": str(t), "t_float": float(t),
                "absolute_t": str(abs_t), "absolute_t_float": float(abs_t)}
        if "chord_error_px" in res["meta"]:
            base["chord_error_px"] = res["meta"]["chord_error_px"]
        emit_frame(f"gt/gt_i{n:04d}_k{k:05d}", res, base)
        gt_count += 1
        if not quiet:
            print(f"  gt interval {n} tick {k} t={t}")

    manifest = {
        "format_version": FORMAT_VERSION,
        "generator": "harness/gen/analytic.py",
        "scene": sc.scene_id,
        "title": sc.title,
        "adversarial": bool(sc.adversarial),
        "size": [sc.size[0], sc.size[1]],
        "seed": int(sc.seed),
        "n_source_frames": n_source_frames,
        "n_intervals": n_intervals,
        "n_ground_truth_frames": gt_count,
        "cadence": cadence_json,
        "colour": sample_meta["colour"],
        "conventions": sample_meta["conventions"],
        "velocity_model": sample_meta["velocity_model"],
        "static_layers": sample_meta["static_layers"],
        "scene_notes": sample_meta["notes"],
        "npy": {
            "flow_dtype": "float32", "flow_shape": "(H, W, 2)",
            "occl_dtype": "uint8", "occl_shape": "(H, W)",
            "static_mask_dtype": "uint8", "static_mask_shape": "(H, W)",
            "written": bool(write_npy),
        },
        "files": files,
    }
    with open(os.path.join(out_dir, "clip.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
        fh.write("\n")
    return manifest


def verify_clip(out_dir):
    """Re-hash every file the manifest lists.  Returns (ok, [problems])."""
    out_dir = os.path.abspath(out_dir)
    with open(os.path.join(out_dir, "clip.json"), "r", encoding="utf-8") as fh:
        manifest = json.load(fh)
    problems = []
    for entry in manifest["files"]:
        if entry.get("sha256") is None:
            continue
        path = os.path.join(out_dir, entry["path"])
        if not os.path.exists(path):
            problems.append(f"missing {entry['path']}")
            continue
        if _sha256(path) != entry["sha256"]:
            problems.append(f"sha mismatch {entry['path']}")
    return (not problems), problems


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------


class _Checks:
    def __init__(self):
        self.failed = 0
        self.total = 0

    def check(self, name, ok, detail=""):
        self.total += 1
        if not ok:
            self.failed += 1
        tag = "PASS" if ok else "FAIL"
        line = f"[{tag}] {name}"
        if detail:
            line += f"  -- {detail}"
        print(line)
        return ok


def _rect_extent_1d(a, b, lo, hi, n):
    """Exact (length, first moment) of [a,b] cap [lo,hi] against the pixel grid."""
    e = np.arange(n, dtype=np.float64)
    x0 = np.clip(e, a, b)
    x1 = np.clip(e + 1.0, a, b)
    length = float(np.sum(x1 - x0))
    moment = float(np.sum((x1 * x1 - x0 * x0) * 0.5))
    return length, moment


def _warp_agrees(sc, t, s, key, nudge=(0, 0)):
    """Do the ENCODED BYTES move where the emitted field says they move?

    Reads the displacement out of the field the module itself emits, then
    compares pixel (x, y) at time `t` with pixel (x + dx, y + dy) at time `s`.
    Only pixels whose square lies wholly inside one visible layer at BOTH times
    are compared, so with an integral displacement the two must be bit-identical
    or the field is wrong.  `nudge` deliberately corrupts the displacement, to
    prove the comparison is capable of failing.

    Returns (tested, mismatched, worst code delta).
    """
    w, h = sc.size
    rt = render(sc, t, return_coverage=True)
    rs = render(sc, s, return_coverage=True)
    tested = bad = worst = 0
    for k in range(len(sc.groups)):
        m = rt["layer"] == k
        if not m.any():
            continue
        f = rt[key][m]
        if not np.all(f == f[0]):
            raise AssertionError(f"layer {k}'s field is not rigid; the fixture is wrong")
        dx = float(f[0][0]) + nudge[0]
        dy = float(f[0][1]) + nudge[1]
        if dx != int(dx) or dy != int(dy):
            raise AssertionError(
                f"this fixture must have integral displacements, layer {k} has {(dx, dy)}")
        dx, dy = int(dx), int(dy)
        ys, xs = np.nonzero(rt["coverage"][k] == 1.0)
        xs2, ys2 = xs + dx, ys + dy
        keep = (xs2 >= 0) & (xs2 < w) & (ys2 >= 0) & (ys2 < h)
        ys, xs, ys2, xs2 = ys[keep], xs[keep], ys2[keep], xs2[keep]
        keep = rs["coverage"][k][ys2, xs2] == 1.0
        ys, xs, ys2, xs2 = ys[keep], xs[keep], ys2[keep], xs2[keep]
        if not len(xs):
            continue
        va = rt["rgb"][ys, xs].astype(np.int16)
        vb = rs["rgb"][ys2, xs2].astype(np.int16)
        tested += int(len(xs))
        bad += int(np.count_nonzero(np.any(va != vb, axis=1)))
        worst = max(worst, int(np.max(np.abs(va - vb))))
    return tested, bad, worst


def selftest():
    c = _Checks()
    print("analytic.py selftest")
    print("-" * 62)

    # --- colour ------------------------------------------------------------
    codes = np.arange(256, dtype=np.float64) / 255.0
    rt = quantize_u8(linear_to_srgb(srgb_to_linear(codes)))
    c.check("srgb roundtrip exact for all 256 codes",
            np.array_equal(rt, np.arange(256, dtype=np.uint8)),
            f"max |delta| = {int(np.max(np.abs(rt.astype(int) - np.arange(256))))}")

    lin_rt = srgb_to_linear(linear_to_srgb(np.linspace(0.0, 1.0, 100001)))
    c.check("srgb encode/decode is an exact float inverse",
            float(np.max(np.abs(lin_rt - np.linspace(0.0, 1.0, 100001)))) < 1e-12,
            f"max err {float(np.max(np.abs(lin_rt - np.linspace(0.0,1.0,100001)))):.2e}")

    join_lo = SRGB_LINEAR_THRESHOLD * SRGB_SLOPE
    join_hi = SRGB_ALPHA * SRGB_LINEAR_THRESHOLD ** (1.0 / SRGB_GAMMA) - (SRGB_ALPHA - 1.0)
    c.check("srgb piecewise branches meet at 0.0031308",
            abs(join_lo - join_hi) < 1e-5 and abs(join_lo - SRGB_ENCODED_THRESHOLD) < 1e-5,
            f"toe {join_lo:.8f} vs power {join_hi:.8f}")

    g22 = np.power(np.linspace(1e-4, 1.0, 4096), 1.0 / 2.2)
    dev = float(np.max(np.abs(linear_to_srgb(np.linspace(1e-4, 1.0, 4096)) - g22)))
    c.check("transfer is NOT the gamma-2.2 shortcut", dev > 0.01,
            f"max |srgb - x^(1/2.2)| = {dev:.4f}")

    # One hard literal, on a RENDERED frame rather than on the transfer function
    # alone: 1.055 * 0.25**(1/2.4) - 0.055 = 0.537099, and round(0.537099*255)
    # = 137, whereas the 2.2 shortcut gives 0.25**(1/2.2) = 0.532544 -> 136.
    # The two functions above could both be correct and the compositor still
    # encode somewhere else; this pins the whole compose-in-linear /
    # encode-once path to a number that was worked out by hand.
    flat = Scene("_flat", "flat 0.25 linear", (24, 20), 0,
                 [Group("bg", (_full_frame_rect(),),
                        (TexDC((0.25, 0.25, 0.25)),), Motion())])
    flat_rgb = render(flat, 0.3125)["rgb"]
    c.check("a rendered flat field of linear 0.25 is sRGB code 137 "
            "(the 2.2 shortcut would give 136)",
            bool(np.all(flat_rgb == 137)),
            f"codes present: {sorted(set(flat_rgb.ravel().tolist()))}")

    # --- texture integrals -------------------------------------------------
    tex = (TexDC((0.2, 0.3, 0.4)),
           TexSin(1.0 / 7.0, -1.0 / 11.0, 0.7, (0.2, 0.1, 0.05)),
           TexSquare(0, 9.0, 0.3137, (0.15, 0.15, 0.15)),
           TexSquare(1, 5.0, 1.1, (0.05, 0.10, 0.05)))
    nsub = 256
    x0 = np.array([13.0, 40.25]);  x1 = np.array([14.0, 41.0])
    y0 = np.array([7.0, 22.5]);    y1 = np.array([8.0, 23.0])
    exact = texture_box_integral(tex, x0, x1, y0, y1)
    riemann = np.zeros_like(exact)
    for jj in range(2):
        for ii in range(2):
            gx = x0[jj] + (np.arange(nsub) + 0.5) * (x1[jj] - x0[jj]) / nsub
            gy = y0[ii] + (np.arange(nsub) + 0.5) * (y1[ii] - y0[ii]) / nsub
            gxx, gyy = np.meshgrid(gx, gy)
            v = texture_value(tex, gxx, gyy)
            riemann[ii, jj] = v.mean(axis=(0, 1)) * (x1[jj] - x0[jj]) * (y1[ii] - y0[ii])
    err = float(np.max(np.abs(exact - riemann)))
    c.check("closed-form box integral matches a 256x256 Riemann sum", err < 2e-4,
            f"max err {err:.2e}")

    # --- per-scene structural invariants ----------------------------------
    # Several t values per scene, including t across an interval boundary
    # (2.875), because the occlusion mask brackets on floor(t) and a scene that
    # only partitions on the first interval would be a trap.
    size = (320, 208)
    for sid in list_scenes():
        worst_cov = 0.0
        partition = True
        classes = set()
        counts = None
        for tt in (0.0, 0.375, 0.75, 2.875):
            res = render(sid, tt, size=size, seed=3, return_coverage=True)
            worst_cov = max(worst_cov, float(np.max(np.abs(res["coverage"].sum(axis=0) - 1.0))))
            occ = res["occl"]
            classes |= set(np.unique(occ).tolist())
            counts = res["meta"]["occl_counts"]
            partition &= classes <= {0, 1, 2} and int(
                np.count_nonzero(occ == 0) + np.count_nonzero(occ == 1)
                + np.count_nonzero(occ == 2)) == occ.size
        c.check(f"{sid}: coverage partitions unity", worst_cov < 1e-9,
                f"max |sum cov - 1| = {worst_cov:.2e} over 4 values of t")
        c.check(f"{sid}: occlusion mask's three classes partition the frame",
                partition, f"classes={sorted(classes)} last counts={counts}")
        c.check(f"{sid}: render is deterministic for a fixed seed",
                np.array_equal(render(sid, 0.375, size=size, seed=3)["rgb"],
                               render(sid, 0.375, size=size, seed=3)["rgb"]))
        # Several consecutive seed pairs, not one lucky pair.  A5's phase used
        # to collapse onto four lattice positions, so most consecutive seeds
        # rendered BIT-IDENTICALLY while a single (3, 4) comparison still passed.
        frames = [render(sid, 0.375, size=size, seed=s)["rgb"] for s in range(5)]
        same = [(s, s + 1) for s in range(4) if np.array_equal(frames[s], frames[s + 1])]
        c.check(f"{sid}: all 4 consecutive seed pairs give different frames "
                f"(the seed is live, not lucky)", not same,
                f"identical pairs: {same}" if same else "4/4 pairs differ")

    # --- integer translation is bit-exact ---------------------------------
    a = render("_translate", 0.0, size=(96, 64), seed=0)["rgb"]
    b = render("_translate", 1.0, size=(96, 64), seed=0)["rgb"]
    rolled = np.roll(a, (3, 5), axis=(0, 1))
    c.check("rectangle translated by an integer vector is bit-exact under np.roll",
            np.array_equal(b, rolled),
            f"{int(np.count_nonzero(np.any(b != rolled, axis=2)))} differing pixels")

    # --- sub-pixel coverage sums to the object's area ----------------------
    sc1 = build_scene("A1", (640, 360), 0)
    t_sub = 0.4375
    res1 = render(sc1, t_sub, size=None, seed=0, return_coverage=True)
    for gi in (1, 2):
        want = sc1.groups[gi].rects[0].area
        got = float(res1["coverage"][gi].sum(dtype=np.float64))
        c.check(f"A1: sub-pixel coverage of '{sc1.groups[gi].name}' sums to its area",
                abs(got - want) < 1e-9, f"{got:.10f} vs {want:.10f}")

    # --- the antialiasing really lands in the pixels ------------------------
    # Independent of the coverage array: recover the edge position back out of
    # the 8-bit sRGB image the module actually writes.  A nearest-neighbour
    # stamp would quantise this to whole pixels; a gamma-2.2 encode would bias
    # it by a fixed fraction.
    sct = build_scene("_translate", (96, 64), 0)
    r0 = sct.groups[1].rects[0]
    edge_err = []
    for tt in (0.0, 0.125, 0.3333, 0.5, 0.71875, 0.9):
        rr = render(sct, tt, size=None, seed=0)
        dx, dy = sct.groups[1].motion.offset(tt)
        row = int(r0.y0 + dy) + 5
        lin = srgb_to_linear(rr["rgb"][row].astype(np.float64) / 255.0)[:, 0]
        cov1 = np.clip((lin - 0.25) / (0.85 - 0.25), 0.0, 1.0)
        j = int(np.argmax(cov1 > 0.02))
        edge_err.append(((j + 1) - cov1[j]) - (r0.x0 + dx))
    q_floor = 1.0 / 255.0 / 0.60  # 8-bit step over the scene's linear contrast
    c.check("sub-pixel edge recovered from the 8-bit image matches the analytic "
            "position to the quantisation floor",
            max(abs(e) for e in edge_err) < 1.5 * q_floor,
            f"max |err| {max(abs(e) for e in edge_err):.5f} px vs floor {q_floor:.5f} px")

    # --- flow predicts the object's position at t+1 ------------------------
    for sid in ("A1", "A2"):
        sc = build_scene(sid, (640, 360), 0)
        res = render(sc, t_sub, size=None, seed=0)
        g = sc.groups[1]
        r0 = g.rects[0]
        # Exact centroid of the object's rendered extent, from the same clipping
        # arithmetic that produces the image, at t and at t+1.
        cent = {}
        for tt in (t_sub, t_sub + 1.0):
            dx, dy = g.motion.offset(tt)
            lx, mx = _rect_extent_1d(r0.x0 + dx, r0.x1 + dx, 0.0, 640.0, 640)
            ly, my = _rect_extent_1d(r0.y0 + dy, r0.y1 + dy, 0.0, 360.0, 360)
            cent[tt] = (mx / lx, my / ly)
        moved = (cent[t_sub + 1.0][0] - cent[t_sub][0],
                 cent[t_sub + 1.0][1] - cent[t_sub][1])
        # Sample the emitted field at the object's centre pixel.
        cx = int(cent[t_sub][0])
        cy = int(cent[t_sub][1])
        f = res["flow"][cy, cx]
        worst = max(abs(float(f[0]) - moved[0]), abs(float(f[1]) - moved[1]))
        c.check(f"{sid}: flow predicts the object's position at t+1 to < 1e-4 px",
                worst < 1e-4, f"max axis error {worst:.3e} px, flow={f.tolist()}")

    # --- the flow against the IMAGE BYTES, not against our own arithmetic ----
    # The centroid check above compares the emitted field with the same clipping
    # arithmetic that produced the image, so a field that is self-consistent yet
    # does not describe where the PICTURE went would survive it.  This one would
    # not: `_warp`'s displacements are integral in all three directions, so
    # every pixel lying wholly inside one visible layer at both times must carry
    # identical encoded bytes, and its layers are textured, so agreement is not
    # free.
    scw = build_scene("_warp", (200, 140), 0)
    for (t_w, s_w, key_w) in ((0.5, 0.0, "flow_to_a"),
                              (0.5, 1.0, "flow_to_b"),
                              (0.5, 1.5, "flow")):
        n_t, n_bad, n_worst = _warp_agrees(scw, t_w, s_w, key_w)
        c.check(f"_warp: {key_w} moves the CONTENT there, bit-exactly "
                f"(t={t_w} -> {s_w})", n_bad == 0 and n_t > 5000,
                f"{n_t} interior px compared, {n_bad} mismatched, "
                f"worst code delta {n_worst}")
    n_t, n_bad, _ = _warp_agrees(scw, 0.5, 1.5, "flow", nudge=(1, 0))
    c.check("_warp: the same comparison FAILS under a 1 px wrong displacement "
            "(so the three above can fail)", n_bad > 0.2 * n_t,
            f"{n_bad}/{n_t} px mismatched under a deliberate +1 px error")

    # --- A2 chord-vs-curve --------------------------------------------------
    sc2 = build_scene("A2", (256, 160), 0)
    accel = sc2.groups[1].motion.a
    ok = True
    worst = 0.0
    for tt in (0.0, 0.125, 0.5, 0.75, 2.375):
        m = render(sc2, tt, size=None, seed=0)["meta"]
        u = tt - math.floor(tt)
        want = (0.5 * accel[0] * u * (1 - u), 0.5 * accel[1] * u * (1 - u))
        got = m["chord_error_px"]
        worst = max(worst, abs(got[0] - want[0]), abs(got[1] - want[1]))
    c.check("A2: reported chord-vs-curve error matches 0.5*a*u*(1-u)", worst < 1e-12,
            f"max err {worst:.2e}; peak |a|/8 = {sc2.groups[1].motion.chord_error_max}")
    # And the chord error really is the gap between the two motion models.
    mot = sc2.groups[1].motion
    u = 0.5
    chordp = (mot.offset(0.0)[0] + u * (mot.offset(1.0)[0] - mot.offset(0.0)[0]))
    c.check("A2: chord model minus true position equals the reported error",
            abs((chordp - mot.offset(u)[0]) - mot.chord_error(0.0, u)[0]) < 1e-12)

    # --- A3 occlusion semantics --------------------------------------------
    sc3 = build_scene("A3", (320, 200), 0)
    r3 = render(sc3, 0.5, size=None, seed=0)
    occ3 = r3["occl"]
    n0 = int(np.count_nonzero(occ3 == 0))
    n1 = int(np.count_nonzero(occ3 == 1))
    n2 = int(np.count_nonzero(occ3 == 2))
    c.check("A3: all three visibility classes are present", n0 > 0 and n1 > 0 and n2 > 0,
            f"both={n0} onlyA={n1} onlyB={n2}")
    fgr = sc3.groups[1].rects[0].translated(*sc3.groups[1].motion.offset(0.5))
    xs = np.arange(occ3.shape[1])[None, :].repeat(occ3.shape[0], axis=0)
    mid = 0.5 * (fgr.x0 + fgr.x1)
    c.check("A3: only-in-B (disoccluded) lies behind a rightward mover",
            bool(np.all(xs[occ3 == 2] < mid)))
    c.check("A3: only-in-A (about to be occluded) lies ahead of it",
            bool(np.all(xs[occ3 == 1] > mid)))
    c.check("A3: the foreground itself is visible in both frames",
            bool(np.all(occ3[r3["layer"] == 1] == 0)))

    # --- A4 static overlay --------------------------------------------------
    sc4 = build_scene("A4", (480, 300), 0)
    top = len(sc4.groups) - 1
    r4 = render(sc4, 0.3125, size=None, seed=0, return_coverage=True)
    sm = r4["static_mask"].astype(bool)
    c.check("A4: the static overlay is non-empty", int(sm.sum()) > 0, f"{int(sm.sum())} px")
    c.check("A4: the overlay's true velocity is exactly zero",
            sc4.groups[top].motion.v == (0.0, 0.0) and sc4.groups[top].motion.a == (0.0, 0.0))
    c.check("A4: flow is exactly zero on the static overlay",
            bool(np.all(r4["flow"][sm] == 0.0)))
    c.check("A4: flow is non-zero everywhere else",
            bool(np.all(np.abs(r4["flow"][~sm]).sum(axis=-1) > 0.0)))
    c.check("A4: the overlay is the frontmost layer wherever the mask is set",
            bool(np.all(r4["layer"][sm] == top)))
    c.check("A4: the overlay's known mask matches its exact coverage",
            bool(np.all(r4["coverage"][top][sm] > 0.0)),
            f"{int(sm.sum())} mask px, coverage area "
            f"{float(r4['coverage'][top].sum(dtype=np.float64)):.3f} px^2 vs "
            f"{sum(r.area for r in sc4.groups[top].rects):.3f} px^2 of rects")

    # --- A5 adversarial -----------------------------------------------------
    a5a = render("A5", 0.0, size=(256, 160), seed=0)
    a5b = render("A5", 1.0, size=(256, 160), seed=0)
    c.check("A5: is labelled adversarial", a5a["meta"]["adversarial"] is True)
    c.check("A5: an exact-period advance makes |A - B| ZERO everywhere",
            np.array_equal(a5a["rgb"], a5b["rgb"]),
            f"{int(np.count_nonzero(np.any(a5a['rgb'] != a5b['rgb'], axis=2)))} differing px")
    c.check("A5: the true field is emitted anyway (16 px/interval, not 0)",
            bool(np.all(a5a["flow"][:, :, 0] == 16.0)) and bool(np.all(a5a["flow"][:, :, 1] == 0.0)))
    a5h = render("A5", 0.5, size=(256, 160), seed=0)
    c.check("A5: the true half-period frame is NOT the blend of A and B",
            int(np.max(np.abs(a5h["rgb"].astype(int) - a5a["rgb"].astype(int)))) > 100,
            "a cross-fade would give flat grey here")
    # The seed varies nothing in A5 but the grating phase, so the phase lattice
    # IS the seed.  Four positions used to make most consecutive seeds identical.
    phases = [build_scene("A5", (128, 96), s).notes["phase_px"] for s in range(24)]
    c.check("A5: 24 seeds give at least 20 distinct grating phases",
            len(set(phases)) >= 20, f"{len(set(phases))} distinct of 24")
    c.check("A5: no phase lands on a half-pixel (an 8-bit rounding tie)",
            all(abs(p * 2.0 - round(p * 2.0)) > 1e-12 for p in phases),
            f"e.g. {phases[:4]}")

    # --- the fourth-class guard actually fires ------------------------------
    # A guard nothing ever trips is a comment.  These are the two hazards the
    # scenes above are shaped to avoid, built deliberately so render() has to
    # refuse them: a diagonally panning full-frame layer loses the frame corner
    # (out through the top going back to A, out through the side going on to B),
    # and a non-convex overlay with gaps narrower than the pan step hides a
    # pixel behind one stroke in A and a different stroke in B.
    diag = Scene("_neither", "diagonal pan under a static mark", (128, 128), 0,
                 [Group("bg", (_full_frame_rect(),), (TexDC((0.30, 0.30, 0.30)),),
                        Motion(v=(5.0, 5.0))),
                  Group("mark", (Rect(60.0, 60.0, 70.0, 70.0),),
                        (TexDC((0.90, 0.90, 0.90)),), Motion())])
    raised = ""
    try:
        render(diag, 0.5)
    except AssertionError as exc:
        raised = str(exc)
    c.check("a pixel visible in NEITHER source frame is REFUSED, not silently "
            "given a fourth class", "NEITHER" in raised,
            raised.split(";")[0][:88] if raised else "render() returned a frame")

    slats = Scene("_slats", "overlay with gaps narrower than the pan step",
                  (140, 100), 0,
                  [Group("bg", (_full_frame_rect(),), (TexDC((0.30, 0.30, 0.30)),),
                         Motion(v=(-9.0, 0.0))),
                   Group("slats", tuple(Rect(20.0 + 12 * i, 10.0, 30.0 + 12 * i, 90.0)
                                        for i in range(6)),
                         (TexDC((0.90, 0.90, 0.90)),), Motion())])
    raised = ""
    try:
        render(slats, 0.5)
    except AssertionError as exc:
        raised = str(exc)
    c.check("a non-convex overlay whose gaps are narrower than the pan step is "
            "REFUSED", "NEITHER" in raised,
            raised.split(";")[0][:88] if raised else "render() returned a frame")

    # --- A4's size floor is real; it must refuse loudly, not build a mess ----
    # A4's cell size is driven by the pan speed, not by legibility, so there is
    # a smallest frame it can lay out.  Measured: 160x120 builds, 128x96 does
    # not.  Stated here so a caller meets a message instead of a surprise.
    small = ""
    try:
        build_scene("A4", (128, 96), 0)
    except ValueError as exc:
        small = str(exc)
    c.check("A4 refuses a frame too small for its layout, with a reason",
            bool(small), small[:88] or "it built a scene at 128x96")
    c.check("A4 does build at 160x120", build_scene("A4", (160, 120), 0).size == (160, 120))

    # --- PNG bytes ----------------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "rt.png")
        rgb = render("A3", 0.625, size=(160, 112), seed=1)["rgb"]
        save_png(p, rgb)
        c.check("PNG round-trips bit-exactly", np.array_equal(read_png(p), rgb))
        names = png_chunk_names(p)
        bad = [n for n in names if n in ("gAMA", "sRGB", "iCCP", "cHRM")]
        c.check("PNG carries no gAMA/sRGB/iCCP/cHRM chunk", not bad, f"chunks={names}")

    # --- cadence is rational ------------------------------------------------
    cad = Cadence.of("165", "24000/1001")
    r = cad.ratio
    c.check("cadence 165 / (24000/1001) is the exact rational 11011/1600",
            r == Fraction(11011, 1600), f"{r} = {float(r)}")
    rounded = Fraction(6875, 1000)
    drift_146 = abs(146 * (r - rounded))
    n_full = None
    n = 1
    while n < 100000:
        if abs(n * (r - rounded)) >= r:
            n_full = n
            break
        n += 1
    c.check("a rounded 6.875 literal slips >1 output frame within 146 intervals",
            drift_146 > 1, f"drift {float(drift_146):.5f} output frames at 146; "
                           f"a full SOURCE frame at {n_full} intervals (~{n_full/24:.1f}s of 24p)")
    ticks = cad.ticks(3)
    c.check("every emitted t is an exact Fraction in [0,1)",
            all(isinstance(t, Fraction) and 0 <= t < 1 for (_, _, t) in ticks),
            f"{len(ticks)} ticks over 3 intervals, first={[str(t) for _,_,t in ticks[:3]]}")
    c.check("tick count over 3 intervals matches the exact ratio",
            len(ticks) == math.ceil(3 * r) - math.ceil(0 * r),
            f"{len(ticks)} vs ceil(3*{r}) = {math.ceil(3*r)}")

    # --- write_clip ---------------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "clip")
        man = write_clip("A2", out, 3, cad, size=(128, 96), seed=5)
        ok, problems = verify_clip(out)
        c.check("write_clip manifest hashes verify", ok, "; ".join(problems) or "all files match")
        c.check("write_clip records the exact rational cadence",
                man["cadence"]["ratio"] == "11011/1600", man["cadence"]["ratio"])
        c.check("write_clip wrote every non-source-aligned tick",
                man["n_ground_truth_frames"] ==
                sum(1 for (_, _, t) in cad.ticks(2) if t != 0),
                f"{man['n_ground_truth_frames']} gt frames")
        c.check("write_clip records a per-file SHA-256",
                all(f["sha256"] for f in man["files"] if f["role"] != "source_aligned"))
        gt = [f for f in man["files"] if f["role"] == "ground_truth" and f["kind"] == "rgb"]
        c.check("ground-truth t values are stored as exact rationals",
                all("/" in f["t"] or f["t"] in ("0", "1") for f in gt),
                f"e.g. {[f['t'] for f in gt[:3]]}")
        c.check("clip.json is valid JSON with no numpy leakage",
                isinstance(json.dumps(man), str))
        sample = os.path.join(out, gt[0]["path"])
        c.check("a written ground-truth PNG re-reads bit-exactly",
                np.array_equal(read_png(sample),
                               render("A2", Fraction(gt[0]["absolute_t"]),
                                      size=(128, 96), seed=5)["rgb"]))

    print("-" * 62)
    print(f"{c.total - c.failed}/{c.total} assertions passed")
    return 0 if c.failed == 0 else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _parse_size(text):
    w, h = text.lower().split("x")
    return (int(w), int(h))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true",
                    help="assert this module's own correctness and print PASS/FAIL")
    ap.add_argument("--list-scenes", action="store_true")
    ap.add_argument("--scene", help="scene id to write, e.g. A3")
    ap.add_argument("--out", help="output directory for --scene")
    ap.add_argument("--frames", type=int, default=8, help="number of SOURCE frames")
    ap.add_argument("--size", type=_parse_size, default=(640, 360), help="WxH")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--stride", type=int,
                    help="uniform intermediate stride (t = m/stride); "
                         "mutually exclusive with --display-hz/--source-fps")
    # No argparse defaults on the rate pair: the defaults are applied below, so
    # "was it given?" stays answerable and --stride can actually be enforced as
    # mutually exclusive instead of only being documented as such.
    ap.add_argument("--display-hz", default=None, help="default 165")
    ap.add_argument("--source-fps", default=None, help="default 24000/1001")
    ap.add_argument("--no-npy", action="store_true", help="skip the flow/occl .npy files")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.list_scenes:
        for s in list_scenes():
            sc = build_scene(s, (320, 200), 0)
            print(f"{s}  {sc.title:22s} adversarial={sc.adversarial}  {sc.notes.get('purpose','')}")
        return 0
    if not args.scene or not args.out:
        ap.print_help()
        return 2

    # `is not None`, not truthiness: --stride 0 is a mistake worth reporting,
    # not a silent fallback to the display cadence that writes a clip nobody
    # asked for.
    if args.stride is not None:
        if args.stride < 1:
            ap.error("--stride must be >= 1")
        if args.display_hz is not None or args.source_fps is not None:
            ap.error("--stride is mutually exclusive with --display-hz/--source-fps")
        spec = args.stride
    else:
        spec = Cadence.of(args.display_hz or "165", args.source_fps or "24000/1001")
    man = write_clip(args.scene, args.out, args.frames, spec,
                     size=args.size, seed=args.seed, write_npy=not args.no_npy,
                     quiet=False)
    total = sum(f["bytes"] or 0 for f in man["files"])
    print(f"wrote {man['n_source_frames']} source + {man['n_ground_truth_frames']} "
          f"ground-truth frames ({total/1e6:.1f} MB) to {os.path.abspath(args.out)}")
    if not args.no_npy:
        print("the flow/occl .npy fields dominate that; --no-npy writes PNGs only")
    print(f"cadence {man['cadence']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
