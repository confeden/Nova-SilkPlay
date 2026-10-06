"""windowed_probe.py - P11: where is the video in a WINDOWED Chrome, seen from outside?

A throwaway, muted Chrome plays a YouTube video in an ordinary window on monitor N
(never fullscreen, never foreground). Three readings of the video rect are compared:

  truth  the page itself over CDP: getBoundingClientRect x devicePixelRatio, offset by
         the render widget's screen origin (physical px) - the harness may look inside
         the page, the product may not (D30)
  uia    tools/window-probe/uia_video.exe: the <video> element's UIA bounding box
  dirty  silkplay.exe --allow-windowed --manual --log-dirty: WGC dirty regions while the
         overlay stays hidden

through three layouts: default, scrolled by 300 CSS px, theater mode. It also measures
Chrome's CPU over 10 s before and after the first UIA read (UIA switches Chrome's
accessibility on, which is the cost the product would impose).

  python windowed_probe.py --out <ABS dir> --url <yt> --start 120 --monitor 2
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time

import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import yt_check as yc  # noqa: E402  (per-monitor DPI aware on import)
import yt_arms as ya  # noqa: E402

cc = yc.cc
user32 = yc.user32
UIA_EXE = os.path.join(yc.ROOT, "tools", "window-probe", "uia_video.exe")

TRUTH_JS = """(() => { const v = document.querySelector('video'); if (!v) return null;
  const r = v.getBoundingClientRect();
  return {x: r.left, y: r.top, w: r.width, h: r.height, dpr: window.devicePixelRatio,
          sy: window.scrollY, theater: !!document.querySelector('ytd-watch-flexy[theater]'),
          vw: v.videoWidth, vh: v.videoHeight}; })()"""


def render_widget_origin(top):
    """Screen origin (physical px) of Chrome's page viewport: its render widget child."""
    hits = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value == "Chrome_RenderWidgetHostHWND" and user32.IsWindowVisible(h):
            r = wt.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            hits.append((r.right - r.left) * (r.bottom - r.top) and (r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True

    user32.EnumChildWindows(top, ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    hits = [h for h in hits if h]
    return max(hits, key=lambda r: r[2] * r[3]) if hits else None


def truth(cdp, sid, hwnd):
    t = yc.js(cdp, sid, TRUTH_JS)
    o = render_widget_origin(hwnd)
    if not t or not o:
        return {"err": "no video or no render widget", "page": t, "origin": o}
    d = t["dpr"]
    x0, y0 = o[0] + t["x"] * d, o[1] + t["y"] * d
    x1, y1 = o[0] + (t["x"] + t["w"]) * d, o[1] + (t["y"] + t["h"]) * d
    t["screen"] = [round(x0, 2), round(y0, 2), round(x1 - x0, 2), round(y1 - y0, 2)]
    t["viewport"] = list(o)
    return t


def uia(*args, timeout=60):
    p = subprocess.run([UIA_EXE, "--title", "YouTube", *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    return p.stdout


def chrome_cpu(profile, seconds):
    def procs():
        out = []
        for p in psutil.process_iter(["name", "cmdline"]):
            try:
                if p.info["name"] == "chrome.exe" and any(profile in c for c in (p.info["cmdline"] or [])):
                    out.append(p)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return out

    def total(ps):
        s = 0.0
        for p in ps:
            try:
                c = p.cpu_times()
                s += c.user + c.system
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return s

    ps = procs()
    a = total(ps)
    time.sleep(seconds)
    return (total(ps) - a) / seconds


def dirty(hwnd_title, seconds, logpath):
    with open(logpath, "w", encoding="utf-8") as f:
        p = subprocess.Popen([yc.ENGINE, "--default-settings", "--target-title", hwnd_title,
                              "--allow-windowed", "--manual", "--log-dirty", "--no-badge",
                              "--stats-every", "1"], stdout=f, stderr=subprocess.STDOUT)
        time.sleep(seconds)
        yc.hotkey_quit_engine()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
    lines = open(logpath, encoding="utf-8", errors="replace").read().splitlines()
    frames = [ln for ln in lines if "[frame]" in ln]
    rects = {}
    for ln in frames:
        i = ln.find("[", ln.find("->"))
        key = ln[i:].strip() if i > 0 else "?"
        rects[key] = rects.get(key, 0) + 1
    top = sorted(rects.items(), key=lambda kv: -kv[1])[:6]
    vr = [ln for ln in lines if "video rect" in ln or "client " in ln][:3]
    return {"frames": len(frames), "top_rects": top, "geometry": vr}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--start", type=float, default=60.0)
    ap.add_argument("--monitor", type=int, default=2)
    ap.add_argument("--proxy")
    ap.add_argument("--load-timeout", type=float, default=240.0)
    ap.add_argument("--no-dirty", action="store_true")
    ap.add_argument("--no-uia", action="store_true")
    ap.add_argument("--dark", action="store_true", help="prefers-color-scheme: dark (YouTube's ambient mode)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    a.profile = os.path.abspath(os.path.join(a.out, "chrome_profile"))
    X, Y, W, H = ya.monitors()[a.monitor - 1]
    res = {"monitor": [X, Y, W, H]}

    stale = os.path.join(a.profile, "DevToolsActivePort")
    if os.path.exists(stale):
        os.remove(stale)
    ya.kill_profile_chrome(a.profile, wait_s=0.0)
    extra = ["--mute-audio", f"--window-position={X + 80},{Y + 60}", "--window-size=1600,900"]
    if a.proxy:
        extra.append(f"--proxy-server={a.proxy}")
    sess = cc.launch_chrome(f"{a.url}&t={int(a.start)}s", user_data_dir=a.profile, extra_args=extra)
    cdp = cc.CDP(sess.ws_url, timeout=60)
    try:
        tgt = yc.wait_for(lambda: next((t for t in cc.list_page_targets(cdp)
                                        if "youtube.com/watch" in t.get("url", "")), None), 60)
        sid = cc.attach_page(cdp, tgt["targetId"])
        if not yc.wait_for(lambda: yc.js(cdp, sid, "!!document.querySelector('video') && document.querySelector('video').readyState >= 2"), a.load_timeout):
            raise RuntimeError("video element never became ready")
        yc.pass_ads(cdp, sid, 120)
        yw = yc.wait_for(ya.find_youtube_window, 10)
        user32.SetWindowPos(yw, None, X + 80, Y + 60, min(1600, W - 160), min(900, H - 120), 0x0010 | 0x0004)
        time.sleep(3.0)
        yc.js(cdp, sid, "(() => { if (document.activeElement) document.activeElement.blur(); return true; })()")
        if a.dark:
            # YouTube keeps its theme in the PREF cookie (f6=400 = dark); ambient mode exists
            # only in the dark theme and is on by default there.
            cdp.send("Network.setCookie", {"name": "PREF", "value": "f6=400&tz=UTC", "domain": ".youtube.com",
                                           "path": "/", "secure": True}, session_id=sid)
            yc.js(cdp, sid, "location.reload(); true")
            time.sleep(3.0)
            if not yc.wait_for(lambda: yc.js(cdp, sid, "!!document.querySelector('video') && document.querySelector('video').readyState >= 2 && !document.querySelector('video').paused"), a.load_timeout):
                raise RuntimeError("video did not resume after the theme reload")
            yc.pass_ads(cdp, sid, 120)
            time.sleep(3.0)
            res["dark"] = yc.js(cdp, sid, "[document.documentElement.hasAttribute('dark'), !!document.querySelector('#cinematics canvas, #cinematics-container canvas')]")

        res["cpu_before_uia"] = round(chrome_cpu(a.profile, 10.0), 3)
        res["default"] = {"truth": truth(cdp, sid, yw)}
        if not a.no_uia:
            res["uia_dump"] = uia("--dump", timeout=120)
            res["default"]["uia"] = uia("--poll", "8", "--every-ms", "250")
            res["cpu_after_uia"] = round(chrome_cpu(a.profile, 10.0), 3)
        if not a.no_dirty:
            res["default"]["dirty"] = dirty("YouTube", 8.0, os.path.join(a.out, "dirty_default.log"))

        yc.js(cdp, sid, "window.scrollBy(0, 300); true")
        time.sleep(1.5)
        res["scrolled"] = {"truth": truth(cdp, sid, yw)}
        if not a.no_dirty:
            res["scrolled"]["dirty"] = dirty("YouTube", 8.0, os.path.join(a.out, "dirty_scrolled.log"))
        yc.js(cdp, sid, "window.scrollTo(0, 0); true")
        time.sleep(1.0)

        yc.js(cdp, sid, "(() => { const b = document.querySelector('.ytp-size-button'); if (b) b.click(); return !!b; })()")
        time.sleep(3.0)
        res["theater"] = {"truth": truth(cdp, sid, yw), "state": yc.js(cdp, sid, yc.STATE_JS)}
        if not a.no_dirty:
            res["theater"]["dirty"] = dirty("YouTube", 8.0, os.path.join(a.out, "dirty_theater.log"))
    except Exception:
        ya.screenshot_on_failure(cdp, os.path.join(a.out, "failure.png"))
        res["page_state"] = yc.js(cdp, sid, yc.STATE_JS) if 'sid' in locals() else None
        raise
    finally:
        with open(os.path.join(a.out, "windowed_probe.json"), "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1)
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        ya.kill_profile_chrome(a.profile)
    print(json.dumps({k: v for k, v in res.items() if k != "uia_dump"}, indent=1))


if __name__ == "__main__":
    main()
