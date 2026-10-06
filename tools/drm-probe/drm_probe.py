#!/usr/bin/env python3
# drm-probe — what this machine actually reports about protected video in Chrome.
#
# WRITTEN, NOT RUN (2026-09-20). Every number this project states must be measured on
# the RTX 5060 Ti or marked `unverified`; this script is how the P11 DRM half stops
# being unverified. Nothing here captures, decodes or works around protection: it reads
# process metadata, a window title, an accessibility property and a PPM the prototype
# already writes. It never touches the protected surface itself.
#
# It answers, in order of how much of the design rests on them:
#   Q1  Does Chrome on THIS machine run a MediaFoundation CDM (hardware DRM / L1) for
#       Netflix / Disney+ / Prime, or only the library CDM (Widevine L3)?
#       -> decides whether the pixel half has any coverage at all.
#   Q2  Are all chrome.exe children's command lines readable from a medium-IL,
#       non-elevated process, and how long does one full scan take?
#       -> decides whether the process scan may be the authoritative signal, and whether
#          a partial scan has to fail closed.
#   Q3  How long does the CDM utility process linger after the protected tab is closed
#       or navigated away?  -> sets kCdmGoneDwellSec (the design assumes 10 s).
#   Q4  Does a UIA read of the page URL work on a FULLSCREEN Chrome window, how long does
#       it take, and does it raise Chrome's CPU afterwards (the accessibility tax)?
#       -> decides whether UI Automation earns its place in the beta at all.
#   Q5  What does Windows.Graphics.Capture return over the video rect of protected
#       playback: black, a stale picture, or something else? And what is the middle-band
#       bright-block fraction there, against the shipping constants?
#       -> sets kDeadBandFrac and proves/disproves the black-frame branch.
#
# Usage (nothing here needs elevation; run it from the repo root or anywhere):
#
#   python drm_probe.py tree                    Q1, Q2: classify the Chrome tree once
#   python drm_probe.py watch --seconds 180     Q1, Q3: timeline of CDM processes
#   python drm_probe.py uia --title "Netflix"   Q4: URL via UIA + Chrome CPU before/after
#   python drm_probe.py window --title "Netflix"      title, fullscreen, display affinity
#   python drm_probe.py band shot_a.ppm         Q5: bright-block fraction of the band
#
# See README.md for the full recipe, including how the PPMs for `band` are produced.

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys
import time

# ---------------------------------------------------------------- Win32 plumbing

ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)

TH32CS_SNAPPROCESS = 0x00000002
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
ProcessCommandLineInformation = 60  # Win8.1+; needs only QUERY_LIMITED_INFORMATION
STATUS_INFO_LENGTH_MISMATCH = 0xC0000004


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class UNICODE_STRING(ctypes.Structure):
    _fields_ = [("Length", ctypes.c_ushort),
                ("MaximumLength", ctypes.c_ushort),
                ("Buffer", ctypes.c_void_p)]


class FILETIME(ctypes.Structure):
    _fields_ = [("lo", wt.DWORD), ("hi", wt.DWORD)]

    def value(self):
        return (self.hi << 32) | self.lo


def snapshot():
    """Every process on the machine: pid -> (name, ppid)."""
    h = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if h == -1:
        raise OSError("CreateToolhelp32Snapshot failed %d" % ctypes.get_last_error())
    out = {}
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(h, ctypes.byref(e))
        while ok:
            out[int(e.th32ProcessID)] = (e.szExeFile, int(e.th32ParentProcessID))
            ok = k32.Process32NextW(h, ctypes.byref(e))
    finally:
        k32.CloseHandle(h)
    return out


def open_limited(pid):
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    return h if h else None


def process_times(pid):
    """(create_filetime, err) — used to reject PID reuse against the parent."""
    h = open_limited(pid)
    if not h:
        return None, "open %d" % ctypes.get_last_error()
    try:
        c, x, kt, ut = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        if not k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(x),
                                   ctypes.byref(kt), ctypes.byref(ut)):
            return None, "times %d" % ctypes.get_last_error()
        return c.value(), None
    finally:
        k32.CloseHandle(h)


