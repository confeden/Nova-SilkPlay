"""Strata masks for the Nova SilkPlay quality harness.

Every metric in this harness is reported PER STRATUM.  A single frame average
hides the exact failure the harness exists to catch: a change that fixes a
moving object while wrecking the static background scores fine on an average.

The strata come from GROUND TRUTH ONLY.  They are never derived from the
engine's own motion field -- a broken field would otherwise define away its own
failure (it would call the region it got wrong "not moving" and be scored on
the easy pixels).

Two sources of strata, one output shape:

  (a) masks_from_analytic(flow, occl)  -- the generator emitted a true flow
      field and its own visible-only-in-A / visible-only-in-B mask.
  (b) masks_from_triple(a, gt, b)      -- decimation of a high-rate capture:
      only pixels exist, no field.  Classification is a THREE-WAY comparison,
      never |A-B| alone (see masks_from_triple's docstring for the trap).

Returned strata (boolean HxW):

  static           the correct output equals A (== B); anything else is error
  moving_interior  inside a moving object, boundary band excluded
  occlusion        the contested band: pixels visible on only one side
  halo_ring        NOT part of the partition -- a diagnostic sub-mask of the
                   non-moving area just outside the moving stratum, where the
                   right answer is known without any ground truth at all.
                   Usable from masks_from_triple; from masks_from_analytic it
                   is empty for any motion that is not axis-aligned, and that
                   docstring says why.

COLOUR CONVENTION (load-bearing).  The engine blends in LINEAR LIGHT: it reads
frames through _SRGB views and writes through an _SRGB render target.  Frames
on disk are 8-bit sRGB PNG.  Anything this module *synthesizes* (only the
selftest does) is therefore composed in linear light and encoded with the exact
piecewise sRGB transfer function -- never a 2.2 power, which biases every
metric in the harness by a fixed amount nobody would ever find.

CLASSIFICATION SPACE.  masks_from_triple compares 8-bit sRGB CODE LEVELS, not
linear values, because its thresholds were measured in code levels (see
DUP_LEVELS_MAX below).  Do not hand it linear-light arrays.

Standalone:  python regions.py --selftest
"""

from __future__ import annotations

import hashlib
import json
import os
import sys

import numpy as np

__all__ = [
    "STRATA",
    "masks_from_analytic",
    "masks_from_triple",
    "stratum_counts",
    "usable_strata",
    "masks_hash",
    "save_masks",
    "load_masks",
    "verify_masks",
    "linear_to_srgb",
    "srgb_to_linear",
    "encode_srgb8",
    "decode_srgb8",
]

# The four mask keys, in the fixed order used by the on-disk hash.  Changing
# this order changes every stored hash, so it is frozen.
STRATA = ("static", "moving_interior", "occlusion", "halo_ring")

# The three that partition the frame.  halo_ring deliberately overlaps static
# and must never be summed with the others.
PARTITION_STRATA = ("static", "moving_interior", "occlusion")

FORMAT_TAG = b"nova-silkplay-regions-v1"


# --------------------------------------------------------------------------
# Constants.  Each one says what set it.
# --------------------------------------------------------------------------

# A duplicate (held) source frame differs from its predecessor by roughly
# 0.7-3 grey levels of mean absolute difference; a scene cut differs by 25-38.
# Measured for this project and recorded in
# .claude/kb/research/gaps-closed-2026-09.md section 4 ("the duplicate and
# scene-cut thresholds are two orders of magnitude apart in the same normalised
# units").  Those two numbers are the whole basis of the triple classifier:
# 3 levels is the noise floor above which a pixel really changed, and 25 levels
# is the frame-mean level at which the triple spans a cut and its strata are
# meaningless.
DUP_LEVELS_MAX = 3.0     # <= this per pixel: "unchanged"
CUT_LEVELS_MIN = 25.0    # >= this as a frame mean of |A-B|: "this is a cut"

# Static means "no estimator could tell this from a held frame".  0.05 px of
# residual displacement moves an edge of realistic post-sRGB contrast
# (<= ~50 code levels across one pixel) by <= ~2.5 levels, i.e. it stays inside
# the 0.7-3 level duplicate noise floor above.  That ties the analytic
# threshold to the same measurement the decimation path uses instead of
# inventing a second, unrelated one.
STATIC_FLOW_PX = 0.05

# The moving stratum must contain only pixels whose correct value depends on
# the object alone.  The mixed band around a boundary is at least
#   1 px  bilinear fetch footprint
# + 1 px  the generator's own antialiased object edge
# + 1 px  margin, so a one-pixel error in the ground-truth edge position still
#         cannot leak boundary pixels into the "interior" score
MOVING_ERODE_PX = 3

# An occlusion halo can physically reach exactly as far as the object moved,
# so the occlusion band is dilated by the LOCAL displacement magnitude, not by
# a constant.  These only bracket it: 1 px because even a zero-displacement
# occlusion edge contaminates its own bilinear neighbourhood, and the cap is a
# guard against a pathological flow field turning the whole frame into band.
OCCL_MIN_DILATE_PX = 1.0
MAX_DILATE_PX = 128.0

# When an occlusion pixel is background, the flow stored AT it is ~0; the
# displacement that matters is the neighbouring object's.  An occlusion strip
# is at most one displacement wide (that is what uncovered it), so the lookup
# radius has to be the displacement itself, not a small constant -- with a
# 2 px lookup only the two innermost columns of an 8 px strip would inherit the
# object's 8 px reach and the band's outer edge came out short.  Capped because
# the flat max filter costs O(r) array passes; masks are computed once per clip
# and frozen, so a few passes are cheap, but a pathological flow should not
# turn this into minutes.
#
# KNOWN COARSENESS, measured, and the reason the count gate below exists: the
# lookup window is square and its radius is the GLOBAL maximum magnitude of the
# field, so the radius a pixel inherits is "the fastest thing within
# ceil(max|flow|) px", not "the speed of the object next to me".  A slow object
# near a fast one is therefore given the fast one's band: two objects 40 px
# apart moving 2 px and 50 px lose the slow one's moving_interior entirely
# (1122 px -> 0).  That is the safe direction -- the pixels land in the
# contested stratum and usable_strata() then refuses to report it -- but it is
# a real loss of coverage, not a rounding effect.  Making it genuinely local
# needs a value-carrying dilation (one cone pass per quantised speed) rather
# than a flat max, which changes every frozen mask and is deliberately not
# done here.
FLOW_LOOKUP_CAP_PX = 64

# Decimation gives no field, so the halo ring's reach cannot be derived there.
# 16 px is one cell of the prototype's block matcher (ROADMAP: "pyramid block
# matcher, 16 px cells"), which is as far as a cell-quantised field can throw a
# background pixel.  It is frozen into the saved masks so it cannot drift
# between two engine versions.
HALO_RING_PX = 16

# PSNR from N pixels has a relative standard error of about sqrt(2/N) on the
# MSE, i.e. ~4.5 % -> ~0.2 dB at N = 1000.  Below that the number moves by more
# than the smallest difference this harness intends to call meaningful, so the
# metric layer must refuse to report the stratum rather than print noise.
MIN_STRATUM_PIXELS = 1000

# L1 <= sqrt(2) * L2 in 2D, so dilating with a separable (and therefore cheap
# and exact) L1 cone of radius sqrt(2)*r covers every pixel within Euclidean r.
# Over-covering is the safe direction for a band we exclude or a ring we only
# use where A ~= B anyway.
_SQRT2 = float(np.sqrt(2.0))

_NEG = -1.0e9  # finite sentinel for max-plus, so -inf never meets +inf


