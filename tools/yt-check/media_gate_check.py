"""media_gate_check.py OUT_DIR FILM - the media gate, live; check --list-displays first (G96). Chrome --mute-audio keeps a session; a `muted` element has none.
   sound: session present -> generation; pause under 30 s keeps it, a longer one disengages; play returns.
   muted: no session -> fullscreen video is not treated as video."""
import http.server, json, os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import film_arms as fa
yc, cc = fa.yc, fa.cc
out, film = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
server, url = fa.serve(film)

def run(name, muted):
    prof = os.path.join(out, f"prof_{name}_{int(time.time())}")
    s = cc.launch_chrome(url, user_data_dir=prof, no_activate=True, extra_args=[
        "--mute-audio", "--window-position=2640,60", "--window-size=1600,900", "--autoplay-policy=no-user-gesture-required"])
    c = cc.CDP(s.ws_url, timeout=60)
    log = os.path.join(out, f"{name}.log"); eng = None
    marks = []
    def mark(m):
        marks.append(m); print(name, m, flush=True)
    def text():
        return open(log, encoding="utf-8", errors="replace").read()
    try:
        t = yc.wait_for(lambda: next(iter(cc.list_page_targets(c)), None), 30)
        fs = cc.attach_page(c, t["targetId"])
        yc.wait_for(lambda: yc.js(c, fs, "!!document.querySelector('video')"), 20)
        yc.js(c, fs, f"(()=>{{const v=document.querySelector('video');v.muted={str(muted).lower()};v.currentTime=466;v.play();return true}})()", gesture=True)
        yc.wait_for(lambda: (lambda st: st and st["t"] > 468 and not st["paused"])(yc.js(c, fs, fa.STATE)), 30)
        yc.js(c, fs, "document.documentElement.requestFullscreen();true", gesture=True)
        yc.wait_for(lambda: yc.js(c, fs, "!!document.fullscreenElement"), 8)
        time.sleep(4)
        f = open(log, "w", encoding="utf-8")
        eng = subprocess.Popen([yc.ENGINE, "--console", "--default-settings", "--target-title", fa.TITLE, "--stats-every", "2", "--no-badge"],
                               cwd=os.path.dirname(yc.ENGINE), stdout=f, stderr=subprocess.STDOUT)
        shown = yc.wait_for(lambda: "overlay shown" in text(), 15, 0.2)
        mark(f"shown within 15 s: {bool(shown)}")
        if muted:
            time.sleep(6)
        else:
            time.sleep(4)
            yc.js(c, fs, "document.querySelector('video').pause();true", gesture=True)
            mark("paused")
            time.sleep(8)
            n = text().count("overlay shown")
            yc.js(c, fs, "document.querySelector('video').play();true", gesture=True)
            t0 = time.time()
            yc.wait_for(lambda: text().count("overlay shown") > n, 10, 0.1)
            mark(f"short pause: back in {time.time() - t0:.1f} s, disengaged: {'disengaging' in text()}")
            time.sleep(3)
            yc.js(c, fs, "document.querySelector('video').pause();true", gesture=True)
            mark("long pause")
            yc.wait_for(lambda: "disengaging" in text(), 45, 0.5)
            mark(f"long pause disengaged: {'disengaging' in text()}")
            n = text().count("overlay shown")
            yc.js(c, fs, "document.querySelector('video').play();true", gesture=True)
            t0 = time.time()
            yc.wait_for(lambda: text().count("overlay shown") > n, 15, 0.1)
            mark(f"after long pause: back in {time.time() - t0:.1f} s, new shows: {text().count('overlay shown') - n}")
    finally:
        if eng:
            mark(f"exit {fa.quit_engine(eng)}")
        try: c.send("Browser.close")
        except Exception: pass
        c.close(); time.sleep(2)
        if s.proc and s.proc.poll() is None: s.proc.terminate()
    lines = [l[:125] for l in text().splitlines() if re.search(r"media sessions|overlay shown|not treated as video|disengaging|still waiting|engaged, mode", l)]
    print("\n".join(lines[:16]), flush=True)
    print('lines with error:', [l[:120] for l in text().splitlines() if 'ERROR' in l.upper()][:4], flush=True)

for name, muted in (("muted", True), ("sound", False)):
    run(name, muted)
server.shutdown()
