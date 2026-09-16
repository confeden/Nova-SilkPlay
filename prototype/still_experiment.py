#!/usr/bin/env python3
"""Does a still shot inside a playing video break generation?

Needs the test page as a real <video> that alternates motion and stillness:
  run_testpage.py --url ".../testmotion.html?fps=24&input=1&video=record&seconds=10&move=2000&still=1500"

Runs silkplay.exe for ~14 s with the pointer parked off the page and reports how
often the overlay hid and re-showed (every hide/show is a visible jump between
the generated picture, ~1 source period late, and the browser's own), plus the
judder and the verdict counts. Extra arguments are passed to silkplay.exe.
"""
import collections
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import live_windows_test as t  # noqa: E402

u = t.user32
tgt = t.find_target()
if not tgt:
    print("open the test page first")
    sys.exit(2)
L, T, R, B = t.client_rect(tgt)
log_path = os.path.join(HERE, "still_experiment.log")
logf = open(log_path, "w", encoding="utf-8")
proc = subprocess.Popen([os.path.join(HERE, "silkplay.exe"), "--target-title", t.TITLE, "--log-dirty",
                         "--stats-every", "1", "--allow-windowed"] + sys.argv[1:], stdout=logf, stderr=subprocess.STDOUT)
saved = wt.POINT()
u.GetCursorPos(ctypes.byref(saved))
u.SetCursorPos(L - 200, T + 50)
t.pump(14000)
u.SetCursorPos(saved.x, saved.y)
t.send_keys([0x11, 0x12, 0x58])
try:
    proc.wait(timeout=5)
except subprocess.TimeoutExpired:
    proc.terminate()
logf.close()
text = open(log_path, encoding="utf-8", errors="replace").read()
print("overlay shown:", len(re.findall(r"overlay shown", text)))
print("source stalled -> hidden:", len(re.findall(r"source stalled", text)))
print("verdicts:", collections.Counter(re.findall(r"-> (VIDEO|ui) \(([^)]*)\)", text)).most_common())
for m in re.findall(r"judder: mean ([\d.]+) ms, >4 ms on (\d+) of (\d+) presents \(source holding (\d+), restarts (\d+)", text):
    print(f"  judder mean {m[0]} ms, big {m[1]}/{m[2]}, holding {m[3]}, restarts {m[4]}")
