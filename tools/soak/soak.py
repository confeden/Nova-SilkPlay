#!/usr/bin/env python3
"""Long-run soak harness for Nova SilkPlay frame-generation engine.

Runs the silkplay.exe engine with a live test page for a measured period,
sampling memory, handle and GDI/USER object counts every few seconds, and
scanning the engine log for error patterns. Thresholds:
- Working set growth: 50 MB/hour (typical app; frame-gen code must not leak).
- Handle count growth: 200/hour (reasonable; device lost often leaks handles).
- GDI or USER object growth: 50/hour (Windows resource exhaustion).

Does not run fullscreen or steal focus. Kills only the processes it started.
Safe to run unattended.
"""
import argparse
import ast
import csv
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime


HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
PROTO = os.path.join(PROJECT, "prototype")

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDpiAwarenessContext.restype = wt.BOOL
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi")

# Win32 constants
TH32CS_SNAPTHREAD = 0x00000004
INVALID_HANDLE_VALUE = -1
CREATE_NEW_PROCESS_GROUP = 0x00000200
CTRL_BREAK_EVENT = 1
EVENT_MODIFY_STATE = 0x0002

# ctypes defaults every return to C int, which truncates a 64-bit HANDLE. The
# quit-event handles below must be declared or SetEvent is called on garbage.
kernel32.OpenEventW.restype = wt.HANDLE
kernel32.OpenEventW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
kernel32.SetEvent.argtypes = [wt.HANDLE]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# Colorless error patterns to find in engine log (case-insensitive)
ERROR_PATTERNS = [
    "device lost",
    "FAILED",
    "error",
    "assert",
    "exception",
    "falling back",
]
# Patterns to exclude from errors (still error-like but not actionable)
EXCLUDE_PATTERNS = [
    "no target window",
    "waiting for",
]


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD),
        ("PageFaultCount", wt.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ThreadID", wt.DWORD),
        ("th32OwnerProcessID", wt.DWORD),
        ("tpBasePri", wt.LONG),
        ("tpDeltaPri", wt.LONG),
        ("dwFlags", wt.DWORD),
    ]


EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def find_window(title):
    """Find a visible window by title substring (case-insensitive)."""
    found = []

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if title.lower() in buf.value.lower():
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found[0] if found else None


def window_rect(hwnd):
    """Get window rect in physical pixels."""
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return {"x": r.left, "y": r.top, "w": r.right - r.left, "h": r.bottom - r.top}


def open_process(pid, desired_access):
    """Open process handle. Try higher privilege first, fall back to limited."""
    h = kernel32.OpenProcess(desired_access, False, pid)
    if h:
        return h
    # Fallback to limited access
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    return h if h else None


def get_process_memory_info(pid):
    """Return (working_set_mb, private_bytes_mb) or (None, None) if unavailable."""
    h = open_process(pid, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)
    if not h:
        return None, None
    try:
        pmc = PROCESS_MEMORY_COUNTERS_EX()
        pmc.cb = ctypes.sizeof(pmc)
        if psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
            ws_mb = pmc.WorkingSetSize / (1024 * 1024)
            pb_mb = pmc.PrivateUsage / (1024 * 1024)
            return ws_mb, pb_mb
    finally:
        kernel32.CloseHandle(h)
    return None, None


def get_handle_count(pid):
    """Return handle count or None if unavailable."""
    h = open_process(pid, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)
    if not h:
        return None
    try:
        count = wt.DWORD()
        if kernel32.GetProcessHandleCount(h, ctypes.byref(count)):
            return count.value
    finally:
        kernel32.CloseHandle(h)
    return None


def get_gui_resources(pid, flags):
    """Get GDI or USER object count. flags: 0 for GDI, 1 for USER."""
    h = open_process(pid, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)
    if not h:
        return None
    try:
        count = user32.GetGuiResources(h, flags)
        return count if count > 0 else None
    finally:
        kernel32.CloseHandle(h)
    return None


