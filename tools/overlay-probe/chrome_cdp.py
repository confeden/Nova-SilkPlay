"""
chrome_cdp.py — Chrome DevTools Protocol harness for the Nova SilkPlay overlay probe.

Overview
--------
This script is one half of a two-part measurement harness.  The other half is a
C++ overlay probe that shows/hides a transparent overlay window over a Chrome
window.  This script drives a *real* (non-headless) Chrome instance via the
Chrome DevTools Protocol (CDP) and samples Chrome's internal GPU compositing
histograms while the overlay is shown or hidden.

Why chrome://histograms instead of Browser.getHistograms?
  CDP's Browser.getHistograms returns ONLY browser-process histograms.
  GPU.DirectComposition.* live in the GPU process and never appear there.
  Opening chrome://histograms/<PREFIX> in a tab and reading
  document.body.innerText via Runtime.evaluate DOES include GPU-process
  histograms.  The page's numbers are cumulative from browser start, so
  consecutive snapshots are diffed to obtain per-phase values.

Subcommands
-----------
run         Launch Chrome at URL, inject JS probes, step through named phases
            (e.g. hidden:20,shown:40,hidden:20), and sample GPU.DirectComposition,
            GPU.OutputPresenter and Compositing histograms plus JS-side RAF/rVFC
            counters at each phase boundary.  Writes a JSON report.

            Phase synchronisation with the external C++ tool:
              - stdout: "PHASE_BEGIN <name> <seconds>\n" (flushed immediately)
              - stdout: "PHASE_END   <name>\n"           (flushed immediately)
              --wait-for-stdin-phases: instead of sleeping, block reading a
              newline from stdin per phase transition.

histograms  Attach to an *already-running* Chrome on the given port
            (started with --remote-debugging-port=N) and dump histograms as JSON.

gpuinfo     Attach to an already-running Chrome and dump SystemInfo.getInfo.

WebSocket implementation
------------------------
Pure stdlib: socket, base64, os.urandom, struct — RFC 6455 client handshake,
masked client frames, text frames, continuation frames, ping/pong, server close.
No websockets, no websocket-client, no requests.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from typing import Any, NamedTuple

# Windows consoles default to cp1252; paths and report text are UTF-8.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

# ---------------------------------------------------------------------------
# Raw RFC 6455 WebSocket client
# ---------------------------------------------------------------------------

_WS_MAGIC = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_OP_CONTINUATION = 0x0
_OP_TEXT = 0x1
_OP_BINARY = 0x2
_OP_CLOSE = 0x8
_OP_PING = 0x9
_OP_PONG = 0xA


class WebSocketError(Exception):
    pass


class WebSocket:
    """Minimal RFC 6455 client WebSocket over a plain TCP socket."""

    def __init__(self, host: str, port: int, path: str, timeout: float = 30.0) -> None:
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._recv_buf = b""
        self._closed = False
        self._lock = threading.Lock()
        self._handshake(host, port, path)

    # ------------------------------------------------------------------
    # Handshake
    # ------------------------------------------------------------------

    def _handshake(self, host: str, port: int, path: str) -> None:
        key = base64.b64encode(os.urandom(16)).decode()
        headers = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self._sock.sendall(headers.encode())
        resp = self._recv_until(b"\r\n\r\n")
        if b"101" not in resp.split(b"\r\n", 1)[0]:
            raise WebSocketError(f"WebSocket upgrade failed: {resp[:200]!r}")
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        if expected.encode() not in resp:
            raise WebSocketError("Sec-WebSocket-Accept mismatch")

    def _recv_until(self, sentinel: bytes) -> bytes:
        while sentinel not in self._recv_buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise WebSocketError("Connection closed during handshake")
            self._recv_buf += chunk
        idx = self._recv_buf.index(sentinel) + len(sentinel)
        result, self._recv_buf = self._recv_buf[:idx], self._recv_buf[idx:]
        return result

    # ------------------------------------------------------------------
    # Send (client to server, must be masked per RFC 6455 section 5.3)
    # ------------------------------------------------------------------

    def send_text(self, text: str) -> None:
        self._send_frame(_OP_TEXT, text.encode("utf-8"))

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        with self._lock:
            if self._closed:
                raise WebSocketError("WebSocket is closed")
            mask = os.urandom(4)
            masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            length = len(payload)
            header = bytes([0x80 | opcode])
            if length < 126:
                header += bytes([0x80 | length])
            elif length < 65536:
                header += struct.pack("!BH", 0x80 | 126, length)
            else:
                header += struct.pack("!BQ", 0x80 | 127, length)
            header += mask
            self._sock.sendall(header + masked)

    # ------------------------------------------------------------------
    # Receive (server to client, never masked)
    # ------------------------------------------------------------------

    def recv_message(self) -> str | None:
        """
        Read one complete message (possibly assembled from continuation frames).
        Returns the UTF-8 text, or None on clean close.
        Handles ping (sends pong) and server-initiated close.
        """
        fragments: list[bytes] = []
        while True:
            opcode, payload, fin = self._recv_frame()
            if opcode == _OP_PING:
                self._send_frame(_OP_PONG, payload)
                continue
            if opcode == _OP_PONG:
                continue
            if opcode == _OP_CLOSE:
                if not self._closed:
                    self._send_frame(_OP_CLOSE, payload[:2] if len(payload) >= 2 else b"")
                    self._closed = True
                return None
            if opcode in (_OP_TEXT, _OP_BINARY):
                fragments = [payload]
            elif opcode == _OP_CONTINUATION:
                fragments.append(payload)
            else:
                raise WebSocketError(f"Unknown opcode {opcode:#x}")
            if fin:
                return b"".join(fragments).decode("utf-8", errors="replace")

    def _recv_exact(self, n: int) -> bytes:
        while len(self._recv_buf) < n:
            chunk = self._sock.recv(max(4096, n - len(self._recv_buf)))
            if not chunk:
                raise WebSocketError("Connection closed while reading frame")
            self._recv_buf += chunk
        result, self._recv_buf = self._recv_buf[:n], self._recv_buf[n:]
        return result

    def _recv_frame(self) -> tuple[int, bytes, bool]:
        header = self._recv_exact(2)
        fin = bool(header[0] & 0x80)
        opcode = header[0] & 0x0F
        masked = bool(header[1] & 0x80)
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        mask_key = self._recv_exact(4) if masked else b""
        payload = self._recv_exact(length)
        if masked:
            payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
        return opcode, payload, fin

    def close(self) -> None:
        if not self._closed:
            try:
                self._send_frame(_OP_CLOSE, struct.pack("!H", 1000))
            except Exception:
                pass
            self._closed = True
        try:
            self._sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# CDP client
# ---------------------------------------------------------------------------

class CDPError(Exception):
    pass


class CDP:
    """
    Synchronous CDP client connected to Chrome's browser-level WebSocket.
    Runs a background reader thread that routes responses to waiting callers
    and drops (buffered) unsolicited events.
    """

    def __init__(self, ws_url: str, timeout: float = 30.0) -> None:
        # Parse ws://host:port/path
        rest = ws_url.removeprefix("ws://")
        host_port, path = rest.split("/", 1)
        host, port_str = host_port.rsplit(":", 1)
        self._ws = WebSocket(host, int(port_str), "/" + path, timeout=timeout)
        self._timeout = timeout
        self._id = 0
        self._id_lock = threading.Lock()
        self._pending: dict[int, dict] = {}
        self._pending_events: list[dict] = []
        self._cond = threading.Condition(threading.Lock())
        self._reader_error: BaseException | None = None
        self._closing = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self) -> None:
        while True:
            try:
                msg = self._ws.recv_message()
            except (socket.timeout, TimeoutError):
                # A quiet CDP connection is normal: during a long measurement
                # phase no events arrive for tens of seconds. Treating the
                # socket read timeout as fatal killed this thread and turned
                # every later send() into a bogus "Timeout waiting for
                # response", which looks like Chrome hanging. Keep waiting.
                if self._closing:
                    break
                continue
            except Exception as exc:                       # noqa: BLE001
                with self._cond:
                    self._reader_error = exc
                    self._cond.notify_all()
                break
            if msg is None:
                with self._cond:
                    self._reader_error = self._reader_error or ConnectionError(
                        "the browser closed the DevTools websocket")
                    self._cond.notify_all()
                break
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                continue
            with self._cond:
                if "id" in data:
                    self._pending[data["id"]] = data
                else:
                    self._pending_events.append(data)
                self._cond.notify_all()

    def send(
        self,
        method: str,
        params: dict | None = None,
        session_id: str | None = None,
    ) -> dict:
        with self._id_lock:
            self._id += 1
            msg_id = self._id
        msg: dict[str, Any] = {"id": msg_id, "method": method}
        if params:
            msg["params"] = params
        if session_id:
            msg["sessionId"] = session_id
        self._ws.send_text(json.dumps(msg))
        deadline = time.monotonic() + self._timeout
        with self._cond:
            while msg_id not in self._pending:
                if self._reader_error is not None:
                    raise CDPError(
                        f"DevTools connection lost while waiting for {method!r}: "
                        f"{self._reader_error!r}"
                    ) from self._reader_error
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise CDPError(
                        f"Timeout waiting for response to {method!r} (id={msg_id})"
                    )
                self._cond.wait(remaining)
            result = self._pending.pop(msg_id)
        if "error" in result:
            err = result["error"]
            raise CDPError(
                f"CDP error in {method!r}: [{err.get('code')}] {err.get('message')}"
            )
        return result.get("result", {})

    def close(self) -> None:
        self._closing = True
        self._ws.close()


# ---------------------------------------------------------------------------
# High-level helpers
# ---------------------------------------------------------------------------

_NO_PROXY_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

CHROME_EXE = r"C:\Program Files\Google\Chrome\Application\chrome.exe"


class ChromeSession(NamedTuple):
    """A live DevTools endpoint plus, when we launched it, the process."""
    proc: subprocess.Popen | None
    port: int
    ws_url: str
    user_data_dir: str


_EXCLUDED_RANGES: list[tuple[int, int]] | None = None


def excluded_tcp_ranges() -> list[tuple[int, int]]:
    """
    Windows reserves TCP port blocks (WinNAT/Hyper-V) that no user process can
    bind.  On this machine 9124-9223 is reserved, which CONTAINS the
    conventional CDP port 9222 — Chrome then silently never opens DevTools.
    Parsed from `netsh interface ipv4 show excludedportrange protocol=tcp`.
    """
    global _EXCLUDED_RANGES
    if _EXCLUDED_RANGES is not None:
        return _EXCLUDED_RANGES
    ranges: list[tuple[int, int]] = []
    try:
        out = subprocess.run(
            ["netsh", "interface", "ipv4", "show", "excludedportrange", "protocol=tcp"],
            capture_output=True, text=True, timeout=15,
        ).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
                ranges.append((int(parts[0]), int(parts[1])))
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"warning: could not read excluded port ranges: {exc}", file=sys.stderr)
    _EXCLUDED_RANGES = ranges
    return ranges


def check_port_bindable(port: int) -> None:
    """Raise ValueError if `port` falls inside a Windows reserved range."""
    if port == 0:
        return
    for lo, hi in excluded_tcp_ranges():
        if lo <= port <= hi:
            raise ValueError(
                f"TCP port {port} is inside the Windows reserved range {lo}-{hi}; "
                f"Chrome cannot bind it and DevTools will never come up. "
                f"Use --port 0 (Chrome picks a free port, we read it back from "
                f"DevToolsActivePort)."
            )


def read_devtools_active_port(user_data_dir: str) -> tuple[int, str] | None:
    """
    Chrome writes <user-data-dir>/DevToolsActivePort while it is running:
    line 1 = the actual TCP port, line 2 = the browser websocket path.
    It is deleted on a clean shutdown, so absence means "not running".
    """
    path = os.path.join(user_data_dir, "DevToolsActivePort")
    try:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    if not lines or not lines[0].strip().isdigit():
        return None
    return int(lines[0].strip()), (lines[1].strip() if len(lines) > 1 else "")


def _poll_devtools(port: int, timeout: float = 30.0) -> dict:
    """Poll /json/version until Chrome answers, return parsed JSON."""
    url = f"http://127.0.0.1:{port}/json/version"
    deadline = time.monotonic() + timeout
    last_exc: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with _NO_PROXY_OPENER.open(url, timeout=2) as r:
                return json.loads(r.read())
        except Exception as exc:
            last_exc = exc
            time.sleep(0.25)
    raise TimeoutError(
        f"Chrome devtools on port {port} did not respond within {timeout}s."
        f" Last error: {last_exc}"
    )


def launch_chrome(
    url: str,
    port: int = 0,
    user_data_dir: str | None = None,
    extra_args: list[str] | None = None,
    timeout: float = 30.0,
) -> ChromeSession:
    """
    Launch Chrome with remote debugging and a throwaway profile.

    `port=0` (the default) lets Chrome pick a free port; we read the real one
    back from <user-data-dir>/DevToolsActivePort.  That is not a nicety: the
    conventional 9222 is inside a Windows reserved range on this machine.
    """
    check_port_bindable(port)
    if user_data_dir is None:
        user_data_dir = tempfile.mkdtemp(prefix="chrome_cdp_")
    args = [
        CHROME_EXE,
        f"--remote-debugging-port={port}",
        f"--user-data-dir={user_data_dir}",
        "--no-first-run",
        "--no-default-browser-check",
        "--autoplay-policy=no-user-gesture-required",
        "--disable-features=Translate",
        url,
    ]
    if extra_args:
        args.extend(extra_args)
    proc = subprocess.Popen(args)

    deadline = time.monotonic() + timeout
    actual: tuple[int, str] | None = None
    while time.monotonic() < deadline:
        actual = read_devtools_active_port(user_data_dir)
        if actual is not None:
            break
        time.sleep(0.1)
    if actual is None:
        proc.terminate()
        raise TimeoutError(
            f"Chrome never wrote DevToolsActivePort in {user_data_dir} within "
            f"{timeout}s (requested port {port}). Chrome may have failed to bind "
            f"the debug port; reserved ranges on this machine: "
            f"{excluded_tcp_ranges()}"
        )
    actual_port, ws_path = actual
    version = _poll_devtools(actual_port, timeout=max(5.0, deadline - time.monotonic()))
    ws_url = version.get("webSocketDebuggerUrl") or f"ws://127.0.0.1:{actual_port}{ws_path}"
    return ChromeSession(proc=proc, port=actual_port, ws_url=ws_url,
                         user_data_dir=user_data_dir)


def find_running_chrome() -> ChromeSession:
    """
    Locate an already-running Chrome that was started with
    --remote-debugging-port, by reading each candidate's DevToolsActivePort.
    """
    ps = (
        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
        "Select-Object -ExpandProperty CommandLine"
    )
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError(f"could not enumerate chrome.exe processes: {exc}") from exc

    seen: set[str] = set()
    for line in out.splitlines():
        if "--remote-debugging-port" not in line or "--type=" in line:
            continue
        m = re.search(r'--user-data-dir=(?:"([^"]+)"|(\S+))', line)
        if not m:
            continue
        udd = m.group(1) or m.group(2)
        if udd in seen:
            continue
        seen.add(udd)
        found = read_devtools_active_port(udd)
        if found is None:
            continue
        port, ws_path = found
        try:
            version = _poll_devtools(port, timeout=3)
        except TimeoutError:
            continue
        ws_url = version.get("webSocketDebuggerUrl") or f"ws://127.0.0.1:{port}{ws_path}"
        return ChromeSession(proc=None, port=port, ws_url=ws_url, user_data_dir=udd)

    raise RuntimeError(
        "No running Chrome with an open DevTools endpoint was found. Start one "
        "with:  chrome.exe --remote-debugging-port=0 --user-data-dir=<dir> <url>  "
        "(port 0 on purpose — 9222 is inside a Windows reserved range here.)"
    )


def resolve_endpoint(port: int = 0, user_data_dir: str | None = None) -> ChromeSession:
    """--port / --user-data-dir / autodetect, in that order of precedence."""
    if port:
        version = _poll_devtools(port, timeout=5)
        ws_url = version["webSocketDebuggerUrl"]
        return ChromeSession(proc=None, port=port, ws_url=ws_url, user_data_dir="")
    if user_data_dir:
        found = read_devtools_active_port(user_data_dir)
        if found is None:
            raise RuntimeError(
                f"No DevToolsActivePort in {user_data_dir} — that Chrome is not "
                f"running, or was started without --remote-debugging-port."
            )
        p, ws_path = found
        version = _poll_devtools(p, timeout=5)
        ws_url = version.get("webSocketDebuggerUrl") or f"ws://127.0.0.1:{p}{ws_path}"
        return ChromeSession(proc=None, port=p, ws_url=ws_url, user_data_dir=user_data_dir)
    return find_running_chrome()


def get_histograms(
    cdp: CDP,
    query: str = "GPU.DirectComposition",
    delta: bool = False,
) -> dict[str, dict]:
    """
    Call Browser.getHistograms and return a dict:
      name -> {"sum": int, "count": int, "buckets": [...]}
    """
    result = cdp.send("Browser.getHistograms", {"query": query, "delta": delta})
    out: dict[str, dict] = {}
    for h in result.get("histograms", []):
        out[h["name"]] = {
            "sum": h.get("sum", 0),
            "count": h.get("count", 0),
            "buckets": [
                {"low": b["low"], "high": b["high"], "count": b["count"]}
                for b in h.get("buckets", [])
            ],
        }
    return out


def list_page_targets(cdp: CDP) -> list[dict]:
    """Return all targets of type 'page'."""
    result = cdp.send("Target.getTargets")
    return [t for t in result.get("targetInfos", []) if t.get("type") == "page"]


def attach_page(cdp: CDP, target_id: str) -> str:
    """Attach to a page target (flat session) and return the sessionId."""
    result = cdp.send("Target.attachToTarget", {"targetId": target_id, "flatten": True})
    return result["sessionId"]


def eval_js(
    cdp: CDP,
    session_id: str,
    expr: str,
    await_promise: bool = False,
) -> Any:
    """Evaluate JS in a page session; return the value or raise on exception."""
    result = cdp.send(
        "Runtime.evaluate",
        {
            "expression": expr,
            "returnByValue": True,
            "awaitPromise": await_promise,
        },
        session_id=session_id,
    )
    exc = result.get("exceptionDetails")
    if exc:
        raise CDPError(f"JS exception in Runtime.evaluate: {exc}")
    obj = result.get("result", {})
    return obj.get("value")



# ---------------------------------------------------------------------------
# chrome://histograms scraper
#
# Browser.getHistograms (established fact A) returns ONLY browser-process
# histograms.  GPU.DirectComposition.* live in the GPU process and never
# appear there.  Opening chrome://histograms/<PREFIX> in a tab (fact B) and
# reading document.body.innerText via Runtime.evaluate DOES include GPU-process
# histograms.  The page numbers are CUMULATIVE from browser start; diff
# consecutive snapshots via diff_histograms() to get per-phase values.
# ---------------------------------------------------------------------------

def open_histogram_tab(cdp: CDP, prefix: str = "") -> tuple[str, str]:
    """
    Open chrome://histograms/<prefix> in a new tab, attach to it, enable
    Runtime, and return (targetId, sessionId).
    """
    url = f"chrome://histograms/{prefix}"
    result = cdp.send("Target.createTarget", {"url": url})
    new_target_id: str = result["targetId"]

    # Wait until the new target is visible in Target.getTargets (up to 10 s).
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        targets = cdp.send("Target.getTargets").get("targetInfos", [])
        if any(t["targetId"] == new_target_id for t in targets):
            break
        time.sleep(0.2)

    session_id = attach_page(cdp, new_target_id)
    cdp.send("Runtime.enable", session_id=session_id)
    return new_target_id, session_id


def scrape_histogram_page(cdp: CDP, session_id: str) -> dict[str, dict]:
    """
    Reload the histogram page, wait for it to be ready, then read and parse
    document.body.innerText.

    Returns {name: {"count": int, "mean": float|None,
                    "buckets": [{"low": int, "count": int}, ...]}}.
    The page's numbers are CUMULATIVE from browser start; diff consecutive
    snapshots (via diff_histograms) to get per-phase values.
    """
    # Reload via JS to avoid needing Page.enable and its event machinery.
    try:
        eval_js(cdp, session_id, "location.reload()")
    except CDPError as exc:
        print(f"warning: histogram page reload failed: {exc}", file=sys.stderr)

    # Poll document.readyState until complete (up to 10 s), then a short settle.
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        try:
            state = eval_js(cdp, session_id, "document.readyState")
            if state == "complete":
                break
        except CDPError as exc:
            print(f"warning: readyState poll error: {exc}", file=sys.stderr)
        time.sleep(0.15)
    time.sleep(0.3)  # let the page's JS finish rendering histogram blocks

    try:
        text: str = eval_js(cdp, session_id, "document.body.innerText") or ""
    except CDPError as exc:
        print(f"warning: could not read histogram page text: {exc}", file=sys.stderr)
        return {}

    return _parse_histogram_text(text)


def _parse_histogram_text(text: str) -> dict[str, dict]:
    """
    Parse the plain-text body of chrome://histograms/<prefix>.

    Header line pattern (one block per histogram, blocks separated by blank lines):
      - Histogram: <Name> recorded <N> samples[, mean = <M>] (flags = ...) [#]
    Bucket line pattern:
      <low_bound>  <bar_chars> (<count> = <pct>%)
    Lines whose body is "..." are elisions (no bucket count) and are skipped.
    Page chrome ("Histograms", "Refresh Download", etc.) is ignored.
    """
    # Header: optional leading "- " prefix, captures name / sample count / mean.
    header_re = re.compile(
        r"^-?\s*Histogram:\s+(\S+)\s+recorded\s+(\d+)\s+samples"
        r"(?:,\s*mean\s*=\s*([0-9.]+))?",
        re.MULTILINE,
    )
    # Bucket line: starts with an integer, body is NOT a pure "...", contains
    # (<count> = <pct>%) parenthetical.
    bucket_re = re.compile(
        r"^\s*(-?\d+)\s+(?!\.\.\.).*\((\d+)\s*=\s*[0-9.]+%\)",
        re.MULTILINE,
    )

    result: dict[str, dict] = {}
    # NOTE: do NOT split on blank lines. chrome://histograms puts a blank line
    # between a histogram's header and its bucket rows, so blank-line blocks
    # would separate every header from its own buckets and silently yield
    # empty bucket lists. Slice on header positions instead.
    headers = list(header_re.finditer(text))
    for i, hm in enumerate(headers):
        name = hm.group(1)
        count = int(hm.group(2))
        mean_str = hm.group(3)
        mean: float | None = float(mean_str) if mean_str is not None else None

        body_end = headers[i + 1].start() if i + 1 < len(headers) else len(text)
        body = text[hm.end():body_end]

        buckets: list[dict] = []
        for bm in bucket_re.finditer(body):
            buckets.append({"low": int(bm.group(1)), "count": int(bm.group(2))})

        result[name] = {"count": count, "mean": mean, "buckets": buckets}

    return result


def diff_histograms(before: dict, after: dict) -> dict:
    """
    Compute per-histogram deltas between two cumulative chrome://histograms
    snapshots.  The page numbers are CUMULATIVE from browser start, so
    subtracting consecutive snapshots gives per-phase increments.

    Returns one entry per histogram that changed:
      {name: {"count_delta": int, "mean_after": float|None, "new": bool,
              "buckets": [{"low": int, "count_delta": int}, ...]}}

    Histograms and buckets whose delta is zero are dropped.
    A histogram present only in `after` (not in `before`) is marked "new": True.
    """
    out: dict = {}
    for name, a_val in after.items():
        b_val = before.get(name)
        is_new = b_val is None
        b_count = 0 if is_new else b_val["count"]
        count_delta = a_val["count"] - b_count

        b_buckets: dict[int, int] = {}
        if not is_new:
            for bkt in b_val.get("buckets", []):
                b_buckets[bkt["low"]] = bkt["count"]

        bucket_deltas: list[dict] = []
        for bkt in a_val.get("buckets", []):
            low = bkt["low"]
            delta = bkt["count"] - b_buckets.get(low, 0)
            if delta != 0:
                bucket_deltas.append({"low": low, "count_delta": delta})

        if count_delta == 0 and not bucket_deltas:
            continue  # nothing changed

        out[name] = {
            "count_delta": count_delta,
            "mean_after": a_val.get("mean"),
            "new": is_new,
            "buckets": bucket_deltas,
        }
    return out


def close_histogram_tab(cdp: CDP, target_id: str) -> None:
    """Close a histogram tab by targetId."""
    try:
        cdp.send("Target.closeTarget", {"targetId": target_id})
    except CDPError as exc:
        print(f"warning: could not close histogram tab {target_id}: {exc}",
              file=sys.stderr)


# ---------------------------------------------------------------------------
# JS probe code injected once at page load
# ---------------------------------------------------------------------------

_PROBE_INIT_JS = r"""
(function() {
  if (window.__probe) return 'already';
  window.__probe = {
    raf: 0, rvfc: 0,
    droppedVideoFrames: 0, totalVideoFrames: 0,
  };
  function rafLoop() {
    window.__probe.raf++;
    requestAnimationFrame(rafLoop);
  }
  requestAnimationFrame(rafLoop);
  function attachVideo(v) {
    function rvfcLoop(now, meta) {
      window.__probe.rvfc++;
      if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(rvfcLoop);
    }
    if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(rvfcLoop);
    setInterval(function() {
      var q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;
      if (q) {
        window.__probe.droppedVideoFrames = q.droppedVideoFrames;
        window.__probe.totalVideoFrames = q.totalVideoFrames;
      }
    }, 500);
  }
  var vid = document.querySelector('video');
  if (vid) {
    attachVideo(vid);
  } else {
    var mo = new MutationObserver(function() {
      var v = document.querySelector('video');
      if (v) { mo.disconnect(); attachVideo(v); }
    });
    mo.observe(document, {childList: true, subtree: true});
  }
  return 'ok';
})()
"""

# Re-query the video element at snapshot time rather than trusting a reference
# captured at injection time.  The original code stored videoWidth/videoHeight/
# currentTime via a stale rvfcLoop closure; if that element was replaced the
# values would silently stay at 0.  Querying fresh each snapshot is robust.
# If no video element exists at snapshot time, return {"video": null} so the
# caller can detect "not measured" rather than silently getting zeros — a zero
# that means "not measured" is worse than an explicit error.
_PROBE_READ_JS = r"""
(function() {
  var p = window.__probe || {};
  var raf_now  = p.raf  || 0;
  var rvfc_now = p.rvfc || 0;
  // Reset per-phase counters so the next snapshot gives a phase delta.
  if (window.__probe) { window.__probe.raf = 0; window.__probe.rvfc = 0; }

  // Re-query the video element fresh — do NOT rely on a stale closure reference.
  var v = document.querySelector('video');
  if (!v) {
    return {
      video: null,
      rafCount: raf_now,
      rvfcCount: rvfc_now,
      droppedVideoFrames: p.droppedVideoFrames || 0,
      totalVideoFrames:   p.totalVideoFrames   || 0,
    };
  }
  // getVideoPlaybackQuality() is the authoritative source; the counters kept on
  // __probe are only a fallback for browsers that do not expose it.
  var q = (typeof v.getVideoPlaybackQuality === 'function')
            ? v.getVideoPlaybackQuality() : null;
  return {
    video: 'ok',
    rafCount:           raf_now,
    rvfcCount:          rvfc_now,
    droppedVideoFrames: q ? q.droppedVideoFrames : (p.droppedVideoFrames || 0),
    totalVideoFrames:   q ? q.totalVideoFrames   : (p.totalVideoFrames   || 0),
    videoWidth:         v.videoWidth,
    videoHeight:        v.videoHeight,
    currentTime:        v.currentTime,
    paused:             v.paused,
    readyState:         v.readyState,
  };
})()
"""


# ---------------------------------------------------------------------------
# CLI: run subcommand
# ---------------------------------------------------------------------------

def _phase_begin(name: str, seconds: float) -> None:
    print(f"PHASE_BEGIN {name} {seconds}", flush=True)


def _phase_end(name: str) -> None:
    print(f"PHASE_END {name}", flush=True)


def cmd_run(args: argparse.Namespace) -> None:
    phases: list[tuple[str, float]] = []
    for token in args.phases.split(","):
        name, _, secs = token.strip().partition(":")
        phases.append((name.strip(), float(secs.strip())))

    session = launch_chrome(args.url, port=args.port,
                            user_data_dir=getattr(args, "user_data_dir", None))
    print(f"chrome devtools on port {session.port} "
          f"(profile {session.user_data_dir})", flush=True)
    try:
        version = _poll_devtools(session.port)
        cdp = CDP(session.ws_url)
        try:
            page_target_id: str | None = None
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                targets = list_page_targets(cdp)
                if targets:
                    page_target_id = targets[0]["targetId"]
                    break
                time.sleep(0.5)
            if page_target_id is None:
                raise RuntimeError("No page target appeared within 20 s")

            session_id = attach_page(cdp, page_target_id)
            cdp.send("Runtime.enable", session_id=session_id)

            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                try:
                    state = eval_js(cdp, session_id, "document.readyState")
                    if state in ("interactive", "complete"):
                        break
                except CDPError:
                    pass
                time.sleep(0.5)

            eval_js(cdp, session_id, _PROBE_INIT_JS)

            # Baseline reset. Queries are substring matches on the histogram
            # name; keep them broad because Chromium renames these often
            # (CompositionMode -> CompositionMode2 -> ...), and a query that
            # silently matches nothing is indistinguishable from "the feature
            # never fired" — which would be a wrong measurement.
            queries: list[str] = args.queries
            for q in queries:
                get_histograms(cdp, query=q, delta=True)

            # GPU-process histograms (every GPU.DirectComposition.*) are NOT
            # reachable through Browser.getHistograms — only the chrome://
            # histograms WebUI merges child-process records. One tab per prefix,
            # opened once, scraped and diffed per phase.
            prefixes: list[str] = [] if args.no_page_histograms else list(args.page_histograms)
            hist_tabs: dict[str, tuple[str, str]] = {}   # prefix -> (targetId, sessionId)
            prev_snapshot: dict[str, dict] = {}
            for pref in prefixes:
                try:
                    hist_tabs[pref] = open_histogram_tab(cdp, pref)
                    prev_snapshot[pref] = scrape_histogram_page(cdp, hist_tabs[pref][1])
                except (CDPError, RuntimeError, TimeoutError) as exc:
                    print(f"warning: histogram tab for {pref!r} failed: {exc}",
                          file=sys.stderr)
            # The video tab must be the foreground one: a background tab is
            # throttled and its compositing path is not what we are measuring.
            cdp.send("Target.activateTarget", {"targetId": page_target_id})

            report: dict[str, Any] = {
                "url": args.url,
                "port": session.port,
                "queries": queries,
                "page_histogram_prefixes": prefixes,
                "phases": [],
                "chrome_version": version.get("Browser", ""),
            }

            for phase_name, phase_secs in phases:
                _phase_begin(phase_name, phase_secs)

                t0 = time.monotonic()
                if args.wait_for_stdin_phases:
                    sys.stdin.readline()
                else:
                    time.sleep(phase_secs)
                actual_s = time.monotonic() - t0

                sampled = {q: get_histograms(cdp, query=q, delta=True)
                           for q in queries}

                js_data: Any = None
                try:
                    js_data = eval_js(cdp, session_id, _PROBE_READ_JS)
                except CDPError as exc:
                    js_data = {"error": str(exc)}

                page_hist: dict[str, dict] = {}
                for pref, (_tid, sid) in hist_tabs.items():
                    try:
                        cur = scrape_histogram_page(cdp, sid)
                        page_hist[pref] = diff_histograms(prev_snapshot.get(pref, {}), cur)
                        prev_snapshot[pref] = cur
                    except (CDPError, RuntimeError, TimeoutError) as exc:
                        page_hist[pref] = {"error": str(exc)}
                        print(f"warning: scrape of {pref!r} failed: {exc}", file=sys.stderr)
                if hist_tabs:
                    cdp.send("Target.activateTarget", {"targetId": page_target_id})

                _phase_end(phase_name)

                report["phases"].append({
                    "name": phase_name,
                    "duration_s": phase_secs,
                    "actual_duration_s": round(actual_s, 3),
                    "histograms": sampled,
                    "page_histograms": page_hist,
                    "js": js_data,
                })

            for _pref, (tid, _sid) in hist_tabs.items():
                try:
                    close_histogram_tab(cdp, tid)
                except CDPError as exc:
                    print(f"warning: could not close histogram tab: {exc}", file=sys.stderr)

            with open(args.out, "w", encoding="utf-8") as f:
                json.dump(report, f, indent=2)
            print(f"Report written to {args.out}", flush=True)
        finally:
            cdp.close()
    finally:
        if session.proc is not None and not getattr(args, "keep_open", False):
            session.proc.terminate()
            try:
                session.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                session.proc.kill()
            # Chrome's launcher process often exits immediately and re-spawns
            # the real browser, so terminating the Popen handle is not enough.
            _kill_chrome_with_profile(session.user_data_dir)


def _kill_chrome_with_profile(user_data_dir: str) -> None:
    """Kill every chrome.exe whose command line names this profile directory."""
    if not user_data_dir:
        return
    escaped = user_data_dir.replace("\\", "\\\\").replace("'", "''")
    ps = (
        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
        f"Where-Object {{ $_.CommandLine -like '*{escaped}*' }} | "
        "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
    )
    try:
        subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                       capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"warning: could not clean up Chrome for {user_data_dir}: {exc}",
              file=sys.stderr)


# ---------------------------------------------------------------------------
# CLI: histograms subcommand
# ---------------------------------------------------------------------------

def cmd_histograms(args: argparse.Namespace) -> None:
    """Attach to an already-running Chrome and dump histograms as JSON."""
    session = resolve_endpoint(args.port, getattr(args, "user_data_dir", None))
    cdp = CDP(session.ws_url)
    try:
        result = get_histograms(cdp, query=args.query, delta=args.delta)
        print(json.dumps(result, indent=2))
    finally:
        cdp.close()


# ---------------------------------------------------------------------------
# CLI: gpuinfo subcommand
# ---------------------------------------------------------------------------

def cmd_gpuinfo(args: argparse.Namespace) -> None:
    """Attach to an already-running Chrome and dump SystemInfo.getInfo as JSON."""
    session = resolve_endpoint(args.port, getattr(args, "user_data_dir", None))
    cdp = CDP(session.ws_url)
    try:
        result = cdp.send("SystemInfo.getInfo")
        print(json.dumps(result, indent=2))
    finally:
        cdp.close()


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="chrome_cdp",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_run = sub.add_parser(
        "run",
        help="Launch Chrome, run phased measurement, write JSON report.",
    )
    p_run.add_argument("--url", required=True, help="URL to open in Chrome.")
    p_run.add_argument(
        "--port", type=int, default=0,
        help="CDP port. Default 0 = let Chrome pick a free one and read it back "
             "from DevToolsActivePort (9222 is inside a Windows reserved range here).",
    )
    p_run.add_argument(
        "--user-data-dir", default=None,
        help="Chrome profile directory to use. Default: a fresh one under %%TEMP%%.",
    )
    p_run.add_argument(
        "--phases",
        default="hidden:10,shown:20,hidden:10",
        help="Comma-separated name:seconds pairs, e.g. hidden:20,shown:40,hidden:20.",
    )
    p_run.add_argument(
        "--queries", nargs="+",
        default=["GPU.", "Compositing", "Media.VideoFrame", "Graphics."],
        help="Histogram name substrings sampled per phase.",
    )
    p_run.add_argument(
        "--page-histograms", action="append", default=None,
        dest="page_histograms",
        help="chrome://histograms prefix to scrape per phase (repeatable). "
             "Default: GPU.DirectComposition, GPU.OutputPresenter, Media.VideoFrameSubmitter.",
    )
    p_run.add_argument(
        "--no-page-histograms", action="store_true",
        help="Skip the chrome://histograms tabs entirely.",
    )
    p_run.add_argument("--out", default="report.json", help="Output JSON path.")
    p_run.add_argument(
        "--keep-open", action="store_true",
        help="Leave Chrome running after the last phase.",
    )
    p_run.add_argument(
        "--wait-for-stdin-phases",
        action="store_true",
        help="Block reading a stdin newline per phase transition instead of sleeping.",
    )

    p_hist = sub.add_parser(
        "histograms",
        help=(
            "Dump histograms from an already-running Chrome "
            "(started with --remote-debugging-port=N)."
        ),
    )
    p_hist.add_argument("--port", type=int, default=0,
                        help="0 = find the endpoint from --user-data-dir or by scanning.")
    p_hist.add_argument("--user-data-dir", default=None,
                        help="Read the live port from this profile's DevToolsActivePort.")
    p_hist.add_argument("--query", default="GPU", help="Histogram name substring.")
    p_hist.add_argument(
        "--delta",
        action="store_true",
        help="Return only counts accumulated since the last call.",
    )

    p_gpu = sub.add_parser(
        "gpuinfo",
        help=(
            "Dump GPU/system info from an already-running Chrome "
            "(started with --remote-debugging-port=N)."
        ),
    )
    p_gpu.add_argument("--port", type=int, default=0,
                       help="0 = find the endpoint from --user-data-dir or by scanning.")
    p_gpu.add_argument("--user-data-dir", default=None,
                       help="Read the live port from this profile's DevToolsActivePort.")

    return parser


def main() -> None:
    parser = _build_parser()
    args = parser.parse_args()
    if getattr(args, "page_histograms", None) is None and args.command == "run":
        args.page_histograms = ["GPU.DirectComposition", "GPU.OutputPresenter",
                                "Media.VideoFrameSubmitter"]
    if args.command == "run":
        cmd_run(args)
    elif args.command == "histograms":
        cmd_histograms(args)
    elif args.command == "gpuinfo":
        cmd_gpuinfo(args)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
