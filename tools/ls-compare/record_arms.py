#!/usr/bin/env python3
"""Record the same analytic clip through three arms: the page alone, our engine,
and Lossless Scaling — on the SECONDARY monitor, fullscreen, byte-exact display
(G0, g0_display_check.py).

  python record_arms.py --scene-dir frames/A3 [--arms page,ours,ls] [--seconds 4]

Writes rec_<scene>_<arm>.raw/.csv next to this file; score them with
  python score.py --scene-dir frames/A3 --arm page=rec_A3_page --arm ours=rec_A3_ours --arm ls=rec_A3_ls

Every arm sees the same page in the same state. Our engine runs with its default
rules plus --no-badge (the watermark would sit inside the scored picture). LS
runs with the owner's own profile, untouched; it is started only if it is not
already running and closed again afterwards if this script started it.
Moves the cursor, clicks the page, presses F11, Ctrl+Alt+Q (LS) and Ctrl+Alt+X.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import g0_display_check as g  # noqa: E402

t = g.t
u = g.u
LS_EXE = r"D:\SteamLibrary\steamapps\common\Lossless Scaling\game\LosslessScaling.exe"
ENGINE = os.path.normpath(os.path.join(HERE, "..", "..", "prototype", "silkplay.exe"))


def pids_of(image):
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", f"IMAGENAME eq {image}"],
                         capture_output=True, text=True).stdout
    return [int(line.split('","')[1]) for line in out.splitlines()
            if line.lower().startswith(f'"{image.lower()}"')]


def ls_windows(pids):
    found = []
    u.MonitorFromWindow.restype = wt.HANDLE
    u.MonitorFromWindow.argtypes = [wt.HWND, wt.DWORD]

    def cb(h, _):
        pid = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in pids and u.IsWindowVisible(h) and not u.IsIconic(h):
            r = wt.RECT()
            u.GetWindowRect(h, ctypes.byref(r))
            found.append((h, (r.left, r.top, r.right, r.bottom)))
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(cb), 0)
    return found


def click_center(hw):
    L, T, R, B = t.client_rect(hw)
    u.SetCursorPos((L + R) // 2, (T + B) // 2)
    t.send_mouse(2)
    t.send_mouse(4)
    t.pump(300)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene-dir", default=os.path.join(HERE, "frames", "A3"))
    ap.add_argument("--arms", default="page,ours,ls")
    ap.add_argument("--seconds", type=float, default=4.0)
    args = ap.parse_args()

    saved = wt.POINT()
    u.GetCursorPos(ctypes.byref(saved))
    hw, clip = g.open_player(args.scene_dir)
    w, h = clip["size"]
    scene = clip["scene"]
    time.sleep(3.0 + clip["frames"] * 0.02)
    started_ls = False
    try:
        rect = g.fullscreen(hw, saved)
        if (rect[2] - rect[0], rect[3] - rect[1]) != (1920, 1080):
            print("not fullscreen:", rect)
            return 2
        time.sleep(4.0)
        for arm in args.arms.split(","):
            prefix = os.path.join(HERE, f"rec_{scene}_{arm}")
            print(f"--- {arm}")
            if arm == "page":
                g.record(rect, w, h, args.seconds, prefix)
            elif arm == "ours":
                log = open(prefix + "_engine.log", "w", encoding="utf-8")
                proc = subprocess.Popen([ENGINE, "--default-settings", "--no-badge", "--stats-every", "2"], stdout=log,
                                        stderr=subprocess.STDOUT, cwd=os.path.dirname(ENGINE))
                deadline = time.time() + 20
                while time.time() < deadline:
                    t.pump(250)
                    if "overlay shown" in open(prefix + "_engine.log", encoding="utf-8", errors="replace").read():
                        break
                t.pump(2500)
                g.record(rect, w, h, args.seconds, prefix)
                t.send_keys([0x11, 0x12, 0x58])
                try:
                    proc.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                log.close()
                print("  engine:", [l[13:150] for l in open(prefix + "_engine.log", encoding="utf-8",
                                                            errors="replace") if " | out " in l][-2:])
            elif arm == "ls":
                if not pids_of("LosslessScaling.exe"):
                    subprocess.Popen([LS_EXE], cwd=os.path.dirname(LS_EXE))
                    started_ls = True
                    for _ in range(40):
                        t.pump(250)
                        if ls_windows(pids_of("LosslessScaling.exe")):
                            break
                    t.pump(2000)
                for hwin, _ in ls_windows(pids_of("LosslessScaling.exe")):
                    u.ShowWindow(hwin, 6)
                t.pump(500)
                click_center(hw)
                t.send_keys([0x11, 0x12, 0x51])
                t.pump(3000)
                u.SetCursorPos(saved.x, saved.y)
                mon = u.MonitorFromWindow(hw, 2)
                wins = ls_windows(pids_of("LosslessScaling.exe"))
                stray = [r for hwin, r in wins if u.MonitorFromWindow(hwin, 2) != mon]
                if stray:
                    print("ABORT: an LS window is on another monitor:", stray)
                    return 3
                if not any(r[2] - r[0] >= 1900 for _, r in wins):
                    print("ABORT: no LS output window appeared", wins)
                    return 3
                t.pump(2000)
                g.record(rect, w, h, args.seconds, prefix)
                click_center(hw)
                t.send_keys([0x11, 0x12, 0x51])
                t.pump(1500)
                u.SetCursorPos(saved.x, saved.y)
            t.pump(1500)
    finally:
        if started_ls and pids_of("LosslessScaling.exe"):
            subprocess.run(["taskkill", "/IM", "LosslessScaling.exe"], capture_output=True)
            t.pump(1500)
            if pids_of("LosslessScaling.exe"):
                subprocess.run(["taskkill", "/F", "/IM", "LosslessScaling.exe"], capture_output=True)
        rect = t.client_rect(hw)
        if (rect[2] - rect[0], rect[3] - rect[1]) == (1920, 1080):
            click_center(hw)
            t.send_keys([0x7A])
            t.pump(800)
        u.SetCursorPos(saved.x, saved.y)
        u.PostMessageW(hw, 0x0010, 0, 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