def get_thread_count(pid):
    """Count threads for this process."""
    h = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if h == INVALID_HANDLE_VALUE:
        return None
    try:
        te = THREADENTRY32()
        te.dwSize = ctypes.sizeof(te)
        count = 0
        if kernel32.Thread32First(h, ctypes.byref(te)):
            while True:
                if te.th32OwnerProcessID == pid:
                    count += 1
                if not kernel32.Thread32Next(h, ctypes.byref(te)):
                    break
        return count
    finally:
        kernel32.CloseHandle(h)
    return None


def get_engine_log_size(log_path):
    """Return size of engine log file in bytes, or 0 if missing."""
    try:
        return os.path.getsize(log_path)
    except FileNotFoundError:
        return 0


def scan_log_for_errors(log_path, last_pos):
    """Scan log from last_pos for error patterns. Return (new_pos, error_dict, excerpts).

    error_dict maps pattern -> count. excerpts is list of (pattern, line) tuples (max 20 total).
    """
    error_dict = {}
    excerpts = []
    new_pos = last_pos

    # Initialize pattern counts
    for p in ERROR_PATTERNS:
        error_dict[p] = 0

    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            f.seek(last_pos)
            # readline(), not iteration: a file being iterated has a read-ahead
            # buffer and tell() on it raises. We need the exact resume offset.
            while True:
                line = f.readline()
                if not line:
                    break
                new_pos = f.tell()
                # Check if line matches any error pattern
                line_lower = line.lower()
                excluded = any(e in line_lower for e in EXCLUDE_PATTERNS)
                if not excluded:
                    for pattern in ERROR_PATTERNS:
                        if pattern in line_lower:
                            error_dict[pattern] += 1
                            if len(excerpts) < 20:
                                excerpts.append((pattern, line.rstrip()))
                            break
    except FileNotFoundError:
        pass

    return new_pos, error_dict, excerpts


