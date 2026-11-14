"""film_arms.py - a real film, fullscreen on monitor 2: Chrome alone against the engine.

The same media moments through each arm, recorded off the screen with ddacap. A moment
counts only if the clock advanced, frames were decoded and the rendition was reached:
a player that never started still shows an animated spinner, which records as a healthy
59.8 distinct pictures/s (G91).

  page    Chrome alone
  ours    prototype/silkplay.exe
  before  the executable given by --before, to compare two builds

  python film_arms.py --out <ABS dir> --file E:/Downloads/tears_of_steel_720p.mp4 --moments 36,303,466
  python film_arms.py --out <ABS dir> --url https://tv.kinohub.vip/movie/5494049 --minw 1920

--file serves the film from loopback, no network needed. --url drives that site's player
inside its cross-origin frame; it needs about 8 s after load before play is accepted.
Never run next to another silkplay.exe: the engine is single-instance.
"""
import argparse
import ctypes
import http.server
import json
import os
import shlex
import shutil
import subprocess
import sys
import threading
import time
from ctypes import wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "..", "overlay-probe")]
import chrome_cdp as cc  # noqa: E402
import yt_check as yc  # noqa: E402
import yt_arms as ya  # noqa: E402
import windowed_check as wc  # noqa: E402

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
TITLE = "SilkPlay film"
PLAY_LABEL = "\u0412\u043e\u0441\u043f\u0440\u043e\u0438\u0437\u0432\u0435\u0441\u0442\u0438"

STATE = r"""(()=>{const v=document.querySelector('video');if(!v)return null;const q=v.getVideoPlaybackQuality();
return {t:v.currentTime,d:v.duration,paused:v.paused,w:v.videoWidth,h:v.videoHeight,rs:v.readyState,
total:q.totalVideoFrames,dropped:q.droppedVideoFrames,fs:!!document.fullscreenElement}})()"""


def serve(path, page=None):
    """Loopback server with Range support; Chrome cannot seek an mp4 without it."""
    size = os.path.getsize(path)
    if page is None:
        page = (f"<!doctype html><title>{TITLE}</title><style>html,body{{margin:0;background:#000;height:100%}}"
                "video{width:100%;height:100%}</style><video autoplay playsinline src='/film'></video>").encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path != "/film":
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
                return
            start, end = 0, size - 1
            rng = self.headers.get("Range")
            if rng:
                a, b = rng.split("=")[1].split("-")
                start = int(a or 0)
                end = int(b) if b else end
            self.send_response(206 if rng else 200)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            if rng:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            try:
                with open(path, "rb") as f:
                    f.seek(start)
                    left = end - start + 1
                    while left > 0:
                        chunk = f.read(min(1 << 20, left))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        left -= len(chunk)
            except OSError:
                pass  # Chrome drops connections while it seeks

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}/"


def player_frame(c):
    for t in c.send("Target.getTargets")["targetInfos"]:
        if t["type"] == "iframe":
            return c.send("Target.attachToTarget", {"targetId": t["targetId"], "flatten": True})["sessionId"]
    return None


def start_playback(a, c):
    """Returns the CDP session that owns the <video>."""
    target = yc.wait_for(lambda: next(iter(cc.list_page_targets(c)), None), 30)
    sid = cc.attach_page(c, target["targetId"])
    if a.file:
        fs = sid
        yc.wait_for(lambda: yc.js(c, fs, "!!document.querySelector('video')"), 20)
        yc.js(c, fs, "document.querySelector('video').play();true", gesture=True)
    else:
        fs = yc.wait_for(lambda: player_frame(c), 40)
        if not fs:
            raise RuntimeError("no player frame")
        yc.wait_for(lambda: yc.js(c, fs, "!!document.querySelector('video')"), 30)
        yc.js(c, sid, "document.querySelector('iframe').scrollIntoView({block:'center'});true")
        time.sleep(8)
        click = "[...document.querySelectorAll('button')].filter(b=>b.innerText.includes(%s)).forEach(x=>x.click());true"
        for _ in range(3):
            yc.js(c, fs, click % json.dumps(PLAY_LABEL), gesture=True)
            if yc.wait_for(lambda: (lambda st: st and st["t"] > 2 and not st["paused"])(yc.js(c, fs, STATE)), 20):
                break
        yc.js(c, fs, r"[...document.querySelectorAll('button')].find(x=>x.innerText.replace(/\s+/g,'').startsWith(%s))?.click();true"
              % json.dumps(a.quality), gesture=True)
    if not yc.wait_for(lambda: (lambda st: st and st["t"] > 1 and st["total"] > 0 and st["w"] >= a.minw)(yc.js(c, fs, STATE)), 90):
        raise RuntimeError(f"playback never started: {yc.js(c, fs, STATE)}")
    return fs


