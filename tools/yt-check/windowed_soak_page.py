"""windowed_soak_page.py - keeps windowed_page.html playing in a windowed Chrome on monitor N
for a soak, and scrolls it down and back every --scroll-every seconds, so the engine's
windowed path (scout -> engage -> guard -> give up -> scout) cycles all run long.

  python windowed_soak_page.py --out <ABS dir> --minutes 32 --monitor 2 [--glow]
then, beside it:
  python tools\\soak\\soak.py --minutes 30 --no-page --target-title "NSP windowed page" --windowed-auto
"""
import argparse
import os
import pathlib
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import yt_check as yc  # noqa: E402
import yt_arms as ya  # noqa: E402

cc = yc.cc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--minutes", type=float, default=32.0)
    ap.add_argument("--monitor", type=int, default=2)
    ap.add_argument("--scroll-every", type=float, default=60.0)
    ap.add_argument("--glow", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    profile = os.path.abspath(os.path.join(a.out, "chrome_profile"))
    X, Y, W, H = ya.monitors()[a.monitor - 1]
    ya.kill_profile_chrome(profile, wait_s=0.0)
    stale = os.path.join(profile, "DevToolsActivePort")
    if os.path.exists(stale):
        os.remove(stale)
    url = pathlib.Path(HERE, "windowed_page.html").as_uri() + ("?dark=1&glow=1" if a.glow else "")
    sess = cc.launch_chrome(url, user_data_dir=profile,
                            extra_args=["--mute-audio", f"--window-position={X + 80},{Y + 60}", "--window-size=1600,900"])
    cdp = cc.CDP(sess.ws_url, timeout=60)
    scrolls = 0
    try:
        tgt = yc.wait_for(lambda: next((t for t in cc.list_page_targets(cdp) if "windowed_page" in t.get("url", "")), None), 30)
        sid = cc.attach_page(cdp, tgt["targetId"])
        end = time.time() + a.minutes * 60.0
        next_scroll = time.time() + a.scroll_every
        while time.time() < end:
            time.sleep(1.0)
            if time.time() >= next_scroll:
                next_scroll += a.scroll_every
                yc.js(cdp, sid, "window.scrollBy(0, 240); true")
                time.sleep(8.0)
                yc.js(cdp, sid, "window.scrollTo(0, 0); true")
                scrolls += 1
        print(f"done: {scrolls} scroll cycles")
    finally:
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        ya.kill_profile_chrome(profile)


if __name__ == "__main__":
    main()
