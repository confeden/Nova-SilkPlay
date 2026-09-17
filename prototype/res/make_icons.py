"""make_icons.py - renders the tray/application icons: a pink silk ribbon.

    python make_icons.py            -> silk.ico, silk_off.ico, icon_preview.png

The ribbon is modelled, not painted: a band swept along an S and twisted once about its own
centreline, so it turns edge-on in the lower bend and shows its deeper pink face in the tail,
with a satin sheen wherever the surface faces the light. It is rendered by splatting a dense
(u, v) sampling of that surface into a supersampled z-buffer and box-filtering down, which
gives smooth shading at 256 px and exact coverage anti-aliasing at 16 px.

Every size is rendered from the model, not downscaled from the large image: the tray draws
16-32 px icons, and there the band is widened and given a darker rim so the silhouette reads
on a light and on a dark taskbar alike. silk_off.ico is the same ribbon in grey, for the
"frame generation off" state.

Output entries follow the layout Windows expects: 32-bit DIBs with an AND mask up to 64 px,
PNG for 256 px.
"""

import io
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


FRONT = srgb_to_linear([255, 138, 190])   # the lit face of the silk
BACK = srgb_to_linear([226, 62, 136])     # the other face, seen after the twist
SHEEN = srgb_to_linear([255, 246, 250])   # satin highlight
RIM = srgb_to_linear([168, 28, 92])       # edge line that keeps small sizes legible


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def knobs_for(size):
    """Small sizes trade realism for legibility: a wider band, a heavier rim, less sheen."""
    if size <= 20:
        return dict(half_width=0.155, rim_px=0.85, sheen=0.7, margin_px=0.5)
    if size <= 32:
        return dict(half_width=0.145, rim_px=0.9, sheen=0.85, margin_px=0.75)
    if size <= 64:
        return dict(half_width=0.135, rim_px=1.0, sheen=1.0, margin_px=1.5)
    return dict(half_width=0.13, rim_px=2.0, sheen=1.0, margin_px=6.0)


def surface(u, v, k):
    """Position (x, y, y down; z toward the viewer), normal and face of the ribbon at (u, v)."""
    # Centreline: an S, top to bottom. The band's cross direction starts as the in-plane
    # normal and twists once about the tangent, so the ribbon turns edge-on in the lower bend
    # and shows its other face in the tail.
    # The y term speeds the curve up through its bends: a plain sine turns tighter than the
    # band is wide, and the inner edge would fold into a hard crease.
    def centre(t):
        y = 0.12 + 0.76 * (t - 0.03 * np.sin(4.0 * np.pi * t))
        return 0.5 - 0.30 * np.sin(2.0 * np.pi * t), y, 0.0 * t

    eps = 1e-4
    x, y, z = centre(u)
    xa, ya, za = centre(np.minimum(u + eps, 1.0))
    xb, yb, zb = centre(np.maximum(u - eps, 0.0))
    dx, dy, dz = xa - xb, ya - yb, za - zb
    tl = np.sqrt(dx * dx + dy * dy + dz * dz) + 1e-12
    tx, ty, tz = dx / tl, dy / tl, dz / tl
    nl = np.sqrt(tx * tx + ty * ty) + 1e-12
    nx, ny = -ty / nl, tx / nl
    theta = -0.9 + 3.6 * u
    c, s = np.cos(theta), np.sin(theta)
    bx, by, bz = c * nx, c * ny, s
    # Slightly narrower at the ends, like a ribbon cut on the bias.
    w = k["half_width"] * (0.55 + 0.45 * np.sin(np.pi * u) ** 0.5)
    px, py, pz = x + v * w * bx, y + v * w * by, z + v * w * bz
    # Normal = tangent x cross direction; a slight curvature across the band softens it.
    cx = ty * bz - tz * by
    cy = tz * bx - tx * bz
    cz = tx * by - ty * bx
    soft = 0.45 * v
    cx, cy, cz = cx + soft * bx, cy + soft * by, cz + soft * bz
    cl = np.sqrt(cx * cx + cy * cy + cz * cz) + 1e-12
    # With y pointing down the cross product faces away from the viewer on the front face.
    cx, cy, cz = -cx / cl, -cy / cl, -cz / cl
    front = cz >= 0.0
    sgn = np.where(front, 1.0, -1.0)
    return px, py, pz, cx * sgn, cy * sgn, cz * sgn, front, w


