#!/usr/bin/env python3
"""yt_check.py - stage 0 reality check: the engine on real YouTube, in YouTube's own
element fullscreen, on the PRIMARY monitor.

For each clip a throwaway, muted Chrome plays the video and the same playback is
measured twice, back to back:

  off  engine not running: the page's truth (requestVideoFrameCallback), Chrome's
       own dropped-frame counter, and a DDA recording of the screen centre
  on   silkplay.exe with its default arguments: the same three, plus its stats log

It answers: does the engine attach and stay shown on real YouTube; what source rate
it estimates against the page's true one; how many distinct pictures per second
reach the screen; and whether running it makes Chrome drop frames.
It takes over the primary monitor for about a minute per clip.

  python yt_check.py --out DIR --clip spring24=https://www.youtube.com/watch?v=WhWc3b3KhnY@120
"""
import argparse
import csv
import ctypes
import ctypes.wintypes as wt
import json
import os
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "overlay-probe"))
import chrome_cdp as cc  # noqa: E402

ENGINE = os.path.join(ROOT, "prototype", "silkplay.exe")
DDACAP = os.path.join(ROOT, "tools", "ls-compare", "ddacap.exe")

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDpiAwarenessContext.restype = wt.BOOL
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # physical pixels everywhere below

VK_CONTROL, VK_MENU, VK_X = 0x11, 0x12, 0x58
KEYEVENTF_KEYUP = 0x2

STATE_JS = """(() => {
  const v = document.querySelector('video'); const p = document.querySelector('#movie_player');
  const q = v ? v.getVideoPlaybackQuality() : null;
  return {t: v ? v.currentTime : null, paused: v ? v.paused : null,
          w: v ? v.videoWidth : null, h: v ? v.videoHeight : null,
          total: q ? q.totalVideoFrames : null, dropped: q ? q.droppedVideoFrames : null,
          ad: !!(p && p.classList.contains('ad-showing')), fs: !!document.fullscreenElement,
          title: document.title, host: location.hostname, now: performance.now()};
})()"""

PROBE_JS = """(() => {
  const v = document.querySelector('video');
  if (!v) return false;
  if (window.__nsp && window.__nsp.v === v) return true;
  window.__nsp = {v: v, log: []};
  const cb = (now, md) => {
    if (window.__nsp.v !== v) return;
    window.__nsp.log.push([now, md.mediaTime, md.presentedFrames, md.expectedDisplayTime, md.width, md.height]);
    v.requestVideoFrameCallback(cb);
  };
  v.requestVideoFrameCallback(cb);
  return true;
})()"""

SKIP_JS = """(() => {
  const b = document.querySelector('.ytp-skip-ad-button, .ytp-ad-skip-button, .ytp-ad-skip-button-modern');
  if (!b) return null;
  const r = b.getBoundingClientRect();
  return r.width > 0 ? [r.left + r.width / 2, r.top + r.height / 2] : null;
})()"""

CONSENT_JS = """(() => {
  const bs = [...document.querySelectorAll('button, input[type=submit]')];
  const b = bs.find(x => /reject all|отклонить все/i.test((x.innerText || x.value || '') + ' ' + (x.getAttribute('aria-label') || '')));
  if (b) { b.click(); return true; }
  return false;
})()"""


def log(msg):
    print(f"[yt_check {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def js(cdp, sid, expr, await_promise=False, gesture=False):
    res = cdp.send("Runtime.evaluate", {"expression": expr, "returnByValue": True,
                                        "awaitPromise": await_promise, "userGesture": gesture},
                   session_id=sid)
    if res.get("exceptionDetails"):
        raise cc.CDPError(f"JS exception: {res['exceptionDetails']}")
    return res.get("result", {}).get("value")


def key(cdp, sid, ch, code, vk):
    cdp.send("Input.dispatchKeyEvent", {"type": "keyDown", "key": ch, "code": code, "text": ch,
                                        "windowsVirtualKeyCode": vk, "nativeVirtualKeyCode": vk},
             session_id=sid)
    cdp.send("Input.dispatchKeyEvent", {"type": "keyUp", "key": ch, "code": code,
                                        "windowsVirtualKeyCode": vk, "nativeVirtualKeyCode": vk},
             session_id=sid)


def click(cdp, sid, x, y):
    cdp.send("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": x, "y": y}, session_id=sid)
    for typ in ("mousePressed", "mouseReleased"):
        cdp.send("Input.dispatchMouseEvent", {"type": typ, "x": x, "y": y, "button": "left",
                                              "clickCount": 1}, session_id=sid)