# --------------------------------------------------------------------------
# sRGB transfer function -- the exact piecewise form, never a 2.2 power.
# Kept here so this module runs standalone; if a shared harness colour module
# lands, these should become imports from it rather than a second copy that can
# drift.
# --------------------------------------------------------------------------

def linear_to_srgb(x):
    """Linear light in [0,1] -> sRGB-encoded in [0,1] (IEC 61966-2-1)."""
    x = np.clip(np.asarray(x, dtype=np.float64), 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)


def srgb_to_linear(s):
    """sRGB-encoded in [0,1] -> linear light in [0,1]."""
    s = np.clip(np.asarray(s, dtype=np.float64), 0.0, 1.0)
    return np.where(s <= 0.04045, s / 12.92, np.power((s + 0.055) / 1.055, 2.4))


def encode_srgb8(linear):
    """Linear light -> 8-bit sRGB code values, the way frames hit the disk."""
    return np.rint(linear_to_srgb(linear) * 255.0).astype(np.uint8)


def decode_srgb8(code):
    """8-bit sRGB code values -> linear light."""
    return srgb_to_linear(np.asarray(code, dtype=np.float64) / 255.0)


# --------------------------------------------------------------------------
# Morphology, pure numpy.
# --------------------------------------------------------------------------

def _shift(arr, dy, dx, fill):
    """arr shifted by (dy, dx); pixels shifted in from outside get `fill`."""
    out = np.empty_like(arr)
    out[...] = fill
    h, w = arr.shape[:2]
    ys_src = slice(max(0, -dy), h - max(0, dy))
    ys_dst = slice(max(0, dy), h - max(0, -dy))
    xs_src = slice(max(0, -dx), w - max(0, dx))
    xs_dst = slice(max(0, dx), w - max(0, -dx))
    if ys_src.start < ys_src.stop and xs_src.start < xs_src.stop:
        out[ys_dst, xs_dst] = arr[ys_src, xs_src]
    return out


def _flat_dilate(mask, r):
    """Binary dilation by a (2r+1)^2 square.  Separable, so 2*(2r+1) shifts.

    Square rather than disc: it over-covers on the diagonal, and every use here
    wants the conservative direction.
    """
    r = int(r)
    if r <= 0:
        return np.array(mask, dtype=bool, copy=True)
    acc = np.zeros_like(mask, dtype=bool)
    for k in range(-r, r + 1):
        acc |= _shift(mask, 0, k, False)
    out = np.zeros_like(mask, dtype=bool)
    for k in range(-r, r + 1):
        out |= _shift(acc, k, 0, False)
    return out


def _flat_erode(mask, r):
    """Binary erosion by a (2r+1)^2 square.

    Shifts fill with False, so the image border erodes too.  That is
    deliberate: content outside the frame is unknown, so a pixel within r of
    the border cannot be certified as object interior either.
    """
    r = int(r)
    if r <= 0:
        return np.array(mask, dtype=bool, copy=True)
    acc = np.ones_like(mask, dtype=bool)
    for k in range(-r, r + 1):
        acc &= _shift(mask, 0, k, False)
    out = np.ones_like(mask, dtype=bool)
    for k in range(-r, r + 1):
        out &= _shift(acc, k, 0, False)
    return out


def _flat_max_f(arr, r):
    """Grey max filter over a (2r+1)^2 square, for looking a magnitude up in a
    small neighbourhood."""
    r = int(r)
    if r <= 0:
        return np.array(arr, dtype=np.float64, copy=True)
    acc = np.full_like(arr, _NEG, dtype=np.float64)
    for k in range(-r, r + 1):
        acc = np.maximum(acc, _shift(arr, 0, k, _NEG))
    out = np.full_like(arr, _NEG, dtype=np.float64)
    for k in range(-r, r + 1):
        out = np.maximum(out, _shift(acc, k, 0, _NEG))
    return out


def _cone_dilate(seed):
    """Max-plus dilation with the cone k(dy,dx) = -(|dy|+|dx|).

    seed[p] holds a per-pixel budget (a radius) at source pixels and _NEG
    elsewhere.  The result at q is max_p (seed[p] - L1(p,q)), so
    `result >= 0` is exactly "q is within seed[p] L1-steps of some source p".

    The L1 cone is separable under max-plus (an L2 or Chebyshev cone is not),
    which makes this two O(N) cumulative-maximum passes instead of one
    dilation per radius level.  Callers pass sqrt(2)*r as the budget so the
    diamond still contains the Euclidean disc of radius r.
    """
    g = np.array(seed, dtype=np.float64, copy=True)
    for axis in (0, 1):
        n = g.shape[axis]
        shape = [1, 1]
        shape[axis] = n
        idx = np.arange(n, dtype=np.float64).reshape(shape)
        fwd = np.maximum.accumulate(g + idx, axis=axis) - idx      # max over j<=i
        rev = np.flip(np.maximum.accumulate(np.flip(g - idx, axis=axis), axis=axis),
                      axis=axis) + idx                             # max over j>=i
        g = np.maximum(fwd, rev)
    return g


def _dilate_by_radius_map(mask, radius_px, max_dilate_px=MAX_DILATE_PX):
    """Dilate `mask` by a PER-PIXEL radius (Euclidean, covered conservatively)."""
    if not mask.any():
        return np.zeros_like(mask, dtype=bool)
    r = np.clip(np.asarray(radius_px, dtype=np.float64), 0.0, float(max_dilate_px))
    seed = np.where(mask, _SQRT2 * r, _NEG)
    return _cone_dilate(seed) >= 0.0


# --------------------------------------------------------------------------
# Input normalisation.
# --------------------------------------------------------------------------

def _colour_planes(img, name):
    """HxW or HxWx3 view of an input, with alpha dropped."""
    a = np.asarray(img)
    if a.ndim == 3:
        if a.shape[2] == 4:
            # PIL hands back RGBA for a PNG with an alpha channel; corpus
            # frames are opaque, and averaging a constant alpha into the
            # per-pixel difference would divide every threshold by 4/3.
            a = a[:, :, :3]
        elif a.shape[2] == 1:
            a = a[:, :, 0]
        elif a.shape[2] != 3:
            raise ValueError("%s: expected HxW, HxWx3 or HxWx4, got %r"
                             % (name, a.shape))
    elif a.ndim != 2:
        raise ValueError("%s: expected HxW or HxWxC, got %r" % (name, a.shape))
    return a


def _levels_scale(imgs, names):
    """Decide ONE float->code-levels scale for a group of frames that will be
    differenced against each other.  Returns 255.0, 1.0, or None (no floats).

    This must be a GROUP decision, not a per-array one.  Range inference on a
    single array cannot tell [0,1] from [0,255] when that array is dark, and a
    near-black frame is ordinary footage -- a fade-out or a cut to black.
    Deciding per array then scaled a 0.784-code-level frame by 255 while its
    200-code-level neighbours were left alone: |A-GT| read as 0.0 instead of
    199.2, so a frame that changed completely came back 100 % `static`, and a
    cut to black came back unflagged.  Both are silently wrong numbers, which
    is the one failure this harness may not have.  Taking the max over the
    whole group makes the dark frame follow its siblings.
    """
    hi = None
    for img, name in zip(imgs, names):
        if img is None:
            continue
        a = _colour_planes(img, name)
        if a.dtype == np.uint8 or not np.issubdtype(a.dtype, np.floating):
            continue
        if a.size == 0:
            continue
        lo = float(np.min(a))
        if lo < -1e-6:
            raise ValueError("%s: negative pixel values (%.4f)" % (name, lo))
        m = float(np.max(a))
        hi = m if hi is None else max(hi, m)
    if hi is None:
        return None
    if hi <= 1.0 + 1e-6:
        return 255.0
    if hi <= 255.0 + 1e-3:
        return 1.0
    raise ValueError("%s: pixel range tops out at %.4f, neither [0,1] nor [0,255]"
                     % ("/".join(names), hi))


