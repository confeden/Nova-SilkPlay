"""Nova SilkPlay quality harness - the metric set.

There is NO single score here, on purpose. `score_frame` returns one flat row per
(clip, frame, t); the report prints clip x stratum. A pooled headline scalar is
rejected by the harness design: on a static pixel A == B == GT the reference error
is zero, so every "improvement ratio" divides by ~0 and the pooled number is set by
whichever clip happened to contain the most sky.

Two conventions run through the whole file and both are load-bearing.

COLOUR. Frames on disk are 8-bit sRGB PNG. The engine reads them through _SRGB
views, so its arithmetic happens on LINEAR values, and writes back through an _SRGB
render target. Every reference this module synthesizes (the cross-fade X in
`ghost_gf_rf` and `grain_floor`) is therefore composed in linear light and encoded
back with the exact piecewise sRGB transfer function - never a 2.2 power. The
shortcut is not harmless: at sRGB 0.5 the two transfers differ by 0.0036 in linear,
a fixed bias on every metric in the harness that nobody would ever find.

DOMAIN PER METRIC, chosen once and stated where it is used:
  * PSNR and grain floor      -> 8-bit sRGB, the domain the viewer actually sees.
  * ghost GF/RF, halo, flicker-> linear light, because they are linear-algebra
                                 statements about the engine's own blend.
  * pos_err_px (NCC template) -> sRGB luma; NCC is contrast-normalised anyway and
                                 sRGB keeps usable contrast in the shadows, so a
                                 dark moving object still registers.
  * edge_w_px                 -> linear luma; the smear is produced by a linear
                                 cross-fade, so its plateau sits at exactly t and
                                 the 10/90 crossings land where the geometry says.

REFUSAL. A metric that cannot be computed returns None and appends the reason to
the caller's `notes` list. It never returns a fabricated number, and `score_frame`
carries the reasons into the row's `notes` column.

Pure stdlib + numpy. Deterministic: no time, no unseeded RNG.

Run `python metrics.py --selftest` to make it prove itself.
"""

from __future__ import annotations

import argparse
import math
import sys

import numpy as np

__all__ = [
    "srgb_to_linear", "linear_to_srgb", "decode_srgb8", "encode_srgb8",
    "psnr_region", "pos_err_px", "edge_w_px", "epe", "ghost_gf_rf",
    "halo_leak", "flicker", "acf_at_lag", "grain_floor",
    "ring_from_mask", "score_frame",
]


# --------------------------------------------------------------------------
# sRGB transfer function
# --------------------------------------------------------------------------
# IEC 61966-2-1. The decode threshold is written here as 12.92 * 0.0031308 rather
# than the spec's rounded 0.04045 so that encode(decode(v)) is exactly the identity.
# The two differ by 5e-7, i.e. between 8-bit codes 10.3147 and 10.3148 - no 8-bit
# value can land in the gap, so this costs nothing and buys an exact round trip.
_LIN_CUT = 0.0031308
_SRGB_CUT = 12.92 * _LIN_CUT


def srgb_to_linear(s):
    """sRGB signal in [0,1] -> linear light in [0,1]. Exact piecewise form."""
    s = np.asarray(s, dtype=np.float64)
    return np.where(s <= _SRGB_CUT, s / 12.92,
                    np.power((np.maximum(s, _SRGB_CUT) + 0.055) / 1.055, 2.4))


def linear_to_srgb(l):
    """Linear light in [0,1] -> sRGB signal in [0,1]. Exact piecewise form."""
    l = np.asarray(l, dtype=np.float64)
    return np.where(l <= _LIN_CUT, l * 12.92,
                    1.055 * np.power(np.maximum(l, _LIN_CUT), 1.0 / 2.4) - 0.055)


# The decode input is always one of 256 codes, so the transfer is a table. This is
# not an approximation: the table holds srgb_to_linear(k/255) evaluated by the same
# function, so the result is bit-identical to computing it per pixel - it just turns
# a pow() over 11 M pixels into an index. Measured on the dev machine: one 1440p
# 3-stratum row went from 16.6 s to 8.8 s, with every column bit-identical.
_SRGB8_TO_LINEAR = srgb_to_linear(np.arange(256, dtype=np.float64) / 255.0)


def decode_srgb8(img):
    """uint8 sRGB image -> float64 linear light, shape (H, W, C)."""
    return _SRGB8_TO_LINEAR[_as_srgb8(img, "image")]


def encode_srgb8(lin):
    """float64 linear light -> uint8 sRGB, the way the engine's _SRGB RTV would."""
    s = linear_to_srgb(np.clip(np.asarray(lin, dtype=np.float64), 0.0, 1.0))
    return np.clip(np.rint(s * 255.0), 0, 255).astype(np.uint8)


# --------------------------------------------------------------------------
# Minimum-support constants. Each one is a refusal threshold, so each states the
# reasoning that set it rather than being a round number someone liked.
# --------------------------------------------------------------------------

# For Gaussian-ish error the MSE estimate has relative standard error sqrt(2/N),
# which at N = 1024 is 4.4 %, i.e. +-0.19 dB. The tier-to-tier differences this
# harness has to resolve are of order 0.3 dB, so below 1024 samples a PSNR cannot
# decide anything and is refused instead of printed. (N counts pixels; the three
# channels are correlated in real content, so they are not counted as 3N.)
MIN_PSNR_PX = 1024

# NCC peak location: same support argument, and the mask is first eroded by the
# search radius so that every candidate shift reads in-frame pixels.
MIN_NCC_PX = 1024

# A single ring band is read as the SHAPE of a profile, not as a decision number,
# so it is allowed to be much smaller - but under 64 px one bad pixel sets it.
MIN_BAND_PX = 64

# EPE is a percentile over motion cells, not pixels; 16 cells is the floor at which
# a p95 is a measurement of anything at all (it interpolates between cells 14 and 15).
MIN_EPE_CELLS = 16

# A profile shorter than this cannot show a 10-90 rise plus the plateaus that define
# its 0 % and 100 % levels.
MIN_EDGE_SAMPLES = 16

# A profile sample is the mean over the masked pixels of one column (or row). Under
# 4 contributing pixels a single grain pixel sets the sample.
MIN_EDGE_COVER = 4

# Below this peak-to-trough contrast (fraction of full linear scale) there is no
# edge, only noise: single-pixel film grain of sigma = 6/255 sRGB is ~0.022 linear
# at mid-grey, so a "10 % crossing" of anything shallower is a grain crossing.
MIN_EDGE_CONTRAST = 0.05

# The prototype's test bar moves 42 px per source frame and the worst position error
# seen so far is +5 px (NVOFA field on the adversarial clip, ROADMAP "Now"). 12 px
# covers ~2.4x that while keeping the (2R+1)^2 map small. A peak that lands on the
# search boundary is REFUSED, not clamped, so too small a radius reports itself.
DEFAULT_POS_RADIUS = 12

# An NCC map is "flat" along an axis below this. The FFT and brute-force maps agree
# to 1.7e-14 (selftest), so 1e-9 sits six orders above the arithmetic noise and far
# below any real structure.
_NCC_FLAT_EPS = 1e-9

# ...and an axis is unobservable when its fall-off is this small a fraction of the
# other axis's, which is the aperture problem stated numerically: a full-height bar
# with a little grain is not exactly flat along y, but 2 % of the horizontal fall-off
# is grain, not position.
_AXIS_OBSERVABLE_FRAC = 0.02

# flicker must span at least this many source intervals: motion is estimated once
# per source pair (I6), so a defect that pulses once per interval is invisible
# inside one interval by construction. Three intervals is the shortest window in
# which such a pulse can be separated from a one-off transient.
MIN_FLICKER_INTERVALS = 3


# --------------------------------------------------------------------------
# small shared helpers
# --------------------------------------------------------------------------

def _as_srgb8(img, name):
    """Frames are 8-bit sRGB by contract; anything else is a caller bug, loudly."""
    a = np.asarray(img)
    if a.dtype != np.uint8:
        raise TypeError(f"{name}: expected uint8 sRGB, got {a.dtype}. "
                        "Floats here would silently be read as 0..255 sRGB codes.")
    if a.ndim == 2:
        a = a[:, :, None]
    if a.ndim != 3 or a.shape[2] not in (1, 3, 4):
        raise ValueError(f"{name}: expected (H,W), (H,W,3) or (H,W,4), got {a.shape}")
    return a[:, :, :3] if a.shape[2] == 4 else a  # alpha is not part of the picture


def _as_mask(mask, shape2, name="mask"):
    m = np.asarray(mask)
    if m.dtype != bool:
        m = m.astype(bool)
    if m.shape != tuple(shape2):
        raise ValueError(f"{name}: shape {m.shape} does not match image {tuple(shape2)}")
    return m


def _note(notes, tag, reason):
    if notes is not None:
        notes.append(f"{tag}: {reason}")
    return None


def _refuse_small(mask, min_px, notes, tag):
    n = int(np.count_nonzero(mask))
    if n < min_px:
        _note(notes, tag, f"refused, mask has {n} px < {min_px} px minimum")
        return True, n
    return False, n


_REC709 = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)


def _luma(arr3):
    """(H,W,C) -> (H,W). Rec.709 weights; C==1 passes through."""
    if arr3.shape[2] == 1:
        return arr3[:, :, 0]
    return arr3 @ _REC709


def _linear_luma(img):
    return _luma(decode_srgb8(img))


def _srgb_luma(img):
    return _luma(_as_srgb8(img, "image").astype(np.float64) / 255.0)


def _xfade_linear(a, b, t):
    """The engine's blend mode: a cross-fade in LINEAR light at phase t."""
    la = decode_srgb8(a)
    lb = decode_srgb8(b)
    return (1.0 - t) * la + t * lb


# --------------------------------------------------------------------------
# 1. PSNR over a mask
# --------------------------------------------------------------------------

def psnr_region(x, gt, mask, min_px=MIN_PSNR_PX, notes=None, tag="psnr"):
    """PSNR of `x` against `gt` restricted to `mask`, in dB. None if refused.

    Computed on 8-BIT sRGB VALUES, not on linear light. That is deliberate: PSNR is
    only ever used here as a coarse "did the picture change" number, and the domain
    the viewer sees is the sRGB one - a linear-light PSNR is dominated by the
    highlights and would rate a crushed-shadow output as excellent.

    Returns +inf when the two are bit-identical on the mask. That is a fact about
    the pair, not a fabricated number, and the caller records it as such.
    """
    xa = _as_srgb8(x, "x")
    ga = _as_srgb8(gt, "gt")
    if xa.shape != ga.shape:
        raise ValueError(f"psnr_region: shape mismatch {xa.shape} vs {ga.shape}")
    m = _as_mask(mask, xa.shape[:2])
    refused, n = _refuse_small(m, min_px, notes, tag)
    if refused:
        return None
    # index first, widen second: on a small stratum of a 1440p frame this converts
    # thousands of pixels instead of eleven million.
    d = xa[m].astype(np.float64) - ga[m].astype(np.float64)
    mse = float(np.mean(d * d))
    if mse == 0.0:
        _note(notes, tag, f"bit-identical on {n} px, PSNR is +inf")
        return math.inf
    return 10.0 * math.log10(255.0 * 255.0 / mse)