def render(size, grey=False):
    k = knobs_for(size)
    ss = 16 if size <= 64 else 4
    S = size * ss
    nu = int(S * 3.0)
    nv = int(S * 0.5) + 8
    u, v = np.meshgrid(np.linspace(0.0, 1.0, nu), np.linspace(-1.0, 1.0, nv), indexing="xy")
    u, v = u.ravel(), v.ravel()
    px, py, pz, nx, ny, nz, front, w = surface(u, v, k)

    # Fit the silhouette into the canvas, centred, keeping its proportions.
    margin = k["margin_px"] / size
    x0, x1, y0, y1 = px.min(), px.max(), py.min(), py.max()
    scale = (1.0 - 2.0 * margin) / max(x1 - x0, y1 - y0)
    px = 0.5 + (px - 0.5 * (x0 + x1)) * scale
    py = 0.5 + (py - 0.5 * (y0 + y1)) * scale

    light = np.array([-0.35, -0.55, 0.76])
    light /= np.linalg.norm(light)
    half = light + np.array([0.0, 0.0, 1.0])
    half /= np.linalg.norm(half)
    ndl = np.clip(nx * light[0] + ny * light[1] + nz * light[2], 0.0, 1.0)
    ndh = np.clip(nx * half[0] + ny * half[1] + nz * half[2], 0.0, 1.0)
    base = np.where(front[:, None], FRONT[None, :], BACK[None, :])
    col = base * (0.50 + 0.60 * ndl)[:, None]
    col += SHEEN[None, :] * (k["sheen"] * (0.75 * ndh ** 40 + 0.10 * ndh ** 6))[:, None]

    # Rim: a fixed width in OUTPUT pixels along both long edges.
    across_px = np.maximum(w * scale * size, 1e-6)
    rim = np.clip(1.0 - (1.0 - np.abs(v)) * across_px / k["rim_px"], 0.0, 1.0)
    col = col * (1.0 - 0.8 * rim)[:, None] + RIM[None, :] * (0.8 * rim)[:, None]

    if grey:
        lum = 0.2126 * col[:, 0] + 0.7152 * col[:, 1] + 0.0722 * col[:, 2]
        col = np.stack([lum * 0.96, lum * 0.98, lum * 1.03], axis=1)

    ix = np.floor(px * S).astype(np.int64)
    iy = np.floor(py * S).astype(np.int64)
    ok = (ix >= 0) & (ix < S) & (iy >= 0) & (iy < S)
    ix, iy, pz, col = ix[ok], iy[ok], pz[ok], col[ok]
    pix = iy * S + ix
    # Z-buffer: per pixel keep the sample nearest the viewer (largest z).
    order = np.lexsort((-pz, pix))
    pix_sorted = pix[order]
    first = np.ones(len(order), dtype=bool)
    first[1:] = pix_sorted[1:] != pix_sorted[:-1]
    keep = order[first]

    rgb = np.zeros((S * S, 3))
    alpha = np.zeros(S * S)
    rgb[pix[keep]] = col[keep]
    alpha[pix[keep]] = 1.0
    rgb = rgb.reshape(S, S, 3) * alpha.reshape(S, S, 1)
    alpha = alpha.reshape(S, S, 1)
    rgb = rgb.reshape(size, ss, size, ss, 3).mean(axis=(1, 3))
    alpha = alpha.reshape(size, ss, size, ss, 1).mean(axis=(1, 3))
    out_rgb = np.where(alpha > 1e-6, rgb / np.maximum(alpha, 1e-6), 0.0)
    out = np.concatenate([linear_to_srgb(out_rgb), alpha], axis=-1)
    return Image.fromarray(np.round(out * 255.0).astype(np.uint8), "RGBA")


def dib_entry(im):
    """32-bit BGRA DIB + AND mask, the classic icon image layout."""
    w, h = im.size
    px = np.asarray(im, dtype=np.uint8)
    bgra = px[..., [2, 1, 0, 3]][::-1]  # bottom-up
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    mask_row = ((w + 31) // 32) * 4
    mask = bytearray()
    for row in px[::-1]:
        bits = bytearray(mask_row)
        for x in range(w):
            if row[x, 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + bgra.tobytes() + bytes(mask)


def png_entry(im):
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(path, images):
    entries = [(im.size[0], png_entry(im) if im.size[0] >= 256 else dib_entry(im)) for im in images]
    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    table = bytearray()
    blob = bytearray()
    for size, data in entries:
        dim = 0 if size >= 256 else size
        table += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(blob))
        blob += data
    with open(path, "wb") as f:
        f.write(header + bytes(table) + bytes(blob))


def preview(on, off, path):
    """Every size 1:1 on a light and a dark taskbar colour, plus the tray sizes at 8x."""
    pad = 12
    bgs = [(243, 243, 243), (32, 32, 32)]
    zoom_sizes = [16, 20, 24, 32]
    zoom = 8
    W = max(pad + sum(s + pad for s in SIZES), pad + sum(s * zoom + pad for s in zoom_sizes))
    row_h = 256 + 2 * pad
    zoom_h = 32 * zoom + 2 * pad
    sheet = Image.new("RGB", (W, row_h * 4 + zoom_h * 2), (255, 255, 255))
    y = 0
    for icons in (on, off):
        for bg in bgs:
            band = Image.new("RGB", (W, row_h), bg)
            x = pad
            for s in SIZES:
                band.paste(icons[s], (x, pad + (256 - s) // 2), icons[s])
                x += s + pad
            sheet.paste(band, (0, y))
            y += row_h
    for bg in bgs:
        band = Image.new("RGB", (W, zoom_h), bg)
        x = pad
        for s in zoom_sizes:
            im = on[s].resize((s * zoom, s * zoom), Image.NEAREST)
            band.paste(im, (x, pad), im)
            x += s * zoom + pad
        sheet.paste(band, (0, y))
        y += zoom_h
    sheet.save(path)


def main():
    on = {s: render(s) for s in SIZES}
    off = {s: render(s, grey=True) for s in SIZES}
    write_ico(os.path.join(HERE, "silk.ico"), [on[s] for s in SIZES])
    write_ico(os.path.join(HERE, "silk_off.ico"), [off[s] for s in SIZES])
    preview(on, off, os.path.join(HERE, "icon_preview.png"))
    print("wrote silk.ico, silk_off.ico, icon_preview.png")


if __name__ == "__main__":
    main()