def hotkey_quit_engine():
    for vk in (VK_CONTROL, VK_MENU, VK_X):
        user32.keybd_event(vk, 0, 0, 0)
    for vk in (VK_X, VK_MENU, VK_CONTROL):
        user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)


def chrome_windows():
    # By class, not by the pid we launched: the chrome.exe that Popen starts is not
    # necessarily the process that owns the browser window.
    found = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value == "Chrome_WidgetWin_1" and user32.IsWindowVisible(h):
            p = wt.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(p))
            r = wt.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            n = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if buf.value:
                found.append((p.value, buf.value, (r.left, r.top, r.right, r.bottom)))
        return True

    user32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return found


def wait_for(pred, timeout, step=0.25):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return None


def pass_ads(cdp, sid, limit):
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit:
        st = js(cdp, sid, STATE_JS)
        if not st["ad"]:
            return True
        pos = js(cdp, sid, SKIP_JS)
        if pos:
            click(cdp, sid, pos[0], pos[1])
        time.sleep(1.0)
    return False


def summarize_rvfc(entries):
    out = {"callbacks": len(entries)}
    if len(entries) < 3:
        return out
    span = (entries[-1][0] - entries[0][0]) / 1000.0
    mts = [e[1] for e in entries]
    steps = [b - a for a, b in zip(mts, mts[1:])]
    fwd = [d for d in steps if d > 1e-6]
    out["span_s"] = round(span, 3)
    out["callbacks_per_s"] = round((len(entries) - 1) / span, 2)
    out["media_advance_per_s"] = round((mts[-1] - mts[0]) / span, 4)
    if fwd:
        per = statistics.median(fwd)
        out["content_fps"] = round(1.0 / per, 3)
        # A step of n content periods between two presented frames means Chrome skipped n-1.
        hist = {}
        for d in fwd:
            n = max(1, round(d / per))
            hist[n] = hist.get(n, 0) + 1
        out["media_step_hist"] = {str(k): v for k, v in sorted(hist.items())}
    out["repeated_media_time"] = sum(1 for d in steps if abs(d) <= 1e-6)
    out["presented_frames_per_s"] = round((entries[-1][2] - entries[0][2]) / span, 2)
    out["frame_size"] = [entries[-1][4], entries[-1][5]]
    return out


def summarize_dda(prefix):
    path = prefix + ".csv"
    if not os.path.exists(path):
        return {"error": "no recording"}
    with open(path, newline="") as f:
        rows = [r for r in csv.reader(line for line in f if not line.startswith("#"))]
    rows = rows[1:]
    if len(rows) < 3:
        return {"frames": len(rows)}
    q = [float(r[1]) for r in rows]
    acc = [int(r[3]) for r in rows]
    hashes = [r[4] for r in rows]
    span = (q[-1] - q[0]) / 1000.0
    changes = sum(1 for a, b in zip(hashes, hashes[1:]) if a != b)
    gaps = [b - a for a, b in zip(q, q[1:])]
    return {"frames": len(rows), "span_s": round(span, 3),
            "updates_per_s": round((len(rows) - 1) / span, 2),
            "distinct_pictures_per_s": round(changes / span, 2),
            "recorder_missed": sum(max(0, a - 1) for a in acc),
            "interval_ms_median": round(statistics.median(gaps), 3),
            "interval_ms_p95": round(sorted(gaps)[int(0.95 * (len(gaps) - 1))], 3)}