# --------------------------------------------------------------------------
# 2. Sub-pixel position error by masked normalized cross-correlation
# --------------------------------------------------------------------------

def _next_fast_len(n):
    """Smallest 2-3-5-smooth integer >= n. numpy's FFT is slow on large primes."""
    while True:
        m = n
        for p in (2, 3, 5):
            while m % p == 0:
                m //= p
        if m == 1:
            return n
        n += 1


def _ncc_map(tmpl, ref, mask, radius):
    """NCC of `tmpl` shifted by (dy,dx) against `ref`, over `mask`, |d| <= radius.

    Returns (C, n_px) with C of shape (2R+1, 2R+1), C[oy,ox] the NCC at
    (dy,dx) = (oy-R, ox-R), or (None, n_px) when there is nothing to correlate.

    Everything is a cross-correlation, so it is done with three FFTs instead of the
    (2R+1)^2 brute-force reductions - the brute force is O(area * R^2) and a
    full-frame stratum at 1440p makes it seconds per frame. `_ncc_map_bruteforce`
    below is the same definition written the obvious way, and the selftest asserts
    the two agree; that is what licenses the fast path.

    With W = sum(mask), gm the masked mean of ref and gc = mask*(ref-gm):
        num(d)   = sum gc(p) * T(p+d)            (= sum w (T-tm)(ref-gm), as sum gc = 0)
        s1(d)    = sum w(p) * T(p+d)
        s2(d)    = sum w(p) * T(p+d)^2
        den_t(d) = s2 - s1^2/W ,  den_g = sum gc^2
        NCC(d)   = num / sqrt(den_t * den_g)
    """
    R = int(radius)
    H, W_ = mask.shape
    # Erode against the image border only, so every candidate shift reads in-frame
    # pixels; the mask's own shape needs no erosion because the template window is
    # the mask's bounding box, not a per-pixel neighbourhood.
    inner = np.zeros_like(mask)
    if H > 2 * R and W_ > 2 * R:
        inner[R:H - R, R:W_ - R] = mask[R:H - R, R:W_ - R]
    n = int(np.count_nonzero(inner))
    if n == 0:
        return None, 0
    ys, xs = np.nonzero(inner)
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x0, x1 = int(xs.min()), int(xs.max()) + 1

    w = inner[y0:y1, x0:x1].astype(np.float64)
    g = ref[y0:y1, x0:x1].astype(np.float64)
    Wsum = float(w.sum())
    gm = float((w * g).sum() / Wsum)
    gc = w * (g - gm)
    den_g = float((gc * gc).sum())
    if den_g <= 0.0:
        return None, n  # the reference is flat here: no feature to locate

    # Offsetting the template by any constant leaves NCC exactly invariant (both
    # num and den_t are offset-free); subtracting gm just keeps the FFT well
    # conditioned - magnitudes stay O(1) instead of O(255^2 * area).
    T = tmpl[y0 - R:y1 + R, x0 - R:x1 + R].astype(np.float64) - gm

    h, wd = w.shape
    # Linear (non-circular) correlation needs N >= h + 2R: for lag o in [0,2R] the
    # index p+o stays below N, so nothing wraps.
    n0 = _next_fast_len(h + 2 * R)
    n1 = _next_fast_len(wd + 2 * R)
    FT = np.fft.rfft2(T, s=(n0, n1))
    FT2 = np.fft.rfft2(T * T, s=(n0, n1))

    def xcorr(small, big_fft):
        sp = np.conj(np.fft.rfft2(small, s=(n0, n1))) * big_fft
        return np.fft.irfft2(sp, s=(n0, n1))[:2 * R + 1, :2 * R + 1]

    num = xcorr(gc, FT)
    s1 = xcorr(w, FT)
    s2 = xcorr(w, FT2)
    den_t = np.maximum(s2 - (s1 * s1) / Wsum, 0.0)
    denom = np.sqrt(den_t * den_g)
    C = np.where(denom > 0.0, num / np.where(denom > 0.0, denom, 1.0), 0.0)
    return C, n


def _ncc_map_bruteforce(tmpl, ref, mask, radius):
    """The same definition written the obvious way. Only the selftest calls it."""
    R = int(radius)
    H, W_ = mask.shape
    inner = np.zeros_like(mask)
    if H > 2 * R and W_ > 2 * R:
        inner[R:H - R, R:W_ - R] = mask[R:H - R, R:W_ - R]
    ys, xs = np.nonzero(inner)
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x0, x1 = int(xs.min()), int(xs.max()) + 1
    w = inner[y0:y1, x0:x1].astype(np.float64)
    g = ref[y0:y1, x0:x1].astype(np.float64)
    Wsum = float(w.sum())
    gm = float((w * g).sum() / Wsum)
    gc = w * (g - gm)
    den_g = float((gc * gc).sum())
    C = np.zeros((2 * R + 1, 2 * R + 1), dtype=np.float64)
    for oy in range(2 * R + 1):
        dy = oy - R
        for ox in range(2 * R + 1):
            dx = ox - R
            tt = tmpl[y0 + dy:y1 + dy, x0 + dx:x1 + dx].astype(np.float64)
            tm = float((w * tt).sum() / Wsum)
            den_t = float((w * tt * tt).sum()) - Wsum * tm * tm
            if den_t <= 0.0:
                continue
            C[oy, ox] = float((gc * tt).sum()) / math.sqrt(den_t * den_g)
    return C


def _parabolic(cm, c0, cp):
    """Sub-sample peak offset in [-1,1] from three samples around a maximum.

    Returns None when the triple is not concave, i.e. when the peak is not actually
    bracketed - fitting a parabola there produces a confident wrong answer.
    """
    den = cm - 2.0 * c0 + cp
    if den >= -1e-12:
        return None
    off = 0.5 * (cm - cp) / den
    return off if -1.0 <= off <= 1.0 else None


# Least-squares quadratic over a 3x3 stencil (Savitzky-Golay, order 2). The columns
# {1,x,y,x^2,y^2,xy} are orthogonal on this grid once x^2 and y^2 have their means
# removed, so the coefficients are these fixed kernels and no solve is needed.
_SG_I = np.array([-1.0, 0.0, 1.0])
_SG_X = np.tile(_SG_I, (3, 1))
_SG_Y = _SG_X.T


def _parabolic2d(p):
    """Sub-sample offset (dy, dx) of the peak of a 3x3 NCC neighbourhood, or None.

    The separable version - one parabola per axis through the integer peak - is
    biased whenever the correlation ridge is TILTED, which it is for any content
    with diagonal structure: the y-parabola is then evaluated half a pixel off the
    true x of the peak and slides along the ridge. Measured on the selftest's
    diagonal texture, that bias was 0.30 px on an axis whose true error is 0, which
    is larger than the errors this metric exists to resolve. Fitting the full
    quadratic (which has the xy cross term) and solving for its stationary point
    removes it.

    Returns None unless the fitted surface really has an interior maximum
    (negative-definite Hessian) within one sample of the integer peak.
    """
    p = np.asarray(p, dtype=np.float64)
    b = float((_SG_X * p).sum()) / 6.0                    # d/dx
    c = float((_SG_Y * p).sum()) / 6.0                    # d/dy
    f = float((_SG_X * _SG_Y * p).sum()) / 4.0            # d2/dxdy
    d = float(((3.0 * _SG_X * _SG_X - 2.0) * p).sum()) / 6.0   # coefficient of x^2
    e = float(((3.0 * _SG_Y * _SG_Y - 2.0) * p).sum()) / 6.0   # coefficient of y^2
    det = 4.0 * d * e - f * f
    if d >= 0.0 or e >= 0.0 or det <= 1e-15:
        return None
    ox = (-2.0 * e * b + f * c) / det
    oy = (f * b - 2.0 * d * c) / det
    if abs(ox) > 1.0 or abs(oy) > 1.0:
        return None
    return oy, ox


def pos_err_px(x, gt, mask, radius=DEFAULT_POS_RADIUS, min_px=MIN_NCC_PX,
               notes=None, tag="pos"):
    """Signed displacement error of the CONTENT of `x` relative to `gt`, in pixels.

    This is the generalisation of the hand-made "where did the bar land" check the
    project used until now (prototype README: A at 711..787, B at 753..829, mc t=0.5
    at 733..809 against a 732..808 ideal). It locates the tracked feature by masked
    normalized cross-correlation and refines the peak with a parabolic fit, so it
    keeps working on content that has no hand-measurable bar - and it has no
    ceiling: a frame can be at 40 dB PSNR and still be 3 px late, which is the error
    a viewer actually sees as judder.

    Sign: +dx means the content in `x` sits dx pixels to the RIGHT of where `gt` has
    it, +dy means further DOWN. (The peak is where `x` must be sampled to align
    with `gt`.)

    Returns {"dx","dy","mag","peak_ncc","n_px","subpx"} or None. `dx` or `dy` is
    individually None when that axis is unobservable (see the aperture-problem note
    below); `mag` is None unless both axes are measured.
    """
    xl = _srgb_luma(x)
    gl = _srgb_luma(gt)
    if xl.shape != gl.shape:
        raise ValueError(f"pos_err_px: shape mismatch {xl.shape} vs {gl.shape}")
    m = _as_mask(mask, xl.shape)
    R = int(radius)
    inner_count = int(np.count_nonzero(m[R:max(R, m.shape[0] - R),
                                        R:max(R, m.shape[1] - R)]))
    if inner_count < min_px:
        _note(notes, tag, f"refused, {inner_count} px inside the {R} px search border "
                          f"< {min_px} px minimum")
        return None
    C, n = _ncc_map(xl, gl, m, R)
    if C is None:
        _note(notes, tag, "refused, reference is flat on this mask (no feature to locate)")
        return None

    # The aperture problem, handled per axis instead of pretended away. The project's
    # own test clip is a FULL-HEIGHT bar: its NCC is flat along y, so its vertical
    # position carries no information and argmax would return an arbitrary row (with
    # grain, a noise-driven one). An axis whose NCC fall-off is under
    # _AXIS_OBSERVABLE_FRAC of the other axis's is reported as None with a reason,
    # and the axis that IS observable is still measured.
    fall_y = float(np.max(C.max(axis=0) - C.min(axis=0)))
    fall_x = float(np.max(C.max(axis=1) - C.min(axis=1)))
    obs_y = fall_y > max(_NCC_FLAT_EPS, _AXIS_OBSERVABLE_FRAC * fall_x)
    obs_x = fall_x > max(_NCC_FLAT_EPS, _AXIS_OBSERVABLE_FRAC * fall_y)
    if not obs_y and not obs_x:
        _note(notes, tag, "refused, NCC is flat in both axes on this mask")
        return None

    if obs_y and obs_x:
        py, px = (int(v) for v in np.unravel_index(int(np.argmax(C)), C.shape))
    elif obs_x:
        px = int(np.argmax(C.mean(axis=0)))
        py = R
        _note(notes, tag, f"dy is None: vertical position is unobservable here "
                          f"(NCC fall-off along y is {fall_y:.2e} vs {fall_x:.2e} along x)")
    else:
        py = int(np.argmax(C.mean(axis=1)))
        px = R
        _note(notes, tag, f"dx is None: horizontal position is unobservable here "
                          f"(NCC fall-off along x is {fall_x:.2e} vs {fall_y:.2e} along y)")

    for lbl, idx, live in (("y", py, obs_y), ("x", px, obs_x)):
        if live and (idx == 0 or idx == 2 * R):
            _note(notes, tag, f"refused, NCC peak at the {lbl} search boundary "
                              f"(d{lbl}={idx - R}, radius {R}); the true shift may be larger")
            return None

    oy = ox = None
    if obs_y and obs_x:
        sub = _parabolic2d(C[py - 1:py + 2, px - 1:px + 2])
        if sub is not None:
            oy, ox = sub
        else:
            # Not an interior maximum: a ridge or a saddle. Fall back to one parabola
            # per axis, which needs only concavity along each.
            _note(notes, tag, "2-D peak fit declined (correlation surface is not an "
                              "interior maximum); fell back to a per-axis parabola")
            oy = _parabolic(C[py - 1, px], C[py, px], C[py + 1, px])
            ox = _parabolic(C[py, px - 1], C[py, px], C[py, px + 1])
    elif obs_x:
        prof = C.mean(axis=0)
        ox = _parabolic(prof[px - 1], prof[px], prof[px + 1])
    else:
        prof = C.mean(axis=1)
        oy = _parabolic(prof[py - 1], prof[py], prof[py + 1])

    if (obs_y and oy is None) or (obs_x and ox is None):
        _note(notes, tag, "sub-pixel refinement declined on at least one axis "
                          "(peak not concave); that axis is the integer peak")
    dy = float((py - R) + (oy or 0.0)) if obs_y else None
    dx = float((px - R) + (ox or 0.0)) if obs_x else None
    return {"dx": dx, "dy": dy,
            "mag": (float(math.hypot(dx, dy)) if (dx is not None and dy is not None)
                    else None),
            "peak_ncc": float(C[py, px]), "n_px": n,
            "subpx": bool((oy is not None or not obs_y) and (ox is not None or not obs_x))}