def _as_levels(img, name, scale=None):
    """Return an HxW(xC) float array of 8-bit sRGB CODE LEVELS.

    uint8            -> taken as-is (this is what a PNG decodes to)
    float in [0,1]   -> sRGB-encoded, scaled by 255
    float in [0,255] -> already code levels
    Anything else is an error rather than a guess: getting this wrong silently
    rescales every threshold in the module.

    `scale` is the group decision from _levels_scale, and every caller that
    compares two or more frames MUST pass it -- see that function for the
    misclassification a per-array decision produces on a near-black frame.
    Left None, this falls back to inferring from this array alone, which is
    only safe for a single array considered on its own.
    """
    a = _colour_planes(img, name)
    if a.dtype == np.uint8:
        return a.astype(np.float64)
    if not np.issubdtype(a.dtype, np.floating):
        raise TypeError("%s: expected uint8 or float, got %s" % (name, a.dtype))
    lo = float(np.min(a)) if a.size else 0.0
    if lo < -1e-6:
        raise ValueError("%s: negative pixel values (%.4f)" % (name, lo))
    if scale is not None:
        return a.astype(np.float64) * float(scale)
    hi = float(np.max(a)) if a.size else 0.0
    if hi <= 1.0 + 1e-6:
        return a.astype(np.float64) * 255.0
    if hi <= 255.0 + 1e-3:
        return a.astype(np.float64)
    raise ValueError("%s: pixel range [%.4f, %.4f] is neither [0,1] nor [0,255]"
                     % (name, lo, hi))


def _diff_levels(x, y):
    """Per-pixel scalar difference in grey levels: the mean of |dx| over
    channels, which is the same statistic the 0.7-3 / 25-38 measurement used,
    just not yet averaged over the frame."""
    d = np.abs(x - y)
    if d.ndim == 3:
        d = d.mean(axis=2)
    return d


def _normalise_occl(occl, shape):
    """Accept the several shapes a generator might emit for its own
    visible-only-in-A / visible-only-in-B mask, and return one bool HxW."""
    if occl is None:
        return np.zeros(shape, dtype=bool)
    if isinstance(occl, (tuple, list)):
        out = np.zeros(shape, dtype=bool)
        for part in occl:
            out |= _normalise_occl(part, shape)
        return out
    a = np.asarray(occl)
    if a.ndim == 3:
        if a.shape[:2] != shape:
            raise ValueError("occl shape %r does not match %r" % (a.shape, shape))
        return np.any(a.astype(bool), axis=2)
    if a.shape != shape:
        raise ValueError("occl shape %r does not match %r" % (a.shape, shape))
    if a.dtype == np.bool_:
        return a.copy()
    # integer code map: 0 = visible in both, anything else = occluded
    return a != 0


# --------------------------------------------------------------------------
# Counts.
# --------------------------------------------------------------------------

def stratum_counts(masks):
    """Pixel count per stratum, plus the leftovers.

    A stratum with too few pixels gives a meaningless PSNR, which is why every
    caller gets the count and not just the mask.
    """
    h, w = masks["static"].shape
    counts = {name: int(masks[name].sum()) for name in STRATA}
    counts["total"] = int(h * w)
    counts["unclassified"] = counts["total"] - sum(counts[n] for n in PARTITION_STRATA)
    return counts


def usable_strata(counts, min_pixels=MIN_STRATUM_PIXELS):
    """Which strata carry enough pixels for a metric to be worth printing."""
    return {name: counts.get(name, 0) >= int(min_pixels) for name in STRATA}


def _finish(masks, meta):
    """Attach counts and metadata, after asserting the partition really is one."""
    part = [masks[n] for n in PARTITION_STRATA]
    for i in range(len(part)):
        for j in range(i + 1, len(part)):
            if np.any(part[i] & part[j]):
                raise AssertionError("strata %s and %s overlap"
                                     % (PARTITION_STRATA[i], PARTITION_STRATA[j]))
    if np.any(masks["halo_ring"] & (masks["moving_interior"] | masks["occlusion"])):
        raise AssertionError("halo_ring must lie outside the moving and occlusion strata")
    out = {name: masks[name] for name in STRATA}
    out["counts"] = stratum_counts(out)
    out["meta"] = meta
    return out


# --------------------------------------------------------------------------
# (a) analytic strata, from the generator's own flow and occlusion mask
# --------------------------------------------------------------------------

def masks_from_analytic(flow,
                        occl=None,
                        *,
                        a=None,
                        b=None,
                        static_flow_px=STATIC_FLOW_PX,
                        moving_flow_px=None,
                        erode_px=MOVING_ERODE_PX,
                        occl_min_dilate_px=OCCL_MIN_DILATE_PX,
                        halo_reach_scale=1.0,
                        max_dilate_px=MAX_DILATE_PX,
                        dup_level=DUP_LEVELS_MAX):
    """Strata from the TRUE flow and the generator's own occlusion mask.

    flow : HxWx2 float, the true A->B displacement in pixels, (dx, dy).
    occl : the generator's visible-only-in-A / visible-only-in-B mask.  bool
           HxW, or HxWx2, or a pair of masks, or an integer code map (0 = seen
           in both).  None means the generator reported no occlusion.
    a, b : optional source frames (8-bit sRGB).  Only used to tighten the halo
           ring with the A ~= B test; the strata themselves never look at them.

    static          |flow| below a sub-pixel threshold
    moving_interior |flow| above it, ERODED so the boundary band is out
    occlusion       the generator's mask, dilated by the displacement magnitude
                    found nearby -- that is how far the halo can physically
                    reach.  See FLOW_LOOKUP_CAP_PX for how local "nearby" is.
    halo_ring       just outside the moving object, background that did not
                    move: the correct answer there is A, known without any
                    ground truth.

    DO NOT rely on halo_ring from this path.  It survives only where occl_raw
    is absent along the object's silhouette, i.e. only for exactly axis-aligned
    motion; add one pixel of the other component and the occlusion band covers
    the whole ring and the stratum is empty (measured: (0,10) -> 1148 px,
    (1,10) -> 0 px, same object, same speed).  Read
    meta["halo_ring_suppressed_by_occlusion_px"] before concluding anything
    from an empty ring, and prefer masks_from_triple's ring, which does not
    have this failure (it subtracts the raw occlusion class, not a band).
    """
    f = np.asarray(flow, dtype=np.float64)
    if f.ndim != 3 or f.shape[2] != 2:
        raise ValueError("flow must be HxWx2, got %r" % (f.shape,))
    shape = f.shape[:2]
    mag = np.hypot(f[:, :, 0], f[:, :, 1])

    if moving_flow_px is None:
        moving_flow_px = static_flow_px
    if moving_flow_px < static_flow_px:
        raise ValueError("moving_flow_px must be >= static_flow_px")

    static_raw = mag < float(static_flow_px)
    moving_raw = mag >= float(moving_flow_px)

    occl_raw = _normalise_occl(occl, shape)

    # The displacement that matters at an occlusion pixel is the neighbouring
    # object's, not the ~0 stored at a background pixel.  "Neighbouring" here
    # means a square window whose radius is the global max magnitude, so this
    # is only as local as the fastest object in the frame -- see
    # FLOW_LOOKUP_CAP_PX for what that costs and why it is left as it is.
    lookup_r = 0
    if mag.size:
        lookup_r = int(min(np.ceil(mag.max()), float(FLOW_LOOKUP_CAP_PX)))
    lookup_r = max(lookup_r, 1)
    mag_local = np.maximum(_flat_max_f(mag, lookup_r), 0.0)

    occl_radius = np.maximum(mag_local, float(occl_min_dilate_px))
    occlusion = _dilate_by_radius_map(occl_raw, occl_radius, max_dilate_px)

    # The occlusion band wins wherever it overlaps: it is the contested
    # stratum, and scoring a contested pixel as "object interior" would flatter
    # the interior number with exactly the pixels the engine gets wrong.
    moving_interior = _flat_erode(moving_raw, erode_px) & ~occlusion
    static = static_raw & ~occlusion

    # The halo reaches as far as the object moves, so the ring is the object
    # dilated by its own displacement -- the same cone as the occlusion band.
    ring_radius = np.maximum(mag_local * float(halo_reach_scale),
                             float(occl_min_dilate_px))
    ring = _dilate_by_radius_map(moving_raw, ring_radius, max_dilate_px)
    ring &= ~moving_raw                   # moving_interior is inside moving_raw
    # How much of the ring the occlusion band eats.  This is NOT a rounding
    # detail: for any motion that is not exactly axis-aligned, occl_raw wraps
    # the whole silhouette, its outward dilation covers the entire ring, and
    # halo_ring comes out EMPTY (measured: a 64 px block moving (0,10) gives
    # 1148 ring px; the same block moving (1,10) gives 0).  The subtraction is
    # sound and must stay -- an object narrower than its own displacement
    # sweeps over background that is in neither occl_raw side, so only the
    # dilated band excludes it -- but the metric layer has to be able to tell
    # "the band ate the ring" from "nothing moved", hence this count.
    ring_suppressed = int(np.count_nonzero(ring & occlusion))
    ring &= ~occlusion
    if a is not None and b is not None:
        scale = _levels_scale((a, b), ("a", "b"))
        la = _as_levels(a, "a", scale)
        lb = _as_levels(b, "b", scale)
        if la.shape[:2] != shape or lb.shape[:2] != shape:
            raise ValueError("a/b shape does not match flow")
        ring &= _diff_levels(la, lb) <= float(dup_level)
    else:
        ring &= static_raw
    halo_ring = ring

    meta = {
        "source": "analytic",
        "height": int(shape[0]),
        "width": int(shape[1]),
        "static_flow_px": float(static_flow_px),
        "moving_flow_px": float(moving_flow_px),
        "erode_px": int(erode_px),
        "occl_min_dilate_px": float(occl_min_dilate_px),
        "halo_reach_scale": float(halo_reach_scale),
        "max_dilate_px": float(max_dilate_px),
        "flow_lookup_px": int(lookup_r),
        "halo_ring_suppressed_by_occlusion_px": ring_suppressed,
        "dup_level": float(dup_level) if (a is not None and b is not None) else None,
        "used_ab_for_ring": bool(a is not None and b is not None),
        "max_flow_px": float(mag.max()) if mag.size else 0.0,
    }
    return _finish({"static": static,
                    "moving_interior": moving_interior,
                    "occlusion": occlusion,
                    "halo_ring": halo_ring}, meta)


