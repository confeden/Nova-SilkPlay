#!/usr/bin/env python3
"""yt_arms.py - the same real YouTube moment through three arms, recorded off the screen.

  page  Chrome alone
  ours  silkplay.exe, default arguments
  ls    Lossless Scaling with the owner's own profile, toggled with Ctrl+Alt+Q

Every arm gets a fresh muted Chrome on the PRIMARY monitor, starts the video at the same
URL time, enters YouTube's element fullscreen, engages the arm, and starts a DDA
recording of the screen centre the moment the media clock reaches --rec-at, so all arms
record the same stretch of the film. There is no ground truth on real content: this is
for looking at the frames side by side (dda_compare.py), not for scoring.
LS is started only if it is not running, and closed again only if this script started it.

  python yt_arms.py --out DIR --url https://www.youtube.com/watch?v=WhWc3b3KhnY --start 120 --rec-at 150
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import yt_check as yc  # noqa: E402  (sets per-monitor DPI awareness on import)

cc = yc.cc
user32 = yc.user32
LS_EXE = r"D:\SteamLibrary\steamapps\common\Lossless Scaling\game\LosslessScaling.exe"
VK_Q = 0x51


def chord(*vks):
    for vk in vks:
        user32.keybd_event(vk, 0, 0, 0)
    for vk in reversed(vks):
        user32.keybd_event(vk, 0, yc.KEYEVENTF_KEYUP, 0)


def pids_of(image):
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", f"IMAGENAME eq {image}"],
                         capture_output=True, text=True).stdout
    return [int(line.split('","')[1]) for line in out.splitlines()
            if line.lower().startswith(f'"{image.lower()}"')]


def windows_of(pids):
    found = []

    def cb(h, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value in pids and user32.IsWindowVisible(h) and not user32.IsIconic(h):
            r = wt.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            found.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    user32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return found


def monitors():
    """Physical-pixel rects (x, y, w, h) of every monitor, primary first."""
    found = []
    MONITORENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD)]

    def cb(hmon, hdc, lprc, lp):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(MONITORINFO)
        user32.GetMonitorInfoW(hmon, ctypes.byref(mi))
        r = mi.rcMonitor
        found.append(((r.left, r.top, r.right - r.left, r.bottom - r.top), bool(mi.dwFlags & 1)))
        return True

    user32.EnumDisplayMonitors(None, None, MONITORENUMPROC(cb), 0)
    found.sort(key=lambda m: not m[1])
    return [m[0] for m in found]


def youtube_hwnd(W, H, X=0, Y=0):
    hits = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value == "Chrome_WidgetWin_1" and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            r = wt.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            if "YouTube" in buf.value and (r.left, r.top, r.right, r.bottom) == (X, Y, X + W, Y + H):
                hits.append(h)
        return True

    user32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return hits[0] if hits else None


def make_foreground(hwnd):
    if user32.GetForegroundWindow() == hwnd:
        return True
    # A bare Alt tap gives this process the right to move the foreground; clicking the
    # video instead would toggle YouTube's pause.
    user32.keybd_event(yc.VK_MENU, 0, 0, 0)
    user32.SetForegroundWindow(hwnd)
    user32.keybd_event(yc.VK_MENU, 0, yc.KEYEVENTF_KEYUP, 0)
    time.sleep(0.3)
    return user32.GetForegroundWindow() == hwnd


def find_youtube_window():
    hits = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value == "Chrome_WidgetWin_1" and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if "YouTube" in buf.value:
                hits.append(h)
        return True

    user32.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return hits[0] if hits else None


def kill_profile_chrome(profile, wait_s=10.0):
    """Chrome processes started on this profile; Browser.close does not always end them,
    and a survivor makes the next launch hand off to it and never open a DevTools port."""
    norm = os.path.normcase(os.path.abspath(profile))
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
          "ForEach-Object { \"$($_.ProcessId)|$($_.CommandLine)\" }")

    def pids():
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
        found = []
        for line in out.splitlines():
            pid, _, cmd = line.partition("|")
            if norm in os.path.normcase(cmd.replace("/", "\\")):
                found.append(int(pid))
        return found

    t0 = time.monotonic()
    while time.monotonic() - t0 < wait_s and pids():
        time.sleep(0.5)
    left = pids()
    for pid in left:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    return len(left)


def screenshot_on_failure(cdp, path):
    try:
        import base64
        tg = cc.list_page_targets(cdp)
        if not tg:
            return
        sid = cc.attach_page(cdp, tg[0]["targetId"])
        shot = cdp.send("Page.captureScreenshot", {"format": "png"}, session_id=sid)
        with open(path, "wb") as f:
            f.write(base64.b64decode(shot["data"]))
    except Exception:  # noqa: BLE001 - evidence is best effort
        pass


def open_fullscreen(a, W, H, X=0, Y=0):
    stale = os.path.join(a.profile, "DevToolsActivePort")
    if os.path.exists(stale):
        os.remove(stale)
    extra = ["--mute-audio", "--window-position=120,120", "--window-size=1600,900"]
    if a.proxy:
        extra.append(f"--proxy-server={a.proxy}")
    kill_profile_chrome(a.profile, wait_s=0.0)
    sess = cc.launch_chrome(f"{a.url}&t={int(a.start)}s", user_data_dir=a.profile, extra_args=extra)
    cdp = cc.CDP(sess.ws_url, timeout=60)
    try:
        return _open_fullscreen(a, W, H, X, Y, sess, cdp)
    except Exception:
        screenshot_on_failure(cdp, os.path.join(a.out, f"{a.name}_open_failure.png"))
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        kill_profile_chrome(a.profile)
        raise


def _open_fullscreen(a, W, H, X, Y, sess, cdp):
    tgt = yc.wait_for(lambda: next((t for t in cc.list_page_targets(cdp)
                                    if "youtube.com/watch" in t.get("url", "")), None), 60)
    if not tgt:
        raise RuntimeError("no YouTube page target")
    sid = cc.attach_page(cdp, tgt["targetId"])
    if not yc.wait_for(lambda: yc.js(cdp, sid, "!!document.querySelector('video') && document.querySelector('video').readyState >= 2"), a.load_timeout):
        raise RuntimeError("video element never became ready")
    yc.pass_ads(cdp, sid, 120)
    if not yc.wait_for(lambda: (lambda st: st["t"] and st["t"] > a.start + 1.0 and st["w"] and not st["paused"])(
            yc.js(cdp, sid, yc.STATE_JS)), 90):
        raise RuntimeError(f"playback did not start: {yc.js(cdp, sid, yc.STATE_JS)}")
    levels = yc.js(cdp, sid, "document.querySelector('#movie_player').getAvailableQualityLevels()") or []
    if a.quality not in levels:
        raise RuntimeError(f"{a.quality} is not offered for this video: {levels}")
    yc.js(cdp, sid, f"document.querySelector('#movie_player').setPlaybackQualityRange('{a.quality}', '{a.quality}'); true")
    win = cdp.send("Browser.getWindowForTarget", {"targetId": tgt["targetId"]})
    cdp.send("Browser.setWindowBounds", {"windowId": win["windowId"], "bounds": {"windowState": "normal"}})
    cdp.send("Browser.setWindowBounds", {"windowId": win["windowId"],
                                         "bounds": {"left": 120, "top": 120, "width": 1600, "height": 900}})
    time.sleep(1.0)
    # Put the window on the target monitor in PHYSICAL pixels (CDP bounds are DIPs and
    # land elsewhere on a scaled desktop); SWP_NOACTIVATE so the owner's focus stays put.
    yw = yc.wait_for(find_youtube_window, 10)
    if yw:
        user32.SetWindowPos(yw, None, X + 80, Y + 80, min(1600, W - 160), min(900, H - 160), 0x0010 | 0x0004)
        time.sleep(1.0)
    yc.js(cdp, sid, "(() => { if (document.activeElement) document.activeElement.blur(); const p = document.querySelector('#movie_player'); if (p) p.focus(); return true; })()")
    yc.key(cdp, sid, "f", "KeyF", 70)
    if not yc.wait_for(lambda: yc.js(cdp, sid, "!!document.fullscreenElement"), 6):
        raise RuntimeError("could not enter fullscreen")
    hwnd = yc.wait_for(lambda: youtube_hwnd(W, H, X, Y), 6)
    if not hwnd:
        raise RuntimeError(f"the fullscreen YouTube window does not cover monitor {X},{Y} {W}x{H} exactly")
    if not yc.js(cdp, sid, yc.PROBE_JS):
        raise RuntimeError("rVFC probe did not install")
    # Comparisons are only valid at the requested rendition (owner: 1440p sources).
    if not yc.wait_for(lambda: (lambda st: st["w"] >= a.min_width and not st["paused"])(yc.js(cdp, sid, yc.STATE_JS)), 60, 0.5):
        raise RuntimeError(f"never reached {a.min_width} px wide: {yc.js(cdp, sid, yc.STATE_JS)}")
    return sess, cdp, sid, hwnd


def run_arm(arm, a, W, H, X=0, Y=0):
    cw, ch = a.crop
    rect = ((W - cw) // 2, (H - ch) // 2, cw, ch)
    prefix = os.path.join(a.out, f"{a.name}_{arm}")
    res = {"arm": arm}
    sess, cdp, sid, hwnd = open_fullscreen(a, W, H, X, Y)
    engine = None
    ls_on = False
    started_ls = False
    try:
        time.sleep(3.0)  # controls fade after fullscreen
        if arm == "ours":
            elog = open(prefix + "_engine.log", "w", encoding="utf-8")
            engine = subprocess.Popen([yc.ENGINE] + a.engine_args.split(), cwd=os.path.dirname(yc.ENGINE), stdout=elog,
                                      stderr=subprocess.STDOUT)
            res["shown"] = bool(yc.wait_for(lambda: "overlay shown" in open(prefix + "_engine.log", encoding="utf-8",
                                                                             errors="replace").read(), 30, 0.25))
        elif arm == "ls":
            if not pids_of("LosslessScaling.exe"):
                subprocess.Popen([LS_EXE], cwd=os.path.dirname(LS_EXE))
                started_ls = True
                yc.wait_for(lambda: windows_of(pids_of("LosslessScaling.exe")), 20, 0.5)
                time.sleep(2.0)
            for hw, _ in windows_of(pids_of("LosslessScaling.exe")):
                user32.ShowWindow(hw, 6)  # minimise LS's own UI so it cannot be the scaled window
            time.sleep(0.5)
            res["foreground"] = make_foreground(hwnd)
            chord(yc.VK_CONTROL, yc.VK_MENU, VK_Q)
            ls_on = True
            out_win = yc.wait_for(lambda: [r for _, r in windows_of(pids_of("LosslessScaling.exe"))
                                           if r == (X, Y, X + W, Y + H)], 15, 0.25)
            res["ls_output_window"] = bool(out_win)
            if not out_win:
                raise RuntimeError("Lossless Scaling did not put an output window over the primary")
        time.sleep(3.0)  # let the arm settle
        st = yc.js(cdp, sid, yc.STATE_JS)
        res["state_before"] = st
        if st["t"] >= a.rec_at - 0.5:
            raise RuntimeError(f"media time {st['t']:.1f} already past --rec-at {a.rec_at}; use a later --rec-at")
        yc.js(cdp, sid, "window.__nsp.log = []; true")
        yc.wait_for(lambda: yc.js(cdp, sid, "document.querySelector('video').currentTime") >= a.rec_at,
                    a.rec_at - st["t"] + 30, 0.01)
        res["media_at_record"] = yc.js(cdp, sid, "document.querySelector('video').currentTime")
        rec = subprocess.run([yc.DDACAP, "--at", f"{X + W // 2},{Y + H // 2}", "--rect", ",".join(map(str, rect)),
                              "--seconds", str(a.rec_seconds), "--out", prefix],
                             capture_output=True, text=True, timeout=180)
        res["state_after"] = yc.js(cdp, sid, yc.STATE_JS)
        res["rvfc"] = yc.summarize_rvfc(yc.js(cdp, sid, "window.__nsp.log") or [])
        res["dda"] = yc.summarize_dda(prefix)
        if rec.stderr.strip():
            res["dda"]["stderr"] = rec.stderr.strip()[-300:]
    finally:
        if engine is not None:
            yc.hotkey_quit_engine()
            try:
                engine.wait(timeout=10)
            except subprocess.TimeoutExpired:
                engine.terminate()
        if ls_on:
            make_foreground(hwnd)
            chord(yc.VK_CONTROL, yc.VK_MENU, VK_Q)
            time.sleep(1.5)
        if started_ls and pids_of("LosslessScaling.exe"):
            subprocess.run(["taskkill", "/IM", "LosslessScaling.exe"], capture_output=True)
            time.sleep(1.5)
            if pids_of("LosslessScaling.exe"):
                subprocess.run(["taskkill", "/F", "/IM", "LosslessScaling.exe"], capture_output=True)
        try:
            cdp.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        cdp.close()
        killed = kill_profile_chrome(a.profile)
        if killed:
            res["chrome_killed"] = killed
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="clip")
    ap.add_argument("--url", required=True)
    ap.add_argument("--start", type=float, required=True, help="URL start time, seconds")
    ap.add_argument("--rec-at", type=float, required=True, help="media time at which recording starts")
    ap.add_argument("--rec-seconds", type=float, default=4.0)
    ap.add_argument("--crop", default="960,540")
    ap.add_argument("--arms", default="page,ours,ls")
    ap.add_argument("--proxy")
    ap.add_argument("--quality", default="hd1440")
    ap.add_argument("--monitor", type=int, default=1, help="1 = primary, 2 = the next monitor, ...")
    ap.add_argument("--load-timeout", type=float, default=240.0,
                    help="seconds to wait for the video to become playable; the direct route to googlevideo is slow here")
    ap.add_argument("--engine-args", default="", help="extra silkplay.exe arguments for the ours arm, e.g. '--warp-lab 22 --warp-lab-p 0.1'")
    ap.add_argument("--min-width", type=int, default=2560, help="an arm below this videoWidth is invalid")
    ap.add_argument("--profile")
    a = ap.parse_args()
    a.crop = tuple(int(v) for v in a.crop.split(","))
    os.makedirs(a.out, exist_ok=True)
    a.profile = a.profile or os.path.join(a.out, "chrome_profile")
    if pids_of("silkplay.exe"):
        sys.exit("silkplay.exe is already running; stop it first")
    mons = monitors()
    if a.monitor < 1 or a.monitor > len(mons):
        sys.exit(f"--monitor {a.monitor}: this desktop has {len(mons)} monitors: {mons}")
    X, Y, W, H = mons[a.monitor - 1]
    yc.log(f"monitor {a.monitor}: {X},{Y} {W}x{H}")
    results = []
    for arm in a.arms.split(","):
        yc.log(f"{a.name} arm {arm}")
        try:
            r = run_arm(arm, a, W, H, X, Y)
            yc.log(f"  {arm}: media {r.get('media_at_record')} size {r['state_after']['w']}x{r['state_after']['h']} "
                   f"rvfc {json.dumps(r['rvfc'])} | dda {json.dumps(r['dda'])}")
        except Exception as exc:  # noqa: BLE001
            yc.log(f"  {arm} FAILED: {exc!r}")
            r = {"arm": arm, "error": repr(exc)}
        results.append(r)
        with open(os.path.join(a.out, f"{a.name}_arms.json"), "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1, ensure_ascii=False)


if __name__ == "__main__":
    main()
