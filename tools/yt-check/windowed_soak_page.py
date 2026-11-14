"""windowed_soak_page.py - keeps windowed_page.html playing in a windowed Chrome on monitor N
for a soak, and scrolls it down and back every --scroll-every seconds, so the engine's
windowed path (scout -> engage -> guard -> give up -> scout) cycles all run long.

  python windowed_soak_page.py --out <ABS dir> --minutes 32 --monitor 2 --fps 24 [--glow]
  (CFR WebM by default; use --fps 30 or --fps 60 for other rates, --source canvas for the old source.)
  Add --topmost only with permission to cover windows on the selected secondary monitor.
then, beside it:
  python tools\\soak\\soak.py --minutes 30 --no-page --target-title "NSP windowed page" --windowed-auto --fps 24
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import pathlib
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import yt_check as yc  # noqa: E402
import yt_arms as ya  # noqa: E402
from windowed_check import find_window  # noqa: E402

cc = yc.cc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--minutes", type=float, default=32.0)
    ap.add_argument("--monitor", type=int, default=2)
    ap.add_argument("--fps", type=int, choices=(24, 30, 60), default=24,
                    help="local source rate")
    ap.add_argument("--source", choices=("cfr", "canvas"), default="cfr",
                    help="fixed-timestamp WebM (default) or RAF-paced canvas")
    ap.add_argument("--scroll-every", type=float, default=60.0)
    ap.add_argument("--glow", action="store_true")
    ap.add_argument("--stop-file", help="close the owned test browser when this file appears")
    ap.add_argument("--topmost", action="store_true",
                    help="keep the owned secondary test browser above other windows without activation")
    a = ap.parse_args()
    if a.topmost and a.monitor == 1:
        ap.error("--topmost is restricted to secondary monitors")
    monitors = ya.monitors()
    if a.monitor < 1 or a.monitor > len(monitors):
        ap.error(f"--monitor {a.monitor}: available monitors: {monitors}")
    os.makedirs(a.out, exist_ok=True)
    profile = os.path.abspath(os.path.join(a.out, "chrome_profile"))
    X, Y, W, H = monitors[a.monitor - 1]
    ya.kill_profile_chrome(profile, wait_s=0.0)
    stale = os.path.join(profile, "DevToolsActivePort")
    if os.path.exists(stale):
        os.remove(stale)
    if a.source == "cfr":
        fixture = os.path.join(HERE, "generated", f"cfr_{a.fps}.webm")
        if not os.path.isfile(fixture):
            raise SystemExit(f"Missing CFR fixture: {fixture}\nGenerate with: python make_cfr_fixtures.py --fps {a.fps}")
    params = [f"fps={a.fps}", f"source={a.source}"]
    if a.glow:
        params.extend(("dark=1", "glow=1"))
    url = pathlib.Path(HERE, "windowed_page.html").as_uri() + "?" + "&".join(params)
    sess = cc.launch_chrome(url, user_data_dir=profile, no_activate=True,
                            extra_args=["--mute-audio", f"--window-position={X + 80},{Y + 60}",
                                        f"--window-size={min(1600, W - 160)},{min(900, H - 120)}"])
    cdp = cc.CDP(sess.ws_url, timeout=60)
    scrolls = 0
    try:
        tgt = yc.wait_for(lambda: next((t for t in cc.list_page_targets(cdp) if "windowed_page" in t.get("url", "")), None), 30)
        sid = cc.attach_page(cdp, tgt["targetId"])
        browser_pid = next(p["id"] for p in cdp.send("SystemInfo.getProcessInfo")["processInfo"]
                           if p["type"] == "browser")
        hwnd = yc.wait_for(lambda: find_window("NSP windowed page", browser_pid), 10)
        if not hwnd:
            raise RuntimeError("test browser window did not appear")
        # Chrome's launch switches are DIPs; all test geometry is physical pixels.
        foreground_before = yc.user32.GetForegroundWindow()
        if not yc.user32.SetWindowPos(hwnd, wt.HWND(-1) if a.topmost else None, X + 80, Y + 60,
                                      min(1600, W - 160), min(900, H - 120), 0x0010):
            raise ctypes.WinError(ctypes.get_last_error())
        if yc.user32.GetForegroundWindow() != foreground_before:
            raise RuntimeError("raising the owned test browser changed the foreground window")
        bounds = wt.RECT()
        if not yc.user32.GetWindowRect(hwnd, ctypes.byref(bounds)):
            raise ctypes.WinError(ctypes.get_last_error())
        if bounds.left < X or bounds.top < Y or bounds.right > X + W or bounds.bottom > Y + H:
            raise RuntimeError("test browser is outside the selected monitor")
        with open(os.path.join(a.out, "page.json"), "w", encoding="utf-8") as f:
            json.dump({"pid": browser_pid, "hwnd": hwnd,
                       "rect": [bounds.left, bounds.top, bounds.right, bounds.bottom],
                       "topmost": a.topmost}, f)
        end = time.time() + a.minutes * 60.0
        next_scroll = time.time() + a.scroll_every
        while time.time() < end:
            if a.stop_file and os.path.exists(a.stop_file):
                break
            time.sleep(1.0)
            if time.time() >= next_scroll:
                next_scroll += a.scroll_every
                yc.js(cdp, sid, "window.scrollBy(0, 240); true")
                time.sleep(8.0)
                yc.js(cdp, sid, "window.scrollTo(0, 0); true")
                scrolls += 1
        print(f"done: {scrolls} scroll cycles at {a.fps} fps ({a.source})")
    finally:
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        ya.kill_profile_chrome(profile)


if __name__ == "__main__":
    main()