def measure(cdp, sid, tag, seconds, rec_seconds, prefix, at, rect):
    js(cdp, sid, "window.__nsp && (window.__nsp.log = []); true")
    s0 = js(cdp, sid, STATE_JS)
    time.sleep(2.0)
    rec = subprocess.Popen([DDACAP, "--at", f"{at[0]},{at[1]}", "--rect", ",".join(map(str, rect)),
                            "--seconds", str(rec_seconds), "--out", prefix],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(max(0.0, seconds - 2.0))
    s1 = js(cdp, sid, STATE_JS)
    entries = js(cdp, sid, "window.__nsp ? window.__nsp.log : []") or []
    try:
        _, err = rec.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        rec.kill()
        err = "ddacap timed out"
    res = {"phase": tag, "state0": s0, "state1": s1,
           "chrome_dropped": (s1["dropped"] or 0) - (s0["dropped"] or 0),
           "chrome_decoded": (s1["total"] or 0) - (s0["total"] or 0),
           "rvfc": summarize_rvfc(entries), "dda": summarize_dda(prefix)}
    if err and err.strip():
        res["dda"]["stderr"] = err.strip()[-500:]
    return res


def run_clip(name, url, start, a, primary):
    W, H = primary
    cw, ch = a.crop
    rect = ((W - cw) // 2, (H - ch) // 2, cw, ch)
    at = (W // 2, H // 2)
    out = {"clip": name, "url": url, "start": start}
    # A previous run that did not shut Chrome down cleanly leaves this file behind,
    # and launch_chrome would then read the dead instance's port.
    stale = os.path.join(a.profile, "DevToolsActivePort")
    if os.path.exists(stale):
        os.remove(stale)
    sess = cc.launch_chrome(f"{url}&t={int(start)}s", user_data_dir=a.profile,
                            extra_args=["--mute-audio", "--window-position=120,120",
                                        "--window-size=1600,900"]
                            + ([f"--proxy-server={a.proxy}"] if a.proxy else []))
    cdp = cc.CDP(sess.ws_url, timeout=60)
    engine = None
    try:
        out["chrome"] = cc._poll_devtools(sess.port).get("Browser")
        tgt = wait_for(lambda: next((t for t in cc.list_page_targets(cdp)
                                     if "youtube" in t.get("url", "") or "consent" in t.get("url", "")), None), 30)
        if not tgt:
            raise RuntimeError("no YouTube page target")
        sid = cc.attach_page(cdp, tgt["targetId"])
        if "consent" in (js(cdp, sid, "location.hostname") or ""):
            log("consent page: rejecting non-essential cookies")
            js(cdp, sid, CONSENT_JS)
            time.sleep(4.0)
            tgt = wait_for(lambda: next((t for t in cc.list_page_targets(cdp)
                                         if "youtube.com/watch" in t.get("url", "")), None), 30)
            sid = cc.attach_page(cdp, tgt["targetId"])
        if not wait_for(lambda: js(cdp, sid, "!!document.querySelector('video') && document.querySelector('video').readyState >= 2"), 90):
            raise RuntimeError("video element never became ready")
        if not pass_ads(cdp, sid, a.ad_limit):
            raise RuntimeError("ads did not finish")
        # The start time rides on the URL (&t=): a seek on this machine's throttled route
        # to googlevideo stalls until the player gives up ("Something went wrong",
        # measured), while the initial request at t lands. No quality override either.
        pass_ads(cdp, sid, a.ad_limit)
        if not wait_for(lambda: (lambda st: st["t"] and st["t"] > start + 1.0 and st["w"] and not st["paused"])(
                js(cdp, sid, STATE_JS)), 90):
            raise RuntimeError(f"playback did not start: {js(cdp, sid, STATE_JS)}")
        win = cdp.send("Browser.getWindowForTarget", {"targetId": tgt["targetId"]})
        cdp.send("Browser.setWindowBounds", {"windowId": win["windowId"], "bounds": {"windowState": "normal"}})
        cdp.send("Browser.setWindowBounds", {"windowId": win["windowId"],
                                             "bounds": {"left": 120, "top": 120, "width": 1600, "height": 900}})
        time.sleep(1.0)
        js(cdp, sid, "(() => { if (document.activeElement) document.activeElement.blur(); const p = document.querySelector('#movie_player'); if (p) p.focus(); return true; })()")
        key(cdp, sid, "f", "KeyF", 70)
        if not wait_for(lambda: js(cdp, sid, "!!document.fullscreenElement"), 5):
            log("the 'f' shortcut did not enter fullscreen; requesting it directly")
            js(cdp, sid, "document.querySelector('#movie_player').requestFullscreen().then(() => true, e => String(e))",
               await_promise=True, gesture=True)
        if not wait_for(lambda: js(cdp, sid, "!!document.fullscreenElement"), 5):
            raise RuntimeError("could not enter fullscreen")
        time.sleep(4.0)  # the fullscreen transition and the controls fade
        wins = chrome_windows()
        out["chrome_windows"] = wins
        if not any(r == (0, 0, W, H) and "YouTube" in title for _, title, r in wins):
            raise RuntimeError(f"no YouTube Chrome window covers the primary monitor exactly: {wins}")
        if not js(cdp, sid, PROBE_JS):
            raise RuntimeError("rVFC probe did not install")
        st = js(cdp, sid, STATE_JS)
        log(f"{name}: '{st['title']}' {st['w']}x{st['h']} at {st['t']:.1f}s, fullscreen={st['fs']}")

        out["off"] = measure(cdp, sid, "off", a.seconds, a.rec_seconds,
                             os.path.join(a.out, f"{name}_off_dda"), at, rect)
        log(f"{name} off: {json.dumps(out['off']['rvfc'])} | {json.dumps(out['off']['dda'])}")

        elog = open(os.path.join(a.out, f"{name}_engine.log"), "w", encoding="utf-8")
        engine = subprocess.Popen([ENGINE], cwd=os.path.dirname(ENGINE), stdout=elog,
                                  stderr=subprocess.STDOUT)
        shown = wait_for(lambda: "SHOWN" in open(os.path.join(a.out, f"{name}_engine.log"),
                                                 encoding="utf-8", errors="replace").read(), 30, 0.5)
        out["engine_shown"] = bool(shown)
        if not shown:
            log(f"{name}: engine never reported SHOWN within 30 s; measuring anyway")
        out["on"] = measure(cdp, sid, "on", a.seconds, a.rec_seconds,
                            os.path.join(a.out, f"{name}_on_dda"), at, rect)
        log(f"{name} on: {json.dumps(out['on']['rvfc'])} | {json.dumps(out['on']['dda'])}")
    except Exception:
        # Leave evidence of what the page showed; a bare "never became ready" is a guess.
        try:
            import base64
            tgts = cc.list_page_targets(cdp)
            out["targets_at_failure"] = [(t.get("type"), t.get("url")) for t in tgts]
            if tgts:
                s = cc.attach_page(cdp, tgts[0]["targetId"])
                shot = cdp.send("Page.captureScreenshot", {"format": "png"}, session_id=s)
                with open(os.path.join(a.out, f"{name}_failure.png"), "wb") as f:
                    f.write(base64.b64decode(shot["data"]))
                out["page_at_failure"] = js(cdp, s, "({url: location.href, title: document.title, "
                                               "videos: document.querySelectorAll('video').length, "
                                               "ready: [...document.querySelectorAll('video')].map(v => v.readyState), "
                                               "text: document.body ? document.body.innerText.slice(0, 600) : ''})")
        except Exception as dbg:  # noqa: BLE001
            out["failure_debug_error"] = repr(dbg)
        with open(os.path.join(a.out, f"{name}_failure.json"), "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1, ensure_ascii=False)
        raise
    finally:
        if engine is not None:
            hotkey_quit_engine()
            try:
                engine.wait(timeout=10)
            except subprocess.TimeoutExpired:
                engine.terminate()
                out["engine_killed"] = True
            out["engine_exit"] = engine.returncode
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        try:
            sess.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            sess.proc.kill()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clip", action="append", required=True, help="name=URL@start_seconds")
    ap.add_argument("--out", required=True)
    ap.add_argument("--profile", help="Chrome user-data-dir (default: <out>/chrome_profile)")
    ap.add_argument("--seconds", type=float, default=20.0, help="length of each phase")
    ap.add_argument("--rec-seconds", type=float, default=4.0, help="DDA recording inside each phase")
    ap.add_argument("--crop", default="640,360", help="centre crop recorded by DDA, W,H")
    ap.add_argument("--ad-limit", type=float, default=120.0)
    ap.add_argument("--proxy", help="proxy for the test Chrome only, e.g. socks5://127.0.0.1:1370")
    ap.add_argument("--quality", default="hd1440", help="YouTube quality label requested through the player API")
    a = ap.parse_args()
    a.crop = tuple(int(v) for v in a.crop.split(","))
    os.makedirs(a.out, exist_ok=True)
    a.profile = a.profile or os.path.join(a.out, "chrome_profile")
    primary = (user32.GetSystemMetrics(0), user32.GetSystemMetrics(1))
    results = []
    for spec in a.clip:
        name, rest = spec.split("=", 1)
        url, start = rest.rsplit("@", 1)
        log(f"clip {name}: {url} from {start}s on the {primary[0]}x{primary[1]} primary")
        try:
            results.append(run_clip(name, url, float(start), a, primary))
        except Exception as exc:  # noqa: BLE001 - one broken clip must not lose the others
            log(f"clip {name} FAILED: {exc!r}")
            results.append({"clip": name, "error": repr(exc)})
        with open(os.path.join(a.out, "yt_check.json"), "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1, ensure_ascii=False)
    log(f"wrote {os.path.join(a.out, 'yt_check.json')}")


if __name__ == "__main__":
    main()