def compute_growth_per_hour(samples, drop_first_60s=True):
    """Compute least-squares slope over samples, scaled to per-hour.

    samples is list of (time_seconds, value) tuples.
    Returns (first, last, max, growth_per_hour).
    """
    if not samples:
        return None, None, None, None

    # Drop first 60 seconds (warm-up)
    if drop_first_60s and samples:
        cutoff_time = samples[0][0] + 60
        samples_after = [(t, v) for t, v in samples if t >= cutoff_time]
        if len(samples_after) < 3:
            # Not enough samples; use all
            samples_after = samples
    else:
        samples_after = samples

    if len(samples_after) < 2:
        first = samples[0][1] if samples else None
        last = samples[-1][1] if samples else None
        max_val = max(v for t, v in samples) if samples else None
        return first, last, max_val, 0.0

    # Least-squares fit
    n = len(samples_after)
    t_vals = [s[0] for s in samples_after]
    v_vals = [s[1] for s in samples_after]
    mean_t = sum(t_vals) / n
    mean_v = sum(v_vals) / n
    numerator = sum((t - mean_t) * (v - mean_v) for t, v in zip(t_vals, v_vals))
    denominator = sum((t - mean_t) ** 2 for t in t_vals)
    slope = numerator / denominator if denominator > 0 else 0.0
    # Scale to per-hour (3600 seconds)
    growth_per_hour = slope * 3600

    first = samples[0][1]
    last = samples[-1][1]
    max_val = max(v_vals) if v_vals else None

    return first, last, max_val, growth_per_hour


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--minutes", type=float, default=30,
                    help="soak duration in minutes")
    ap.add_argument("--sample-seconds", type=float, default=10,
                    help="metric sampling period in seconds")
    ap.add_argument("--fps", type=int, default=24,
                    help="test page FPS")
    ap.add_argument("--speed", type=int, default=900,
                    help="test page motion speed")
    ap.add_argument("--out", default=os.path.join(HERE, "runs"),
                    help="output directory")
    ap.add_argument("--no-page", action="store_true",
                    help="do not launch Chrome; assume window exists")
    ap.add_argument("--engine-arg", action="append", default=[],
                    help="extra engine arguments (repeatable)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    # Timestamps and file names
    now = datetime.now()
    stamp = now.strftime("%Y%m%d-%H%M%S")
    csv_path = os.path.join(args.out, f"{stamp}_samples.csv")
    log_path = os.path.join(args.out, f"{stamp}_engine.log")
    report_path = os.path.join(args.out, f"{stamp}_report.json")

    # Phase 1: launch test page (unless --no-page)
    hwnd = None
    if not args.no_page:
        print(f"Launching test page with fps={args.fps}, speed={args.speed}...")
        result = subprocess.run(
            [sys.executable, os.path.join(PROTO, "run_testpage.py"),
             f"--fps={args.fps}", f"--speed={args.speed}"],
            cwd=PROTO,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(f"run_testpage.py failed: {result.stderr}", file=sys.stderr)
            return 1

        # Parse output: "window 0x12345 at {'x': 3521, 'y': 462, 'w': 954, 'h': 566}"
        for line in result.stdout.split("\n"):
            if "window 0x" in line:
                # Extract hwnd and rect
                parts = line.split(" at ")
                if len(parts) == 2:
                    hwnd_str = parts[0].replace("window 0x", "").strip()
                    rect_str = parts[1].strip()
                    try:
                        hwnd = int(hwnd_str, 16)
                        rect_dict = ast.literal_eval(rect_str)
                    except (ValueError, SyntaxError):
                        print(f"Could not parse window line: {line}", file=sys.stderr)
                        return 1
                    break

        if not hwnd:
            print("Test page output did not contain window info", file=sys.stderr)
            return 1
        print(f"Test page ready: hwnd=0x{hwnd:X}")
    else:
        hwnd = find_window("Nova SilkPlay motion source")
        if not hwnd:
            print("Could not find test page window", file=sys.stderr)
            return 1
        print(f"Found existing test page: hwnd=0x{hwnd:X}")

    # Phase 2: launch engine
    print("Launching engine...")
    cmd = [
        os.path.join(PROTO, "silkplay.exe"),
        "--target-title", "Nova SilkPlay motion source",
        "--allow-windowed",
    ]
    cmd.extend(args.engine_arg)

    with open(log_path, "w", encoding="utf-8", buffering=1) as log_f:
        proc = subprocess.Popen(
            cmd,
            stdout=log_f,
            stderr=subprocess.STDOUT,
            creationflags=CREATE_NEW_PROCESS_GROUP,
            text=True,
        )

    engine_pid = proc.pid
    deadline = time.time() + args.minutes * 60
    sample_interval = args.sample_seconds
    next_sample = time.time() + sample_interval

    # Phase 3: sampling loop
    print(f"Sampling for {args.minutes} minutes...")
    samples = {
        "working_set_mb": [],
        "private_bytes_mb": [],
        "handles": [],
        "gdi_objects": [],
        "user_objects": [],
        "threads": [],
    }
    log_pos = 0
    error_dict = {}
    error_excerpts = []
    samples_written = 0
    warmup_dropped = True

    with open(csv_path, "w", newline="", encoding="utf-8") as csv_f:
        csv_w = csv.writer(csv_f)
        csv_w.writerow([
            "t_seconds", "working_set_mb", "private_bytes_mb", "handles",
            "gdi_objects", "user_objects", "threads", "engine_log_bytes"
        ])

        while time.time() < deadline:
            # Check if process is still alive
            if proc.poll() is not None:
                exit_code = proc.returncode
                with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                    lines = f.readlines()
                    last_60 = lines[-60:] if len(lines) > 60 else lines
                print(f"Engine exited with code {exit_code}", file=sys.stderr)
                print("Last 60 lines of engine log:", file=sys.stderr)
                for line in last_60:
                    print(line.rstrip(), file=sys.stderr)
                # FAIL: process died early
                report = {
                    "ok": False,
                    "minutes": (time.time() - (deadline - args.minutes * 60)) / 60,
                    "exit_code": exit_code,
                    "samples": samples_written,
                    "working_set_mb": {"first": None, "last": None, "max": None, "growth_per_hour": None},
                    "private_bytes_mb": {"first": None, "last": None, "max": None, "growth_per_hour": None},
                    "handles": {"first": None, "last": None, "max": None, "growth_per_hour": None},
                    "gdi_objects": {"first": None, "last": None, "max": None, "growth_per_hour": None},
                    "user_objects": {"first": None, "last": None, "max": None, "growth_per_hour": None},
                    "threads": {"first": None, "last": None, "max": None, "growth_per_hour": None},
                    "log_hits": error_dict,
                    "log_excerpts": [f"{p}: {line}" for p, line in error_excerpts],
                    "failures": ["Process exited early"],
                    "warmup_dropped": warmup_dropped,
                }
                with open(report_path, "w", encoding="utf-8") as f:
                    json.dump(report, f, indent=2)
                print(f"Report: {report_path}")
                return 1

            # Sample at intervals
            now = time.time()
            if now >= next_sample:
                t = now - (deadline - args.minutes * 60)
                ws, pb = get_process_memory_info(engine_pid)
                handles = get_handle_count(engine_pid)
                gdi = get_gui_resources(engine_pid, 0)
                user = get_gui_resources(engine_pid, 1)
                threads = get_thread_count(engine_pid)
                log_size = get_engine_log_size(log_path)

                csv_w.writerow([
                    f"{t:.1f}",
                    f"{ws:.2f}" if ws is not None else "",
                    f"{pb:.2f}" if pb is not None else "",
                    handles if handles is not None else "",
                    gdi if gdi is not None else "",
                    user if user is not None else "",
                    threads if threads is not None else "",
                    log_size,
                ])
                samples_written += 1

                if ws is not None:
                    samples["working_set_mb"].append((t, ws))
                if pb is not None:
                    samples["private_bytes_mb"].append((t, pb))
                if handles is not None:
                    samples["handles"].append((t, handles))
                if gdi is not None:
                    samples["gdi_objects"].append((t, gdi))
                if user is not None:
                    samples["user_objects"].append((t, user))
                if threads is not None:
                    samples["threads"].append((t, threads))

                # Scan log for errors
                log_pos, new_errors, new_excerpts = scan_log_for_errors(log_path, log_pos)
                for pattern in ERROR_PATTERNS:
                    if pattern not in error_dict:
                        error_dict[pattern] = 0
                    error_dict[pattern] += new_errors.get(pattern, 0)
                error_excerpts.extend(new_excerpts)
                if len(error_excerpts) > 20:
                    error_excerpts = error_excerpts[:20]

                next_sample = now + sample_interval

            time.sleep(0.1)

    # Phase 4: stop engine cleanly
    print("Stopping engine...")
    try:
        # The engine is a windowless (GUI-subsystem) process: it has no console,
        # so Ctrl+Break cannot reach it. It opens a named manual-reset event and
        # leaves its loop by the same path as Ctrl+Alt+X when that event is set.
        h = kernel32.OpenEventW(0x0002, False, "Local\\NovaSilkPlay.Quit")  # EVENT_MODIFY_STATE
        if h:
            kernel32.SetEvent(h)
            kernel32.CloseHandle(h)
        else:
            print("quit event not found - falling back to terminate()")
    except Exception:
        pass
    time.sleep(5)
    if proc.poll() is None:
        proc.terminate()
        time.sleep(1)
    if proc.poll() is None:
        proc.kill()
    proc.wait()

    # Phase 5: close test page window
    hwnd = find_window("Nova SilkPlay motion source")
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        time.sleep(1)

    # Kill any leftover chrome.exe started with OUR throwaway profile, and nothing else.
    #
    # PowerShell, not wmic: wmic.exe is NOT PRESENT on this machine (Windows 11 IoT LTSC
    # 2024 - it was deprecated and the optional feature is not installed), so the wmic
    # version of this sweep silently matched nothing and left every test Chrome running.
    # It failed safe, which is why it went unnoticed; it also never did its job.
    try:
        ps = (
            "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
            "Where-Object { $_.CommandLine -like '*nsp-proto-profile*' } | "
            "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
        )
        subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
            capture_output=True, text=True, timeout=30,
        )
    except Exception as exc:
        print(f"could not sweep the test Chrome: {exc}")

    # Phase 6: compute metrics and verdict
    failures = []
    elapsed = (time.time() - (deadline - args.minutes * 60)) / 60

    # Check for errors in log
    if error_dict and any(error_dict.values()):
        failures.append("Error patterns found in engine log")

    # Compute growth metrics
    def make_metric_dict(samples_list):
        if not samples_list:
            return {"first": None, "last": None, "max": None, "growth_per_hour": None}
        first, last, max_val, growth = compute_growth_per_hour(samples_list, drop_first_60s=True)
        return {
            "first": round(first, 2) if first is not None else None,
            "last": round(last, 2) if last is not None else None,
            "max": round(max_val, 2) if max_val is not None else None,
            "growth_per_hour": round(growth, 2) if growth is not None else None,
        }

    ws_metric = make_metric_dict(samples["working_set_mb"])
    if ws_metric["growth_per_hour"] is not None and ws_metric["growth_per_hour"] > 50:
        failures.append(f"Working set grew {ws_metric['growth_per_hour']:.1f} MB/hour (limit 50)")

    handles_metric = make_metric_dict(samples["handles"])
    if handles_metric["growth_per_hour"] is not None and handles_metric["growth_per_hour"] > 200:
        failures.append(f"Handles grew {handles_metric['growth_per_hour']:.1f}/hour (limit 200)")

    gdi_metric = make_metric_dict(samples["gdi_objects"])
    if gdi_metric["growth_per_hour"] is not None and gdi_metric["growth_per_hour"] > 50:
        failures.append(f"GDI objects grew {gdi_metric['growth_per_hour']:.1f}/hour (limit 50)")

    user_metric = make_metric_dict(samples["user_objects"])
    if user_metric["growth_per_hour"] is not None and user_metric["growth_per_hour"] > 50:
        failures.append(f"USER objects grew {user_metric['growth_per_hour']:.1f}/hour (limit 50)")

    threads_metric = make_metric_dict(samples["threads"])
    private_metric = make_metric_dict(samples["private_bytes_mb"])

    ok = proc.returncode == 0 and len(failures) == 0

    # Determine if we dropped warmup: check if we have >= 3 samples after 60 seconds elapsed.
    # If we do, we used the post-warmup data. If not, we used all data and should report false.
    warmup_dropped = True
    for metric_samples in samples.values():
        if metric_samples:
            postdrop = [s for s in metric_samples if s[0] >= 60]
            if len(postdrop) < 3:
                warmup_dropped = False
                break

    report = {
        "ok": ok,
        "minutes": elapsed,
        "exit_code": proc.returncode if proc.returncode is not None else 0,
        "samples": samples_written,
        "working_set_mb": ws_metric,
        "private_bytes_mb": private_metric,
        "handles": handles_metric,
        "gdi_objects": gdi_metric,
        "user_objects": user_metric,
        "threads": threads_metric,
        "log_hits": {k: v for k, v in error_dict.items() if v > 0} or {},
        "log_excerpts": [f"{p}: {line}" for p, line in error_excerpts],
        "failures": failures,
        "warmup_dropped": warmup_dropped,
    }

    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    print(f"\nSoak complete: {report['minutes']:.1f} minutes, {samples_written} samples")
    print(f"Report: {report_path}")
    print(f"CSV: {csv_path}")
    print(f"Log: {log_path}")

    if ok:
        print("PASS")
        return 0
    else:
        print("FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
