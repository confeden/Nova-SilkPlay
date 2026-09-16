#!/usr/bin/env python3
"""Dump top-level windows in z-order above a target window with visual coverage attributes."""
import argparse, ctypes, os, sys
from ctypes import wintypes

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

user32, dwmapi, kernel32, gdi32 = ctypes.windll.user32, ctypes.windll.dwmapi, ctypes.windll.kernel32, ctypes.windll.gdi32

try:
    user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user32.SetProcessDpiAwarenessContext.restype = wintypes.BOOL
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

COLORREF = getattr(wintypes, "COLORREF", wintypes.DWORD)
HRESULT = getattr(wintypes, "HRESULT", ctypes.c_long)

user32.GetTopWindow.argtypes, user32.GetTopWindow.restype = [wintypes.HWND], wintypes.HWND
user32.GetWindow.argtypes, user32.GetWindow.restype = [wintypes.HWND, wintypes.UINT], wintypes.HWND
user32.IsWindowVisible.argtypes, user32.IsWindowVisible.restype = [wintypes.HWND], wintypes.BOOL
user32.IsIconic.argtypes, user32.IsIconic.restype = [wintypes.HWND], wintypes.BOOL
user32.GetWindowTextLengthW.argtypes, user32.GetWindowTextLengthW.restype = [wintypes.HWND], ctypes.c_int
user32.GetWindowTextW.argtypes, user32.GetWindowTextW.restype = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int], ctypes.c_int
user32.GetClassNameW.argtypes, user32.GetClassNameW.restype = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int], ctypes.c_int
user32.GetWindowThreadProcessId.argtypes, user32.GetWindowThreadProcessId.restype = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)], wintypes.DWORD
user32.GetWindowLongW.argtypes, user32.GetWindowLongW.restype = [wintypes.HWND, ctypes.c_int], wintypes.LONG
user32.GetLayeredWindowAttributes.argtypes = [wintypes.HWND, ctypes.POINTER(COLORREF), ctypes.POINTER(wintypes.BYTE), ctypes.POINTER(wintypes.DWORD)]
user32.GetLayeredWindowAttributes.restype = wintypes.BOOL
user32.GetClientRect.argtypes, user32.GetClientRect.restype = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], wintypes.BOOL
user32.ClientToScreen.argtypes, user32.ClientToScreen.restype = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)], wintypes.BOOL
user32.GetWindowRect.argtypes, user32.GetWindowRect.restype = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], wintypes.BOOL
user32.GetWindowRgnBox.argtypes, user32.GetWindowRgnBox.restype = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], ctypes.c_int
dwmapi.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
dwmapi.DwmGetWindowAttribute.restype = HRESULT
kernel32.OpenProcess.argtypes, kernel32.OpenProcess.restype = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes, kernel32.CloseHandle.restype = [wintypes.HANDLE], wintypes.BOOL

EX_FLAG_DEFS = [
    ("TOPMOST", 0x8), ("TRANSPARENT", 0x20), ("TOOLWINDOW", 0x80),
    ("LAYERED", 0x80000), ("NOACTIVATE", 0x8000000),
    ("NOREDIRECTIONBITMAP", 0x200000), ("APPWINDOW", 0x40000),
]
RGN_NAMES = {0: "ERROR/NOREGION", 1: "NULLREGION", 2: "SIMPLEREGION", 3: "COMPLEXREGION"}

def get_exe_name(pid):
    if not pid: return "?"
    h = kernel32.OpenProcess(0x1000, False, pid)
    if not h: return "?"
    try:
        buf, size = ctypes.create_unicode_buffer(1024), wintypes.DWORD(1024)
        return os.path.basename(buf.value) if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)) else "?"
    except Exception: return "?"
    finally: kernel32.CloseHandle(h)

def get_win_title(hwnd):
    try:
        buf = ctypes.create_unicode_buffer(user32.GetWindowTextLengthW(hwnd) + 1)
        user32.GetWindowTextW(hwnd, buf, len(buf))
        return buf.value
    except Exception: return ""

