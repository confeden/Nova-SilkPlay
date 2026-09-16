#!/usr/bin/env python3
"""Go/no-go probe: can Lossless Scaling's output be recorded at all?

kb/quality-harness.md blocks every LS comparison on this question. LS imports
SetWindowDisplayAffinity; if its output window is excluded from capture, Desktop
Duplication returns the desktop UNDER it — the un-generated source — and any
comparison would report beating LS by an infinite margin.

Procedure (everything on the SECONDARY monitor; the owner works on the primary):
  1. the deterministic test page, fullscreen, 24 fps canvas;
  2. record 3 s with Desktop Duplication — the baseline, ~24 distinct pictures/s;
  3. start Lossless Scaling if needed, activate the page, press its hotkey;
     abort at once if any LS window lands on another monitor;
  4. read GetWindowDisplayAffinity of every LS window, record 3 s again;
  5. stop LS scaling, close LS if this script started it, leave fullscreen.

Gates, all must pass for a pixel comparison to be sound:
  A  no LS output window carries WDA_EXCLUDEFROMCAPTURE (0x11) or WDA_MONITOR;
  B  the LS recording shows clearly MORE distinct pictures per second than the
     baseline (generated frames are really in the recording);
  C  the source was not throttled: the baseline's distinct rate is ~24/s;
  D  the LS recording is not the baseline picture: its frames differ from the
     page's own frames (an LS FPS counter or scaling would show here).
It moves the cursor, clicks the test page and presses F11 and Ctrl+Alt+Q.
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.normpath(os.path.join(HERE, "..", "..", "prototype"))
sys.path.insert(0, PROTO)
import live_windows_test as t  # noqa: E402

LS_EXE = r"D:\SteamLibrary\steamapps\common\Lossless Scaling\game\LosslessScaling.exe"
u = t.user32
u.GetWindowDisplayAffinity.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.MonitorFromWindow.restype = wt.HANDLE
u.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]
u.MonitorFromPoint.restype = wt.HANDLE


def pids_of(image):
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", f"IMAGENAME eq {image}"],
                         capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == image.lower():
            pids.append(int(parts[1]))
    return pids


def windows_of(pids):
    found = []
    cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        pid = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in pids and u.IsWindowVisible(h):
            r = wt.RECT()
            u.GetWindowRect(h, ctypes.byref(r))
            aff = wt.DWORD(0)
            u.GetWindowDisplayAffinity(h, ctypes.byref(aff))
            b = ctypes.create_unicode_buffer(128)
            u.GetClassNameW(h, b, 128)
            found.append((h, b.value, (r.left, r.top, r.right, r.bottom), aff.value))
        return True

    u.EnumWindows(cb_t(cb), 0)
    return found


def record(at, seconds, prefix):
    exe = os.path.join(HERE, "ddacap.exe")
    r = subprocess.run([exe, "--at", f"{at[0]},{at[1]}", "--rect", "0,0,1920,1080", "--seconds", str(seconds),
                        "--out", prefix], capture_output=True, text=True)
    print("  ddacap:", r.stdout.strip(), r.stderr.strip())
    rows = []
    with open(prefix + ".csv", encoding="utf-8") as f:
        for line in f:
            if line[0].isdigit():
                idx, qpc, lp, acc, h = line.strip().split(",")
                rows.append((int(idx), float(qpc), float(lp), int(acc), h))
    return rows


def distinct_per_second(rows):
    if len(rows) < 2:
        return 0.0, 0
    changes = sum(1 for a, b in zip(rows, rows[1:]) if a[4] != b[4])
    span = (rows[-1][1] - rows[0][1]) / 1000.0
    return changes / span if span > 0 else 0.0, sum(r[3] - 1 for r in rows if r[3] > 1)


def save_png(prefix, index, path):
    import numpy as np
    from PIL import Image
    with open(prefix + ".raw", "rb") as f:
        f.seek(index * 1920 * 1080 * 4)
        buf = np.frombuffer(f.read(1920 * 1080 * 4), dtype=np.uint8).reshape(1080, 1920, 4)
    Image.fromarray(buf[:, :, 2::-1]).save(path)


def click(x, y):
    u.SetCursorPos(x, y)
    t.send_mouse(2)
    t.send_mouse(4)
    t.pump(250)


def main():
    tgt = t.find_target()
    if not tgt:
        print("open the test page first (prototype/run_testpage.py)")
        return 2
    saved = wt.POINT()
    u.GetCursorPos(ctypes.byref(saved))
    started_ls = False
    results = []

    def gate(name, ok, detail):
        results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")

    L, T, R, B = t.client_rect(tgt)
    try:
        click((L + R) // 2, (T + B) // 2)
        t.send_keys([0x7A])  # F11
        t.pump(2500)
        L, T, R, B = t.client_rect(tgt)
        at = ((L + R) // 2, (T + B) // 2)
        if (R - L, B - T) != (1920, 1080):
            print("the page did not go fullscreen at 1920x1080:", (L, T, R, B))
            return 2
        u.SetCursorPos(saved.x, saved.y)
        t.pump(1500)

        print("baseline (page only):")
        base = record(at, 3, os.path.join(HERE, "probe_base"))
        base_rate, base_missed = distinct_per_second(base)
        print(f"  {len(base)} frames, {base_rate:.1f} distinct/s, {base_missed} coalesced")

        if not pids_of("LosslessScaling.exe"):
            subprocess.Popen([LS_EXE], cwd=os.path.dirname(LS_EXE))
            started_ls = True
            for _ in range(40):
                t.pump(250)
                if windows_of(pids_of("LosslessScaling.exe")):
                    break
            t.pump(2000)
        ls_pids = pids_of("LosslessScaling.exe")
        if not ls_pids:
            print("Lossless Scaling did not start")
            return 2
        before = {w[0] for w in windows_of(ls_pids)}
        for h in before:
            u.ShowWindow(h, 6)  # SW_MINIMIZE its UI so it is not in the way
        t.pump(500)

        click(*at)
        t.send_keys([0x11, 0x12, 0x51])  # Ctrl+Alt+Q: LS's own hotkey (silkplay is not running)
        t.pump(3000)
        u.SetCursorPos(saved.x, saved.y)

        wins = windows_of(ls_pids)
        print("LS windows now:", [(hex(h), c, r, hex(a)) for h, c, r, a in wins])
        mon_page = u.MonitorFromWindow(tgt, 2)
        stray = []
        for h, c, r, a in wins:
            if u.IsIconic(h):
                continue
            if u.MonitorFromWindow(h, 2) != mon_page and (r[2] - r[0]) * (r[3] - r[1]) > 200 * 200:
                stray.append((hex(h), c, r))
        if stray:
            print("ABORT: an LS window is on another monitor:", stray)
            return 3
        output = [w for w in wins if not u.IsIconic(w[0]) and (w[2][2] - w[2][0]) >= 1900]
        gate("A no LS output window is excluded from capture",
             bool(output) and all(w[3] not in (0x01, 0x11) for w in output),
             f"output windows {[(hex(w[0]), w[1], hex(w[3])) for w in output] or 'NONE FOUND'}")

        print("LS on:")
        ls = record(at, 3, os.path.join(HERE, "probe_ls"))
        ls_rate, ls_missed = distinct_per_second(ls)
        print(f"  {len(ls)} frames, {ls_rate:.1f} distinct/s, {ls_missed} coalesced")
        gate("B the LS recording carries generated frames", ls_rate > base_rate * 1.8,
             f"{ls_rate:.1f}/s against baseline {base_rate:.1f}/s")
        gate("C the source was not throttled", 20.0 <= base_rate <= 28.0, f"baseline {base_rate:.1f}/s")
        overlap = len({r[4] for r in ls} & {r[4] for r in base})
        gate("D the LS pictures are not the page's own", overlap < len(ls) * 0.1,
             f"{overlap} of {len(ls)} LS frames hash-identical to a baseline frame")
        for name, rows, prefix in (("base", base, "probe_base"), ("ls", ls, "probe_ls")):
            if rows:
                save_png(os.path.join(HERE, prefix), len(rows) // 2, os.path.join(HERE, f"probe_{name}_mid.png"))
    finally:
        if pids_of("LosslessScaling.exe"):
            tgt_rect = t.client_rect(tgt)
            click((tgt_rect[0] + tgt_rect[2]) // 2, (tgt_rect[1] + tgt_rect[3]) // 2)
            t.send_keys([0x11, 0x12, 0x51])  # stop scaling
            t.pump(1500)
            if started_ls:
                subprocess.run(["taskkill", "/IM", "LosslessScaling.exe"], capture_output=True)
                t.pump(1500)
                if pids_of("LosslessScaling.exe"):
                    subprocess.run(["taskkill", "/F", "/IM", "LosslessScaling.exe"], capture_output=True)
        tr = t.client_rect(tgt)
        if (tr[2] - tr[0], tr[3] - tr[1]) == (1920, 1080):
            click((tr[0] + tr[2]) // 2, (tr[1] + tr[3]) // 2)
            t.send_keys([0x7A])
            t.pump(1500)
        u.SetCursorPos(saved.x, saved.y)
    print(f"{sum(results)}/{len(results)} gates passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