# --------------------------------------------------------------------------
# 3. Edge width - the ghosting number
# --------------------------------------------------------------------------

def _walk_cross(prof, start, step, level, below):
    """From `start`, walk in direction `step` to the first sample on the far side of
    `level`, then return the linearly interpolated crossing position."""
    n = len(prof)
    i = start
    while 0 <= i < n:
        v = prof[i]
        if (v <= level) if below else (v >= level):
            j = i - step
            if not (0 <= j < n):
                return float(i)
            vj = prof[j]
            if vj == v:
                return float(i)
            return float(j) + step * (level - vj) / (v - vj)
        i += step
    return None


def edge_w_px(x, mask, axis=1, notes=None, tag="edge"):
    """10-90 % rise width of a moving edge, in pixels, plus the feature's support.

    Ghosting shows up here directly and automatically. On the project's own test
    case - a 77 px bar moving 42 px per source frame - a correct motion-compensated
    frame gives one sharp edge (rise ~0.8 px, support ~77.8 px) while the linear
    cross-fade at t=0.5 gives rise ~42.7 px and support ~119.7 px: the 118 px smear
    the prototype measured by hand, now a number.

    `axis` is the axis the profile runs ALONG (1 = a horizontal profile of x, for an
    edge that moves horizontally). The profile is the masked mean over the other
    axis of LINEAR luma - linear because the smear is made by a linear cross-fade,
    so its plateau sits at exactly t and the 10/90 crossings land where the geometry
    puts them rather than where the transfer curve bends them.

    A trough (dark feature on a light background) is handled by inverting the
    profile, so "rise" always means the leading flank of the feature; `polarity`
    records which it was. A profile with only ONE flank - an object edge that the
    stratum mask clips, which is the ordinary case on real content - is left alone
    and reported as "peak", with the flank in whichever of rise/fall it really is
    and the other None.

    Returns {"rise_px","fall_px","support_px","contrast","polarity","n_samples"}
    or None.
    """
    if axis not in (0, 1):
        raise ValueError("edge_w_px: axis must be 0 (vertical profile) or 1 (horizontal)")
    lum = _linear_luma(x)
    m = _as_mask(mask, lum.shape)
    red = 1 - axis
    cover = m.sum(axis=red)
    num = (lum * m).sum(axis=red)
    valid = cover >= MIN_EDGE_COVER
    if not valid.any():
        _note(notes, tag, f"refused, no line has {MIN_EDGE_COVER} masked pixels")
        return None
    # Largest contiguous run of valid samples: a profile with holes has meaningless
    # crossings, and a mask that is two separate objects has two separate edges.
    best_s = best_e = 0
    s = None
    for i, v in enumerate(np.append(valid, False)):
        if v and s is None:
            s = i
        elif not v and s is not None:
            if i - s > best_e - best_s:
                best_s, best_e = s, i
            s = None
    if best_e - best_s < MIN_EDGE_SAMPLES:
        _note(notes, tag, f"refused, longest contiguous profile run is "
                          f"{best_e - best_s} samples < {MIN_EDGE_SAMPLES}")
        return None
    prof = num[best_s:best_e] / cover[best_s:best_e]

    lo, hi = float(prof.min()), float(prof.max())
    contrast = hi - lo
    if contrast < MIN_EDGE_CONTRAST:
        _note(notes, tag, f"refused, profile contrast {contrast:.4f} < "
                          f"{MIN_EDGE_CONTRAST} of full linear scale (no edge, only noise)")
        return None

    d = np.diff(prof)
    k_rise = int(np.argmax(d))
    k_fall = int(np.argmin(d))
    polarity = "peak"
    # A trough is a BOUNDED dip: the profile starts high, dips, and comes back high.
    # `k_fall < k_rise` alone is not that test. On a profile with only one flank the
    # missing flank's argmax/argmin lands on an arbitrary sample of the flat part
    # (argmin of a non-negative diff array is just its first zero), so a plain rising
    # STEP satisfied k_fall < k_rise and was inverted: it came back polarity="trough"
    # with its width in `fall_px` and `rise_px` empty, i.e. score_frame's edgerise_*
    # column was silently blank for exactly the masks that clip one object edge.
    # Requiring both ends of the profile to sit in its upper half makes the decision
    # about the profile's SHAPE instead of about where a tie happened to break.
    ends_high = min(float(prof[0]), float(prof[-1])) > lo + 0.5 * contrast
    if k_fall < k_rise and ends_high:
        # A trough: invert so the feature is always an excursion upwards.
        prof = (hi + lo) - prof
        d = np.diff(prof)
        k_rise = int(np.argmax(d))
        k_fall = int(np.argmin(d))
        polarity = "trough"

    t10 = lo + 0.10 * contrast
    t90 = lo + 0.90 * contrast
    x10r = _walk_cross(prof, k_rise + 1, -1, t10, True)
    x90r = _walk_cross(prof, k_rise + 1, +1, t90, False)
    x90f = _walk_cross(prof, k_fall, -1, t90, False)
    x10f = _walk_cross(prof, k_fall + 1, +1, t10, True)

    rise = None if (x10r is None or x90r is None) else float(x90r - x10r)
    fall = None if (x10f is None or x90f is None) else float(x10f - x90f)
    support = None
    if x10r is not None and x10f is not None and k_fall > k_rise:
        support = float(x10f - x10r)
    if rise is None and fall is None:
        _note(notes, tag, "refused, no 10-90 flank found in the profile")
        return None
    if support is None:
        _note(notes, tag, "support width undefined (profile is a single flank, "
                          "not a bounded feature)")
    return {"rise_px": rise, "fall_px": fall, "support_px": support,
            "contrast": float(contrast), "polarity": polarity,
            "n_samples": int(best_e - best_s)}


# --------------------------------------------------------------------------
# 4. Endpoint error of the motion field itself
# --------------------------------------------------------------------------

def epe(flow_est, flow_true, mask, notes=None, tag="epe"):
    """Endpoint error percentiles of an estimated flow field. GT-A clips only.

    Both fields are (h, w, 2) on the estimator's own cell grid, [...,0] = u
    (rightwards, px), [...,1] = v (downwards, px), and `mask` is (h, w) on the same
    grid. Only a synthetic GT-A clip has a true field, which is exactly the point:
    this measures the ESTIMATOR with the warp taken out of the loop, so an estimator
    regression cannot hide behind a forgiving synthesis pass.

    Returns {"p50","p95","bad1","n_cells","mean"} or None. bad1 is the fraction of
    cells off by more than 1 px - the Middlebury-style outlier rate, the number that
    separates "slightly soft everywhere" from "wrong in a few places", which the
    percentiles alone cannot.
    """
    fe = np.asarray(flow_est, dtype=np.float64)
    ft = np.asarray(flow_true, dtype=np.float64)
    if fe.shape != ft.shape or fe.ndim != 3 or fe.shape[2] != 2:
        raise ValueError(f"epe: expected matching (h,w,2) fields, got {fe.shape} / {ft.shape}")
    m = _as_mask(mask, fe.shape[:2], "flow mask")
    refused, n = _refuse_small(m, MIN_EPE_CELLS, notes, tag)
    if refused:
        return None
    d = fe[m] - ft[m]
    e = np.sqrt(d[:, 0] ** 2 + d[:, 1] ** 2)
    return {"p50": float(np.percentile(e, 50)),
            "p95": float(np.percentile(e, 95)),
            "bad1": float(np.mean(e > 1.0)),
            "mean": float(np.mean(e)),
            "n_cells": n}


# --------------------------------------------------------------------------
# 5. Ghost factor and residual factor
# --------------------------------------------------------------------------

def ghost_gf_rf(x, a, b, gt, mask, t=0.5, min_px=MIN_PSNR_PX, notes=None, tag="ghost",
                xfade_lin=None):
    """How much of the output's error is just the cross-fade's error, and what is left.

    With X the LINEAR-light cross-fade of a and b at the same t, E_o = O - GT and
    E_x = X - GT, both over the mask:

        GF = <E_o, E_x> / <E_x, E_x>       (projection of our error onto the ghost)
        RF = ||E_o - GF*E_x||^2 / ||E_o||^2 (energy fraction that survives removal)

    GF alone is ambiguous and that is why RF exists. Take the project's translating
    bar, width 77, motion 42, t = 0.5: a frame that simply HOLDS A scores GF = 1
    exactly, indistinguishable from a pure cross-fade - the symmetry <E_a,E_x> =
    <E_x,E_x> makes it so. RF separates them in closed form: the cross-fade has
    RF = 0, holding A has RF = 0.5*(1 - <E_a,E_b>/||E_a||^2) = 0.5 for that geometry.
    GF says "how ghosty", RF says "and the rest is a different mistake".

    Computed in linear light: it is a linear-algebra statement about the engine's own
    blend, which happens in linear.

    `xfade_lin` lets the caller hand in the linear cross-fade it has already built.
    X does not depend on the mask, so `score_frame` builds it once for all strata.

    Returns {"gf","rf","err_rms","xfade_err_rms","resid_rms","n_px"} or None.
    RF is None (with a reason) when the output is exact on the mask: 0/0.
    """
    lo = decode_srgb8(x)
    lg = decode_srgb8(gt)
    lx = _xfade_linear(a, b, t) if xfade_lin is None else np.asarray(xfade_lin)
    if not (lo.shape == lg.shape == lx.shape):
        raise ValueError("ghost_gf_rf: frame shapes differ")
    m = _as_mask(mask, lo.shape[:2])
    refused, n = _refuse_small(m, min_px, notes, tag)
    if refused:
        return None
    eo = (lo[m] - lg[m]).ravel()
    ex = (lx[m] - lg[m]).ravel()
    exx = float(ex @ ex)
    eoo = float(eo @ eo)
    k = eo.size
    if exx <= 0.0:
        _note(notes, tag, "refused, the cross-fade is already exact here "
                          "(A == B == GT on this mask), GF divides by zero")
        return None
    gf = float((eo @ ex) / exx)
    if eoo <= 0.0:
        _note(notes, tag, "output is exact on this mask; RF is 0/0 and is refused")
        rf = None
        resid_rms = 0.0
    else:
        r = eo - gf * ex
        rr = float(r @ r)
        rf = float(rr / eoo)
        resid_rms = math.sqrt(rr / k)
    return {"gf": gf, "rf": rf,
            "err_rms": math.sqrt(eoo / k),
            "xfade_err_rms": math.sqrt(exx / k),
            "resid_rms": resid_rms, "n_px": n}