# --------------------------------------------------------------------------
# (b) decimation strata, from (A, GT, B) pixels alone
# --------------------------------------------------------------------------

def masks_from_triple(a, gt, b,
                      *,
                      change_level=DUP_LEVELS_MAX,
                      cut_level=CUT_LEVELS_MIN,
                      halo_ring_px=HALO_RING_PX,
                      erode_px=0):
    """Strata from (A, GT, B) pixels only -- GT-B decimation, no field.

    Classification is the THREE-WAY comparison |A-GT|, |B-GT| (with |A-B| kept
    for the ring and the cut flag), never |A-B| alone.  The one-way test is
    empty on an aliasing grating that advances a whole period: |A-B| = 0
    everywhere, so a |A-B|-only classifier calls the frame 100 % static and
    files a completely FROZEN output as ideal.  The three-way test sees that
    both sides differ from GT and refuses.

      both |A-GT| and |B-GT| large -> occlusion (the pixel is contested at
                                      every phase; a strongly textured moving
                                      region lands here too, which is coarse
                                      but is the settled rule)
      exactly one large            -> moving
      both small                   -> static

    Inputs are 8-bit sRGB code levels (uint8 HxW or HxWx3, or float [0,1] /
    [0,255] in the same encoding).  NOT linear light: the thresholds were
    measured in code levels.
    """
    scale = _levels_scale((a, gt, b), ("a", "gt", "b"))
    la = _as_levels(a, "a", scale)
    lgt = _as_levels(gt, "gt", scale)
    lb = _as_levels(b, "b", scale)
    if not (la.shape == lgt.shape == lb.shape):
        raise ValueError("a/gt/b shapes differ: %r %r %r"
                         % (la.shape, lgt.shape, lb.shape))
    shape = la.shape[:2]

    d_ab = _diff_levels(la, lb)
    d_agt = _diff_levels(la, lgt)
    d_bgt = _diff_levels(lb, lgt)

    thr = float(change_level)
    big_a = d_agt > thr
    big_b = d_bgt > thr

    occlusion = big_a & big_b
    moving = big_a ^ big_b
    static = ~big_a & ~big_b

    # Erosion is off by default here: without a field there is no object
    # boundary to erode away, only the intensity band, and that band IS the
    # moving class.  The knob exists so a caller can tighten it deliberately.
    # An eroded-off band belongs to no stratum; it does NOT fall back to static.
    moving_interior = _flat_erode(moving, erode_px) if erode_px else moving

    ring = _flat_dilate(moving, int(halo_ring_px))
    ring &= ~moving                       # moving_interior is inside moving
    ring_suppressed = int(np.count_nonzero(ring & occlusion))
    ring &= ~occlusion
    ring &= d_ab <= thr           # background that did not move: answer is A
    halo_ring = ring

    mean_ab = float(d_ab.mean()) if d_ab.size else 0.0
    scene_cut = mean_ab >= float(cut_level)

    meta = {
        "source": "triple",
        "height": int(shape[0]),
        "width": int(shape[1]),
        "change_level": thr,
        "cut_level": float(cut_level),
        "halo_ring_px": int(halo_ring_px),
        "erode_px": int(erode_px),
        "halo_ring_suppressed_by_occlusion_px": ring_suppressed,
        "mean_abs_ab_levels": mean_ab,
        # A cut between A and B makes every stratum meaningless: there is no
        # "correct" interpolation across a cut, so the metric layer must refuse
        # the whole triple, not just a thin stratum.
        "scene_cut": bool(scene_cut),
    }
    return _finish({"static": static,
                    "moving_interior": moving_interior,
                    "occlusion": occlusion,
                    "halo_ring": halo_ring}, meta)


# --------------------------------------------------------------------------
# Freezing: masks are computed once per clip and hashed, so that every engine
# version and any competitor is scored on byte-identical pixel sets.
# --------------------------------------------------------------------------

def masks_hash(masks):
    """SHA-256 over the PIXEL SETS, in the frozen STRATA order.

    Deliberately not a hash of the container: .npz is a zip and stores file
    mtimes, so the file bytes differ between two runs that produced identical
    masks.  What must be identical between runs is which pixels are in which
    stratum, and that is exactly what this covers.
    """
    h = hashlib.sha256()
    h.update(FORMAT_TAG + b"\n")
    hh, ww = masks["static"].shape
    h.update(("%dx%d\n" % (hh, ww)).encode("ascii"))
    for name in STRATA:
        m = np.ascontiguousarray(masks[name], dtype=bool)
        if m.shape != (hh, ww):
            raise ValueError("mask %s has shape %r, expected %r"
                             % (name, m.shape, (hh, ww)))
        h.update(name.encode("ascii") + b"\n")
        h.update(np.packbits(m, axis=None).tobytes())
    return h.hexdigest()


