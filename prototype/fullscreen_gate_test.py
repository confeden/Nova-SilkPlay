#!/usr/bin/env python3
"""Fullscreen-gate test for silkplay.exe.

Moves the mouse, clicks the test page and presses F11 to toggle fullscreen.
The test page ('Nova SilkPlay motion source') should be on the secondary monitor.
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import live_windows_test as t

LOG_PATH = os.path.join(HERE, "fullscreen_gate_test.log")

user32 = t.user32
user32.MonitorFromWindow.restype = wt.HANDLE
user32.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT),
                ("rcWork", wt.RECT), ("dwFlags", wt.DWORD)]


def monitor_rect(hwnd):
    """Return (L,T,R,B) of the monitor the window is on."""
    hmon = user32.MonitorFromWindow(hwnd, 2)  # MONITOR_DEFAULTTONEAREST
    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(MONITORINFO)
    user32.GetMonitorInfoW(hmon, ctypes.byref(mi))
    r = mi.rcMonitor
    return r.left, r.top, r.right, r.bottom


def is_fullscreen(hwnd):
    L, T, R, B = t.client_rect(hwnd)
    mL, mT, mR, mB = monitor_rect(hwnd)
    return (L, T, R, B) == (mL, mT, mR, mB)


def click_center(hwnd):
    L, T, R, B = t.client_rect(hwnd)
    cx, cy = (L + R) // 2, (T + B) // 2
    user32.SetCursorPos(cx, cy)
    t.send_mouse(2)   # LEFTDOWN
    t.send_mouse(4)   # LEFTUP


def log_text(logf):
    logf.flush()
    with open(LOG_PATH, encoding="utf-8", errors="replace") as f:
        return f.read()


def main():
    hwnd = t.find_target()
    if not hwnd:
        print(f"ABORT: no window with title '{t.TITLE}' found")
        return 2

    saved = wt.POINT()
    user32.GetCursorPos(ctypes.byref(saved))

    logf = open(LOG_PATH, "w", encoding="utf-8")
    proc = None
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}")

    try:
        # Step 2: ensure we start windowed
        if is_fullscreen(hwnd):
            click_center(hwnd)
            t.pump(300)
            t.send_keys([0x7A])   # F11
            t.pump(2500)

        # Step 3: launch silkplay
        cmd = [os.path.join(HERE, "silkplay.exe"), "--default-settings",
               "--target-title", t.TITLE, "--stats-every", "1"]
        proc = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=HERE)

        # Phase: windowed (4 s)
        t.pump(4000)
        txt = log_text(logf)
        check("windowed: waiting message present",
              "waiting for a fullscreen browser window" in txt)
        check("windowed: not engaged",
              "engaged, mode=" not in txt)
        check("windowed: no-window-matched not spammy",
              txt.count("no window matched") <= 2,
              f"count={txt.count('no window matched')}")

        # Phase: enter fullscreen
        mark = len(log_text(logf))
        click_center(hwnd)
        t.pump(300)
        t.send_keys([0x7A])   # F11
        deadline = time.perf_counter() + 8.0
        while time.perf_counter() < deadline:
            t.pump(200)
            if "overlay shown" in log_text(logf)[mark:]:
                break
        new = log_text(logf)[mark:]
        check("fullscreen: engaged", "engaged, mode=" in new)
        check("fullscreen: overlay shown", "overlay shown" in new)

        # Phase: leave fullscreen
        mark2 = len(log_text(logf))
        # client rect is now fullscreen; re-read it
        L, T, R, B = t.client_rect(hwnd)
        cx, cy = (L + R) // 2, (T + B) // 2
        user32.SetCursorPos(cx, cy)
        t.send_mouse(2)
        t.send_mouse(4)
        t.pump(300)
        t.send_keys([0x7A])   # F11
        t.pump(4000)
        seg = log_text(logf)[mark2:]
        detach_strings = ("target moved/resized", "left fullscreen")
        detach_found = any(d in seg for d in detach_strings)
        check("leave-fs: detach message present", detach_found)
        # Find last detach position
        last_detach = -1
        for d in detach_strings:
            idx = seg.rfind(d)
            if idx > last_detach:
                last_detach = idx
        after_detach = seg[last_detach:] if last_detach >= 0 else seg
        check("leave-fs: no re-show after detach",
              "overlay shown" not in after_detach)
        engaged_after = after_detach.find("engaged, mode=")
        check("leave-fs: no re-engage after detach",
              engaged_after == -1,
              f"pos={engaged_after}")

    finally:
        t.send_keys([0x11, 0x12, 0x58])   # Ctrl+Alt+X
        if proc is not None:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.terminate()
        logf.close()
        user32.SetCursorPos(saved.x, saved.y)

    passed = sum(results)
    total = len(results)
    print(f"{passed}/{total} passed; log: {LOG_PATH}")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