# --------------------------------------------------------------------------
# 6. Halo leak
# --------------------------------------------------------------------------

def _dilate1(m):
    """3x3 Chebyshev dilation, separable, no scipy."""
    out = m.copy()
    out[1:, :] |= m[:-1, :]
    out[:-1, :] |= m[1:, :]
    t = out.copy()
    out[:, 1:] |= t[:, :-1]
    out[:, :-1] |= t[:, 1:]
    return out


def ring_from_mask(mask, max_d):
    """Chebyshev-distance ring around `mask`: ring[y,x] = d in 1..max_d, 0 = not in
    the ring. The halo band is what the block matcher damages around a moving
    object - it hands background pixels the object's vector - so the ring is
    measured OUTWARDS from the motion mask, and band d has 8*d pixels around a
    single-pixel object."""
    m = np.asarray(mask, dtype=bool)
    cur = m.copy()
    ring = np.zeros(m.shape, dtype=np.int32)
    for d in range(1, int(max_d) + 1):
        nxt = _dilate1(cur)
        band = nxt & ~cur
        if not band.any():
            break
        ring[band] = d
        cur = nxt
    return ring


def halo_leak(x, a, b, gt, ring, notes=None, tag="halo"):
    """Normalised RMS error in the halo ring, plus a per-distance profile.

    `ring` is an int array as produced by `ring_from_mask`: band index in pixels,
    0 outside. The magnitude alone is not a diagnosis - a strong leak one pixel wide
    is a soft edge, the same magnitude eight pixels wide is the grey ghost wing the
    prototype README describes - so the profile is reported band by band and the
    extent is derived from it.

    Normalisation is by RMS(B - A) on the SAME pixels, in linear light. That is what
    makes bands and clips comparable: where nothing changes between the two source
    frames no halo is possible, and an error the size of the change means the halo is
    total. nrms ~ 1 therefore reads as "this band is as wrong as the motion is big".

    Returns {"nrms","rms","ref_rms","extent_px","n_px","profile"} or None, where
    profile is a list of {"d","n","rms","nrms"} - None for a band under
    MIN_BAND_PX pixels.
    """
    lo = decode_srgb8(x)
    lg = decode_srgb8(gt)
    la = decode_srgb8(a)
    lb = decode_srgb8(b)
    r = np.asarray(ring)
    if r.shape != lo.shape[:2]:
        raise ValueError(f"halo_leak: ring shape {r.shape} vs image {lo.shape[:2]}")
    sel = r > 0
    refused, n = _refuse_small(sel, MIN_BAND_PX, notes, tag)
    if refused:
        return None
    err = lo - lg
    ref = lb - la

    def _rms(mask2):
        e = err[mask2]
        return float(np.sqrt(np.mean(e * e)))

    def _refrms(mask2):
        e = ref[mask2]
        return float(np.sqrt(np.mean(e * e)))

    rms_all = _rms(sel)
    ref_all = _refrms(sel)
    if ref_all <= 0.0:
        _note(notes, tag, "refused, B == A across the whole ring: no motion, so no "
                          "halo is possible and the normaliser is zero")
        return None
    profile = []
    for d in range(1, int(r.max()) + 1):
        band = r == d
        nb = int(np.count_nonzero(band))
        if nb < MIN_BAND_PX:
            profile.append({"d": d, "n": nb, "rms": None, "nrms": None})
            continue
        rr = _refrms(band)
        br = _rms(band)
        profile.append({"d": d, "n": nb, "rms": br,
                        "nrms": (br / rr) if rr > 0 else None})
    # Extent: the outermost band still at half of the STRONGEST band's leak. A reading
    # aid for the report, not a gate - it turns the profile into one number that says
    # "the leak reaches this far", which is the thing an A/B has to move.
    #
    # It was normalised by the INNERMOST band instead, and that returned None with no
    # note whenever band 1 happens to be clean - a real shape, produced by any matcher
    # whose block grid is offset from the object boundary, or by an occlusion mask that
    # already covers d=1. That is precisely the profile this column exists to describe,
    # and it was the one it went blank on. Normalising by the peak is identical
    # whenever band 1 IS the peak, so no previously-reported extent changes.
    extent = None
    live = [p["nrms"] for p in profile if p["nrms"] is not None]
    peak = max(live) if live else None
    if peak is not None and peak > 0.0:
        for p in profile:
            if p["nrms"] is not None and p["nrms"] >= 0.5 * peak:
                extent = p["d"]
    else:
        _note(notes, tag, f"extent_px is None: no band has both >= {MIN_BAND_PX} px "
                          "and a non-zero leak, so there is no profile to read an "
                          "extent from")
    return {"nrms": rms_all / ref_all, "rms": rms_all, "ref_rms": ref_all,
            "extent_px": extent, "n_px": n, "profile": profile}


# --------------------------------------------------------------------------
# 7. Flicker across source intervals
# --------------------------------------------------------------------------

def flicker(frames, mask, interval_ids=None, min_intervals=MIN_FLICKER_INTERVALS,
            notes=None, tag="flicker"):
    """RMS temporal deviation over consecutive OUTPUT frames, in linear light.

    `interval_ids[i]` is the index of the source interval frame i was generated in
    (i.e. which A,B pair). It is REQUIRED, and the window is refused unless it spans
    at least `min_intervals` distinct intervals with the frames consecutive and in
    order. That is not bookkeeping pedantry: motion is estimated once per source pair
    (I6), so a defect that pulses once per source interval is CONSTANT inside one
    interval and a window that does not cross a pair boundary cannot see it at all.
    A number computed over one interval would be a confident zero.

    Two figures are returned because they answer different questions:
      * rms    - deviation from the per-pixel temporal mean. Meaningful on a mask
                 where the truth is constant (a static region).
      * rms_dt - deviation from a per-pixel LINEAR trend in time, so a pixel that
                 legitimately ramps through a gradient as the picture moves
                 contributes ~0 and only the pulse survives.
    `rms_srgb8` is the same deviation measured in 8-bit sRGB levels, for a report a
    human has to read.

    `series` is the per-frame RMS deviation, which is what `acf_at_lag` consumes.

    Returns {"rms","rms_dt","rms_srgb8","series","n_frames","n_intervals","n_px"}
    or None.
    """
    if len(frames) < 2:
        _note(notes, tag, f"refused, {len(frames)} frames is not a sequence")
        return None
    if interval_ids is None:
        _note(notes, tag, "refused, interval_ids not supplied so the >= "
                          f"{min_intervals} source-interval span cannot be verified")
        return None
    ids = [int(v) for v in interval_ids]
    if len(ids) != len(frames):
        raise ValueError("flicker: interval_ids must have one entry per frame")
    if any(ids[i + 1] < ids[i] for i in range(len(ids) - 1)):
        _note(notes, tag, "refused, interval_ids are not non-decreasing "
                          "(frames are not consecutive output frames)")
        return None
    n_int = len(set(ids))
    if n_int < min_intervals:
        _note(notes, tag, f"refused, window spans {n_int} source interval(s) < "
                          f"{min_intervals}; a once-per-interval pulse is invisible here")
        return None

    m = _as_mask(mask, _as_srgb8(frames[0], "frame").shape[:2])
    n_px = int(np.count_nonzero(m))
    if n_px < MIN_PSNR_PX:
        _note(notes, tag, f"refused, mask has {n_px} px < {MIN_PSNR_PX} px minimum")
        return None

    T = len(frames)
    xs = np.arange(T, dtype=np.float64)
    xs -= xs.mean()
    sxx = float(xs @ xs)

    # Streamed in two passes: a stacked (T, n_px, 3) float64 buffer is a gigabyte for
    # a full-frame mask at 1440p, and this metric must be usable on every stratum.
    s0 = None
    s1 = None
    s0_8 = None
    for i in range(T):
        y = decode_srgb8(frames[i])[m]
        y8 = _as_srgb8(frames[i], "frame").astype(np.float64)[m]
        if s0 is None:
            s0 = np.zeros_like(y)
            s1 = np.zeros_like(y)
            s0_8 = np.zeros_like(y8)
        s0 += y
        s1 += xs[i] * y
        s0_8 += y8
    mean = s0 / T
    slope = s1 / sxx if sxx > 0 else np.zeros_like(s1)
    mean8 = s0_8 / T

    acc_d = 0.0
    acc_r = 0.0
    acc_8 = 0.0
    series = np.empty(T, dtype=np.float64)
    for i in range(T):
        y = decode_srgb8(frames[i])[m]
        d = y - mean
        r = d - xs[i] * slope
        acc_d += float(np.sum(d * d))
        acc_r += float(np.sum(r * r))
        d8 = _as_srgb8(frames[i], "frame").astype(np.float64)[m] - mean8
        acc_8 += float(np.sum(d8 * d8))
        series[i] = math.sqrt(float(np.mean(d * d)))
    k = T * mean.size
    return {"rms": math.sqrt(acc_d / k), "rms_dt": math.sqrt(acc_r / k),
            "rms_srgb8": math.sqrt(acc_8 / k), "series": series,
            "n_frames": T, "n_intervals": n_int, "n_px": n_px}


# --------------------------------------------------------------------------
# 8. Autocorrelation at a lag - the bracket-pumping detector
# --------------------------------------------------------------------------

def acf_at_lag(series, lag, notes=None, tag="acf"):
    """Autocorrelation of a per-frame series at `lag`, in [-1, 1]. None if refused.

    Run it on the static-region residual energy and on the per-cell velocity step
    with lag = one source interval in output frames. Estimating motion once per
    source pair (I6) is what creates "bracket pumping": the field is fresh at the
    start of each interval and stalest at its end, so the error breathes with period
    exactly one interval. Nothing inside a single interval can see it; a peak here at
    lag = frames-per-interval is its signature.

    `series` may be (T,) or (T, K). For (T, K) each column is centred separately and
    the result is POOLED (numerators and denominators summed over columns), which is
    the right question - "do the cells pump together" - and `per_unit_median` is
    reported alongside for the spread.

    Biased estimator: the denominator is the full-sample variance, so a perfectly
    periodic series of length N reads (N-lag)/N, not 1. That is the standard
    convention and it keeps the value bounded; the selftest pins the exact number.
    """
    s = np.asarray(series, dtype=np.float64)
    if s.ndim == 1:
        s = s[:, None]
    if s.ndim != 2:
        raise ValueError(f"acf_at_lag: expected (T,) or (T,K), got {s.shape}")
    T = s.shape[0]
    lag = int(lag)
    if lag <= 0:
        raise ValueError("acf_at_lag: lag must be >= 1")
    if T <= lag + 1:
        _note(notes, tag, f"refused, series of {T} frames cannot support lag {lag}")
        return None
    c = s - s.mean(axis=0, keepdims=True)
    den_cols = np.sum(c * c, axis=0)
    num_cols = np.sum(c[:T - lag] * c[lag:], axis=0)
    den = float(den_cols.sum())
    if den <= 0.0:
        _note(notes, tag, "refused, series is constant (zero variance)")
        return None
    live = den_cols > 0
    per = num_cols[live] / den_cols[live]
    return {"r": float(num_cols.sum() / den),
            "per_unit_median": float(np.median(per)) if per.size else None,
            "lag": lag, "n": T}


