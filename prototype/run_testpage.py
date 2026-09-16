#!/usr/bin/env python3
"""Launch the deterministic test page in a throwaway Chrome, where it was last left.

Chrome does not restore an --app window's placement for a throwaway profile, and
--window-position/--window-size are in DIPs, so on a scaled monitor they do not
land where you asked. This moves the window with SetWindowPos afterwards, in
physical pixels, which is what everything else in this project speaks.

  python run_testpage.py                 # launch where it was last saved
  python run_testpage.py --save          # remember where the window is NOW
  python run_testpage.py --fps 30 --speed 600
  python run_testpage.py --url file:///... --no-move

The placement lives in testpage_placement.json next to this file, so moving the
window once and running --save is enough to make it stick.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
PLACEMENT = os.path.join(HERE, "testpage_placement.json")
TITLE = "Nova SilkPlay motion source"
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDpiAwarenessContext.restype = wt.BOOL
try:  # PER_MONITOR_AWARE_V2: every rect below is then physical pixels
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def find_window(title):
    found = []

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if title.lower() in buf.value.lower():
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found[0] if found else None


def window_rect(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return {"x": r.left, "y": r.top, "w": r.right - r.left, "h": r.bottom - r.top}


def load_placement():
    try:
        with open(PLACEMENT, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=24)
    ap.add_argument("--speed", type=int, default=900)
    ap.add_argument("--url", help="override the page URL entirely")
    ap.add_argument("--save", action="store_true",
                    help="do not launch: record where the window is now")
    ap.add_argument("--no-move", action="store_true", help="launch without repositioning")
    ap.add_argument("--profile", default=os.path.join(os.environ.get("TEMP", "."), "nsp-proto-profile"))
    args = ap.parse_args()

    if args.save:
        hwnd = find_window(TITLE)
        if not hwnd:
            print(f"no window titled '{TITLE}' is open", file=sys.stderr)
            return 1
        place = window_rect(hwnd)
        with open(PLACEMENT, "w", encoding="utf-8") as f:
            json.dump(place, f, indent=2)
        print(f"saved placement {place} -> {PLACEMENT}")
        return 0

    url = args.url or (
        "file:///" + urllib.parse.quote(os.path.join(HERE, "testmotion.html").replace("\\", "/"))
        + f"?fps={args.fps}&speed={args.speed}"
    )

    cmd = [
        CHROME,
        f"--user-data-dir={args.profile}",
        "--no-first-run",
        "--no-default-browser-check",
        # Without this Chrome throttles rAF whenever the window is covered, the
        # page stops compositing and the engine sees a stalled source. Fine for
        # a measurement rig; the product must handle occlusion for real (I9).
        "--disable-features=CalculateNativeWinOcclusion",
        f"--app={url}",
    ]
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    hwnd = None
    for _ in range(60):
        time.sleep(0.25)
        hwnd = find_window(TITLE)
        if hwnd:
            break
    if not hwnd:
        print("the window never appeared", file=sys.stderr)
        return 2

    place = None if args.no_move else load_placement()
    if place:
        time.sleep(0.6)  # let Chrome finish its own placement first
        SWP_NOZORDER, SWP_NOACTIVATE = 0x0004, 0x0010
        user32.SetWindowPos(hwnd, None, place["x"], place["y"], place["w"], place["h"],
                            SWP_NOZORDER | SWP_NOACTIVATE)
    print(f"window 0x{hwnd:X} at {window_rect(hwnd)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