def process_cpu_seconds(pid):
    h = open_limited(pid)
    if not h:
        return None
    try:
        c, x, kt, ut = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        if not k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(x),
                                   ctypes.byref(kt), ctypes.byref(ut)):
            return None
        return (kt.value() + ut.value()) / 1e7
    finally:
        k32.CloseHandle(h)


def command_line(pid):
    """(cmdline, error). This is Q2: it must work for EVERY chrome.exe child."""
    h = open_limited(pid)
    if not h:
        return None, "OpenProcess err %d" % ctypes.get_last_error()
    try:
        need = wt.ULONG(0)
        st = ntdll.NtQueryInformationProcess(h, ProcessCommandLineInformation,
                                             None, 0, ctypes.byref(need))
        st &= 0xFFFFFFFF
        if st not in (STATUS_INFO_LENGTH_MISMATCH, 0) or need.value == 0:
            return None, "NtQuery size 0x%08X" % st
        buf = ctypes.create_string_buffer(need.value)
        st = ntdll.NtQueryInformationProcess(h, ProcessCommandLineInformation,
                                             buf, need.value, ctypes.byref(need)) & 0xFFFFFFFF
        if st != 0:
            return None, "NtQuery 0x%08X" % st
        us = ctypes.cast(buf, ctypes.POINTER(UNICODE_STRING)).contents
        if not us.Buffer or us.Length == 0:
            return "", None
        return ctypes.wstring_at(us.Buffer, us.Length // 2), None
    finally:
        k32.CloseHandle(h)


# ---------------------------------------------------------------- classification
#
# THE MATCHER THE ENGINE WILL SHIP. Kept identical here so the probe measures the
# real rule, not an approximation of it.
#
#   media.mojom.MediaFoundationService[Broker]  -> HARDWARE DRM (Widevine L1 / PlayReady
#                                                  SL3000). Capture is expected to come
#                                                  back black over the video.
#   media.mojom.CdmService[Broker]              -> the library CDM (Widevine L3).
#                                                  Capture returns REAL PIXELS, so this
#                                                  process is the ONLY coverage there.
#
# Deliberately broad on the right-hand side: a Chrome rename inside the media.mojom
# namespace must still be caught, and the probe prints whatever it matched so a rename
# is visible rather than silent. Nothing benign in an observed Chrome 153 tree uses the
# `media.mojom.` prefix (audio is audio.mojom.AudioService, capture is
# video_capture.mojom.VideoCaptureService) — verify that again in the output.

SUBTYPE_RE = re.compile(r"--utility-sub-type=([^\s\"]+)", re.I)
HW_RE = re.compile(r"media\.mojom\.MediaFoundationService", re.I)
SW_RE = re.compile(r"media\.mojom\.(Cdm|Widevine)", re.I)
ANY_MEDIA_RE = re.compile(r"media\.mojom\.", re.I)


def classify(cmd):
    """-> ('hw' | 'sw' | 'media?' | None, subtype-string-or-None)"""
    if not cmd:
        return None, None
    m = SUBTYPE_RE.search(cmd)
    sub = m.group(1) if m else None
    hay = sub or cmd
    if HW_RE.search(hay):
        return "hw", sub
    if SW_RE.search(hay):
        return "sw", sub
    if ANY_MEDIA_RE.search(hay):
        return "media?", sub
    return None, sub


def chrome_tree(browser_pid=None):
    """Children of the Chrome BROWSER process (the one that owns the window).

    Returns (browser_pid, [ (pid, name, cmd, err, kind, subtype) ]).
    With no pid given, picks the chrome.exe whose parent is not chrome.exe and which
    has the most chrome.exe children — the ordinary user profile's browser process.
    """
    snap = snapshot()
    chromes = {p: v for p, v in snap.items() if v[0].lower() == "chrome.exe"}
    if browser_pid is None:
        roots = [p for p, (_, pp) in chromes.items()
                 if pp not in chromes]
        if not roots:
            return None, []
        roots.sort(key=lambda p: sum(1 for _, (_, pp) in chromes.items() if pp == p),
                   reverse=True)
        browser_pid = roots[0]
    parent_created, _ = process_times(browser_pid)
    rows = []
    for pid, (name, ppid) in chromes.items():
        if ppid != browser_pid or pid == browser_pid:
            continue
        # PID reuse guard: a child cannot predate its parent.
        created, _ = process_times(pid)
        if parent_created and created and created < parent_created:
            rows.append((pid, name, None, "pid reuse (older than the browser)", None, None))
            continue
        cmd, err = command_line(pid)
        kind, sub = classify(cmd)
        rows.append((pid, name, cmd, err, kind, sub))
    rows.sort(key=lambda r: r[0])
    return browser_pid, rows


# ---------------------------------------------------------------- commands

def cmd_tree(args):
    t0 = time.perf_counter()
    bp, rows = chrome_tree(args.pid)
    dt = (time.perf_counter() - t0) * 1000.0
    if bp is None:
        print("no chrome.exe browser process found")
        return 1
    print("browser process %d, %d children, full scan %.2f ms" % (bp, len(rows), dt))
    unreadable = 0
    hw = sw = 0
    for pid, name, cmd, err, kind, sub in rows:
        if err:
            unreadable += 1
        if kind == "hw":
            hw += 1
        if kind == "sw":
            sw += 1
        tag = {"hw": "HW-DRM", "sw": "L3-CDM", "media?": "MEDIA?"}.get(kind, "      ")
        print("  %6d %s %-46s %s" % (pid, tag, (sub or "-")[:46], err or ""))
    print()
    print("Q1 verdict : hardware CDM %s, library CDM %s"
          % ("PRESENT" if hw else "absent", "PRESENT" if sw else "absent"))
    print("Q2 verdict : %d of %d children unreadable  (0 => the process scan may be "
          "authoritative and a partial scan need not fail closed)"
          % (unreadable, len(rows)))
    print("Q2 timing  : %.2f ms for a cold full scan; the engine caches per pid, so "
          "steady state is the snapshot alone" % dt)
    return 0


def cmd_watch(args):
    """Q3: when does the CDM process appear and, crucially, when does it go away?

    Recipe: start this, then in Chrome start protected playback, let it run ~30 s,
    then navigate that tab to about:blank (NOT close the window) and wait. The gap
    between the navigation and the last line is the dwell the design must tolerate.
    """
    print("watching for %d s, sampling every %.1f s — start/stop protected playback now"
          % (args.seconds, args.interval))
    print("%-9s %-6s %s" % ("t(s)", "event", "detail"))
    seen = {}
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        _, rows = chrome_tree(args.pid)
        now = {pid: (kind, sub) for pid, _, _, _, kind, sub in rows if kind}
        t = time.time() - t0
        for pid, v in now.items():
            if pid not in seen:
                print("%-9.2f %-6s pid %d  %s  %s" % (t, "APPEAR", pid, v[0], v[1]))
        for pid, v in list(seen.items()):
            if pid not in now:
                print("%-9.2f %-6s pid %d  %s  %s" % (t, "GONE", pid, v[0], v[1]))
        seen = now
        time.sleep(args.interval)
    print("\nQ3 verdict: kCdmGoneDwellSec must exceed the largest "
          "navigate-away -> GONE gap above (the design assumes 10 s).")
    return 0


# ---------------------------------------------------------------- window / UIA

def find_window(title_sub, exe="chrome.exe"):
    """Topmost visible window whose title contains `title_sub`, owned by `exe`."""
    result = []
    GetWindowTextW = u32.GetWindowTextW
    GetWindowTextLengthW = u32.GetWindowTextLengthW
    GetWindowThreadProcessId = u32.GetWindowThreadProcessId
    snap = snapshot()

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not u32.IsWindowVisible(hwnd):
            return True
        n = GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        GetWindowTextW(hwnd, buf, n + 1)
        title = buf.value
        pid = wt.DWORD(0)
        GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        name = snap.get(int(pid.value), ("", 0))[0]
        if exe and name.lower() != exe.lower():
            return True
        if title_sub.lower() in title.lower():
            result.append((hwnd, title, int(pid.value)))
        return True

    u32.EnumWindows(cb, 0)
    return result[0] if result else (None, None, None)


def cmd_window(args):
    hwnd, title, pid = find_window(args.title, args.exe)
    if not hwnd:
        print("no %s window whose title contains %r" % (args.exe, args.title))
        return 1
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    aff = wt.DWORD(0)
    ok = u32.GetWindowDisplayAffinity(hwnd, ctypes.byref(aff))
    print("hwnd 0x%X  pid %d" % (hwnd, pid))
    print("title   : %r" % title)
    print("rect    : %d,%d %dx%d" % (r.left, r.top, r.right - r.left, r.bottom - r.top))
    print("affinity: %s (%s)" % (aff.value if ok else "call failed",
                                 "I7: POSITIVE-ONLY, never the gate"))
    print()
    print("Title-scoping check — the beta's allowlist is a suffix test. Record the exact")
    print("fullscreen title for: YouTube, Netflix, Disney+, Prime Video. A fullscreen")
    print("Chrome window still carries the tab title, which is what makes this free.")
    return 0


def cmd_uia(args):
    """Q4: can we read the page URL from outside the page, and what does it cost?

    comtypes is used rather than raw vtable work: the probe is measuring Chrome's
    behaviour, not ours. `pip install comtypes` into the project venv if missing.
    """
    try:
        import comtypes.client as cc
    except ImportError:
        print("comtypes is not installed — `pip install comtypes` and re-run")
        return 2

    hwnd, title, pid = find_window(args.title, args.exe)
    if not hwnd:
        print("no %s window whose title contains %r" % (args.exe, args.title))
        return 1

    _, rows = chrome_tree(None)
    watched = [pid] + [r[0] for r in rows]

    def cpu_total():
        return sum(process_cpu_seconds(p) or 0.0 for p in watched)

    before = cpu_total()
    time.sleep(args.settle)
    baseline = (cpu_total() - before) / args.settle

    UIA = cc.GetModule("UIAutomationCore.dll")
    auto = cc.CreateObject("{E22AD333-B25F-460C-83D0-0581107395C9}",  # CUIAutomation8
                           interface=UIA.IUIAutomation2)
    try:
        auto.ConnectionTimeout = 500   # ms — never block the caller on a busy renderer
        auto.TransactionTimeout = 500
    except Exception as e:                                    # noqa: BLE001
        print("could not bound the timeouts (%s) — a hung renderer could stall us" % e)

    t0 = time.perf_counter()
    el = auto.ElementFromHandle(hwnd)
    cond = auto.CreatePropertyCondition(UIA.UIA_ControlTypePropertyId,
                                        UIA.UIA_DocumentControlTypeId)
    doc = el.FindFirst(UIA.TreeScope_Descendants, cond)
    url = None
    if doc:
        try:
            url = doc.GetCurrentPropertyValue(UIA.UIA_ValueValuePropertyId)
        except Exception as e:                                # noqa: BLE001
            url = "<Value pattern failed: %s>" % e
    dt = (time.perf_counter() - t0) * 1000.0

    print("UIA read     : %.1f ms" % dt)
    print("document URL : %r" % url)
    print("window title : %r" % title)

    after0 = cpu_total()
    time.sleep(args.settle)
    tax = (cpu_total() - after0) / args.settle
    print()
    print("Chrome CPU, whole tree: %.3f core-s/s before, %.3f after the UIA read"
          % (baseline, tax))
    print("Q4 verdict: UIA earns its place only if the URL came back on a FULLSCREEN")
    print("window, the read stayed well under one source period, and the CPU line did")
    print("not move. Any of the three failing => the beta scopes by window title and")
    print("UIA stays behind a flag, off by default.")
    return 0


# ---------------------------------------------------------------- band analysis

# THE SHIPPING CONSTANTS. Kept in step with prototype/nsp_capture.cpp — the middle band
# is chosen so no player UI reaches it: no progress bar, no captions, no title card.
K_BAND_LEFT, K_BAND_RIGHT = 0.06, 0.94
K_BAND_TOP, K_BAND_BOTTOM = 0.20, 0.72
K_BLOCK_PX = 16
K_BLACK_MAX = 16 / 255.0     # video black in limited range; hardware-DRM black is 0


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("%s is not a binary PPM (P6)" % path)
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, maxv = fields
    i += 1
    if maxv != 255:
        raise ValueError("only 8-bit PPMs")
    return w, h, memoryview(data)[i : i + w * h * 3]


def cmd_band(args):
    w, h, px = read_ppm(args.ppm)
    x0 = int(w * K_BAND_LEFT)
    y0 = int(h * K_BAND_TOP)
    nbx = (int(w * K_BAND_RIGHT) - x0) // K_BLOCK_PX
    nby = (int(h * K_BAND_BOTTOM) - y0) // K_BLOCK_PX
    thr = int(round(K_BLACK_MAX * 255))
    bright = 0
    band_max = 0
    for by in range(nby):
        for bx in range(nbx):
            bmax = 0
            for y in range(y0 + by * K_BLOCK_PX, y0 + (by + 1) * K_BLOCK_PX, 2):
                row = (y * w) * 3
                for x in range(x0 + bx * K_BLOCK_PX, x0 + (bx + 1) * K_BLOCK_PX, 2):
                    o = row + x * 3
                    v = max(px[o], px[o + 1], px[o + 2])
                    if v > bmax:
                        bmax = v
            band_max = max(band_max, bmax)
            if bmax > thr:
                bright += 1
    total = nbx * nby
    frac = bright / total if total else 0.0
    print("%s  %dx%d" % (os.path.basename(args.ppm), w, h))
    print("band       : x %d..%d  y %d..%d  -> %dx%d blocks of %d px"
          % (x0, int(w * K_BAND_RIGHT), y0, int(h * K_BAND_BOTTOM), nbx, nby, K_BLOCK_PX))
    print("bright     : %d of %d blocks = %.5f   (threshold: any channel > %d)"
          % (bright, total, frac, thr))
    print("band max   : %d" % band_max)
    print()
    print("Q5 reading:")
    print("  frac ~ 0.000 on protected playback  => the black branch has real coverage;")
    print("                                         set kDeadBandFrac just above it.")
    print("  frac high on protected playback     => capture returned REAL PIXELS (L3):")
    print("                                         the pixel half has NO coverage and")
    print("                                         the process scan is the whole gate.")
    print("  frac on a genuine night scene / a fade-to-black frame is the false-positive")
    print("  margin — it must sit well ABOVE kDeadBandFrac, or raise kDeadSec instead.")
    return 0


# ---------------------------------------------------------------- main

def main(argv):
    # The file's header is a comment block, not a docstring, so __doc__ is None.
    ap = argparse.ArgumentParser(
        description="drm-probe - what this machine reports about protected video in Chrome")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("tree", help="Q1/Q2: classify the Chrome process tree once")
    p.add_argument("--pid", type=int, default=None, help="the browser process pid")
    p.set_defaults(fn=cmd_tree)

    p = sub.add_parser("watch", help="Q1/Q3: timeline of CDM utility processes")
    p.add_argument("--seconds", type=float, default=180.0)
    p.add_argument("--interval", type=float, default=0.5)
    p.add_argument("--pid", type=int, default=None)
    p.set_defaults(fn=cmd_watch)

    p = sub.add_parser("window", help="title, rect and display affinity of a window")
    p.add_argument("--title", required=True)
    p.add_argument("--exe", default="chrome.exe")
    p.set_defaults(fn=cmd_window)

    p = sub.add_parser("uia", help="Q4: page URL via UI Automation + the CPU it costs")
    p.add_argument("--title", required=True)
    p.add_argument("--exe", default="chrome.exe")
    p.add_argument("--settle", type=float, default=5.0, help="CPU sampling window, s")
    p.set_defaults(fn=cmd_uia)

    p = sub.add_parser("band", help="Q5: bright-block fraction of a dumped PPM")
    p.add_argument("ppm")
    p.set_defaults(fn=cmd_band)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    if sys.platform != "win32":
        print("Windows only")
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