# --------------------------------------------------------------------------
# 9. Grain floor
# --------------------------------------------------------------------------

def grain_floor(a, b, gt, mask, t=0.5, min_px=MIN_PSNR_PX, notes=None, tag="grain",
                xfade_srgb8=None):
    """The two references every PSNR column has to be read against.

    psnr_xfade is the plain LINEAR-light cross-fade at the same t; psnr_hold_a is
    frame A held. Both against GT on the same pixels, in 8-bit sRGB.

    MEASURED FACT, and the reason this column is not optional. With sigma = 6/255
    film grain, a PERFECT motion compensation scores 29.55 dB while a cross-fade
    scores 30.80 dB. Perfect MC carries one grain realisation into a frame whose GT
    carries an independent one, so its error variance is 2*sigma^2 -> 29.56 dB; the
    cross-fade averages two independent realisations, error variance 1.5*sigma^2 ->
    30.81 dB. The cross-fade wins by 1.25 dB while being visibly, structurally wrong.
    Any report that prints PSNR without this column will eventually be read as "our
    motion path is worse than blending", and that reading will be false.

    psnr_hold_b is included as a free symmetry check: a bidirectional method whose
    hold-A and hold-B floors differ is looking at an asymmetric pair.

    `xfade_srgb8` lets the caller hand in the encoded cross-fade it has already
    built; it does not depend on the mask.

    Returns {"psnr_xfade","psnr_hold_a","psnr_hold_b","n_px"} or None.
    """
    ga = _as_srgb8(gt, "gt")
    m = _as_mask(mask, ga.shape[:2])
    refused, n = _refuse_small(m, min_px, notes, tag)
    if refused:
        return None
    xf = encode_srgb8(_xfade_linear(a, b, t)) if xfade_srgb8 is None else xfade_srgb8
    return {"psnr_xfade": psnr_region(xf, ga, m, min_px, notes, tag + ".xfade"),
            "psnr_hold_a": psnr_region(_as_srgb8(a, "a"), ga, m, min_px, notes, tag + ".holdA"),
            "psnr_hold_b": psnr_region(_as_srgb8(b, "b"), ga, m, min_px, notes, tag + ".holdB"),
            "n_px": n}


# --------------------------------------------------------------------------
# 10. The row
# --------------------------------------------------------------------------

# The flicker branch writes three bookkeeping columns whose names are built the same
# way as the per-stratum ones (flick_<name>), so a stratum called "lag", "frames" or
# "intervals" would land on top of them and the CSV would carry an RMS where the
# report expects a frame count - silently, in the one file the whole harness's
# credibility rests on. The collision is refused loudly instead.
_RESERVED_STRATUM_NAMES = frozenset({"lag", "frames", "intervals"})


def _num(v):
    """CSV cell: keep None as None, coerce numpy scalars to plain floats."""
    if v is None:
        return None
    if isinstance(v, (bool, str, int)):
        return v
    return float(v)


def score_frame(x, a, b, gt, t, strata, *, ring=None, halo_max_d=8,
                flow_est=None, flow_true=None, flow_mask=None,
                frames=None, interval_ids=None, frames_per_interval=None,
                edge_axis=1, pos_radius=DEFAULT_POS_RADIUS, meta=None):
    """Every metric x every stratum for one generated frame, as one flat CSV row.

    x  - the generated frame (uint8 sRGB); a, b - the two source frames it sits
    between; gt - the ground-truth frame for phase t (from decimation); t - the phase
    in (0,1) the frame was generated at, which MUST be the phase used to build it or
    the cross-fade references are measured at the wrong place.

    strata - {name: bool mask}. The report is clip x stratum, so the names are the
    table's rows: typically "all", "static", "motion", "halo".

    Optional: `ring` (or `strata["motion"]` + halo_max_d, from which one is built)
    for halo_leak; `flow_est`/`flow_true`/`flow_mask` for EPE on a GT-A clip;
    `frames`+`interval_ids` for flicker and its ACF.

    Anything refused is None in the row and its reason is appended to `notes`, so a
    blank cell is always explained and never a fabricated number.
    """
    bad = sorted(set(strata) & _RESERVED_STRATUM_NAMES)
    if bad:
        raise ValueError(
            f"score_frame: stratum name(s) {bad} collide with the flicker bookkeeping "
            "columns flick_lag / flick_frames / flick_intervals and would overwrite "
            "them in the row. Rename the stratum.")

    row = {}
    if meta:
        row.update(meta)
    row["t"] = float(t)
    notes = []

    # The cross-fade reference does not depend on the mask; building it once instead
    # of once per stratum took the same 1440p 3-stratum row from 8.8 s to 6.4 s
    # (measured on the dev machine), again with every column bit-identical.
    xfade_lin = _xfade_linear(a, b, t)
    xfade_8 = encode_srgb8(xfade_lin)

    for name, mask in strata.items():
        m = _as_mask(mask, _as_srgb8(x, "x").shape[:2], f"stratum {name}")
        row[f"n_px_{name}"] = int(np.count_nonzero(m))

        row[f"psnr_{name}"] = _num(psnr_region(x, gt, m, notes=notes, tag=f"psnr[{name}]"))

        g = grain_floor(a, b, gt, m, t, notes=notes, tag=f"grain[{name}]",
                        xfade_srgb8=xfade_8)
        row[f"psnr_xfade_{name}"] = _num(g and g["psnr_xfade"])
        row[f"psnr_holdA_{name}"] = _num(g and g["psnr_hold_a"])
        row[f"psnr_holdB_{name}"] = _num(g and g["psnr_hold_b"])

        gh = ghost_gf_rf(x, a, b, gt, m, t, notes=notes, tag=f"ghost[{name}]",
                         xfade_lin=xfade_lin)
        row[f"gf_{name}"] = _num(gh and gh["gf"])
        row[f"rf_{name}"] = _num(gh["rf"]) if gh else None
        row[f"errrms_{name}"] = _num(gh and gh["err_rms"])
        row[f"xferms_{name}"] = _num(gh and gh["xfade_err_rms"])

        p = pos_err_px(x, gt, m, pos_radius, notes=notes, tag=f"pos[{name}]")
        row[f"posdx_{name}"] = _num(p and p["dx"])
        row[f"posdy_{name}"] = _num(p and p["dy"])
        row[f"posncc_{name}"] = _num(p and p["peak_ncc"])

        e = edge_w_px(x, m, edge_axis, notes=notes, tag=f"edge[{name}]")
        row[f"edgerise_{name}"] = _num(e["rise_px"]) if e else None
        row[f"edgefall_{name}"] = _num(e["fall_px"]) if e else None
        row[f"edgesup_{name}"] = _num(e["support_px"]) if e else None
        row[f"edgecontrast_{name}"] = _num(e and e["contrast"])

    # Halo ring: one measurement, not per stratum - it IS a stratum, and its shape
    # in pixels is the point.
    if ring is None and "motion" in strata:
        ring = ring_from_mask(strata["motion"], halo_max_d)
    if ring is not None:
        h = halo_leak(x, a, b, gt, ring, notes=notes, tag="halo")
        row["halo_nrms"] = _num(h and h["nrms"])
        row["halo_rms"] = _num(h and h["rms"])
        row["halo_extent_px"] = _num(h and h["extent_px"])
        row["halo_n_px"] = _num(h and h["n_px"])
        prof = {p["d"]: p["nrms"] for p in (h["profile"] if h else [])}
        for d in range(1, halo_max_d + 1):
            row[f"halo_nrms_d{d}"] = _num(prof.get(d))

    if flow_est is not None and flow_true is not None:
        fm = flow_mask if flow_mask is not None else np.ones(np.asarray(flow_est).shape[:2], bool)
        ep = epe(flow_est, flow_true, fm, notes=notes, tag="epe")
        row["epe_p50"] = _num(ep and ep["p50"])
        row["epe_p95"] = _num(ep and ep["p95"])
        row["epe_bad1"] = _num(ep and ep["bad1"])
        row["epe_cells"] = _num(ep and ep["n_cells"])

    if frames is not None:
        lag = frames_per_interval
        if lag is None and interval_ids is not None:
            ids = [int(v) for v in interval_ids]
            runs = [ids.count(v) for v in sorted(set(ids))]
            lag = max(set(runs), key=runs.count)  # modal frames-per-interval
        row["flick_lag"] = _num(lag)
        row["flick_frames"] = len(frames)
        row["flick_intervals"] = len(set(int(v) for v in interval_ids)) \
            if interval_ids is not None else None
        for name, mask in strata.items():
            f = flicker(frames, mask, interval_ids, notes=notes, tag=f"flicker[{name}]")
            row[f"flick_{name}"] = _num(f and f["rms"])
            row[f"flickdt_{name}"] = _num(f and f["rms_dt"])
            row[f"flick8_{name}"] = _num(f and f["rms_srgb8"])
            # The ACF of the per-frame residual energy at lag = one source interval:
            # this is the bracket-pumping column, and it is the only one in the row
            # that can see a defect whose period is the estimator's own update rate.
            ac = acf_at_lag(f["series"], lag, notes=notes, tag=f"acf[{name}]") \
                if (f and lag) else None
            row[f"acfres_{name}"] = _num(ac and ac["r"])

    row["notes"] = " | ".join(notes)
    return row


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------

class _Check:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def ok(self, name, cond, detail=""):
        if cond:
            self.passed += 1
            print(f"PASS  {name}" + (f"   {detail}" if detail else ""))
        else:
            self.failed += 1
            print(f"FAIL  {name}   {detail}")

    def close(self, name, got, want, tol):
        good = got is not None and abs(float(got) - want) <= tol
        g = "None" if got is None else f"{float(got):.6g}"
        self.ok(name, good, f"got {g}, want {want:.6g} +-{tol:g}")

    def between(self, name, got, lo, hi):
        good = got is not None and lo <= float(got) <= hi
        g = "None" if got is None else f"{float(got):.6g}"
        self.ok(name, good, f"got {g}, want [{lo:g}, {hi:g}]")


def _bar_frame(h, w, x0, width, lin_bar=0.9, lin_bg=0.02):
    """A vertical bar composed in LINEAR light, then encoded to sRGB - the harness
    colour rule, not a convenience."""
    lin = np.full((h, w, 3), lin_bg, dtype=np.float64)
    lin[:, x0:x0 + width, :] = lin_bar
    return encode_srgb8(lin)


