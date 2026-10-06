"""windowed_check.py - P11 end to end: silkplay.exe on a video in a WINDOWED Chrome.

A throwaway, muted Chrome plays YouTube in an ordinary window on monitor N (never
fullscreen, never foreground, the pointer never moved). silkplay.exe runs with its defaults
(no --allow-windowed, no --rect): it must find the video itself. The page's own rect, read
over CDP, is the truth the engine's rect is compared with, through:

  found     time from start to the first "overlay shown", and the engaged rect vs truth
  scroll    CDP wheel (no OS input, so this is the capture-side guard alone): the overlay
            must hide, and come back on the video's NEW rect
  theater   the player's size button: same
  pause     the overlay hides (stall), and comes back on play

  python windowed_check.py --out <ABS dir> --url <yt> --start 125 --monitor 2 [--dark]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import yt_check as yc  # noqa: E402
import yt_arms as ya  # noqa: E402
import windowed_probe as wp  # noqa: E402

cc = yc.cc
user32 = yc.user32
LINE = re.compile(r"^\[\s*([\d.]+)\]\s*(.*)$")


class Engine:
    def __init__(self, path, args):
        self.path = path
        self.f = open(path, "w", encoding="utf-8")
        self.p = subprocess.Popen([yc.ENGINE, *args], stdout=self.f, stderr=subprocess.STDOUT)
        self.mark_at = 0

    def text(self):
        return open(self.path, encoding="utf-8", errors="replace").read()

    def mark(self):
        self.mark_at = len(self.text())

    def since(self):
        return self.text()[self.mark_at:]

    def wait(self, needle, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.since():
                return round(time.time() - t0, 2)
            time.sleep(0.1)
        return None

    def engaged_rect(self):
        """The last 'video rect: client .. -> screen X,Y WxH' the engine logged."""
        m = re.findall(r"video rect: client \S+ \S+ -> screen (-?\d+),(-?\d+) (\d+)x(\d+)", self.text())
        return [int(v) for v in m[-1]] if m else None

    def quit(self):
        yc.hotkey_quit_engine()
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.p.kill()
        self.f.close()


def find_window(title):
    hits = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value == "Chrome_WidgetWin_1" and user32.IsWindowVisible(h):
            buf = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(h, buf, 512)
            if title in buf.value:
                hits.append(h)
        return True

    user32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return hits[0] if hits else None


def compare(rect, truth):
    if not rect or not truth or "screen" not in truth:
        return None
    t = truth["screen"]
    return {"engine": rect, "truth": t,
            "dl": round(rect[0] - t[0], 2), "dt": round(rect[1] - t[1], 2),
            "dr": round(rect[0] + rect[2] - (t[0] + t[2]), 2), "db": round(rect[1] + rect[3] - (t[1] + t[3]), 2)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--url", help="YouTube watch URL (or --local)")
    ap.add_argument("--local", action="store_true",
                    help="tools/yt-check/windowed_page.html instead of YouTube: no network, a muted canvas "
                         "video (so the engine gets --scout-always), the same scroll/theater/pause steps")
    ap.add_argument("--start", type=float, default=60.0)
    ap.add_argument("--monitor", type=int, default=2)
    ap.add_argument("--title", default="Spring", help="--target-title for the engine (keeps it off the owner's Chrome)")
    ap.add_argument("--dark", action="store_true")
    ap.add_argument("--load-timeout", type=float, default=240.0)
    ap.add_argument("--engine-args", default="")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    profile = os.path.abspath(os.path.join(a.out, "chrome_profile"))
    X, Y, W, H = ya.monitors()[a.monitor - 1]
    res = {"monitor": [X, Y, W, H]}
    stale = os.path.join(profile, "DevToolsActivePort")
    if os.path.exists(stale):
        os.remove(stale)
    ya.kill_profile_chrome(profile, wait_s=0.0)
    if a.local:
        import pathlib
        page = pathlib.Path(HERE, "windowed_page.html").as_uri()
        url = page + ("?dark=1&glow=1" if a.dark else "")
        a.title = "NSP windowed page"
        a.engine_args = (a.engine_args + " --scout-always").strip()
    else:
        url = f"{a.url}&t={int(a.start)}s"
    sess = cc.launch_chrome(url, user_data_dir=profile,
                            extra_args=["--mute-audio", f"--window-position={X + 80},{Y + 60}", "--window-size=1600,900"])
    cdp = cc.CDP(sess.ws_url, timeout=60)
    eng = None
    sid = None
    try:
        tgt = yc.wait_for(lambda: next((t for t in cc.list_page_targets(cdp)
                                        if "youtube.com/watch" in t.get("url", "") or "windowed_page" in t.get("url", "")), None), 60)
        sid = cc.attach_page(cdp, tgt["targetId"])
        ready = "!!document.querySelector('video') && document.querySelector('video').readyState >= 2"
        if not yc.wait_for(lambda: yc.js(cdp, sid, ready), a.load_timeout):
            raise RuntimeError("video element never became ready")
        if a.dark and not a.local:
            cdp.send("Network.setCookie", {"name": "PREF", "value": "f6=400&tz=UTC", "domain": ".youtube.com",
                                           "path": "/", "secure": True}, session_id=sid)
            yc.js(cdp, sid, "location.reload(); true")
            time.sleep(3.0)
            if not yc.wait_for(lambda: yc.js(cdp, sid, ready), a.load_timeout):
                raise RuntimeError("video not ready after the theme reload")
        if not a.local:
            yc.pass_ads(cdp, sid, 120)
        yw = yc.wait_for(lambda: find_window(a.title), 10)
        user32.SetWindowPos(yw, None, X + 80, Y + 60, min(1600, W - 160), min(900, H - 120), 0x0010 | 0x0004)
        time.sleep(2.0)
        yc.js(cdp, sid, "(() => { if (document.activeElement) document.activeElement.blur(); return true; })()")
        playing = "(() => { const v = document.querySelector('video'); return v && !v.paused && v.currentTime > 0; })()"
        if not yc.wait_for(lambda: yc.js(cdp, sid, playing), 60):
            yc.js(cdp, sid, "document.querySelector('video').play(); true", gesture=True)

        eng = Engine(os.path.join(a.out, "engine.log"),
                     ["--default-settings", "--target-title", a.title, "--stats-every", "2"] + a.engine_args.split())
        t_found = eng.wait("overlay shown", 30)
        res["found_s"] = t_found
        res["default"] = compare(eng.engaged_rect(), wp.truth(cdp, sid, yw))
        time.sleep(4.0)

        # SCROLL by CDP wheel: no OS input event, the capture-side guard alone.
        eng.mark()
        cdp.send("Input.dispatchMouseEvent", {"type": "mouseWheel", "x": 600, "y": 400, "deltaX": 0, "deltaY": 240},
                 session_id=sid)
        res["scroll_hide_s"] = eng.wait("overlay hidden", 5)
        res["scroll_reshown_s"] = eng.wait("overlay shown", 20)
        time.sleep(1.0)
        res["scrolled"] = compare(eng.engaged_rect(), wp.truth(cdp, sid, yw))
        res["scroll_log"] = [ln for ln in eng.since().splitlines() if "windowed" in ln or "overlay" in ln][:12]
        yc.js(cdp, sid, "window.scrollTo(0, 0); true")
        time.sleep(4.0)

        # THEATER via the player's own button.
        eng.mark()
        yc.js(cdp, sid, "(() => { const b = document.querySelector('.ytp-size-button'); if (b) b.click(); return !!b; })()")
        res["theater_hide_s"] = eng.wait("overlay hidden", 5)
        res["theater_reshown_s"] = eng.wait("overlay shown", 20)
        time.sleep(1.0)
        res["theater"] = compare(eng.engaged_rect(), wp.truth(cdp, sid, yw))
        time.sleep(3.0)

        # PAUSE and PLAY.
        eng.mark()
        yc.js(cdp, sid, "document.querySelector('video').pause(); true")
        res["pause_hide_s"] = eng.wait("hidden", 5)
        time.sleep(3.0)
        eng.mark()
        yc.js(cdp, sid, "document.querySelector('video').play(); true", gesture=True)
        res["play_reshown_s"] = eng.wait("overlay shown", 20)
        res["play_state"] = yc.js(cdp, sid, yc.STATE_JS)
        time.sleep(3.0)
        # FULLSCREEN and back: the ordinary whole-client path, then the windowed one again.
        eng.mark()
        yc.js(cdp, sid, "document.querySelector('video').requestFullscreen().then(() => true, e => String(e))",
              await_promise=True, gesture=True)
        res["fullscreen_shown_s"] = eng.wait("overlay shown", 20)
        time.sleep(1.0)
        res["fullscreen_rect"] = eng.engaged_rect()
        res["fullscreen_monitor"] = [X, Y, W, H]
        time.sleep(3.0)
        eng.mark()
        yc.js(cdp, sid, "document.exitFullscreen().then(() => true, e => String(e))", await_promise=True, gesture=True)
        res["unfullscreen_shown_s"] = eng.wait("overlay shown", 20)
        time.sleep(1.0)
        res["unfullscreen"] = compare(eng.engaged_rect(), wp.truth(cdp, sid, yw))
        res["errors"] = [ln for ln in eng.text().splitlines() if "ERROR" in ln or "FAIL" in ln][:10]
    finally:
        if eng:
            eng.quit()
        with open(os.path.join(a.out, "windowed_check.json"), "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1)
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        ya.kill_profile_chrome(profile)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