def get_win_class(hwnd):
    try:
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, len(buf))
        return buf.value
    except Exception: return "?"

def get_win_pid(hwnd):
    try:
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value
    except Exception: return 0

def calc_intersect(r1, r2):
    if not r1 or not r2: return 0
    w = max(0, min(r1[2], r2[2]) - max(r1[0], r2[0]))
    h = max(0, min(r1[3], r2[3]) - max(r1[1], r2[1]))
    return w * h

def main():
    parser = argparse.ArgumentParser(description="Dump top-level windows in z-order")
    parser.add_argument("--title", default="Nova SilkPlay motion source", help="Window title substring")
    parser.add_argument("--exe", default=None, help="Process executable basename filter")
    parser.add_argument("--all", action="store_true", help="Walk past target to bottom of z-order")
    args = parser.parse_args()

    target_hwnd = None
    wnd_enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def enum_cb(hwnd, _):
        nonlocal target_hwnd
        try:
            if not user32.IsWindowVisible(hwnd): return True
            title = get_win_title(hwnd)
            if args.title.lower() in title.lower():
                pid = get_win_pid(hwnd)
                exe = get_exe_name(pid)
                if args.exe and args.exe.lower() != exe.lower(): return True
                target_hwnd = hwnd
                return False
        except Exception: pass
        return True

    user32.EnumWindows.argtypes, user32.EnumWindows.restype = [wnd_enum_proc, wintypes.LPARAM], wintypes.BOOL
    user32.EnumWindows(wnd_enum_proc(enum_cb), 0)

    if not target_hwnd:
        print(f"Target window not found (title: '{args.title}', exe: '{args.exe}').", file=sys.stderr)
        sys.exit(2)

    t_pid, t_exe, t_cls, t_title = get_win_pid(target_hwnd), get_exe_name(get_win_pid(target_hwnd)), get_win_class(target_hwnd), get_win_title(target_hwnd)
    rc_c = wintypes.RECT()
    user32.GetClientRect(target_hwnd, ctypes.byref(rc_c))
    pt_tl, pt_br = wintypes.POINT(rc_c.left, rc_c.top), wintypes.POINT(rc_c.right, rc_c.bottom)
    user32.ClientToScreen(target_hwnd, ctypes.byref(pt_tl))
    user32.ClientToScreen(target_hwnd, ctypes.byref(pt_br))
    target_client_screen = (pt_tl.x, pt_tl.y, pt_br.x, pt_br.y)
    tc_str = f"{pt_tl.x},{pt_tl.y} {pt_br.x - pt_tl.x}x{pt_br.y - pt_tl.y}"

    rc_w = wintypes.RECT()
    user32.GetWindowRect(target_hwnd, ctypes.byref(rc_w))
    t_wr_str = f"{rc_w.left},{rc_w.top} {rc_w.right - rc_w.left}x{rc_w.bottom - rc_w.top}"

    rc_e = wintypes.RECT()
    hr_e = dwmapi.DwmGetWindowAttribute(target_hwnd, 9, ctypes.byref(rc_e), ctypes.sizeof(rc_e))
    t_efb_str = f"{rc_e.left},{rc_e.top} {rc_e.right - rc_e.left}x{rc_e.bottom - rc_e.top}" if hr_e == 0 else "err"

    print(f"Target HWND:         0x{target_hwnd:08X}")
    print(f"Target PID:          {t_pid}\nTarget EXE:          {t_exe}\nTarget Class:        {t_cls}\nTarget Title:        {t_title}")
    print(f"Target Client (Scr): {tc_str}\nTarget WindowRect:   {t_wr_str}\nTarget ExtFrameBnds: {t_efb_str}\n")

    headers = [
        "IDX", "HWND", "PID", "EXE", "CLASS", "TITLE", "V", "I", "CLOAKED",
        "STYLE", "EXSTYLE", "EX_FLAGS", "LAYERED", "RECT", "EXT_FRAME", "OWNER",
        "REGION", "OVERLAP",
    ]
    print(" | ".join(headers))

    w, idx = user32.GetTopWindow(None), 0
    while w:
        w_val = w if isinstance(w, int) else getattr(w, "value", 0)
        if not w_val or (not args.all and w_val == target_hwnd): break

        try:
            w_pid, w_exe, w_cls = get_win_pid(w_val), get_exe_name(get_win_pid(w_val)), get_win_class(w_val)
            w_title = get_win_title(w_val).replace("\r", " ").replace("\n", " ")[:30]
            vis = "V" if user32.IsWindowVisible(w_val) else "-"
            iconic = "I" if user32.IsIconic(w_val) else "-"

            cloaked = wintypes.DWORD()
            hr_c = dwmapi.DwmGetWindowAttribute(w_val, 14, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
            cloaked_str = str(cloaked.value) if hr_c == 0 else "err"

            style, exstyle = user32.GetWindowLongW(w_val, -16) & 0xFFFFFFFF, user32.GetWindowLongW(w_val, -20) & 0xFFFFFFFF
            style_str, exstyle_str = f"{style:08X}", f"{exstyle:08X}"

            matched = [name for name, mask in EX_FLAG_DEFS if (exstyle & mask) == mask]
            ex_flags_str = ",".join(matched) if matched else "-"

            if exstyle & 0x80000:
                cr_key, alpha, flags = COLORREF(), wintypes.BYTE(), wintypes.DWORD()
                if user32.GetLayeredWindowAttributes(w_val, ctypes.byref(cr_key), ctypes.byref(alpha), ctypes.byref(flags)):
                    layered_str = f"ok alpha={alpha.value} flags=0x{flags.value:X}"
                else: layered_str = "no-attrs"
            else: layered_str = "-"

            wr = wintypes.RECT()
            if user32.GetWindowRect(w_val, ctypes.byref(wr)):
                wr_str = f"{wr.left},{wr.top} {wr.right - wr.left}x{wr.bottom - wr.top}"
                wr_box = (wr.left, wr.top, wr.right, wr.bottom)
            else: wr_str, wr_box = "err", None

            efb = wintypes.RECT()
            hr_efb = dwmapi.DwmGetWindowAttribute(w_val, 9, ctypes.byref(efb), ctypes.sizeof(efb))
            if hr_efb == 0:
                efb_str = f"{efb.left},{efb.top} {efb.right - efb.left}x{efb.bottom - efb.top}"
                efb_box = (efb.left, efb.top, efb.right, efb.bottom)
            else: efb_str, efb_box = "err", None

            owner = user32.GetWindow(w_val, 4)
            owner_str = f"0x{owner:08X}" if owner else "-"

            rgn = wintypes.RECT()
            rgn_code = user32.GetWindowRgnBox(w_val, ctypes.byref(rgn))
            rgn_str = f"{RGN_NAMES.get(rgn_code, str(rgn_code))} {rgn.left},{rgn.top} {rgn.right - rgn.left}x{rgn.bottom - rgn.top}"

            effective_efb = efb_box if efb_box is not None else wr_box
            ovl_efb = calc_intersect(effective_efb, target_client_screen)
            ovl_wr = calc_intersect(wr_box, target_client_screen)
            ovl_str = f"ovl_efb={ovl_efb} ovl_wr={ovl_wr}"

            row = [
                str(idx), f"0x{w_val:08X}", str(w_pid), w_exe, w_cls, w_title,
                vis, iconic, cloaked_str, style_str, exstyle_str, ex_flags_str,
                layered_str, wr_str, efb_str, owner_str, rgn_str, ovl_str,
            ]
            print(" | ".join(row))
        except Exception as e:
            print(f"{idx} | 0x{w_val:08X} | <row failed: {e!r}>")

        idx += 1
        try: w = user32.GetWindow(w, 2)
        except Exception: break

if __name__ == "__main__":
    main()