def _texture(h, w, seed):
    """Deterministic band-limited texture: a sum of sinusoids, so it is smooth
    enough for sub-pixel correlation and has no spectral holes."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    acc = np.zeros((h, w), dtype=np.float64)
    for _ in range(12):
        fx, fy = rng.uniform(0.02, 0.18, 2)
        ph = rng.uniform(0, 2 * math.pi)
        acc += np.sin(2 * math.pi * (fx * xx + fy * yy) + ph)
    acc = (acc - acc.min()) / (acc.max() - acc.min())
    lin = np.repeat((0.05 + 0.9 * acc)[:, :, None], 3, axis=2)
    return encode_srgb8(lin)


def _selftest():
    c = _Check()
    print("=== colour ===")
    codes = np.arange(256, dtype=np.uint8).reshape(1, 256)
    rt = encode_srgb8(decode_srgb8(codes))[:, :, 0]
    c.ok("srgb roundtrip exact for all 256 codes", bool(np.array_equal(rt, codes)),
         f"{int(np.count_nonzero(rt != codes))} mismatches")
    c.close("srgb_to_linear(0.5) piecewise value", float(srgb_to_linear(0.5)),
            0.21404114048223255, 1e-12)
    c.close("srgb_to_linear(0.04) uses the 12.92 leg",
            float(srgb_to_linear(0.04)), 0.04 / 12.92, 1e-15)
    gap = abs(0.5 ** 2.2 - float(srgb_to_linear(0.5)))
    c.ok("gamma-2.2 shortcut is NOT equivalent", gap > 3e-3, f"differs by {gap:.5f} linear")

    # Independent re-derivation of the transfer, scalar python straight off IEC
    # 61966-2-1, so the 256-entry decode LUT cannot drift from the spec without this
    # failing. (The round-trip check above would still pass if BOTH directions were
    # wrong in the same way; this one would not.)
    def _spec_s2l(s):
        return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4

    def _spec_l2s(l):
        return l * 12.92 if l <= 0.0031308 else 1.055 * (l ** (1 / 2.4)) - 0.055

    worst = max(abs(float(_SRGB8_TO_LINEAR[k]) - _spec_s2l(k / 255.0)) for k in range(256))
    c.ok("decode LUT == a from-scratch scalar IEC 61966-2-1 decode, all 256 codes",
         worst == 0.0, f"max |diff| = {worst:.3e}")
    worst = max(abs(float(linear_to_srgb(np.array(v))) - _spec_l2s(v)) for v in
                (0.0, 1e-6, 0.0031307, 0.0031308, 0.0031309, 0.01, 0.214041, 0.5, 0.9, 1.0))
    c.ok("encode == the scalar spec on both sides of the knee", worst == 0.0,
         f"max |diff| = {worst:.3e}")
    # No 8-bit code can land between the spec's rounded 0.04045 and the 12.92*0.0031308
    # used here, which is the whole justification for preferring the exact-round-trip
    # form. Asserted rather than asserted-in-a-comment.
    _c_lo, _c_hi = sorted((0.04045, _SRGB_CUT))
    c.ok("no 8-bit code lies between the two sRGB cut conventions",
         not any(_c_lo < k / 255.0 <= _c_hi for k in range(256)),
         f"gap is ({_c_lo:.8f}, {_c_hi:.8f}], i.e. codes "
         f"{_c_lo * 255:.4f}..{_c_hi * 255:.4f}")
    # And the shortcut is not merely "different", it is bigger than the effect size:
    # the worst 8-bit cross-fade disagreement is ~3 codes, which as a uniform offset
    # is a PSNR shift far larger than the ~0.3 dB the harness has to resolve.
    _ks = np.arange(0, 256, 5, dtype=np.float64) / 255.0
    _aa, _bb = np.meshgrid(_ks, _ks, indexing="ij")
    _lin_c = linear_to_srgb(0.5 * srgb_to_linear(_aa) + 0.5 * srgb_to_linear(_bb)) * 255.0
    _g22_c = ((0.5 * _aa ** 2.2 + 0.5 * _bb ** 2.2) ** (1 / 2.2)) * 255.0
    _worst_codes = float(np.max(np.abs(_lin_c - _g22_c)))
    c.ok("a gamma-2.2 cross-fade would be off by whole 8-bit codes", _worst_codes > 2.0,
         f"worst {_worst_codes:.2f} codes = a {10 * math.log10(255.0 ** 2 / _worst_codes ** 2):.1f} dB "
         "uniform-offset PSNR, against the ~0.3 dB this harness must resolve")

    print("=== psnr_region ===")
    img = _texture(128, 128, 1)
    full = np.ones((128, 128), bool)
    n = []
    c.ok("identical -> +inf", psnr_region(img, img, full, notes=n) == math.inf, n[-1])
    small = np.zeros((128, 128), bool)
    small[:10, :10] = True
    n = []
    c.ok("tiny mask -> refused (None)", psnr_region(img, img, small, notes=n) is None, n[-1])
    off = np.clip(img.astype(np.int32) + 4, 0, 255).astype(np.uint8)
    # The texture spans codes 63..249, so nothing clips and the MSE is exactly 16:
    # 10*log10(255^2/16) = 36.0896038. The target used to be 36.0906, which cleared
    # its own +-1e-3 window by 4e-6 - a pass that measured the typo, not the code.
    # 1e-9 pins the closed form.
    c.close("uniform +4 code offset -> 10*log10(255^2/16) dB",
            psnr_region(off, img, full), 10.0 * math.log10(255.0 * 255.0 / 16.0), 1e-9)
    half = np.zeros((128, 128), bool)
    half[:, :64] = True
    mixed = img.copy()
    mixed[:, 64:] = 0
    c.ok("masked PSNR ignores error outside the mask",
         psnr_region(mixed, img, half) == math.inf)

    print("=== pos_err_px ===")
    # exact quadratic -> the 3x3 fit must return its stationary point exactly
    qy, qx = -0.4, 0.3
    zz = np.array([[-(2 * (j - qx) ** 2 + 3 * (i - qy) ** 2 + 1.5 * (j - qx) * (i - qy))
                    for j in (-1.0, 0.0, 1.0)] for i in (-1.0, 0.0, 1.0)])
    sub = _parabolic2d(zz)
    c.close("2-D peak fit recovers a known stationary point (x)", sub and sub[1], qx, 1e-12)
    c.close("2-D peak fit recovers a known stationary point (y)", sub and sub[0], qy, 1e-12)

    gt = _texture(160, 200, 7)
    mask = np.zeros((160, 200), bool)
    mask[30:130, 40:160] = True
    shifted = np.roll(np.roll(gt, -3, axis=0), 5, axis=1)
    p = pos_err_px(shifted, gt, mask)
    # 0.01 px, not 0: the estimate is a real sub-pixel fit on 8-bit data, so an exact
    # integer shift still lands a few thousandths of a pixel off.
    c.close("integer shift dx", p and p["dx"], 5.0, 0.01)
    c.close("integer shift dy", p and p["dy"], -3.0, 0.01)
    c.between("integer shift peak NCC", p and p["peak_ncc"], 0.999, 1.0000001)
    # Sub-pixel sweep. The parabola is fitted to the true autocorrelation, which is
    # not itself a parabola, so a residual model bias is expected and is what the
    # tolerances below encode - MEASURED here as 0.05 px RMS / 0.11 px worst over
    # 0..4 px on this texture (the separable per-axis fit, for comparison, leaks up
    # to 0.30 px onto the stationary axis on the same data, which is why the 2-D fit
    # is the default).
    lin = decode_srgb8(gt)
    errs = []
    for fx in np.arange(0.0, 4.01, 0.25):
        i = int(math.floor(fx))
        f = fx - i
        sx = encode_srgb8((1 - f) * np.roll(lin, i, axis=1) + f * np.roll(lin, i + 1, axis=1))
        pp = pos_err_px(sx, gt, mask)
        errs.append((pp["dx"] - fx, pp["dy"]))
    errs = np.array(errs)
    c.between("sub-pixel sweep 0..4 px: dx RMS error", float(np.sqrt((errs[:, 0] ** 2).mean())),
              0.0, 0.10)
    c.between("sub-pixel sweep 0..4 px: dx worst error", float(np.abs(errs[:, 0]).max()),
              0.0, 0.15)
    c.between("sub-pixel sweep: dy does not drift off the stationary axis",
              float(np.abs(errs[:, 1]).max()), 0.0, 0.10)
    subx = encode_srgb8(0.5 * np.roll(lin, 2, axis=1) + 0.5 * np.roll(lin, 3, axis=1))
    p2 = pos_err_px(subx, gt, mask)
    c.close("sub-pixel shift of +2.5 px", p2 and p2["dx"], 2.5, 0.15)
    # Both SIGNS on both axes, built by slicing so there is no wraparound anywhere in
    # the frame. A transposed sign convention passes a single +x test and fails here.
    _hh, _ww = gt.shape[:2]

    def _shift_exact(im, dy, dx):
        out = np.zeros_like(im)
        out[max(dy, 0):_hh + min(dy, 0), max(dx, 0):_ww + min(dx, 0)] = \
            im[max(-dy, 0):_hh + min(-dy, 0), max(-dx, 0):_ww + min(-dx, 0)]
        return out

    _bad = []
    for _dy, _dx in ((0, +4), (0, -4), (+6, 0), (-6, 0), (+3, -5), (-2, +7)):
        _p = pos_err_px(_shift_exact(gt, _dy, _dx), gt, mask, radius=12)
        if _p is None or abs(_p["dx"] - _dx) > 0.03 or abs(_p["dy"] - _dy) > 0.03:
            _bad.append(f"({_dy:+d},{_dx:+d})->{_p and (_p['dy'], _p['dx'])}")
    c.ok("all four signs on both axes recovered (+dx = content sits right, +dy = down)",
         not _bad, "; ".join(_bad) if _bad else "6 exact shifts, all within 0.03 px")
    n = []
    # A bar 42 px away with a 6 px search: NCC climbs monotonically to the boundary,
    # which must be refused rather than reported as +6. (A periodic texture would
    # instead lock onto a spurious interior peak - G24 - which is why this case uses
    # an isolated feature.)
    barA = _bar_frame(120, 900, 711, 77)
    barB = _bar_frame(120, 900, 753, 77)
    barmask = np.ones((120, 900), bool)
    c.ok("shift beyond the search radius -> refused",
         pos_err_px(barB, barA, barmask, radius=6, notes=n) is None, n[-1] if n else "")
    # The full-height bar: dx is exactly the 42 px the prototype measured by hand,
    # dy is unobservable and must come back None rather than as a noise-driven row.
    n = []
    pbar = pos_err_px(barB, barA, barmask, radius=48, notes=n)
    c.close("full-height bar: dx == the 42 px motion", pbar and pbar["dx"], 42.0, 0.05)
    c.ok("full-height bar: dy is refused, not invented",
         pbar is not None and pbar["dy"] is None and pbar["mag"] is None,
         n[-1] if n else "")
    n = []
    flat = np.full_like(gt, 128)
    c.ok("flat reference -> refused", pos_err_px(flat, flat, mask, notes=n) is None,
         n[-1] if n else "")
    # the fast path is only trustworthy because it equals the obvious one
    tl, rl = _srgb_luma(shifted), _srgb_luma(gt)
    Cf, _ = _ncc_map(tl, rl, mask, 6)
    Cb = _ncc_map_bruteforce(tl, rl, mask, 6)
    err = float(np.max(np.abs(Cf - Cb)))
    c.ok("FFT NCC map == brute-force NCC map", err < 1e-9, f"max |diff| = {err:.2e}")

    print("=== edge_w_px ===")
    H = 120
    A = _bar_frame(H, 900, 711, 77)
    B = _bar_frame(H, 900, 753, 77)
    MID = _bar_frame(H, 900, 732, 77)
    fullm = np.ones((H, 900), bool)
    e_sharp = edge_w_px(MID, fullm, axis=1)
    c.between("sharp bar rise width", e_sharp and e_sharp["rise_px"], 0.5, 1.5)
    c.between("sharp bar support == bar width 77", e_sharp and e_sharp["support_px"], 77.0, 78.5)
    xf = encode_srgb8(_xfade_linear(A, B, 0.5))
    e_xf = edge_w_px(xf, fullm, axis=1)
    c.between("cross-fade rise width == the 42 px motion", e_xf and e_xf["rise_px"], 41.0, 44.0)
    c.between("cross-fade support == the 118 px smear", e_xf and e_xf["support_px"], 118.0, 121.0)
    c.ok("cross-fade support is WIDER than the 77 px bar",
         e_xf["support_px"] > 77.0 + 1.0,
         f"{e_xf['support_px']:.1f} px vs 77 px bar, "
         f"sharp frame gives {e_sharp['support_px']:.1f}")
    dark = encode_srgb8(1.0 - decode_srgb8(MID).astype(np.float64))
    e_tr = edge_w_px(dark, fullm, axis=1)
    c.ok("trough polarity handled", e_tr and e_tr["polarity"] == "trough",
         f"support {e_tr and e_tr['support_px']:.1f} px")
    # axis=0 was never exercised. The same bar transposed must give the identical
    # numbers, or the profile is being reduced along the wrong axis.
    horiz = np.transpose(MID, (1, 0, 2)).copy()
    e_a0 = edge_w_px(horiz, np.ones((900, H), bool), axis=0)
    c.ok("axis=0 on the transposed bar == axis=1 on the original",
         e_a0 is not None and abs(e_a0["rise_px"] - e_sharp["rise_px"]) < 1e-12
         and abs(e_a0["support_px"] - e_sharp["support_px"]) < 1e-12
         and e_a0["polarity"] == e_sharp["polarity"],
         f"axis0 rise {e_a0 and e_a0['rise_px']:.4f} sup {e_a0 and e_a0['support_px']:.4f}")
    n = []
    c.ok("profiling that bar along the WRONG axis is refused, not invented",
         edge_w_px(MID, fullm, axis=0, notes=n) is None, n[-1] if n else "")
    # A single flank - one object edge that the stratum mask clips. This used to come
    # back polarity="trough" with rise_px empty and the width hiding in fall_px,
    # because argmin over a non-negative diff array put a phantom "fall" at sample 0.
    step_lin = np.full((H, 400, 3), 0.02)
    step_lin[:, 200:, :] = 0.9
    e_up = edge_w_px(encode_srgb8(step_lin), np.ones((H, 400), bool), axis=1)
    e_dn = edge_w_px(encode_srgb8(step_lin[:, ::-1, :].copy()), np.ones((H, 400), bool), axis=1)
    c.ok("lone RISING step: polarity peak, width in rise_px, no support",
         e_up is not None and e_up["polarity"] == "peak" and e_up["fall_px"] is None
         and e_up["support_px"] is None and abs(e_up["rise_px"] - e_sharp["rise_px"]) < 1e-9,
         f"polarity={e_up and e_up['polarity']} rise={e_up and e_up['rise_px']} "
         f"fall={e_up and e_up['fall_px']}")
    c.ok("lone FALLING step: mirror image, same width in fall_px",
         e_dn is not None and e_dn["polarity"] == "peak" and e_dn["rise_px"] is None
         and abs(e_dn["fall_px"] - e_up["rise_px"]) < 1e-9,
         f"polarity={e_dn and e_dn['polarity']} rise={e_dn and e_dn['rise_px']} "
         f"fall={e_dn and e_dn['fall_px']}")
    n = []
    flat_img = np.full((H, 900, 3), 128, np.uint8)
    c.ok("flat image -> refused (no edge)",
         edge_w_px(flat_img, fullm, 1, notes=n) is None, n[-1] if n else "")

    print("=== epe ===")
    k = 10
    ft = np.zeros((k, k, 2))
    fe = ft.copy()
    fe[:, :, 0] = np.arange(100).reshape(k, k)  # EPE = 0..99, one per cell
    r = epe(fe, ft, np.ones((k, k), bool))
    c.close("EPE p50 of 0..99", r and r["p50"], 49.5, 1e-9)
    c.close("EPE p95 of 0..99", r and r["p95"], 94.05, 1e-9)
    c.close("EPE bad1 of 0..99", r and r["bad1"], 0.98, 1e-12)
    n = []
    c.ok("too few cells -> refused",
         epe(fe[:2, :2], ft[:2, :2], np.ones((2, 2), bool), notes=n) is None,
         n[-1] if n else "")

    print("=== ghost_gf_rf ===")
    # Closed form for a 77 px bar moving 42 px, t = 0.5, GT at the midpoint (+21):
    #   <E_a,E_b> = 0 and ||E_a||^2 = 42*h*V^2, so hold-A gives GF = 1, RF = 0.5.
    GTm = _bar_frame(H, 900, 732, 77)
    gh_x = ghost_gf_rf(xf, A, B, GTm, fullm, 0.5)
    c.close("pure cross-fade GF", gh_x and gh_x["gf"], 1.0, 1e-9)
    # not exactly 0: `xf` is the linear cross-fade after an 8-bit sRGB round trip,
    # so its quantization survives the projection. 1e-3 of the error energy is the
    # size of that round trip and nothing else.
    c.close("pure cross-fade RF", gh_x and gh_x["rf"], 0.0, 1e-3)
    gh_a = ghost_gf_rf(A, A, B, GTm, fullm, 0.5)
    c.close("hold-A GF (ambiguous with the cross-fade)", gh_a and gh_a["gf"], 1.0, 0.02)
    c.close("hold-A RF (what disambiguates it)", gh_a and gh_a["rf"], 0.5, 0.02)
    n = []
    gh_p = ghost_gf_rf(GTm, A, B, GTm, fullm, 0.5, notes=n)
    c.ok("exact output -> GF ~0 and RF refused",
         gh_p is not None and gh_p["rf"] is None and abs(gh_p["gf"]) < 1e-6,
         f"gf={gh_p['gf']:.2e}; {n[-1] if n else ''}")
    # t != 0.5 was never covered, and t = 0.5 is the one phase where the cross-fade is
    # symmetric - an asymmetric-t bug would have hidden there. Two flat regions with
    # different codes, so the two error vectors are genuinely non-collinear and RF is
    # not trivially 0; the expected GF/RF come from the scalar spec transfer above,
    # not from this module's own arithmetic.
    _c = {"a": (40, 90), "b": (200, 60), "gt": (120, 210), "x": (90, 150)}
    _fr = {}
    for _k, (_v1, _v2) in _c.items():
        _im = np.zeros((64, 64, 3), np.uint8)
        _im[:32] = _v1
        _im[32:] = _v2
        _fr[_k] = _im
    _t = 0.25
    _n = 32 * 64 * 3
    _ex, _eo = [], []
    for _i in (0, 1):
        _xf = (1 - _t) * _spec_s2l(_c["a"][_i] / 255) + _t * _spec_s2l(_c["b"][_i] / 255)
        _ex.append(_xf - _spec_s2l(_c["gt"][_i] / 255))
        _eo.append(_spec_s2l(_c["x"][_i] / 255) - _spec_s2l(_c["gt"][_i] / 255))
    _dxx = _n * (_ex[0] ** 2 + _ex[1] ** 2)
    _doo = _n * (_eo[0] ** 2 + _eo[1] ** 2)
    _gf_w = _n * (_eo[0] * _ex[0] + _eo[1] * _ex[1]) / _dxx
    _rf_w = _n * ((_eo[0] - _gf_w * _ex[0]) ** 2 + (_eo[1] - _gf_w * _ex[1]) ** 2) / _doo
    gh_t = ghost_gf_rf(_fr["x"], _fr["a"], _fr["b"], _fr["gt"], np.ones((64, 64), bool), _t)
    c.close("GF at t=0.25 == the scalar-spec hand derivation", gh_t and gh_t["gf"], _gf_w, 1e-12)
    c.close("RF at t=0.25 == the scalar-spec hand derivation", gh_t and gh_t["rf"], _rf_w, 1e-12)
    c.ok("...and that RF is not trivially zero (non-collinear errors)", _rf_w > 0.01,
         f"rf = {_rf_w:.4f}")

    print("=== halo_leak / ring_from_mask ===")
    single = np.zeros((41, 41), bool)
    single[20, 20] = True
    rg = ring_from_mask(single, 4)
    counts = [int(np.count_nonzero(rg == d)) for d in (1, 2, 3, 4)]
    c.ok("Chebyshev ring band d has 8d pixels", counts == [8, 16, 24, 32], f"{counts}")
    hh, ww = 200, 200
    obj = np.zeros((hh, ww), bool)
    obj[60:140, 60:140] = True
    rg = ring_from_mask(obj, 6)
    lin_gt = np.full((hh, ww, 3), 0.20)
    lin_a = np.full((hh, ww, 3), 0.10)
    lin_b = np.full((hh, ww, 3), 0.30)
    lin_x = lin_gt.copy()
    for d, dv in ((1, 0.08), (2, 0.06), (3, 0.05), (4, 0.001), (5, 0.001), (6, 0.001)):
        lin_x[rg == d] = 0.20 + dv
    GT8, A8, B8, X8 = (encode_srgb8(v) for v in (lin_gt, lin_a, lin_b, lin_x))
    hl = halo_leak(X8, A8, B8, GT8, rg)
    dec = decode_srgb8
    ref_rms = float(np.sqrt(np.mean((dec(B8) - dec(A8))[rg > 0] ** 2)))
    want_d1 = float(np.sqrt(np.mean((dec(X8) - dec(GT8))[rg == 1] ** 2))) / ref_rms
    c.close("halo band-1 nrms matches the decoded pixels",
            hl["profile"][0]["nrms"], want_d1, 1e-9)
    c.ok("halo profile decreases outwards",
         all(hl["profile"][i]["nrms"] >= hl["profile"][i + 1]["nrms"] for i in range(5)),
         ", ".join(f"d{p['d']}={p['nrms']:.3f}" for p in hl["profile"]))
    c.ok("halo extent stops at the last band above half the peak",
         hl["extent_px"] == 3, f"extent_px = {hl['extent_px']}")
    n = []
    c.ok("no motion (B == A) -> refused",
         halo_leak(X8, A8, A8, GT8, rg, notes=n) is None, n[-1] if n else "")
    # A profile whose INNERMOST band is clean and whose damage starts at d=2 - what a
    # matcher with a grid offset from the object boundary produces. extent_px was
    # normalised by band 1, so this shape returned None with no note at all.
    lin_x2 = lin_gt.copy()
    lin_x2[rg == 2] = 0.20 + 0.06
    X2 = encode_srgb8(lin_x2)
    hl2 = halo_leak(X2, A8, B8, GT8, rg)
    c.ok("extent_px is found when band 1 is clean and the leak starts at d=2",
         hl2 is not None and hl2["extent_px"] == 2,
         ", ".join(f"d{p['d']}={p['nrms']:.3f}" for p in (hl2["profile"] if hl2 else [])) +
         f" -> extent {hl2 and hl2['extent_px']}")
    n = []
    hl3 = halo_leak(GT8, A8, B8, GT8, rg, notes=n)
    c.ok("a ring with no leak at all reports extent None WITH a reason",
         hl3 is not None and hl3["extent_px"] is None and any("extent_px" in s for s in n),
         n[-1] if n else "no note recorded")

    print("=== flicker ===")
    base = np.full((64, 64, 3), 100, np.uint8)
    pulse = np.full((64, 64, 3), 110, np.uint8)
    seq = [pulse if i % 4 == 0 else base for i in range(12)]
    ids = [i // 4 for i in range(12)]           # 4 output frames per source interval
    delta = float(srgb_to_linear(110 / 255.0) - srgb_to_linear(100 / 255.0))
    fl = flicker(seq, np.ones((64, 64), bool), ids)
    c.close("flicker RMS of a 3-in-12 pulse == sqrt(3)/4 * delta",
            fl and fl["rms"], math.sqrt(3) / 4 * delta, 1e-12)
    c.ok("flicker spans 3 source intervals", fl and fl["n_intervals"] == 3,
         f"{fl['n_intervals']} intervals over {fl['n_frames']} frames")
    n = []
    c.ok("window inside one source interval -> refused",
         flicker(seq[:4], np.ones((64, 64), bool), ids[:4], notes=n) is None,
         n[-1] if n else "")
    n = []
    c.ok("missing interval_ids -> refused",
         flicker(seq, np.ones((64, 64), bool), None, notes=n) is None, n[-1] if n else "")
    still = [base] * 12
    c.close("no flicker on a still sequence", flicker(still, np.ones((64, 64), bool), ids)["rms"],
            0.0, 1e-15)
    # rms_dt was returned but never asserted. Its claim is that a pixel ramping
    # legitimately through a gradient contributes ~0 while a pulse survives, so both
    # halves need a case. The ramp has to be linear in LINEAR LIGHT, which is where
    # the detrending happens - a ramp that is linear in 8-bit CODE space is curved
    # there and correctly leaves that curvature behind (measured: rms/rms_dt only 29,
    # against 124 for the linear-light ramp below).
    ramp = [encode_srgb8(np.full((64, 64, 3), 0.10 + 0.05 * i)) for i in range(12)]
    fl_r = flicker(ramp, np.ones((64, 64), bool), ids)
    c.ok("a linear-light temporal RAMP: large rms, rms_dt at the 8-bit floor",
         fl_r["rms"] > 100 * fl_r["rms_dt"] and fl_r["rms_dt"] < 2e-3,
         f"rms={fl_r['rms']:.6f} rms_dt={fl_r['rms_dt']:.3e} "
         f"(one code step here is ~{float(srgb_to_linear(0.75)) - float(srgb_to_linear(0.75 - 1 / 255)):.1e} linear)")
    ramp_p = [encode_srgb8(np.full((64, 64, 3), 0.10 + 0.05 * i + (0.03 if i % 4 == 0 else 0.0)))
              for i in range(12)]
    fl_rp = flicker(ramp_p, np.ones((64, 64), bool), ids)
    c.ok("...but a once-per-interval pulse riding on it survives detrending",
         fl_rp["rms_dt"] > 5 * fl_r["rms_dt"],
         f"ramp rms_dt={fl_r['rms_dt']:.3e} vs ramp+pulse {fl_rp['rms_dt']:.3e}")
    # rms_srgb8 is the same deviation in 8-bit levels, for a human-readable report.
    _codes = np.arange(100, 124, 2, dtype=np.float64)
    fl_c = flicker([np.full((64, 64, 3), int(v), np.uint8) for v in _codes],
                   np.ones((64, 64), bool), ids)
    c.close("rms_srgb8 == the closed-form std of the code series", fl_c["rms_srgb8"],
            float(np.sqrt(np.mean((_codes - _codes.mean()) ** 2))), 1e-9)

    print("=== acf_at_lag ===")
    sq = np.array([1.0 if (i // 4) % 2 == 0 else -1.0 for i in range(64)])
    a8 = acf_at_lag(sq, 8)
    a4 = acf_at_lag(sq, 4)
    c.close("ACF of a period-8 square wave at lag 8 == (N-k)/N", a8 and a8["r"], 56 / 64, 1e-12)
    c.close("ACF of a period-8 square wave at lag 4 == -(N-k)/N", a4 and a4["r"], -60 / 64, 1e-12)
    n = []
    c.ok("constant series -> refused", acf_at_lag(np.ones(32), 4, notes=n) is None,
         n[-1] if n else "")
    n = []
    c.ok("lag too long for the series -> refused",
         acf_at_lag(sq[:5], 8, notes=n) is None, n[-1] if n else "")
    pooled = acf_at_lag(np.stack([sq, -sq], axis=1), 8)
    c.close("pooled ACF over two anti-phase cells", pooled and pooled["r"], 56 / 64, 1e-12)
    # the real use: the flicker series itself pumps once per source interval
    acr = acf_at_lag(fl["series"], 4)
    c.close("ACF of the pulse series at lag = 1 source interval",
            acr and acr["r"], (12 - 4) / 12, 1e-9)

    print("=== grain_floor  (sigma = 6/255) ===")
    rng = np.random.default_rng(20260906)
    hg = wg = 256
    clean = np.full((hg, wg, 3), 128.0)
    sigma = 6.0

    def grainy():
        return np.clip(np.rint(clean + rng.normal(0.0, sigma, clean.shape)),
                       0, 255).astype(np.uint8)

    Ag, Bg, GTg = grainy(), grainy(), grainy()
    gf = grain_floor(Ag, Bg, GTg, np.ones((hg, wg), bool), 0.5)
    # perfect MC on static content carries A's grain into a GT with an independent
    # one: variance 2*sigma^2 -> 29.56 dB. The cross-fade averages two independent
    # realisations: 1.5*sigma^2 -> 30.81 dB.
    c.close("perfect MC (hold-A here) == 29.55 dB", gf and gf["psnr_hold_a"], 29.557, 0.15)
    c.close("cross-fade == 30.80 dB", gf and gf["psnr_xfade"], 30.807, 0.15)
    c.between("cross-fade beats perfect MC by ~1.25 dB",
              gf["psnr_xfade"] - gf["psnr_hold_a"], 1.15, 1.35)
    c.between("hold-A and hold-B floors agree (symmetric pair)",
              abs(gf["psnr_hold_a"] - gf["psnr_hold_b"]), 0.0, 0.1)

    print("=== score_frame ===")
    strata = {"all": fullm,
              "static": np.zeros((H, 900), bool),
              "motion": np.zeros((H, 900), bool)}
    strata["static"][:, :600] = True
    strata["motion"][:, 690:860] = True
    row = score_frame(MID, A, B, GTm, 0.5, strata, halo_max_d=4,
                      meta={"clip": "bar42", "frame": 1})
    c.ok("row is flat and CSV-ready",
         all(v is None or isinstance(v, (int, float, str, bool)) for v in row.values()),
         f"{len(row)} columns")
    c.ok("exact frame scores +inf PSNR on every stratum",
         all(row[f"psnr_{s}"] == math.inf for s in strata))
    row2 = score_frame(xf, A, B, GTm, 0.5, strata, halo_max_d=4,
                       meta={"clip": "bar42", "frame": 1})
    c.ok("cross-fade row: motion-stratum GF ~1, RF ~0",
         abs(row2["gf_motion"] - 1.0) < 1e-6 and abs(row2["rf_motion"]) < 1e-3,
         f"gf={row2['gf_motion']:.4f} rf={row2['rf_motion']:.2e}")
    c.ok("cross-fade row: edge support wider than the mc row",
         row2["edgesup_all"] > row["edgesup_all"] + 30,
         f"{row2['edgesup_all']:.1f} px vs {row['edgesup_all']:.1f} px")
    tiny = {"tiny": np.zeros((H, 900), bool)}
    tiny["tiny"][:5, :5] = True
    row3 = score_frame(MID, A, B, GTm, 0.5, tiny, ring=ring_from_mask(strata["motion"], 4))
    c.ok("refused metrics are None and explained in notes",
         row3["psnr_tiny"] is None and "refused" in row3["notes"],
         row3["notes"].split(" | ")[0])

    # the sequence and flow branches, end to end through score_frame
    seq4 = [_bar_frame(H, 900, 711 + (i % 4) * 10, 77) if i % 4 else
            _bar_frame(H, 900, 711 + (i % 4) * 10 + 3, 77) for i in range(12)]
    ids4 = [i // 4 for i in range(12)]
    kk = 8
    ft4 = np.zeros((kk, kk, 2))
    fe4 = ft4.copy()
    fe4[:, :, 0] = 0.5                      # a uniform half-pixel underestimate
    fe4[0, 0, 0] = 4.0                      # one outlier cell
    row4 = score_frame(MID, A, B, GTm, 0.5, {"all": fullm}, halo_max_d=4,
                       flow_est=fe4, flow_true=ft4,
                       frames=seq4, interval_ids=ids4)
    c.close("score_frame carries EPE p50 through", row4["epe_p50"], 0.5, 1e-9)
    c.close("score_frame carries EPE bad1 through", row4["epe_bad1"], 1 / 64, 1e-12)
    c.ok("score_frame carries flicker and its ACF through",
         row4["flick_all"] is not None and row4["acfres_all"] is not None
         and row4["flick_lag"] == 4 and row4["flick_intervals"] == 3,
         f"flick={row4['flick_all']:.5f} acf@4={row4['acfres_all']:.4f} "
         f"lag={row4['flick_lag']} intervals={row4['flick_intervals']}")
    c.ok("a defect pulsing once per source interval shows in the ACF",
         row4["acfres_all"] > 0.3, f"acf@lag4 = {row4['acfres_all']:.4f}")
    c.ok("no column is written twice in a row",
         len(row4) == len(set(row4)), f"{len(row4)} columns")
    # A stratum called "frames" used to land on top of the flicker bookkeeping column
    # of the same name and replace a frame count with an RMS, silently, in the CSV.
    try:
        score_frame(MID, A, B, GTm, 0.5, {"frames": fullm}, halo_max_d=4,
                    frames=seq4, interval_ids=ids4)
        c.ok("a stratum named 'frames' is refused, not silently merged", False,
             "score_frame accepted it")
    except ValueError as exc:
        c.ok("a stratum named 'frames' is refused, not silently merged",
             "collide" in str(exc), str(exc).split(". ")[0])

    import csv
    import io
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=list(row2.keys()))
    w.writeheader()
    w.writerow(row2)
    c.ok("row round-trips through csv.DictWriter", len(buf.getvalue().splitlines()) == 2)

    print()
    print(f"{c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true",
                    help="run the closed-form assertions and print PASS/FAIL per check")
    args = ap.parse_args(argv)
    if args.selftest:
        return _selftest()
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
