#!/usr/bin/env python3
"""Live check of the two window behaviours the owner asked for (2026-09-13).

  1. The pointer on the video must not stop frame generation, and the page under
     the overlay must still get every move, click and wheel notch.
  2. Generation must pause while any window covers even part of the video, and
     resume when it is gone. Slivers at the edge (< 8 px deep) do not count.

Runs silkplay.exe against the test page (launch it first with
run_testpage.py --url ".../testmotion.html?fps=24&input=1", which keeps it on the
secondary monitor), drives real input and real windows over it, and reads the
engine's log. It moves the cursor and clicks the TEST PAGE, restoring the cursor
position afterwards.

  python live_windows_test.py [--exe silkplay.exe] [--keep-log]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TITLE = "Nova SilkPlay motion source"

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
user32.DefWindowProcW.restype = LRESULT
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMENU,
                                   wt.HINSTANCE, wt.LPVOID]
user32.GetWindow.restype = wt.HWND
user32.GetWindow.argtypes = [wt.HWND, wt.UINT]
user32.FindWindowW.restype = wt.HWND
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, wt.UINT]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH), ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def qpc_ms():
    c = ctypes.c_int64()
    f = ctypes.c_int64()
    kernel32.QueryPerformanceCounter(ctypes.byref(c))
    kernel32.QueryPerformanceFrequency(ctypes.byref(f))
    return c.value * 1000.0 / f.value


def pump(ms):
    msg = wt.MSG()
    end = time.perf_counter() + ms / 1000.0
    while time.perf_counter() < end:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.002)


def find_target():
    found = []
    cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        if user32.IsWindowVisible(h):
            b = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(h, b, 256)
            if TITLE in b.value:
                found.append(h)
                return False
        return True

    user32.EnumWindows(cb_t(cb), 0)
    return found[0] if found else None


def client_rect(h):
    r = wt.RECT()
    user32.GetClientRect(h, ctypes.byref(r))
    tl = wt.POINT(r.left, r.top)
    br = wt.POINT(r.right, r.bottom)
    user32.ClientToScreen(h, ctypes.byref(tl))
    user32.ClientToScreen(h, ctypes.byref(br))
    return tl.x, tl.y, br.x, br.y


def page_counts(h):
    b = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(h, b, 256)
    m = re.search(r"\| d(\d+) u(\d+) m(\d+) w(\d+)", b.value)
    return tuple(int(x) for x in m.groups()) if m else None


def send_mouse(flags, dx=0, dy=0, data=0):
    i = INPUT(type=0)
    i.mi = MOUSEINPUT(dx, dy, data & 0xFFFFFFFF, flags, 0, 0)
    user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def send_keys(vks):
    seq = []
    for vk in vks:
        i = INPUT(type=1)
        i.ki = KEYBDINPUT(vk, 0, 0, 0, 0)
        seq.append(i)
    for vk in reversed(vks):
        i = INPUT(type=1)
        i.ki = KEYBDINPUT(vk, 0, 2, 0, 0)
        seq.append(i)
    arr = (INPUT * len(seq))(*seq)
    user32.SendInput(len(seq), arr, ctypes.sizeof(INPUT))


_keep = []


def make_window(x, y, w, h, popup, topmost=False):
    hinst = kernel32.GetModuleHandleW(None)
    cls = "NspLiveOccluder"
    if not _keep:
        proc = WNDPROC(lambda hw, m, wp, lp: user32.DefWindowProcW(hw, m, wp, lp))
        wc = WNDCLASSW()
        wc.lpfnWndProc = proc
        wc.hInstance = hinst
        gdi32 = ctypes.WinDLL("gdi32")
        gdi32.GetStockObject.restype = wt.HGDIOBJ
        wc.hbrBackground = gdi32.GetStockObject(0)  # white
        wc.lpszClassName = cls
        user32.RegisterClassW(ctypes.byref(wc))
        _keep.extend([proc, wc])
    WS_POPUP, WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW = 0x80000000, 0x00CF0000, 0x80
    style = WS_POPUP if popup else WS_OVERLAPPEDWINDOW
    hw = user32.CreateWindowExW(WS_EX_TOOLWINDOW | (0x8 if topmost else 0), cls, "occluder", style,
                                x, y, w, h, None, None, hinst, None)
    return hw


_target = None


def show(hw):
    user32.ShowWindow(hw, 4)  # SW_SHOWNOACTIVATE
    # A window shown without activation by a background process lands BELOW the
    # foreground browser, and SetWindowPos(HWND_TOP) "succeeds" without moving it
    # (measured: the first run's occluders were all under Chrome, and the engine
    # rightly ignored them). Inserting after the window directly above the
    # browser is allowed, and is what a window stacked over the video looks like.
    # A topmost window goes to the top of its band with HWND_TOPMOST, which is
    # allowed from the background; inserting it after a normal window would
    # strip its topmost bit.
    topmost = bool(user32.GetWindowLongW(hw, -20) & 0x8)
    after = wt.HWND(-1) if topmost else user32.GetWindow(_target, 3)
    user32.SetWindowPos(hw, after, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
    t = qpc_ms()
    pump(30)
    return t


def is_above(a, b):
    w = user32.GetWindow(b, 3)  # GW_HWNDPREV
    while w:
        if w == a:
            return True
        w = user32.GetWindow(w, 3)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(HERE, "silkplay.exe"))
    ap.add_argument("--keep-log", action="store_true")
    ap.add_argument("--extra", default="", help="extra silkplay arguments")
    args = ap.parse_args()

    global _target
    target = find_target()
    _target = target
    if not target or not page_counts(target):
        print("open the test page with ?input=1 first (run_testpage.py --url ...)")
        return 2
    L, T, R, B = client_rect(target)
    cx, cy = (L + R) // 2, (T + B) // 2
    print(f"target 0x{target:X} client {L},{T}-{R},{B}")

    log_path = os.path.join(HERE, "live_windows_test.log")
    logf = open(log_path, "w", encoding="utf-8")
    # --allow-windowed: the test page is normally a window; the fullscreen-only
    # rule has its own test (fullscreen_gate_test.py).
    cmd = [args.exe, "--default-settings", "--target-title", TITLE, "--stats-every", "2", "--allow-windowed"] + args.extra.split()
    proc = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=HERE)

    def log_text():
        with open(log_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}")

    try:
        t_end = time.time() + 20
        while time.time() < t_end and "overlay shown" not in log_text():
            time.sleep(0.2)
        check("engine engaged and showed the overlay", "overlay shown" in log_text())
        time.sleep(1.0)

        # ---- 1. pointer on the video, real input
        saved = wt.POINT()
        user32.GetCursorPos(ctypes.byref(saved))
        before = page_counts(target)
        mark = len(log_text())
        # Foreground away from the browser first, so the click below really
        # activates it and the z-order correction is exercised every run.
        user32.SetForegroundWindow(user32.GetShellWindow())
        pump(150)
        user32.SetCursorPos(cx, cy)
        for i in range(60):  # 3 s of small moves over the video
            send_mouse(0x0001, 3 if i % 2 else -3, 0)
            pump(50)
        send_mouse(0x0002)
        send_mouse(0x0004)
        pump(60)
        send_mouse(0x0800, data=-120)
        pump(400)
        for i in range(20):  # keep the pointer there another second
            send_mouse(0x0001, 2 if i % 2 else -2, 0)
            pump(50)
        after = page_counts(target)
        user32.SetCursorPos(saved.x, saved.y)
        d = [a - b for a, b in zip(after, before)]
        check("page got the click, moves and wheel through the overlay",
              d[0] >= 1 and d[1] >= 1 and d[2] >= 20 and d[3] >= 1, f"down/up/move/wheel {d}")
        seg = log_text()[mark:]
        check("pointer on the video did not pause generation",
              "pointer is on the video" not in seg and "PAUSED" not in seg)
        stats = re.findall(r"\| (SHOWN|hidden)\n", seg)
        check("overlay stayed SHOWN in every stats line while the pointer was on the video",
              len(stats) >= 1 and all(s == "SHOWN" for s in stats), f"{stats}")
        hits = re.findall(r"hit tests on overlay (\d+)", seg)
        check("overlay took no hit tests", hits and all(h == "0" for h in hits), f"{hits}")
        ov = user32.FindWindowW("NovaSilkPlayOverlay", None)
        check("overlay is back directly above the browser after the click activated it",
              bool(ov) and user32.GetWindow(target, 3) == ov,
              f"prev=0x{(user32.GetWindow(target, 3) or 0):X} overlay=0x{(ov or 0):X}")
        zf = re.findall(r"z-order fixes (\d+)", seg)
        check("z-order was corrected at least once (the click raised Chrome)",
              any(int(z) > 0 for z in zf), f"{zf}")

        # ---- 2. windows over the video
        def occlusion_case(name, rect, popup, expect_pause, topmost=False):
            mark = len(log_text())
            hw = make_window(*rect, popup, topmost)
            t_show = show(hw)
            pump(1500)
            above = is_above(hw, target)
            seg = log_text()[mark:]
            paused = "generation PAUSED" in seg
            lat = None
            m = re.search(r"generation PAUSED\n.*?\(qpc ([\d.]+) ms\)", seg)
            if m:
                lat = float(m.group(1)) - t_show
            if expect_pause:
                check(f"{name}: generation paused", paused and above,
                      f"above={above} reaction={lat if lat is None else round(lat, 1)} ms")
            else:
                check(f"{name}: generation NOT paused", not paused, f"above={above}")
            mark2 = len(log_text())
            t_hide = qpc_ms()
            user32.DestroyWindow(hw)
            pump(1500)
            seg2 = log_text()[mark2:]
            if expect_pause:
                m2 = re.search(r"generation resumes\n.*?\(qpc ([\d.]+) ms\)", seg2)
                check(f"{name}: generation resumed after the window closed", bool(m2),
                      f"reaction={round(float(m2.group(1)) - t_hide, 1) if m2 else None} ms")
            return lat

        W, H = R - L, B - T
        occlusion_case("framed window over the right quarter",
                       (L + W * 3 // 4, T + H // 4, W // 2, H // 2), False, True)
        occlusion_case("small popup fully inside the picture",
                       (L + W // 3, T + H // 3, 120, 40), True, True)
        occlusion_case("5 px sliver at the right edge", (R - 5, T + 50, 300, 200), True, False)
        mark_w = len(log_text())
        occlusion_case("small always-on-top widget (default: ignored)",
                       (L + W // 3, T + H // 3, 160, 60), True, False, topmost=True)
        check("the ignored widget was logged", "ignoring always-on-top widget" in log_text()[mark_w:])
        occlusion_case("big always-on-top window (30 % of the picture)",
                       (L + W // 4, T + H // 4, W * 55 // 100, H * 55 // 100), True, True, topmost=True)

        # a window moved in programmatically: no event fires, the poll must find it
        mark = len(log_text())
        hw = make_window(R + 20, T + 60, 260, 160, True)
        show(hw)
        pump(300)
        t_in = None
        for x in range(R + 20, R - 200, -10):
            user32.SetWindowPos(hw, None, x, T + 60, 0, 0, 0x0001 | 0x0004 | 0x0010)
            if t_in is None and x <= R - 8:
                t_in = qpc_ms()
            pump(16)
        pump(800)
        seg = log_text()[mark:]
        m = re.search(r"generation PAUSED\n.*?\(qpc ([\d.]+) ms\)", seg)
        check("window slid in by SetWindowPos (no event): paused", bool(m),
              f"reaction={round(float(m.group(1)) - t_in, 1) if m and t_in else None} ms")
        user32.DestroyWindow(hw)
        pump(800)

        tail = log_text()
        costs = re.findall(r"occlusion tests (\d+) \(avg (\d+) us, max (\d+) us\)", tail)
        if costs:
            print("occlusion test cost per stats window (n, avg us, max us):", costs[-4:])
        planes = re.findall(r"\| plane (\w+) \|", tail)
        print("plane per stats line:", planes[-6:])
    finally:
        send_keys([0x11, 0x12, 0x58])  # Ctrl+Alt+X
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
        logf.close()

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed; log: {log_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
