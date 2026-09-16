#!/usr/bin/env python3
"""G0 for the live comparison: does the screen show the source PNGs bit for bit?

Opens player.html in a throwaway Chrome app window on the secondary monitor,
makes it fullscreen, records the centred picture with ddacap in hold mode and in
playback, and requires every recorded picture to be byte-identical to one of the
source frames. Without this, no recorded engine output can be scored against the
analytic truth. Moves the cursor, clicks the page and presses F11.
"""
import ctypes, ctypes.wintypes as wt, json, os, subprocess, sys, time, urllib.parse
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "prototype")))
import live_windows_test as t  # noqa: E402
u = t.user32
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
TITLE = "Nova SilkPlay frame player"
PLACE = json.load(open(os.path.join(HERE, "..", "..", "prototype", "testpage_placement.json")))


def find(title):
    out = []
    cb = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(lambda h, _: (out.append(h) if u.IsWindowVisible(h) and title in (lambda b: (u.GetWindowTextW(h, b, 256), b.value)[1])(ctypes.create_unicode_buffer(256)) else None) or True)
    u.EnumWindows(cb, 0)
    return out[0] if out else None


def open_player(scene_dir, extra=""):
    clip = json.load(open(os.path.join(scene_dir, "clip.json")))
    w, h = clip["size"]
    url = ("file:///" + urllib.parse.quote(os.path.join(HERE, "player.html").replace("\\", "/"))
           + f"?dir={urllib.parse.quote(os.path.abspath(scene_dir).replace(chr(92), '/'))}&n={clip['frames']}&w={w}&h={h}&fps={clip['fps']}{extra}")
    subprocess.Popen([CHROME, f"--user-data-dir={os.path.join(os.environ['TEMP'], 'nsp-proto-profile')}",
                      "--no-first-run", "--no-default-browser-check",
                      "--disable-features=CalculateNativeWinOcclusion", "--force-color-profile=srgb",
                      f"--app={url}"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(80):
        time.sleep(0.25)
        hw = find(TITLE)
        if hw:
            break
    time.sleep(0.6)
    u.SetWindowPos(hw, None, PLACE["x"], PLACE["y"], PLACE["w"], PLACE["h"], 0x0004 | 0x0010)
    return hw, clip


def fullscreen(hw, saved):
    L, T, R, B = t.client_rect(hw)
    u.SetCursorPos((L + R) // 2, (T + B) // 2)
    t.send_mouse(2); t.send_mouse(4); t.pump(300)
    t.send_keys([0x7A]); t.pump(2500)
    u.SetCursorPos(saved.x, saved.y)
    return t.client_rect(hw)


def record(rect, w, h, seconds, prefix):
    L, T, R, B = rect
    ox, oy = (R - L - w) // 2, (B - T - h) // 2
    r = subprocess.run([os.path.join(HERE, "ddacap.exe"), "--at", f"{L + 5},{T + 5}", "--rect",
                        f"{ox},{oy},{w},{h}", "--seconds", str(seconds), "--out", prefix],
                       capture_output=True, text=True)
    print("  ddacap:", r.stdout.strip(), r.stderr.strip())
    raw = np.fromfile(prefix + ".raw", dtype=np.uint8)
    return raw.reshape(-1, h, w, 4)[:, :, :, 2::-1]


def main():
    scene_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "frames", "A3")
    saved = wt.POINT(); u.GetCursorPos(ctypes.byref(saved))
    hw, clip = open_player(scene_dir)
    w, h = clip["size"]
    time.sleep(3.0 + clip["frames"] * 0.02)  # bitmaps decode
    src = [np.asarray(Image.open(os.path.join(scene_dir, f)).convert("RGB")) for f in clip["files"]]
    keys = {s.tobytes(): i for i, s in enumerate(src)}
    try:
        rect = fullscreen(hw, saved)
        if (rect[2] - rect[0], rect[3] - rect[1]) != (1920, 1080):
            print("not fullscreen:", rect); return 2
        time.sleep(4.0)  # Chrome's exit-fullscreen bubble fades
        frames = record(rect, w, h, 3, os.path.join(HERE, "g0_play"))
        exact = [keys.get(f.tobytes()) for f in frames]
        n_exact = sum(e is not None for e in exact)
        seq = [e for e in exact if e is not None]
        distinct = sum(1 for a, b in zip(seq, seq[1:]) if a != b)
        print(f"playback: {n_exact}/{len(frames)} recorded pictures are byte-identical to a source frame; "
              f"{distinct} source changes in 3 s")
        if n_exact < len(frames):
            bad = frames[[i for i, e in enumerate(exact) if e is None][0]].astype(int)
            best = min(range(len(src)), key=lambda i: np.abs(src[i].astype(int) - bad).mean())
            d = np.abs(src[best].astype(int) - bad)
            print(f"  first mismatch vs nearest source {best}: mean |d| {d.mean():.3f}, max {d.max()}, "
                  f"pixels differing {np.count_nonzero(d.max(axis=2))}")
        ok = n_exact == len(frames) and 20 <= distinct / 3.0 <= 28
        print("G0", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        rect = t.client_rect(hw)
        u.SetCursorPos((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2)
        t.send_mouse(2); t.send_mouse(4); t.pump(200)
        t.send_keys([0x7A]); t.pump(800)
        u.SetCursorPos(saved.x, saved.y)
        u.PostMessageW(hw, 0x0010, 0, 0)


if __name__ == "__main__":
    sys.exit(main())
