#!/usr/bin/env python3
"""What do WGC dirty regions look like for video frames vs. player-UI updates?

Runs silkplay.exe --log-dirty against testmotion.html?input=1&controls=1 and
drives the pointer through the phases a YouTube viewer goes through: idle, moving
(controls fade in), still (controls up, progress ticking), waiting (fade out), a
click (centre flash). Prints, per phase, the arrival count and the dirty-rect
shapes, so the classifier can be designed from what DWM actually reports.
"""
import collections, ctypes, ctypes.wintypes as wt, os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_windows_test as t

u = t.user32
tgt = t.find_target()
L, T, R, B = t.client_rect(tgt)
cx, cy = (L + R) // 2, (T + B) // 2
log_path = "dirty_experiment.log"
logf = open(log_path, "w", encoding="utf-8")
proc = subprocess.Popen([os.path.join(os.path.dirname(os.path.abspath(__file__)), "silkplay.exe"), "--target-title", t.TITLE, "--log-dirty", "--stats-every", "0.5", "--allow-windowed"] + sys.argv[1:],
                        stdout=logf, stderr=subprocess.STDOUT)
def text():
    return open(log_path, encoding="utf-8", errors="replace").read()
end = time.time() + 20
while time.time() < end and "overlay shown" not in text():
    time.sleep(0.2)
saved = wt.POINT(); u.GetCursorPos(ctypes.byref(saved))
# park the pointer OFF the page first
u.SetCursorPos(L - 200, T + 50)
t.pump(3000)
marks = []
def mark(name): marks.append((name, len(text())))
mark("idle")
t.pump(1500)
mark("moving")
u.SetCursorPos(cx, cy)
for i in range(20):
    t.send_mouse(1, 4 if i % 2 else -4, 0); t.pump(50)
mark("still-controls-up")
t.pump(1500)
mark("fade-out")
t.pump(1800)
mark("idle-after")
t.pump(1500)
mark("click")
t.send_mouse(2); t.send_mouse(4)
t.pump(700)
mark("end")
u.SetCursorPos(saved.x, saved.y)
t.send_keys([0x11, 0x12, 0x58])
try: proc.wait(timeout=5)
except subprocess.TimeoutExpired: proc.terminate()
logf.close()
full = text()
for (name, a), (_, b) in zip(marks, marks[1:]):
    seg = full[a:b]
    lines = [(dt, n, "0", "0", rest) for dt, n, rest in re.findall(r"\[frame\] dt\s+([\d.]+) ms\s+n (-?\d+) (.*)", seg)]
    verdicts = collections.Counter(re.findall(r"-> (VIDEO|ui) \(([^)]*)\)", seg))
    for (v, why), c in verdicts.most_common():
        print(f"   verdict {v:5s} x{c:3d}  ({why})")
    shapes = collections.Counter()
    dts = []
    for dt, n, sw, sh, rects in lines:
        dts.append(float(dt))
        rs = re.findall(r"\[(-?\d+),(-?\d+) (\d+)x(\d+)\]", rects)
        shapes[" ".join(f"{x},{y} {w}x{h}" for x, y, w, h in rs) or f"n={n}"] += 1
    dur = 0
    print(f"--- {name}: {len(lines)} frames, dt min/med/max {min(dts) if dts else 0:.1f}/{sorted(dts)[len(dts)//2] if dts else 0:.1f}/{max(dts) if dts else 0:.1f} ms")
    for s, c in shapes.most_common(5):
        print(f"   {c:4d} x  {s}")
    for m in re.findall(r"judder: mean ([\d.]+) ms, >4 ms on (\d+) of (\d+)", seg):
        print(f"   judder mean {m[0]} ms, big {m[1]}/{m[2]}")
    for m in re.findall(r"est\. source ([\d.]+) fps.*?held (\d+)", seg):
        print(f"   est source {m[0]} fps, held {m[1]}")