def save_masks(path, masks, extra_meta=None):
    """Freeze a mask set to disk.  Returns the SHA-256 hex digest."""
    digest = masks_hash(masks)
    hh, ww = masks["static"].shape
    meta = dict(masks.get("meta") or {})
    if extra_meta:
        meta.update(extra_meta)
    meta["strata_order"] = list(STRATA)
    meta["format"] = FORMAT_TAG.decode("ascii")
    meta["counts"] = masks.get("counts") or stratum_counts(masks)
    payload = {"sha256": np.array(digest),
               "shape": np.array([hh, ww], dtype=np.int64),
               "meta_json": np.array(json.dumps(meta, sort_keys=True))}
    for name in STRATA:
        payload["packed_" + name] = np.packbits(
            np.ascontiguousarray(masks[name], dtype=bool), axis=None)
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    # Write through a file object: np.savez(path) silently appends ".npz" to a
    # path that lacks it, and a scoring run that then reads back the path it
    # asked for would get FileNotFoundError instead of the masks it froze.
    with open(path, "wb") as fh:
        np.savez_compressed(fh, **payload)
    return digest


def _read_masks(path):
    with np.load(path, allow_pickle=False) as z:
        hh, ww = (int(v) for v in z["shape"])
        stored = str(z["sha256"])
        meta = json.loads(str(z["meta_json"]))
        masks = {}
        for name in STRATA:
            bits = np.unpackbits(z["packed_" + name], count=hh * ww)
            masks[name] = bits.reshape(hh, ww).astype(bool)
    return masks, meta, stored


def load_masks(path, verify=True):
    """Load a frozen mask set.  Returns (masks_dict, meta).

    With verify=True (the default) a mismatch raises: a stratum silently
    recomputed with a different threshold between two runs would make two
    engine versions incomparable while looking fine.
    """
    masks, meta, stored = _read_masks(path)
    actual = masks_hash(masks)
    if verify and actual != stored:
        raise ValueError("mask hash mismatch in %s: stored %s, computed %s"
                         % (path, stored, actual))
    masks["counts"] = stratum_counts(masks)
    masks["meta"] = meta
    masks["sha256"] = stored
    return masks, meta


def verify_masks(path, expected=None):
    """Check a frozen mask file.  Returns (ok, stored_hex, computed_hex).

    `expected` additionally pins the file to a hash the caller remembers, which
    is what a scoring run should do when comparing two engine versions.
    """
    masks, _meta, stored = _read_masks(path)
    actual = masks_hash(masks)
    ok = (actual == stored) and (expected is None or expected == stored)
    return ok, stored, actual


# ==========================================================================
# selftest
# ==========================================================================

_PASS = 0
_FAIL = 0