def seek(c, fs, moment, minw):
    def there():
        st = yc.js(c, fs, STATE)
        return st and st["rs"] >= 3 and moment + 2 <= st["t"] < moment + 40 and not st["paused"] and st["w"] >= minw
    for _ in range(4):
        yc.js(c, fs, f"(()=>{{const v=document.querySelector('video');v.currentTime={moment};v.play();return true}})()", gesture=True)
        if yc.wait_for(there, 25):
            return True
    return False


def quit_engine(engine):
    event = kernel32.OpenEventW(0x0002, False, "Local\\NovaSilkPlay.Quit")
    if event:
        kernel32.SetEvent(event)
        kernel32.CloseHandle(event)
    try:
        engine.wait(timeout=10)
    except subprocess.TimeoutExpired:
        engine.terminate()
    return engine.returncode


def overlay_shown(pid):
    for hwnd, _ in ya.windows_of([pid]):
        cls = ctypes.create_unicode_buffer(80)
        user32.GetClassNameW(hwnd, cls, len(cls))
        if cls.value == "NovaSilkPlayOverlay":
            return True
    return False


def run_arm(a, arm, url):
    X, Y, W, H = a.monitor_rect
    if a.monitor_rect not in ya.monitors():
        raise RuntimeError("selected monitor disappeared; refusing to open the test browser")
    profile = os.path.join(a.out, f"profile_{arm}_{int(time.time())}")
    os.makedirs(profile, exist_ok=True)
    session = cc.launch_chrome(url, user_data_dir=profile, no_activate=a.monitor != 1, extra_args=[
        "--mute-audio", f"--window-position={X + 80},{Y + 60}", "--window-size=1600,900",
        "--autoplay-policy=no-user-gesture-required"])
    c = cc.CDP(session.ws_url, timeout=60)
    result = {"arm": arm, "topmost": a.topmost, "moments": []}
    engine = None
    try:
        fs = start_playback(a, c)
        browser_pid = next(p["id"] for p in c.send("SystemInfo.getProcessInfo")["processInfo"]
                           if p["type"] == "browser")
        if not seek(c, fs, a.moments[0], a.minw):
            raise RuntimeError(f"first seek failed: {yc.js(c, fs, STATE)}")
        if a.monitor == 1:
            c.send("Page.bringToFront", session_id=fs)
        yc.js(c, fs, "document.documentElement.requestFullscreen().then(()=>true)",
              await_promise=True, gesture=True)
        if not yc.wait_for(lambda: yc.js(c, fs, "!!document.fullscreenElement"), 8):
            raise RuntimeError("no fullscreen")
        hwnd = wc.find_window(a.title, browser_pid)
        bounds = wt.RECT()
        if not hwnd or not user32.GetWindowRect(hwnd, ctypes.byref(bounds)):
            raise RuntimeError("owned fullscreen browser window disappeared")
        if (bounds.left, bounds.top, bounds.right, bounds.bottom) != (X, Y, X + W, Y + H):
            raise RuntimeError("owned fullscreen browser is outside the selected monitor")
        foreground_before = user32.GetForegroundWindow()
        if not user32.SetWindowPos(hwnd, wt.HWND(-1) if a.topmost else None,
                                 0, 0, 0, 0, 0x0010 | 0x0001 | 0x0002):
            raise RuntimeError("could not raise the owned browser without activation")
        result["foreground_unchanged"] = user32.GetForegroundWindow() == foreground_before
        if not result["foreground_unchanged"]:
            raise RuntimeError("raising the test browser changed the foreground window")
        time.sleep(3)
        yc.js(c, fs, yc.PROBE_JS)
        if arm != "page":
            log_path = os.path.join(a.out, f"{a.name}_{arm}_engine.log")
            exe = a.before if arm == "before" else yc.ENGINE
            log = open(log_path, "w", encoding="utf-8")
            engine = subprocess.Popen([exe, "--console", "--default-settings", "--target-title", a.title,
                                       "--target-pid", str(browser_pid),
                                       "--stats-every", "2", "--no-badge"] + shlex.split(a.engine_args),
                                      cwd=os.path.dirname(yc.ENGINE), stdout=log, stderr=subprocess.STDOUT)
            t0 = time.time()
            shown = yc.wait_for(lambda: "overlay shown" in open(log_path, encoding="utf-8", errors="replace").read(), 30, 0.2)
            result["shown_s"] = round(time.time() - t0, 2) if shown else None
            if not shown:
                raise RuntimeError(f"engine never showed its overlay; inspect {log_path}")
        cw, ch = a.crop
        for i, moment in enumerate(a.moments):
            if not seek(c, fs, moment, a.minw):
                raise RuntimeError(f"seek to {moment} failed: {yc.js(c, fs, STATE)}")
            time.sleep(2)
            yc.js(c, fs, "window.__nsp&&(window.__nsp.log=[]);true")
            before = yc.js(c, fs, STATE)
            if engine and not overlay_shown(engine.pid):
                raise RuntimeError("overlay hidden before recording; this is not an engine arm")
            prefix = os.path.join(a.out, f"{a.name}_{arm}_m{i}")
            rec = subprocess.run([yc.DDACAP, "--at", f"{X + W // 2},{Y + H // 2}", "--rect", f"{(W - cw) // 2},{(H - ch) // 2},{cw},{ch}",
                                  "--seconds", str(a.seconds), "--out", prefix], capture_output=True, text=True, timeout=120)
            if rec.returncode or rec.stderr.strip():
                raise RuntimeError(f"recorder failed ({rec.returncode}): {rec.stderr.strip()}")
            after = yc.js(c, fs, STATE)
            if engine and not overlay_shown(engine.pid):
                raise RuntimeError("overlay hidden during recording; exclude this arm")
            m = {"moment": moment, "t0": before["t"], "t1": after["t"], "chrome_dropped": after["dropped"] - before["dropped"],
                 "chrome_decoded": after["total"] - before["total"], "rvfc": yc.summarize_rvfc(yc.js(c, fs, "window.__nsp?window.__nsp.log:[]") or []),
                 "dda": yc.summarize_dda(prefix), "stderr": rec.stderr.strip()[-200:]}
            m["valid"] = bool(after["t"] - before["t"] > a.seconds * 0.8 and m["chrome_decoded"] > 0 and not after["paused"])
            m["cadence_valid"] = bool(m["valid"] and m["dda"].get("recorder_missed") == 0
                                      and m["dda"].get("span_s", 0) >= a.seconds * 0.8)
            result["moments"].append(m)
            print(arm, json.dumps({k: m[k] for k in ("moment", "t0", "t1", "chrome_dropped", "valid", "cadence_valid")}), m["dda"], flush=True)
    finally:
        if engine:
            result["engine_exit"] = quit_engine(engine)
        try:
            c.send("Browser.close")
        except Exception:  # noqa: BLE001
            pass
        c.close()
        time.sleep(2)
        session_proc = getattr(session, "proc", None)
        if session_proc and session_proc.poll() is None:
            session_proc.terminate()
        shutil.rmtree(profile, ignore_errors=True)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="film")
    ap.add_argument("--file")
    ap.add_argument("--url")
    ap.add_argument("--before", help="executable for the before arm")
    ap.add_argument("--arms", default="page,ours")
    ap.add_argument("--moments", default="36,303,466")
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--crop", default="800,450")
    ap.add_argument("--quality", default="1080p")
    ap.add_argument("--minw", type=int, default=800)
    ap.add_argument("--title", default=None, help="--target-title for the engine; keeps it off the owner's Chrome")
    ap.add_argument("--monitor", type=int, default=2, help="1 = primary; abort if the selected monitor is absent")
    ap.add_argument("--topmost", action="store_true",
                    help="keep the owned secondary test browser above other windows without activation")
    ap.add_argument("--engine-args", default="", help="extra lab engine options")
    a = ap.parse_args()
    if a.topmost and a.monitor == 1:
        ap.error("--topmost is restricted to secondary monitors")
    a.out = os.path.abspath(a.out)
    if bool(a.file) == bool(a.url):
        ap.error("give exactly one of --file or --url")
    a.moments = [float(x) for x in a.moments.split(",")]
    a.crop = tuple(int(x) for x in a.crop.split(","))
    monitors = ya.monitors()
    if a.monitor < 1 or a.monitor > len(monitors):
        ap.error(f"--monitor {a.monitor}: available monitors: {monitors}")
    a.monitor_rect = monitors[a.monitor - 1]
    if len(a.crop) != 2 or any(v <= 0 or v > limit for v, limit in zip(a.crop, a.monitor_rect[2:])):
        ap.error("--crop must fit inside the selected monitor")
    if set(a.arms.split(",")) - {"page", "ours", "before"}:
        ap.error("--arms supports page,ours,before")
    if "before" in a.arms.split(",") and not a.before:
        ap.error("the before arm requires --before")
    if ya.pids_of("silkplay.exe"):
        ap.error("silkplay.exe is already running; stop it before comparing arms")
    a.title = a.title or (TITLE if a.file else "Kinobox")
    os.makedirs(a.out, exist_ok=True)
    server = None
    url = a.url
    if a.file:
        server, url = serve(a.file)
    results = {}
    for arm in a.arms.split(","):
        try:
            results[arm] = run_arm(a, arm, url)
        except Exception as e:  # noqa: BLE001
            results[arm] = {"error": repr(e)}
            print("FAILED", arm, repr(e), flush=True)
        time.sleep(3)
    with open(os.path.join(a.out, f"{a.name}.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1, ensure_ascii=False)
    if server:
        server.shutdown()
    return 1 if any("error" in r or any(not m["valid"] for m in r["moments"])
                    for r in results.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
