"""
run_sm2.py — drive both halves of spike S-M2 from one clock.

S-M2 asks two questions that can kill the browser path (see
.claude/kb/research/directives-2026-09.md#6):

  1. Does our DirectComposition overlay cost Chrome its hardware overlay plane?
     Chrome's own answer lives in GPU-process histograms, which only the
     chrome://histograms WebUI exposes (gotcha G16) — chrome_cdp.py scrapes it.
  2. Does Windows.Graphics.Capture return real pixels while Chrome's video is on
     an MPO plane at all? overlay_probe.exe answers that from its own capture
     session's per-frame black/duplicate statistics.

Neither half can answer alone, and the two must agree on when a phase starts and
ends. This script owns the clock: overlay_probe.exe runs with --stdin-sync and
advances only when this script writes a newline to its stdin, so a phase boundary
is one event, not two racing timers.

Phases alternate hidden/shown. "hidden" is the control: the overlay exists, is
positioned, and is simply not shown — so a difference between phases is caused by
the overlay being composited, not by the probe running.

Usage
-----
  python run_sm2.py --video "D:\\Documents\\Video\\clip.mp4" --geom client
  python run_sm2.py --video ... --geom rect --rect 0,0,1280,720
  python run_sm2.py --video ... --no-transparent      # S-M11 negative control

Outputs <out-prefix>_combined.json plus the probe's own CSV/JSON.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from typing import Any

import chrome_cdp as C

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_EXE = os.path.join(HERE, "overlay_probe.exe")
PAGE_TITLE_MARK = "Compositing Probe Testpage"


class ProbeProcess:
    """overlay_probe.exe with --stdin-sync, plus a reader thread for its stdout."""

    def __init__(self, args: list[str]) -> None:
        self.proc = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
            errors="replace", bufsize=1,
        )
        self.lines: list[str] = []
        self._events: list[str] = []
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            with self._lock:
                self.lines.append(line)
                if "PHASE_BEGIN" in line or "PHASE_END" in line:
                    self._events.append(line)
            print(f"  [probe] {line}", flush=True)

    def wait_for(self, marker: str, timeout: float = 30.0) -> str:
        """Block until a stdout line containing `marker` appears."""
        deadline = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < deadline:
            with self._lock:
                for i in range(seen, len(self._events)):
                    if marker in self._events[i]:
                        return self._events[i]
                seen = len(self._events)
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"overlay_probe exited with code {self.proc.returncode} "
                    f"while waiting for {marker!r}"
                )
            time.sleep(0.05)
        raise TimeoutError(f"overlay_probe never printed {marker!r} within {timeout}s")

    def advance(self) -> None:
        """Release one phase boundary."""
        assert self.proc.stdin is not None
        self.proc.stdin.write("\n")
        self.proc.stdin.flush()

    def finish(self, timeout: float = 30.0) -> int:
        if self.proc.stdin:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return -1


def find_target_window(title_mark: str, timeout: float = 20.0) -> dict[str, Any]:
    """Ask overlay_probe --list-windows for the Chrome window showing our page."""
    deadline = time.monotonic() + timeout
    last: list[str] = []
    while time.monotonic() < deadline:
        out = subprocess.run([PROBE_EXE, "--list-windows"], capture_output=True,
                             text=True, encoding="utf-8", errors="replace").stdout
        last = out.splitlines()
        for line in last:
            if title_mark in line and "chrome.exe" in line:
                parts = line.split()
                # HWND PID IMAGE CLASS WINDOW_RECT CLIENT_RECT CLOAKED TITLE...
                if len(parts) >= 7:
                    return {"hwnd": parts[0], "pid": int(parts[1]),
                            "cls": parts[3], "window_rect": parts[4],
                            "client_rect": parts[5], "line": line}
        time.sleep(0.5)
    raise RuntimeError(
        f"No chrome.exe window titled like {title_mark!r} appeared within {timeout}s. "
        f"--list-windows last returned {len(last)} lines."
    )


def parse_phases(spec: str) -> list[tuple[str, str, float]]:
    """'hidden:10,shown:20,hidden:10' -> [(name, kind, seconds), ...]"""
    out: list[tuple[str, str, float]] = []
    for i, token in enumerate(spec.split(",")):
        bits = token.strip().split(":")
        if len(bits) == 2:
            kind, secs = bits[0].strip(), bits[1].strip()
            name = f"{kind}{i}"
        elif len(bits) == 3:
            name, kind, secs = (b.strip() for b in bits)
        else:
            raise ValueError(f"bad phase token {token!r}; use kind:seconds or name:kind:seconds")
        if kind not in ("hidden", "shown"):
            raise ValueError(f"phase kind must be hidden|shown, got {kind!r}")
        out.append((name, kind, float(secs)))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(prog="run_sm2", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--video", required=True, help="Local video file to play in Chrome.")
    ap.add_argument("--phases", default="hidden:15,shown:30,hidden:15")
    ap.add_argument("--geom", default="client", choices=["client", "rect"])
    ap.add_argument("--rect", default=None, help="X,Y,W,H in target-client px (with --geom rect).")
    ap.add_argument("--inset", type=int, default=0)
    ap.add_argument("--no-transparent", action="store_true",
                    help="S-M11 negative control: drop WS_EX_TRANSPARENT.")
    ap.add_argument("--no-wgc", action="store_true")
    ap.add_argument("--wgc-source", default=None, choices=["window", "monitor"],
                    help="Capture the target window or the monitor it is on. "
                         "Window capture is known to cost Chrome its overlay plane (G17); "
                         "this is how the monitor alternative gets tested.")
    ap.add_argument("--out-prefix", default="sm2")
    ap.add_argument("--keep-open", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(PROBE_EXE):
        sys.exit(f"{PROBE_EXE} is missing — run build.cmd first.")
    phases = parse_phases(args.phases)

    # 1. Build the test page around the requested clip.
    page = subprocess.run(
        [sys.executable, os.path.join(HERE, "make_testpage.py"),
         "--video", args.video, "--out", os.path.join(HERE, "testpage.html")],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if page.returncode != 0:
        sys.exit(f"make_testpage.py failed:\n{page.stdout}\n{page.stderr}")
    page_url = page.stdout.strip().splitlines()[-1]
    print(f"test page: {page_url}", flush=True)

    # 2. Chrome, with a throwaway profile and a port Windows will actually let it
    #    bind (gotcha G15: 9222 is inside a reserved range on this machine).
    session = C.launch_chrome(page_url)
    print(f"chrome devtools port {session.port}", flush=True)
    cdp = C.CDP(session.ws_url)
    report: dict[str, Any] = {
        "page_url": page_url, "video": args.video, "phases": [],
        "chrome_port": session.port, "geom": args.geom, "rect": args.rect,
        "style_transparent": not args.no_transparent,
    }
    probe: ProbeProcess | None = None
    hist_tabs: dict[str, tuple[str, str]] = {}
    try:
        # Attach to the video page and start its raf/rvfc counters.
        page_target = None
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and page_target is None:
            for t in C.list_page_targets(cdp):
                if t.get("url", "").endswith("testpage.html"):
                    page_target = t["targetId"]
                    break
            time.sleep(0.3)
        if page_target is None:
            raise RuntimeError("the test page never appeared as a CDP target")
        sid = C.attach_page(cdp, page_target)
        cdp.send("Runtime.enable", session_id=sid)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if C.eval_js(cdp, sid, "document.readyState") == "complete":
                break
            time.sleep(0.3)
        C.eval_js(cdp, sid, C._PROBE_INIT_JS)

        # Let the video actually start and the compositor settle before any
        # baseline is taken: the first seconds contain startup-only records.
        time.sleep(4.0)

        prefixes = ["GPU.DirectComposition", "GPU.OutputPresenter",
                    "Media.VideoFrameSubmitter"]
        prev: dict[str, dict] = {}
        for pref in prefixes:
            hist_tabs[pref] = C.open_histogram_tab(cdp, pref)
            prev[pref] = C.scrape_histogram_page(cdp, hist_tabs[pref][1])
        cdp.send("Target.activateTarget", {"targetId": page_target})

        # 3. The overlay probe, over the Chrome window showing our page.
        target = find_target_window(PAGE_TITLE_MARK)
        report["target_window"] = target
        print(f"target window: {target['line']}", flush=True)

        probe_args = [
            PROBE_EXE,
            "--target-pid", str(target["pid"]),
            "--target-class", "Chrome_WidgetWin_1",
            "--geom", args.geom,
            "--inset", str(args.inset),
            "--stdin-sync",
            "--phases", ",".join(f"{n}:{k}:{s}" for n, k, s in phases),
            "--csv", os.path.join(HERE, f"{args.out_prefix}_samples.csv"),
            "--json", os.path.join(HERE, f"{args.out_prefix}_probe.json"),
        ]
        if args.rect:
            probe_args += ["--rect", args.rect]
        if args.no_transparent:
            probe_args += ["--no-transparent"]
        if args.no_wgc:
            probe_args += ["--no-wgc"]
        if args.wgc_source:
            probe_args += ["--wgc-source", args.wgc_source]
        report["probe_argv"] = probe_args
        probe = ProbeProcess(probe_args)

        # 4. One clock: wait for the probe to enter a phase, hold it for the
        #    requested duration, sample Chrome, then release the boundary.
        for name, kind, secs in phases:
            probe.wait_for(f"PHASE_BEGIN {name}", timeout=60)
            print(f"=== phase {name} ({kind}) {secs}s ===", flush=True)
            t0 = time.monotonic()
            time.sleep(secs)
            actual = time.monotonic() - t0

            js = C.eval_js(cdp, sid, C._PROBE_READ_JS)
            page_hist: dict[str, Any] = {}
            for pref, (_tid, hsid) in hist_tabs.items():
                cur = C.scrape_histogram_page(cdp, hsid)
                page_hist[pref] = C.diff_histograms(prev.get(pref, {}), cur)
                prev[pref] = cur
            cdp.send("Target.activateTarget", {"targetId": page_target})

            report["phases"].append({
                "name": name, "kind": kind, "requested_s": secs,
                "actual_s": round(actual, 3), "js": js,
                "page_histograms": page_hist,
            })
            probe.advance()

        rc = probe.finish()
        report["probe_exit_code"] = rc
        probe_json = os.path.join(HERE, f"{args.out_prefix}_probe.json")
        if os.path.exists(probe_json):
            with open(probe_json, "r", encoding="utf-8") as f:
                report["probe_report"] = json.load(f)
        else:
            report["probe_report"] = None
            print("warning: the probe wrote no JSON report", file=sys.stderr)
    finally:
        for _pref, (tid, _sid) in hist_tabs.items():
            try:
                C.close_histogram_tab(cdp, tid)
            except Exception as exc:            # noqa: BLE001 - reported, not hidden
                print(f"warning: closing histogram tab: {exc}", file=sys.stderr)
        try:
            cdp.close()
        except Exception as exc:                # noqa: BLE001
            print(f"warning: closing CDP: {exc}", file=sys.stderr)
        if probe is not None and probe.proc.poll() is None:
            probe.proc.kill()
        if not args.keep_open:
            C._kill_chrome_with_profile(session.user_data_dir)

    out = os.path.join(HERE, f"{args.out_prefix}_combined.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f"\ncombined report: {out}", flush=True)

    # A deliberately thin summary: the enum meanings are not settled yet, so this
    # prints the raw per-phase deltas and draws no conclusion.
    print("\nper-phase Chrome-side deltas (raw; bucket meanings unresolved):")
    for ph in report["phases"]:
        print(f"  {ph['name']:>10} [{ph['kind']:>6}] {ph['actual_s']:>5.1f}s")
        for pref, hs in ph["page_histograms"].items():
            if not isinstance(hs, dict):
                continue
            for hname, hv in hs.items():
                if not isinstance(hv, dict):
                    continue
                buckets = hv.get("buckets") or []
                bs = " ".join(f"{b['low']}:{b['count_delta']}" for b in buckets)
                print(f"      {hname}: n={hv.get('count_delta')} {bs}")


if __name__ == "__main__":
    main()