def _check(label, cond, detail=""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print("PASS  %s" % label)
    else:
        _FAIL += 1
        print("FAIL  %s %s" % (label, detail))
    return bool(cond)


def _bar_frame(h, w, x0, bar_w, bg_lin=0.02, fg_lin=0.80):
    """A hard-edged bar composed in LINEAR light, encoded to 8-bit sRGB."""
    lin = np.full((h, w, 3), bg_lin, dtype=np.float64)
    x1 = min(w, x0 + bar_w)
    if x0 < w and x1 > 0:
        lin[:, max(0, x0):x1, :] = fg_lin
    return encode_srgb8(lin)


def _block_frame(h, w, y0, x0, bh, bw, bg_lin=0.02, fg_lin=0.80):
    """A hard-edged rectangular block, composed in LINEAR light."""
    lin = np.full((h, w, 3), bg_lin, dtype=np.float64)
    lin[y0:y0 + bh, x0:x0 + bw, :] = fg_lin
    return encode_srgb8(lin)


def _grating(h, w, phase, period=8, lo_lin=0.02, hi_lin=0.80):
    """Square-wave grating, composed in linear light.  Advancing it by exactly
    `period` px reproduces the same bytes -- the pathological case."""
    x = (np.arange(w) - phase) % period
    col = np.where(x < period // 2, hi_lin, lo_lin)
    lin = np.repeat(col[None, :, None], h, axis=0).repeat(3, axis=2)
    return encode_srgb8(lin)


def _texture(h, w, seed, lo_lin, hi_lin):
    """Deterministic textured frame (seeded RNG only -- no unseeded randomness
    anywhere in this harness)."""
    rng = np.random.default_rng(seed)
    lin = lo_lin + (hi_lin - lo_lin) * rng.random((h, w, 3))
    return encode_srgb8(lin)


def _png_chunk_types(raw):
    """Walk a PNG's chunk table.  A substring search over the whole file would
    also match those four ASCII tags inside compressed IDAT data, so the
    'no colour chunk' claim is made against the real chunk list."""
    import struct
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    types, i = [], 8
    while i < len(raw):
        (ln,) = struct.unpack(">I", raw[i:i + 4])
        types.append(raw[i + 4:i + 8].decode("ascii"))
        i += 12 + ln            # length + type + data + CRC
    return types


def _png_roundtrip_ok(tmpdir, name, arr):
    """Write a PNG the way the corpus is written and prove the bytes come back
    unchanged and carry no colour chunk that could re-transform them."""
    from PIL import Image
    path = os.path.join(tmpdir, name)
    Image.fromarray(arr).save(path, icc_profile=None)
    back = np.array(Image.open(path))
    with open(path, "rb") as fh:
        raw = fh.read()
    types = _png_chunk_types(raw)
    chunks_absent = not ({"gAMA", "sRGB", "iCCP", "cHRM"} & set(types))
    return np.array_equal(arr, back), chunks_absent


def _selftest():
    import tempfile

    print("--- colour: the exact piecewise sRGB transfer function ---")
    _check("srgb(0) == 0 and srgb(1) == 1",
           abs(float(linear_to_srgb(0.0))) < 1e-12 and abs(float(linear_to_srgb(1.0)) - 1.0) < 1e-12)
    _check("srgb(0.5) == 0.735357 (reference value)",
           abs(float(linear_to_srgb(0.5)) - 0.7353569830524495) < 1e-9,
           "got %.12f" % float(linear_to_srgb(0.5)))
    xs = np.linspace(0.0, 1.0, 4097)
    _check("srgb_to_linear(linear_to_srgb(x)) == x",
           float(np.max(np.abs(srgb_to_linear(linear_to_srgb(xs)) - xs))) < 1e-12)
    codes = np.arange(256, dtype=np.uint8)
    _check("encode_srgb8(decode_srgb8(code)) == code for all 256 levels",
           bool(np.array_equal(encode_srgb8(decode_srgb8(codes)), codes)))
    _check("the two branches meet at 0.0031308",
           abs(float(linear_to_srgb(0.0031308)) - 0.0031308 * 12.92) < 1e-12)
    # The round-trip assertion above is satisfied by ANY invertible pair,
    # including a pure gamma-2.2 one, so it cannot carry the colour convention
    # on its own.  This is the external anchor that a 2.2 power fails:
    # published sRGB, code 128 -> linear 0.21586050011389926, where
    # (128/255)^2.2 = 0.219520.
    _check("decode_srgb8(128) == 0.2158605 (published anchor, 2.2 gives 0.21952)",
           abs(float(decode_srgb8(np.uint8(128))) - 0.21586050011389926) < 1e-12,
           "got %.17f" % float(decode_srgb8(np.uint8(128))))
    g22 = np.power(xs, 1.0 / 2.2) * 255.0
    exact = linear_to_srgb(xs) * 255.0
    _check("a gamma-2.2 shortcut would shift code levels by >1 (why it is banned)",
           float(np.max(np.abs(g22 - exact))) > 1.0,
           "max delta %.2f levels" % float(np.max(np.abs(g22 - exact))))

    print("--- helpers: cone dilation covers the Euclidean disc ---")
    m = np.zeros((41, 41), dtype=bool)
    m[20, 20] = True
    band = _dilate_by_radius_map(m, np.full((41, 41), 5.0))
    yy, xx = np.mgrid[0:41, 0:41]
    l2 = np.hypot(yy - 20, xx - 20)
    l1 = np.abs(yy - 20) + np.abs(xx - 20)
    _check("band contains every pixel within Euclidean 5", bool(np.all(band[l2 <= 5.0])))
    _check("band contains nothing beyond L1 ceil(sqrt(2)*5)",
           bool(np.all(~band[l1 > np.ceil(_SQRT2 * 5.0)])))
    _check("erosion by 3 removes a 3 px rim",
           not _flat_erode(np.ones((9, 9), dtype=bool), 3)[2, 4]
           and _flat_erode(np.ones((9, 9), dtype=bool), 3)[4, 4])

    H, W = 96, 128
    lvl = DUP_LEVELS_MAX

    print("--- triple: pure translation (hard-edged bar, 8 px per source pair) ---")
    bar_w, x_a, d = 20, 30, 8
    A = _bar_frame(H, W, x_a, bar_w)
    G = _bar_frame(H, W, x_a + d // 2, bar_w)
    B = _bar_frame(H, W, x_a + d, bar_w)
    tr = masks_from_triple(A, G, B)
    moving_cols = np.unique(np.nonzero(tr["moving_interior"])[1])
    expect_cols = np.array(sorted(set(range(30, 38)) | set(range(50, 58))))
    _check("moving columns are exactly the two 8 px edge bands",
           np.array_equal(moving_cols, expect_cols), "got %r" % (moving_cols,))
    _check("moving count == 16 columns * H", tr["counts"]["moving_interior"] == 16 * H,
           "got %d" % tr["counts"]["moving_interior"])
    _check("a pure translation produces no occlusion class",
           tr["counts"]["occlusion"] == 0, "got %d" % tr["counts"]["occlusion"])
    _check("everything else is static",
           tr["counts"]["static"] == H * W - 16 * H, "got %d" % tr["counts"]["static"])
    _check("no scene cut flagged", tr["meta"]["scene_cut"] is False)
    _check("halo ring is non-empty and sits outside the moving stratum",
           tr["counts"]["halo_ring"] > 0
           and not np.any(tr["halo_ring"] & tr["moving_interior"]))
    _check("halo ring only covers pixels where A ~= B",
           bool(np.all(_diff_levels(_as_levels(A, "a"), _as_levels(B, "b"))[tr["halo_ring"]] <= lvl)))

    def _rgba(img):
        alpha = np.full(img.shape[:2] + (1,), 255, dtype=np.uint8)
        return np.concatenate([img, alpha], axis=2)
    _check("grayscale HxW input classifies identically to RGB",
           masks_hash(masks_from_triple(A[:, :, 0], G[:, :, 0], B[:, :, 0])) == masks_hash(tr))
    _check("an opaque alpha channel does not shift the thresholds",
           masks_hash(masks_from_triple(_rgba(A), _rgba(G), _rgba(B))) == masks_hash(tr))
    _check("float [0,1] sRGB input classifies identically to uint8",
           masks_hash(masks_from_triple(A / 255.0, G / 255.0, B / 255.0)) == masks_hash(tr))

    print("--- triple: the range of a group of frames is decided ONCE ---")
    # A near-black frame is ordinary footage (a fade-out, a cut to black).
    # Deciding [0,1] vs [0,255] per array used to scale it by 255 while its
    # bright neighbours were left alone, which reported a frame that changed by
    # 199 levels as 100 % static and swallowed the scene cut with it.
    dark_f = np.full((16, 16, 3), 0.784)      # 0.784 CODE LEVELS, i.e. black
    bright_f = np.full((16, 16, 3), 200.0)    # 200 code levels
    grp = masks_from_triple(bright_f, dark_f, bright_f)
    _check("a near-black float frame does not get rescaled past its siblings",
           grp["counts"]["static"] == 0,
           "got %d/%d static" % (grp["counts"]["static"], 16 * 16))
    cut_f = masks_from_triple(np.full((16, 16, 3), 0.9),
                             np.full((16, 16, 3), 0.9),
                             np.full((16, 16, 3), 240.0))
    _check("and the cut it hides is flagged (mean |A-B| ~ 239 levels)",
           cut_f["meta"]["scene_cut"] is True,
           "got %.1f" % cut_f["meta"]["mean_abs_ab_levels"])
    _check("float code levels agree with the same content as uint8",
           masks_hash(grp) == masks_hash(masks_from_triple(
               np.full((16, 16, 3), 200, np.uint8),
               np.full((16, 16, 3), 1, np.uint8),
               np.full((16, 16, 3), 200, np.uint8))))

    print("--- triple: the channel reduction and the threshold edge ---")
    flat = np.full((8, 8, 3), 100, np.uint8)
    r9 = flat.copy(); r9[..., 0] = 109        # mean 3.0, max 9
    _check("the reduction is the channel MEAN: R+9,G+0,B+0 -> 3.0 -> unchanged",
           masks_from_triple(flat, r9, flat)["counts"]["static"] == 64,
           "a max reduction would call this changed")
    r10 = flat.copy(); r10[..., 0] = 110      # mean 3.33
    _check("...and R+10 -> 3.33 -> changed",
           masks_from_triple(flat, r10, flat)["counts"]["static"] == 0)
    e3 = np.full((8, 8, 3), 103, np.uint8)
    _check("the test is '> 3.0', so exactly DUP_LEVELS_MAX is still unchanged",
           masks_from_triple(flat, e3, flat)["counts"]["static"] == 64)

    print("--- triple: duplicate frame (held source, +-1 level of noise) ---")
    base = _texture(H, W, 1234, 0.05, 0.6)

    def _jitter(img, r):
        """+-1 code level of seeded noise: a held frame is not bit-identical."""
        n = np.random.default_rng(r).integers(-1, 2, size=img.shape, dtype=np.int16)
        return np.clip(img.astype(np.int16) + n, 0, 255).astype(np.uint8)
    dupA, dupG, dupB = _jitter(base, 1), _jitter(base, 2), _jitter(base, 3)
    du = masks_from_triple(dupA, dupG, dupB)
    _check("a duplicate frame is 100 % static", du["counts"]["static"] == H * W,
           "got %d/%d" % (du["counts"]["static"], H * W))
    _check("duplicate: nothing moving, nothing occluded",
           du["counts"]["moving_interior"] == 0 and du["counts"]["occlusion"] == 0)
    _check("duplicate frame-mean |A-B| is inside the measured 0.7-3 level band",
           du["meta"]["mean_abs_ab_levels"] <= DUP_LEVELS_MAX,
           "got %.3f" % du["meta"]["mean_abs_ab_levels"])

    print("--- triple: hard cut ---")
    cutA = _texture(H, W, 7, 0.0, 0.06)
    cutB = _texture(H, W, 8, 0.45, 1.0)
    cu = masks_from_triple(cutA, cutB, cutB)   # the cut lands between A and GT
    _check("a cut is flagged so the metric layer can refuse the triple",
           cu["meta"]["scene_cut"] is True,
           "mean |A-B| = %.1f levels" % cu["meta"]["mean_abs_ab_levels"])
    _check("cut frame-mean |A-B| is in the measured 25-38+ band",
           cu["meta"]["mean_abs_ab_levels"] >= CUT_LEVELS_MIN,
           "got %.1f" % cu["meta"]["mean_abs_ab_levels"])
    _check("nothing across a cut is called static", cu["counts"]["static"] == 0,
           "got %d" % cu["counts"]["static"])

    print("--- triple: the grating that advances exactly one period ---")
    period = 8
    grA = _grating(H, W, 0, period)
    grG = _grating(H, W, period // 2, period)
    grB = _grating(H, W, period, period)
    _check("the trap is real: A and B are byte-identical", np.array_equal(grA, grB))
    d_ab = _diff_levels(_as_levels(grA, "a"), _as_levels(grB, "b"))
    _check("a |A-B|-only classifier would call 100 % of it static",
           bool(np.all(d_ab <= lvl)))
    gr = masks_from_triple(grA, grG, grB)
    _check("THE requirement: the grating is NOT classified as static",
           gr["counts"]["static"] == 0, "got %d static px" % gr["counts"]["static"])
    _check("the grating lands in the occlusion (contested) stratum",
           gr["counts"]["occlusion"] == H * W,
           "got %d/%d" % (gr["counts"]["occlusion"], H * W))
    _check("a frozen output would therefore be scored on the contested stratum",
           gr["counts"]["static"] + gr["counts"]["moving_interior"] == 0)

    print("--- analytic: moving block with true flow and a real occlusion mask ---")
    # A square block, not the full-height bar above: an object that spans the
    # frame has no background above or below it, and the halo ring around a
    # translating object lives exactly there -- to the sides of the motion the
    # background is the occlusion band, not free ground truth.
    AH, AW = 128, 256
    by0, bx0, bs, bd = 40, 60, 48, 8
    A2 = _block_frame(AH, AW, by0, bx0, bs, bs)
    B2 = _block_frame(AH, AW, by0, bx0 + bd, bs, bs)
    flow = np.zeros((AH, AW, 2), dtype=np.float64)
    flow[by0:by0 + bs, bx0:bx0 + bs, 0] = float(bd)     # the block's A->B motion
    occ_only_b = np.zeros((AH, AW), dtype=bool)
    occ_only_b[by0:by0 + bs, bx0:bx0 + bd] = True       # disoccluded behind it
    occ_only_a = np.zeros((AH, AW), dtype=bool)
    occ_only_a[by0:by0 + bs, bx0 + bs:bx0 + bs + bd] = True  # covered by it in B
    an = masks_from_analytic(flow, (occ_only_a, occ_only_b), a=A2, b=B2)
    cy, cx = by0 + bs // 2, bx0 + bs // 2
    _check("moving_interior is eroded: the object's own edge row is excluded",
           not an["moving_interior"][by0, cx] and not an["moving_interior"][by0 + bs - 1, cx])
    _check("moving_interior starts erode_px inside the object",
           bool(an["moving_interior"][by0 + MOVING_ERODE_PX, cx]))
    _check("moving_interior keeps the object's middle", bool(an["moving_interior"][cy, cx]))
    _check("moving_interior never leaves the true moving support",
           bool(np.all(~an["moving_interior"][:by0, :])
                and np.all(~an["moving_interior"][by0 + bs:, :])
                and np.all(~an["moving_interior"][:, :bx0])
                and np.all(~an["moving_interior"][:, bx0 + bs:])))
    _check("the generator's occlusion mask is fully inside the occlusion band",
           bool(np.all(an["occlusion"][occ_only_a | occ_only_b])))
    _check("the occlusion band is dilated by the displacement (8 px), not by 1",
           bool(an["occlusion"][cy, bx0 + bs + bd + 3]), "band ends too early")
    _check("and it stops there: 20 px past the strip is not band",
           not an["occlusion"][cy, bx0 + bs + bd + 20])
    _check("the band does not swallow the frame",
           an["counts"]["occlusion"] < AH * AW // 3,
           "got %d of %d" % (an["counts"]["occlusion"], AH * AW))
    _check("static is the far background only",
           bool(an["static"][5, 5]) and not an["static"][cy, bx0 + 2])
    _check("halo_ring is non-empty and disjoint from moving/occlusion",
           an["counts"]["halo_ring"] > 0
           and not np.any(an["halo_ring"] & (an["moving_interior"] | an["occlusion"])))
    _check("halo_ring sits perpendicular to the motion, where A really is B",
           bool(an["halo_ring"][by0 - 3, cx]) and bool(an["halo_ring"][by0 + bs + 2, cx]))
    _check("halo_ring only covers pixels where A ~= B",
           bool(np.all(_diff_levels(_as_levels(A2, "a"), _as_levels(B2, "b"))[an["halo_ring"]] <= lvl)))
    _check("halo_ring reach follows the displacement, not a constant",
           not an["halo_ring"][by0 - int(np.ceil(_SQRT2 * bd)) - 2, cx])
    _check("the three partition strata are disjoint and do not exceed the frame",
           an["counts"]["static"] + an["counts"]["moving_interior"]
           + an["counts"]["occlusion"] + an["counts"]["unclassified"] == AH * AW)

    print("--- analytic: the strata checked against a ground truth I compose ---")
    # Everything above checks WHICH pixels are in each stratum.  This checks
    # what the strata MEAN, against a t=0.5 frame built independently: the
    # displacement is even, so the half-step is an exact integer shift and the
    # comparison can be bit-exact rather than tolerant.  Diagonal on purpose --
    # every analytic case above moves along one axis, which is the one
    # direction that hides the halo_ring failure asserted further down.
    SH, SW, sy, sx, ss = 160, 220, 40, 50, 64
    sdy, sdx = 6, 10
    _rng = np.random.default_rng(20260906)
    _bg = 0.02 + 0.30 * _rng.random((SH, SW, 3))
    _tex = 0.40 + 0.55 * _rng.random((ss, ss, 3))

    def _scene(y, x):
        lin = _bg.copy()
        lin[y:y + ss, x:x + ss, :] = _tex
        return encode_srgb8(lin)

    sA = _scene(sy, sx)
    sG = _scene(sy + sdy // 2, sx + sdx // 2)     # the true t=0.5 frame
    sB = _scene(sy + sdy, sx + sdx)
    SUP_A = np.zeros((SH, SW), bool); SUP_A[sy:sy + ss, sx:sx + ss] = True
    SUP_B = np.zeros((SH, SW), bool)
    SUP_B[sy + sdy:sy + sdy + ss, sx + sdx:sx + sdx + ss] = True
    sflow = np.zeros((SH, SW, 2))
    sflow[SUP_A, 0] = float(sdx)
    sflow[SUP_A, 1] = float(sdy)
    sm = masks_from_analytic(sflow, (SUP_B & ~SUP_A, SUP_A & ~SUP_B), a=sA, b=sB)
    gt_is_a = np.all(sA == sG, axis=2)
    _check("DEFINITION of static: the true t=0.5 frame equals A on every "
           "static pixel, bit-exactly",
           bool(np.all(gt_is_a[sm["static"]])),
           "%d of %d violate" % (int(np.sum(~gt_is_a & sm["static"])),
                                 int(sm["static"].sum())))
    _check("static is not trivially small (%d px)" % sm["counts"]["static"],
           sm["counts"]["static"] > 10000)
    _shifted = np.zeros_like(sA)
    _shifted[sdy // 2:, sdx // 2:, :] = sA[:SH - sdy // 2, :SW - sdx // 2, :]
    rigid = np.all(sG == _shifted, axis=2)
    _check("DEFINITION of moving_interior: GT[p] == A[p - d/2] there, i.e. the "
           "pixel really is object content at all three instants",
           bool(np.all(rigid[sm["moving_interior"]])),
           "%d of %d violate" % (int(np.sum(~rigid & sm["moving_interior"])),
                                 int(sm["moving_interior"].sum())))
    _check("moving_interior is non-empty (%d px) and genuinely not static"
           % sm["counts"]["moving_interior"],
           sm["counts"]["moving_interior"] > 200
           and float(np.mean(~gt_is_a[sm["moving_interior"]])) > 0.9)

    print("--- analytic: halo_ring dies off-axis, and says so ---")
    def _blkflow(dy, dx):
        f = np.zeros((200, 260, 2))
        sa = np.zeros((200, 260), bool); sa[60:124, 70:134] = True
        sb = np.zeros((200, 260), bool); sb[60 + dy:124 + dy, 70 + dx:134 + dx] = True
        f[sa, 0] = float(dx); f[sa, 1] = float(dy)
        return f, (sb & ~sa, sa & ~sb)
    f_ax, o_ax = _blkflow(0, 10)
    f_di, o_di = _blkflow(1, 10)
    ax = masks_from_analytic(f_ax, o_ax)
    di = masks_from_analytic(f_di, o_di)
    _check("axis-aligned motion leaves a ring (%d px)" % ax["counts"]["halo_ring"],
           ax["counts"]["halo_ring"] > 500)
    _check("ONE pixel of the other component empties it -- occl_raw then wraps "
           "the whole silhouette and the band covers the ring",
           di["counts"]["halo_ring"] == 0,
           "got %d" % di["counts"]["halo_ring"])
    _check("and the meta says the band ate it, so an empty ring is never "
           "mistaken for 'nothing moved'",
           di["meta"]["halo_ring_suppressed_by_occlusion_px"] > 500
           and ax["meta"]["halo_ring_suppressed_by_occlusion_px"]
           < di["meta"]["halo_ring_suppressed_by_occlusion_px"],
           "%r" % (di["meta"]["halo_ring_suppressed_by_occlusion_px"],))
    _check("the triple path's ring does NOT have this failure",
           masks_from_triple(_scene(sy, sx), _scene(sy + 5, sx + 5),
                             _scene(sy + 10, sx + 10))["counts"]["halo_ring"] > 500)

    print("--- analytic: a fast object inflates a slow neighbour's band ---")
    # Pinned because it is a real loss of coverage, not a rounding effect, and
    # because the only thing standing between it and a printed number is the
    # MIN_STRATUM_PIXELS gate.
    def _two(gap):
        f = np.zeros((120, 640, 2))
        slow = np.zeros((120, 640), bool); slow[40:80, 40:80] = True
        fx = 80 + gap
        fast = np.zeros((120, 640), bool); fast[40:80, fx:fx + 40] = True
        f[slow, 0] = 2.0
        f[fast, 0] = 50.0
        slow_b = np.zeros((120, 640), bool); slow_b[40:80, 42:82] = True
        fast_b = np.zeros((120, 640), bool); fast_b[40:80, fx + 50:fx + 90] = True
        m = masks_from_analytic(f, (slow_b ^ slow) | (fast_b ^ fast))
        return int(m["moving_interior"][:, :fx - 10].sum())
    _check("400 px away, the slow object keeps its interior (%d px)" % _two(400),
           _two(400) > 1000)
    _check("40 px away it loses ALL of it (the lookup window is the global max)",
           _two(40) == 0, "got %d" % _two(40))
    _check("...and the count gate is what catches that",
           usable_strata({"moving_interior": _two(40)})["moving_interior"] is False)

    print("--- analytic: a static clip has no moving or occlusion stratum ---")
    zero = masks_from_analytic(np.zeros((H, W, 2)), None)
    _check("|flow| = 0 everywhere is 100 % static", zero["counts"]["static"] == H * W)
    _check("no ring where nothing moves", zero["counts"]["halo_ring"] == 0)
    sub = masks_from_analytic(np.full((H, W, 2), 0.02), None)
    _check("sub-threshold flow (0.028 px) is still static",
           sub["counts"]["static"] == H * W, "got %d" % sub["counts"]["static"])

    print("--- counts: a thin stratum must be refusable ---")
    counts = an["counts"]
    use = usable_strata(counts)
    _check("usable_strata answers for every stratum",
           set(use) == set(STRATA))
    tiny = {"static": 10, "moving_interior": 10 ** 6, "occlusion": 0, "halo_ring": 999}
    ut = usable_strata(tiny)
    _check("a 10 px and a 999 px stratum are refused, a 1e6 px one is not",
           ut["static"] is False and ut["halo_ring"] is False
           and ut["moving_interior"] is True)

    print("--- freezing: save / load / verify / tamper ---")
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "clip", "masks.npz")
        digest = save_masks(p, an, {"clip": "selftest"})
        loaded, meta = load_masks(p)
        _check("every stratum survives the round trip byte-identically",
               all(np.array_equal(loaded[n], an[n]) for n in STRATA))
        _check("the hash is stable across a round trip", masks_hash(loaded) == digest)
        ok, stored, actual = verify_masks(p)
        _check("verify_masks accepts an untouched file",
               ok and stored == actual == digest)
        ok2, _, _ = verify_masks(p, expected=digest)
        _check("verify_masks accepts a caller-pinned hash", ok2)
        ok3, _, _ = verify_masks(p, expected="0" * 64)
        _check("verify_masks rejects a different pinned hash", not ok3)
        _check("metadata (thresholds + clip id) travels with the masks",
               meta["clip"] == "selftest" and meta["source"] == "analytic"
               and meta["erode_px"] == MOVING_ERODE_PX)
        _check("counts are stored, so a thin stratum stays refusable after loading",
               meta["counts"]["static"] == an["counts"]["static"])
        p_noext = os.path.join(tmp, "masks_no_extension")
        save_masks(p_noext, tr)
        _check("a path without .npz is written exactly where it was asked for",
               os.path.exists(p_noext) and verify_masks(p_noext)[0])

        # tamper: flip one pixel and re-pack, leaving the stored hash alone
        with np.load(p, allow_pickle=False) as z:
            payload = {k: z[k] for k in z.files}
        bad = loaded["static"].copy()
        idx = np.argwhere(bad)[0]
        bad[idx[0], idx[1]] = False
        payload["packed_static"] = np.packbits(bad, axis=None)
        with open(p, "wb") as fh:
            np.savez(fh, **payload)
        okb, storedb, actualb = verify_masks(p)
        _check("verify_masks catches a single flipped pixel",
               (not okb) and storedb != actualb)
        raised = False
        try:
            load_masks(p)
        except ValueError:
            raised = True
        _check("load_masks refuses a tampered file", raised)

        print("--- disk: PNG round trip, no colour chunks ---")
        eq, no_chunks = _png_roundtrip_ok(tmp, "a.png", A)
        _check("a written PNG reads back byte-identical", eq)
        _check("no gAMA/sRGB/iCCP/cHRM chunk that could re-transform it", no_chunks)
        eqg, _ = _png_roundtrip_ok(tmp, "g.png", grG)
        _check("the grating round-trips too", eqg)

        print("--- determinism ---")
        again = masks_from_triple(A, G, B)
        _check("recomputing the same triple gives the same hash",
               masks_hash(again) == masks_hash(tr))
        an2 = masks_from_analytic(flow, (occ_only_a, occ_only_b), a=A2, b=B2)
        _check("recomputing the analytic masks gives the same hash",
               masks_hash(an2) == masks_hash(an))

    print("")
    print("%d passed, %d failed" % (_PASS, _FAIL))
    return 0 if _FAIL == 0 else 1


def main(argv):
    if "--selftest" in argv:
        return _selftest()
    print(__doc__)
    print("usage: python regions.py --selftest")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
