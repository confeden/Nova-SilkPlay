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
  python windowed_check.py --out <ABS dir> --local --monitor 2 --fps 24  # or 30 or 60
"""
import argparse
import base64
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
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenEventW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
        kernel32.OpenEventW.restype = wt.HANDLE
        kernel32.SetEvent.argtypes = [wt.HANDLE]
        kernel32.CloseHandle.argtypes = [wt.HANDLE]
        try:
            if self.p.poll() is None:
                h = kernel32.OpenEventW(0x0002, False, "Local\\NovaSilkPlay.Quit")
                if not h:
                    print(f"quit event unavailable: {ctypes.WinError(ctypes.get_last_error())}", file=sys.stderr)
                    self.p.terminate()
                else:
                    try:
                        if not kernel32.SetEvent(h):
                            print(f"could not signal quit: {ctypes.WinError(ctypes.get_last_error())}", file=sys.stderr)
                            self.p.terminate()
                    finally:
                        kernel32.CloseHandle(h)
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                print("engine quit timed out", file=sys.stderr)
                self.p.kill()
                self.p.wait()
        finally:
            self.f.close()


def find_window(title, pid=None):
    hits = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value == "Chrome_WidgetWin_1" and user32.IsWindowVisible(h):
            owner = wt.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(owner))
            if pid is not None and owner.value != pid:
                return True
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
    viewport = truth.get("viewport")
    if viewport:
        x, y = max(t[0], viewport[0]), max(t[1], viewport[1])
        right = min(t[0] + t[2], viewport[0] + viewport[2])
        bottom = min(t[1] + t[3], viewport[1] + viewport[3])
        if right <= x or bottom <= y:
            return None
        t = [x, y, right - x, bottom - y]
    return {"engine": rect, "truth": t,
            "dl": round(rect[0] - t[0], 2), "dt": round(rect[1] - t[1], 2),
            "dr": round(rect[0] + rect[2] - (t[0] + t[2]), 2), "db": round(rect[1] + rect[3] - (t[1] + t[3]), 2)}


def capture_source_failure(cdp, sid, out, res):
    res["source_failure_state"] = yc.js(cdp, sid, yc.STATE_JS)
    res["source_failure_player"] = yc.js(cdp, sid, """(() => {
        const v = document.querySelector('video'), p = document.querySelector('#movie_player');
        return {muted: v && v.muted, volume: v && v.volume, ready: v && v.readyState,
                error: v && v.error && v.error.message,
                playerState: p && p.getPlayerState ? p.getPlayerState() : null,
                message: document.querySelector('.ytp-error-content-wrap')?.innerText || ''};
    })()""")
    shot = cdp.send("Page.captureScreenshot", {"format": "png"}, session_id=sid)
    with open(os.path.join(out, "source-failure.png"), "wb") as f:
        f.write(base64.b64decode(shot["data"]))


def validate_result(res):
    """Require every exercised transition and pixel-aligned rect, not just a JSON dump."""
    failures = []
    fullscreen = res.get("scope") != "windowed"
    for key in ("found_s", "scroll_hide_s", "scroll_reshown_s", "theater_hide_s",
                "theater_reshown_s", "pause_hide_s", "play_reshown_s",
                "clipped_hide_s", "clipped_return_s") + (("fullscreen_shown_s", "unfullscreen_shown_s") if fullscreen else ()):
        if res.get(key) is None:
            failures.append(f"{key}: transition not observed")
    for key in ("default", "scrolled", "theater") + (("unfullscreen",) if fullscreen else ()):
        rect = res.get(key)
        if not rect or any(abs(rect[edge]) > 2 for edge in ("dl", "dt", "dr", "db")):
            failures.append(f"{key}: video rect missing or differs by more than 2 px")
    if fullscreen and (res.get("fullscreen_rect") != res.get("fullscreen_monitor") or not res.get("fullscreen_rect")):
        failures.append("fullscreen: video rect does not match the selected monitor")
    if "scroll_actual" in res and res["scroll_actual"] < .5:
        failures.append("scroll: source page did not move")
    if res.get("errors"):
        failures.append("engine errors found in log")
    if res.get("foreground_unchanged") is False:
        failures.append("raising the test browser changed the foreground window")
    if res.get("clipped_stayed_hidden") is not True:
        failures.append("clipped video: overlay was not kept hidden")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--url", help="YouTube watch URL (or --local)")
    ap.add_argument("--local", action="store_true",
                    help="tools/yt-check/windowed_page.html instead of YouTube: no network, a muted local "
                         "video (so the engine gets --scout-always), the same scroll/theater/pause steps")
    ap.add_argument("--fps", type=int, choices=(24, 30, 60),
                    help="local source rate; only valid with --local (default: 24)")
    ap.add_argument("--source", choices=("cfr", "canvas"), default="cfr",
                    help="local test source: fixed-timestamp WebM (default) or RAF-paced canvas; only with --local")
    ap.add_argument("--start", type=float, default=60.0)
    ap.add_argument("--monitor", type=int, default=2)
    ap.add_argument("--windowed-only", action="store_true", help="explicitly omit fullscreen coverage")
    ap.add_argument("--topmost", action="store_true",
                    help="keep only the owned test browser visible on a secondary monitor")
    ap.add_argument("--title", default="Spring", help="--target-title for the engine (keeps it off the owner's Chrome)")
    ap.add_argument("--dark", action="store_true")
    ap.add_argument("--load-timeout", type=float, default=240.0)
    ap.add_argument("--engine-args", default="")
    a = ap.parse_args()
    if a.fps is not None and not a.local:
        ap.error("--fps is only valid with --local")
    if a.source != "cfr" and not a.local:
        ap.error("--source is only valid with --local")
    if a.topmost and a.monitor == 1:
        ap.error("--topmost is restricted to secondary monitors")
    os.makedirs(a.out, exist_ok=True)
    profile = os.path.abspath(os.path.join(a.out, "chrome_profile"))
    monitors = ya.monitors()
    if a.monitor < 1 or a.monitor > len(monitors):
        ap.error(f"--monitor {a.monitor}: available monitors: {monitors}")
    X, Y, W, H = monitors[a.monitor - 1]
    res = {"monitor": [X, Y, W, H], "topmost": a.topmost,
           "scope": "windowed" if a.windowed_only else "windowed+fullscreen"}
    stale = os.path.join(profile, "DevToolsActivePort")
    if os.path.exists(stale):
        os.remove(stale)
    ya.kill_profile_chrome(profile, wait_s=0.0)
    if a.local:
        import pathlib
        page = pathlib.Path(HERE, "windowed_page.html").as_uri()
        fps = a.fps or 24
        if a.source == "cfr":
            fixture = os.path.join(HERE, "generated", f"cfr_{fps}.webm")
            if not os.path.isfile(fixture):
                raise SystemExit(f"Missing CFR fixture: {fixture}\nGenerate with: python make_cfr_fixtures.py --fps {fps}")
        params = [f"fps={fps}", f"source={a.source}"]
        if a.dark:
            params.extend(("dark=1", "glow=1"))
        url = page + "?" + "&".join(params)
        res["source_fps"] = fps
        res["source"] = a.source
        a.title = "NSP windowed page"
        a.engine_args = (a.engine_args + " --scout-always").strip()
    else:
        url = f"{a.url}&t={int(a.start)}s"
    sess = cc.launch_chrome(url, user_data_dir=profile, no_activate=True,
                            extra_args=["--mute-audio", f"--window-position={X + 80},{Y + 60}",
                                        f"--window-size={min(1600, W - 160)},{min(900, H - 120)}"])
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
            res["consent_rejected"] = bool(yc.js(cdp, sid, yc.CONSENT_JS, gesture=True))
            if res["consent_rejected"]:
                time.sleep(2.0)
                if not yc.wait_for(lambda: yc.js(cdp, sid, ready), a.load_timeout):
                    raise RuntimeError("video not ready after cookie consent")
            if not yc.pass_ads(cdp, sid, 120):
                raise RuntimeError("ads did not finish")
            yc.js(cdp, sid, "document.querySelector('video').play().then(() => true, e => String(e))",
                  await_promise=True, gesture=True)
            if not yc.wait_for(lambda: ya.playable_state(yc.js(cdp, sid, yc.STATE_JS)), 20):
                capture_source_failure(cdp, sid, a.out, res)
                raise RuntimeError(f"online video did not start playing: {res['source_failure_state']}")
        browser_pid = next(p["id"] for p in cdp.send("SystemInfo.getProcessInfo")["processInfo"]
                           if p["type"] == "browser")
        yw = yc.wait_for(lambda: find_window(a.title, browser_pid), 10)
        if not yw:
            raise RuntimeError("owned test browser window did not appear")
        res["browser_pid"] = browser_pid
        res["browser_hwnd"] = yw
        foreground_before = user32.GetForegroundWindow()
        if not user32.SetWindowPos(yw, wt.HWND(-1) if a.topmost else None, X + 80, Y + 60,
                                  min(1600, W - 160), min(900, H - 120), 0x0010):
            raise ctypes.WinError(ctypes.get_last_error())
        res["foreground_unchanged"] = user32.GetForegroundWindow() == foreground_before
        bounds = wt.RECT()
        if not user32.GetWindowRect(yw, ctypes.byref(bounds)):
            raise ctypes.WinError(ctypes.get_last_error())
        if bounds.left < X or bounds.top < Y or bounds.right > X + W or bounds.bottom > Y + H:
            raise RuntimeError("test browser is outside the selected monitor")
        time.sleep(2.0)
        yc.js(cdp, sid, "(() => { if (document.activeElement) document.activeElement.blur(); return true; })()")
        playing = "(() => { const v = document.querySelector('video'); return v && !v.paused && v.currentTime > 0; })()"
        if not yc.wait_for(lambda: yc.js(cdp, sid, playing), 60):
            yc.js(cdp, sid, "document.querySelector('video').play(); true", gesture=True)

        res["initial_geometry"] = wp.truth(cdp, sid, yw)
        if not a.local:
            before = yc.js(cdp, sid, yc.STATE_JS)
            res["source_progress_before"] = before
            def advanced():
                after = yc.js(cdp, sid, yc.STATE_JS)
                if not ya.playable_state(before) or not ya.playable_state(after):
                    return None
                return after if after["t"] - before["t"] >= .5 and (after.get("total") or 0) > (before.get("total") or 0) else None
            res["source_progress_after"] = yc.wait_for(advanced, 30, .2)
            if res["source_progress_after"] is None:
                capture_source_failure(cdp, sid, a.out, res)
                raise RuntimeError("source video clock/decoding did not advance; engine arm invalid")

        eng = Engine(os.path.join(a.out, "engine.log"),
                     ["--default-settings", "--target-title", a.title, "--target-pid", str(browser_pid),
                      "--stats-every", "2"] + a.engine_args.split())
        t_found = eng.wait("overlay shown", 30)
        res["found_s"] = t_found
        if t_found is None:
            capture_source_failure(cdp, sid, a.out, res)
            raise RuntimeError("engine never showed the initial overlay; inspect engine.log before testing transitions")
        res["default"] = compare(eng.engaged_rect(), wp.truth(cdp, sid, yw))
        time.sleep(4.0)
        if a.local:
            res["source_timing_initial"] = yc.js(cdp, sid, "window.__nspSourceStats ? window.__nspSourceStats() : null")

        # Keep both video edges visible; G105 deliberately refuses clipped video.
        # No OS input event: exercise the capture-side guard alone. YouTube's
        # masthead is only ~12 CSS px above the initial picture.
        eng.mark()
        before_scroll = yc.js(cdp, sid, "window.scrollY")
        scroll_css = 48 if a.local else 8
        yc.js(cdp, sid, f"window.scrollTo(0, {scroll_css}); true")
        res["scroll_hide_s"] = eng.wait("overlay hidden", 5)
        res["scroll_reshown_s"] = eng.wait("overlay shown", 20)
        time.sleep(1.0)
        res["scroll_actual"] = yc.js(cdp, sid, "window.scrollY") - before_scroll
        res["scrolled"] = compare(eng.engaged_rect(), wp.truth(cdp, sid, yw))
        res["scroll_log"] = [ln for ln in eng.since().splitlines() if "windowed" in ln or "overlay" in ln][:12]
        yc.js(cdp, sid, "window.scrollTo(0, 0); true")
        time.sleep(4.0)

        # A vertically clipped video must hide, stay refused, then return intact.
        eng.mark()
        clipped_css = 240 if a.local else 48
        res["clipped_scroll_css"] = clipped_css
        yc.js(cdp, sid, f"window.scrollTo(0, {clipped_css}); true")
        res["clipped_hide_s"] = eng.wait("overlay hidden", 5)
        eng.mark()
        res["clipped_stayed_hidden"] = eng.wait("overlay shown", 3) is None
        eng.mark()
        yc.js(cdp, sid, "window.scrollTo(0, 0); true")
        res["clipped_return_s"] = eng.wait("overlay shown", 20)
        time.sleep(1.0)

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
        if not a.windowed_only:
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
            res["engine_exit_code"] = eng.p.returncode
            res["errors"] = [ln for ln in eng.text().splitlines() if "ERROR" in ln or "FAIL" in ln][:10]
        res["failures"] = validate_result(res)
        if res.get("engine_exit_code") != 0:
            res["failures"].append("engine did not exit cleanly")
        res["ok"] = not res["failures"]
        with open(os.path.join(a.out, "windowed_check.json"), "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1)
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        ya.kill_profile_chrome(profile)
    print(json.dumps(res, indent=1))
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
