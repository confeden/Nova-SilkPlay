#!/usr/bin/env python3
"""Build a photographic corpus from Netflix Open Content, with no video decoder.

Netflix publishes its Open Content titles (CC-BY 4.0) as per-frame TIFF
sequences that are UNCOMPRESSED 16-bit chunky RGB. That is the whole reason this
project needs no ffmpeg and no codec anywhere in the measurement path: a ~40-line
numpy reader gives an exact, documented, third-party-reproducible pixel
transform, where a decoder would give us a black box whose output depends on its
version and its flags.

VERIFIED BY DIRECT INSPECTION, not assumed (HTTP range read of one file's IFD):
    Meridian/tiffs/Meridian_UHD4k5994p_HDR_P3PQ_00000.tif
    little-endian, magic 42, IFD at the END of the file (offset 49,783,724)
    ImageWidth 3840, ImageLength 2160, Compression 1 (none), Photometric 2 (RGB),
    SamplesPerPixel 3, RowsPerStrip 1 (2160 strips), no PlanarConfig tag (chunky),
    no ICC profile tag. Range requests answer 206.

ACCESS NOTE, also learned the hard way: the friendly host
`download.opencontent.netflix.com` is an S3 *website* endpoint, and website
endpoints do not speak HTTPS at all — the TLS handshake simply fails. The bucket
itself is named with dots, so virtual-host HTTPS cannot work either. The one URL
shape that works over HTTPS is path style:
    https://s3.amazonaws.com/download.opencontent.netflix.com/<key>

The source is HDR: P3 primaries, PQ transfer, 4K. The corpus stores 8-bit sRGB
PNG at half resolution, so the tone map below is part of the measurement and is
PINNED — changing any constant in it changes every number computed from this
corpus, so it is versioned by TONEMAP_ID and written into the manifest.

  python from_tiff.py --clip meridian --first 0 --count 60 --out ../corpus/R2
  python from_tiff.py --list
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import urllib.request

import numpy as np
from PIL import Image

BUCKET = "https://s3.amazonaws.com/download.opencontent.netflix.com"

CLIPS = {
    # id: (key prefix, filename pattern, index width, exact rational rate)
    "meridian": ("Meridian/tiffs/", "Meridian_UHD4k5994p_HDR_P3PQ_%05d.tif", 5, (60000, 1001)),
    "chimera60": ("Chimera/tif_DCI4k5994p/", "Chimera_DCI4k5994p_HDR_P3PQ_%06d.tif", 6, (60000, 1001)),
    "chimera24": ("Chimera/tif_DCI4k2398p/", "Chimera_DCI4k2398p_HDR_P3PQ_%05d.tif", 5, (24000, 1001)),
}

# ---------------------------------------------------------------- tone map
# Pinned. Every constant here is part of the corpus definition.
TONEMAP_ID = "pq203-reinhard1000-p3d65-to-709-srgb-v1"

# SMPTE ST 2084 (PQ) inverse EOTF constants.
PQ_M1 = 2610.0 / 16384.0
PQ_M2 = 2523.0 / 4096.0 * 128.0
PQ_C1 = 3424.0 / 4096.0
PQ_C2 = 2413.0 / 4096.0 * 32.0
PQ_C3 = 2392.0 / 4096.0 * 32.0
PQ_PEAK_NITS = 10000.0

# BT.2408 reference white for HDR graphics is 203 cd/m^2; mapping it to sRGB 1.0
# is what keeps faces and mid-greys where an SDR viewer expects them.
DIFFUSE_WHITE_NITS = 203.0
# Highlights above white roll off instead of clipping: a hard clip would create
# large flat regions, and a flat region is a hole in every motion estimator.
ROLLOFF_PEAK_NITS = 1000.0

# P3-D65 -> BT.709, both D65, so this is a pure primaries change (Bradford not
# needed). Derived from the standard RGB->XYZ matrices.
P3D65_TO_BT709 = np.array([
    [1.224940, -0.224940, 0.000000],
    [-0.042057, 1.042057, 0.000000],
    [-0.019637, -0.078636, 1.098273],
], dtype=np.float64)


def pq_to_nits(x):
    """ST 2084 EOTF: normalized code value -> absolute luminance in cd/m^2."""
    x = np.maximum(x, 0.0)
    xp = np.power(x, 1.0 / PQ_M2)
    num = np.maximum(xp - PQ_C1, 0.0)
    den = PQ_C2 - PQ_C3 * xp
    return PQ_PEAK_NITS * np.power(num / np.maximum(den, 1e-12), 1.0 / PQ_M1)


def srgb_oetf(linear):
    """Linear (0..1) -> sRGB, the exact piecewise curve, never a 2.2 power."""
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92,
                    1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def tonemap_pq_p3_to_srgb8(rgb16):
    """4K 16-bit P3-PQ -> 8-bit sRGB, by the pinned transform above."""
    x = rgb16.astype(np.float64) / 65535.0
    nits = pq_to_nits(x)

    # Extended Reinhard: unity at the diffuse white point, rolling off so that
    # ROLLOFF_PEAK_NITS maps to 1.0 rather than to a clipped plateau.
    l = nits / DIFFUSE_WHITE_NITS
    lmax = ROLLOFF_PEAK_NITS / DIFFUSE_WHITE_NITS
    linear_p3 = l * (1.0 + l / (lmax * lmax)) / (1.0 + l)

    linear_709 = linear_p3 @ P3D65_TO_BT709.T
    return np.round(srgb_oetf(linear_709) * 255.0).astype(np.uint8)


# ------------------------------------------------------------------- tiff
def read_uncompressed_tiff_rgb16(data):
    """Minimal reader for exactly the shape Netflix ships: uncompressed, chunky,
    16-bit RGB, one row per strip. Anything else raises rather than guessing."""
    if data[:2] not in (b"II", b"MM"):
        raise ValueError("not a TIFF")
    bo = "<" if data[:2] == b"II" else ">"
    magic, ifd_off = struct.unpack(bo + "HI", data[2:8])
    if magic != 42:
        raise ValueError(f"unexpected TIFF magic {magic}")

    n, = struct.unpack(bo + "H", data[ifd_off:ifd_off + 2])
    tags = {}
    for i in range(n):
        off = ifd_off + 2 + i * 12
        tag, typ, cnt = struct.unpack(bo + "HHI", data[off:off + 8])
        raw = data[off + 8:off + 12]
        if typ == 3 and cnt == 1:
            val = struct.unpack(bo + "H", raw[:2])[0]
        else:
            val = struct.unpack(bo + "I", raw)[0]
        tags[tag] = (typ, cnt, val)

    def scalar(tag, default=None):
        return tags[tag][2] if tag in tags else default

    w, h = scalar(256), scalar(257)
    if scalar(259, 1) != 1:
        raise ValueError("compressed TIFF: this reader is deliberately minimal")
    if scalar(262) != 2 or scalar(277) != 3:
        raise ValueError("not chunky RGB")
    if scalar(284, 1) != 1:
        raise ValueError("planar TIFF")

    # BitsPerSample is 3 shorts, so it lives at an offset.
    bps_off = tags[258][2]
    bps = struct.unpack(bo + "HHH", data[bps_off:bps_off + 6])
    if bps != (16, 16, 16):
        raise ValueError(f"unexpected bit depth {bps}")

    # StripOffsets: 2160 longs at an offset. The rows are contiguous in every
    # file inspected, which the assert below re-checks per file rather than
    # trusting — a non-contiguous file would otherwise be read as garbage.
    so_typ, so_cnt, so_val = tags[273]
    if so_cnt == 1:
        first = so_val
    else:
        offs = np.frombuffer(data, dtype=np.dtype(bo + "u4"), count=so_cnt, offset=so_val)
        first = int(offs[0])
        row_bytes = w * 3 * 2
        if not np.array_equal(offs, first + np.arange(so_cnt, dtype=np.int64) * row_bytes):
            raise ValueError("strips are not contiguous")

    need = w * h * 3 * 2
    px = np.frombuffer(data, dtype=np.dtype(bo + "u2"), count=w * h * 3, offset=first)
    if px.nbytes != need:
        raise ValueError("short pixel block")
    return px.reshape(h, w, 3)


def box_downscale_2x2(img):
    """Exact 2x2 box average, computed in the SOURCE's linear-ish 16-bit domain
    before tone mapping would be wrong; this runs on the 8-bit sRGB result, so
    it is done in float to avoid a second quantisation."""
    h, w = img.shape[:2]
    h -= h % 2
    w -= w % 2
    f = img[:h, :w].astype(np.float64).reshape(h // 2, 2, w // 2, 2, -1)
    return np.round(f.mean(axis=(1, 3))).astype(np.uint8)


# ------------------------------------------------------------------ fetch
def fetch(key, retries=3):
    url = f"{BUCKET}/{key}"
    last = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=300) as f:
                return f.read()
        except Exception as e:  # noqa: BLE001 - any transport failure is retryable
            last = e
            print(f"    retry {attempt + 1}/{retries} after {type(e).__name__}", file=sys.stderr)
    raise RuntimeError(f"failed to fetch {key}: {last}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", choices=sorted(CLIPS), default="meridian")
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--out", default="corpus/R2")
    ap.add_argument("--no-downscale", action="store_true")
    ap.add_argument("--list", action="store_true", help="print the clip table and exit")
    args = ap.parse_args()

    if args.list:
        for k, (prefix, pat, _, rate) in CLIPS.items():
            print(f"{k:12s} {prefix:28s} {rate[0]}/{rate[1]} fps  {pat}")
        return 0

    prefix, pattern, _, rate = CLIPS[args.clip]
    os.makedirs(args.out, exist_ok=True)
    manifest_path = os.path.join(args.out, "clip.json")
    manifest = {
        "clip": args.clip,
        "source": f"{BUCKET}/{prefix}",
        "licence": "CC-BY 4.0 (Netflix Open Content)",
        "native_rate": {"num": rate[0], "den": rate[1]},
        "tonemap": TONEMAP_ID,
        "downscale": "none" if args.no_downscale else "box2x2",
        "frames": [],
    }
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)

    have = {e["index"] for e in manifest["frames"]}
    for i in range(args.first, args.first + args.count):
        out_png = os.path.join(args.out, f"{i:06d}.png")
        if i in have and os.path.exists(out_png):
            print(f"  {i:06d} already present")
            continue
        key = prefix + (pattern % i)
        print(f"  {i:06d} fetching {key}", flush=True)
        blob = fetch(key)
        rgb16 = read_uncompressed_tiff_rgb16(blob)
        rgb8 = tonemap_pq_p3_to_srgb8(rgb16)
        if not args.no_downscale:
            rgb8 = box_downscale_2x2(rgb8)
        Image.fromarray(rgb8, mode="RGB").save(out_png, optimize=False)
        digest = hashlib.sha256(rgb8.tobytes()).hexdigest()
        manifest["frames"] = [e for e in manifest["frames"] if e["index"] != i]
        manifest["frames"].append({
            "index": i,
            "file": os.path.basename(out_png),
            "source_key": key,
            "source_bytes": len(blob),
            "pixels_sha256": digest,
            "size": [int(rgb8.shape[1]), int(rgb8.shape[0])],
        })
        manifest["frames"].sort(key=lambda e: e["index"])
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
        print(f"    -> {out_png}  {rgb8.shape[1]}x{rgb8.shape[0]}  {digest[:16]}", flush=True)

    print(f"{len(manifest['frames'])} frames in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
