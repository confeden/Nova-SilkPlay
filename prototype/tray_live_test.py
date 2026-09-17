"""tray_live_test.py - the tray thread, the Settings window and the engine's reaction to them, live.

Drives silkplay.exe the way Explorer and the user would, through window messages:
  NIN_SELECT on the tray window (a left click)      -> Settings opens on monitor 2,
  WM_COMMAND on the Settings window                 -> a switch flipped, the language changed, Esc,
  WM_COMMAND on the tray window                     -> the menu: generation off, Settings, Quit,
then runs that end with Ctrl+Break (B), start from a hand-broken settings file (C), and are
closed from outside with WM_CLOSE, the way taskkill without /F closes a program (D). Checks the
settings file, the window titles, the log and the exit codes.

It never attaches to a browser, so it is safe while a video plays: runs A and D pass --target-exe
naming a program that does not exist, and runs B and C start with Chrome unselected (C because
an unreadable file must fail closed) and never select it. Its settings file and logs go to %TEMP%\\nsp_tray_live, never to the
real %LOCALAPPDATA%\\Nova SilkPlay\\settings.json.

    python tray_live_test.py        (after build.cmd; 40 checks)
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "silkplay.exe")
TMP = os.path.join(tempfile.gettempdir(), "nsp_tray_live")
SETTINGS = os.path.join(TMP, "settings.json")
RU_TITLE = "Nova SilkPlay — Настройки"

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.restype = wt.HWND
user32.GetDlgItem.restype = wt.HWND
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

WM_NSP_TRAY = 0x8000 + 11  # nsp_tray.cpp
NIN_SELECT = 0x400
WM_COMMAND = 0x0111
BN_CLICKED = 0
CBN_SELCHANGE = 1
CB_SETCURSEL = 0x014E
IDCANCEL = 2
# nsp_tray.cpp menu ids and nsp_settings_window.cpp control ids
CMD_SETTINGS, CMD_TOGGLE, CMD_QUIT = 100, 101, 103
ID_CHROME, ID_POTPLAYER, ID_COUNTER, ID_LANGUAGE = 1000, 1001, 1002, 1003

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else ": " + detail))


def wait_for(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.05)
    return fn()


def read_json():
    try:
        with open(SETTINGS, encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}


def title(hwnd):
    buf = ctypes.create_unicode_buffer(128)
    user32.GetWindowTextW(hwnd, buf, 128)
    return buf.value


def settings_windows():
    found = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        if cls.value == "NovaSilkPlaySettings":
            found.append(h)
        return True

    user32.EnumWindows(proc(cb), 0)
    return found


def click(parent, ctl_id):
    user32.SendMessageW(parent, WM_COMMAND, (BN_CLICKED << 16) | ctl_id, user32.GetDlgItem(parent, ctl_id))


def run_a():
    with open(SETTINGS, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "capture": {"browsers": {"chrome": False}, "players": {"potplayer": True}},
                   "general": {"fpsCounter": True, "language": "en"}}, f)
    log_path = os.path.join(TMP, "run_a.log")
    with open(log_path, "w", encoding="utf-8") as log:
        p = subprocess.Popen([EXE, "--settings-file", SETTINGS, "--ui-monitor", "2",
                              "--target-exe", "nsp_no_such_app.exe"],
                             stdout=log, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            tray = wait_for(lambda: user32.FindWindowW("NovaSilkPlayTray", None), 10)
            check("tray window exists", tray)
            time.sleep(1.5)

            user32.PostMessageW(tray, WM_NSP_TRAY, 0, NIN_SELECT | (1 << 16))
            sw = wait_for(lambda: user32.FindWindowW("NovaSilkPlaySettings", None), 5)
            check("left click opens Settings", sw)
            r = wt.RECT()
            user32.GetWindowRect(sw, ctypes.byref(r))
            primary_w = user32.GetSystemMetrics(0)
            check("Settings opened on monitor 2", r.left >= primary_w or r.right <= 0,
                  f"rect {r.left},{r.top},{r.right},{r.bottom}")
            check("English title from the file", title(sw) == "Nova SilkPlay — Settings", title(sw))
            user32.PostMessageW(tray, WM_NSP_TRAY, 0, NIN_SELECT | (1 << 16))
            time.sleep(0.5)
            check("second click reuses the window", len(settings_windows()) == 1,
                  f"{len(settings_windows())} windows")

            ctls = [user32.GetDlgItem(sw, i) for i in (ID_CHROME, ID_POTPLAYER, ID_COUNTER, ID_LANGUAGE)]
            check("controls exist", all(ctls))
            check("PotPlayer switch is disabled", not user32.IsWindowEnabled(ctls[1]))

            click(sw, ID_CHROME)
            time.sleep(0.6)
            j = read_json()
            check("Chrome on saved", j.get("capture", {}).get("browsers", {}).get("chrome") is True, str(j))
            check("PotPlayer saved as false", j.get("capture", {}).get("players", {}).get("potplayer") is False, str(j))

            click(sw, ID_POTPLAYER)  # disabled: even a click that reaches the window changes nothing
            time.sleep(0.3)
            check("PotPlayer click ignored",
                  read_json().get("capture", {}).get("players", {}).get("potplayer") is False)

            click(sw, ID_COUNTER)
            time.sleep(0.6)
            check("counter off saved", read_json().get("general", {}).get("fpsCounter") is False, str(read_json()))

            user32.SendMessageW(ctls[3], CB_SETCURSEL, 2, 0)
            user32.SendMessageW(sw, WM_COMMAND, (CBN_SELCHANGE << 16) | ID_LANGUAGE, ctls[3])
            time.sleep(0.6)
            check("language ru saved", read_json().get("general", {}).get("language") == "ru", str(read_json()))
            check("window relabelled in Russian", title(sw) == RU_TITLE, title(sw))

            user32.SendMessageW(sw, WM_COMMAND, IDCANCEL, 0)
            check("Esc closes Settings", wait_for(lambda: not user32.FindWindowW("NovaSilkPlaySettings", None), 3))

            user32.PostMessageW(tray, WM_COMMAND, CMD_TOGGLE, 0)
            time.sleep(0.6)
            user32.PostMessageW(tray, WM_COMMAND, CMD_SETTINGS, 0)
            sw2 = wait_for(lambda: user32.FindWindowW("NovaSilkPlaySettings", None), 5)
            check("menu Settings reopens the window", sw2)
            check("reopened window keeps Russian", title(sw2) == RU_TITLE, title(sw2))
            time.sleep(0.5)
            # Quit with Settings still open: the tray thread must close it and exit cleanly.
            user32.PostMessageW(tray, WM_COMMAND, CMD_QUIT, 0)
            try:
                rc = p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                rc = None
            check("quit from the menu exits with 0", rc == 0, f"rc={rc}")
            check("tray window gone", not user32.FindWindowW("NovaSilkPlayTray", None))
            check("Settings window gone", not user32.FindWindowW("NovaSilkPlaySettings", None))
        finally:
            if p.poll() is None:
                p.kill()

    text = open(log_path, encoding="utf-8", errors="replace").read()
    for needle in ["settings: Google Chrome selected (no effect: a --target-* flag decides the target)",
                   "settings: frame rate counter off",
                   "settings: language ru",
                   "frame generation OFF (tray)",
                   "quit (tray)"]:
        check(f"log: {needle}", needle in text)
    check("log: no tray error", "tray:" not in text and "settings window:" not in text)


def run_b():
    with open(SETTINGS, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "capture": {"browsers": {"chrome": False}}, "general": {"language": "en"}}, f)
    log_path = os.path.join(TMP, "run_b.log")
    with open(log_path, "w", encoding="utf-8") as log:
        p = subprocess.Popen([EXE, "--settings-file", SETTINGS, "--ui-monitor", "2"],
                             stdout=log, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        sent = False
        try:
            wait_for(lambda: user32.FindWindowW("NovaSilkPlayTray", None), 10)
            time.sleep(1.5)
            try:
                os.kill(p.pid, signal.CTRL_BREAK_EVENT)
                sent = True
            except OSError as e:
                print(f"SKIP Ctrl+Break: could not send ({e})")
            if sent:
                try:
                    rc = p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    rc = None
                check("Ctrl+Break exits with 0", rc == 0, f"rc={rc}")
                check("tray window gone after Ctrl+Break", not user32.FindWindowW("NovaSilkPlayTray", None))
        finally:
            if p.poll() is None:
                p.kill()
    text = open(log_path, encoding="utf-8", errors="replace").read()
    if sent:
        check("log: console quit line", "console closed or Ctrl+C - quitting" in text)
    check("no browser selected: nothing captured", "no browser is selected in Settings - nothing to capture" in text)
    check("no browser selected: nothing engaged", "target: hwnd=" not in text and "engaged, mode=" not in text)


def run_c():
    """An unreadable settings file: nothing captured, the file kept aside, not overwritten."""
    aside = SETTINGS + ".unreadable"
    if os.path.exists(aside):
        os.remove(aside)
    with open(SETTINGS, "w", encoding="utf-8") as f:
        f.write('{"capture": {"browsers": {"chrome": true,}}  <- hand-edited, broken')
    log_path = os.path.join(TMP, "run_c.log")
    with open(log_path, "w", encoding="utf-8") as log:
        p = subprocess.Popen([EXE, "--settings-file", SETTINGS, "--ui-monitor", "2"],
                             stdout=log, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            tray = wait_for(lambda: user32.FindWindowW("NovaSilkPlayTray", None), 10)
            time.sleep(2.5)
            user32.PostMessageW(tray, WM_COMMAND, CMD_QUIT, 0)
            try:
                rc = p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                rc = None
            check("unreadable settings: quits with 0", rc == 0, f"rc={rc}")
        finally:
            if p.poll() is None:
                p.kill()
    text = open(log_path, encoding="utf-8", errors="replace").read()
    check("unreadable settings: the user's file kept aside", os.path.exists(aside) and
          "broken" in open(aside, encoding="utf-8", errors="replace").read())
    check("unreadable settings: not rewritten with defaults", not os.path.exists(SETTINGS))
    check("unreadable settings: logged as kept", ".unreadable; nothing is captured" in text)
    check("unreadable settings: nothing captured", "no browser is selected in Settings - nothing to capture" in text)
    check("unreadable settings: nothing engaged", "target: hwnd=" not in text and "engaged, mode=" not in text)


def run_d():
    """Closed from outside: WM_CLOSE on the tray window quits the program, it does not just end
    the UI thread and leave the engine running without an icon. Also --default-settings."""
    log_path = os.path.join(TMP, "run_d.log")
    with open(log_path, "w", encoding="utf-8") as log:
        p = subprocess.Popen([EXE, "--default-settings", "--target-exe", "nsp_no_such_app.exe"],
                             stdout=log, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            tray = wait_for(lambda: user32.FindWindowW("NovaSilkPlayTray", None), 10)
            time.sleep(1.5)
            user32.PostMessageW(tray, 0x0010, 0, 0)  # WM_CLOSE
            try:
                rc = p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                rc = None
            check("WM_CLOSE on the tray window quits the program with 0", rc == 0, f"rc={rc}")
            check("tray window gone after WM_CLOSE", not user32.FindWindowW("NovaSilkPlayTray", None))
        finally:
            if p.poll() is None:
                p.kill()
    text = open(log_path, encoding="utf-8", errors="replace").read()
    check("--default-settings logged", "settings: built-in defaults (--default-settings)" in text)
    check("WM_CLOSE logged as a quit", "quit (tray)" in text)


def main():
    if not os.path.exists(EXE):
        sys.exit(f"{EXE} not found - run build.cmd first")
    if user32.FindWindowW("NovaSilkPlayTray", None):
        sys.exit("ABORT: silkplay.exe is already running (its tray window exists)")
    os.makedirs(TMP, exist_ok=True)
    run_a()
    run_b()
    run_c()
    run_d()
    print(f"\n{sum(results)}/{len(results)} passed (logs in {TMP})")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
