// nsp_tray.cpp — see nsp_tray.h for why this runs on its own thread and owns its own window.

#include "nsp_tray.h"

#include "nsp_resource.h"
#include "nsp_settings_window.h"
#include "nsp_ui_text.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace nsp {
namespace {

constexpr UINT WM_NSP_TRAY = WM_APP + 11;      // the icon's callback
constexpr UINT WM_NSP_STATE = WM_APP + 12;     // engine -> UI: status or on/off changed
constexpr UINT WM_NSP_SHUTDOWN = WM_APP + 13;  // engine -> UI: remove everything and exit
constexpr UINT kIconId = 1;
constexpr UINT_PTR kTimerAddIcon = 1;
constexpr int kAddIconTries = 30;  // every 2 s: a minute for a busy or restarting Explorer

constexpr UINT kCmdSettings = 100;
constexpr UINT kCmdToggle = 101;
constexpr UINT kCmdMode = 102;
constexpr UINT kCmdQuit = 103;

const wchar_t* kClassName = L"NovaSilkPlayTray";

// The shell re-creates the notification area when Explorer restarts, and every icon added
// before that is gone with no notification other than this broadcast message. Registering for it
// is the difference between a tray icon and one that quietly disappears the first time Explorer
// crashes. Written once by the UI thread before its window exists, read only by that thread.
UINT g_taskbarCreated = 0;

}  // namespace

struct Tray::Impl {
    // ---- shared with the engine thread, under `mu`
    mutable std::mutex mu;
    std::deque<Command> commands;
    Settings settings;
    std::wstring status = L"Nova SilkPlay";
    bool engineOn = true;

    // ---- written before `ready` is signalled, read-only afterwards
    std::wstring settingsPath;
    int uiMonitor = 0;
    HANDLE thread = nullptr;
    HANDLE ready = nullptr;
    HWND hwnd = nullptr;
    std::string startErr;
    // Coalesces WM_NSP_STATE: one pending post however often the engine calls in.
    std::atomic<bool> statePosted{false};

    // ---- UI thread only
    HICON iconOn = nullptr;
    HICON iconOff = nullptr;
    bool iconAdded = false;
    std::wstring shownTip;
    bool shownOn = true;
    bool menuOpen = false;
    bool shuttingDown = false;
    int shutdownRetries = 0;
    int addTries = 0;
    SettingsWindow window;

    static DWORD WINAPI ThreadMain(LPVOID param);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void PostState();
    void AddIcon();
    void SyncIcon();
    void RemoveIcon();
    void ShowMenu(POINT anchor);
    void OpenSettings();
    void Push(Command c);
    void OnSettingsChanged(const Settings& s);
    NOTIFYICONDATAW BaseNid() const;
};

NOTIFYICONDATAW Tray::Impl::BaseNid() const {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kIconId;
    return nid;
}

void Tray::Impl::PostState() {
    if (hwnd && !statePosted.exchange(true)) PostMessageW(hwnd, WM_NSP_STATE, 0, 0);
}

void Tray::Impl::AddIcon() {
    NOTIFYICONDATAW nid = BaseNid();
    // NIF_SHOWTIP: with NOTIFYICON_VERSION_4 the standard tooltip is suppressed unless asked for.
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_NSP_TRAY;
    {
        std::lock_guard<std::mutex> lock(mu);
        wcsncpy_s(nid.szTip, status.c_str(), _TRUNCATE);
        shownTip = status;
        shownOn = engineOn;
    }
    nid.hIcon = shownOn ? iconOn : iconOff;
    // After an Explorer restart the old icon is normally gone, but not always: when the broadcast
    // was only a taskbar refresh, NIM_ADD fails because the icon still exists.
    bool ok = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!ok) ok = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
    if (!ok) {
        // A busy shell can also report a timeout for an icon it did add, so "failed" is not proof
        // of absence: retry on a timer (NIM_MODIFY then finds the icon) rather than wait for a
        // TaskbarCreated that may never come.
        const DWORD e = GetLastError();
        iconAdded = false;
        if (addTries == 0)
            LogErr("tray: Shell_NotifyIcon(NIM_ADD) failed, GetLastError=%lu - retrying every 2 s", e);
        if (++addTries < kAddIconTries) {
            SetTimer(hwnd, kTimerAddIcon, 2000, nullptr);
        } else {
            KillTimer(hwnd, kTimerAddIcon);
            LogErr("tray: the icon could not be added after %d tries - it appears if Explorer restarts",
                   addTries);
        }
        return;
    }
    KillTimer(hwnd, kTimerAddIcon);
    if (addTries > 0) Log("tray: icon added after %d failed tries", addTries);
    addTries = 0;
    // Version 4: a left click arrives as NIN_SELECT, a right click or the menu key as
    // WM_CONTEXTMENU, each with the anchor point in wParam.
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    iconAdded = true;
}

void Tray::Impl::SyncIcon() {
    std::wstring tip;
    bool on = true;
    {
        std::lock_guard<std::mutex> lock(mu);
        tip = status;
        on = engineOn;
    }
    if (!iconAdded || (tip == shownTip && on == shownOn)) return;
    NOTIFYICONDATAW nid = BaseNid();
    nid.uFlags = NIF_TIP | NIF_ICON | NIF_SHOWTIP;
    nid.hIcon = on ? iconOn : iconOff;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        shownTip = tip;
        shownOn = on;
    }
}

void Tray::Impl::RemoveIcon() {
    // Unconditionally: an add that reported a timeout may still have put the icon there, and a
    // delete of an icon that does not exist is harmless.
    NOTIFYICONDATAW nid = BaseNid();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    iconAdded = false;
}

void Tray::Impl::Push(Command c) {
    std::lock_guard<std::mutex> lock(mu);
    commands.push_back(c);
}

void Tray::Impl::OnSettingsChanged(const Settings& s) {
    {
        std::lock_guard<std::mutex> lock(mu);
        settings = s;
        commands.push_back(Command::kSettingsChanged);
    }
    // Saved on every change, on this thread: a few hundred bytes, and a setting that is on screen
    // but not on disk is lost by the next crash.
    if (settingsPath.empty()) {
        Log("settings: changed for this run only (--default-settings, or no file this run can write)");
        return;
    }
    std::string err;
    if (!SaveSettingsTo(settingsPath, s, &err))
        LogErr("%s - the change applies to this run only", err.c_str());
}

void Tray::Impl::OpenSettings() {
    Settings s;
    {
        std::lock_guard<std::mutex> lock(mu);
        s = settings;
    }
    std::string err;
    if (!window.Show(s, [this](const Settings& changed) { OnSettingsChanged(changed); }, uiMonitor, true, &err))
        LogErr("settings window: %s", err.c_str());
}

void Tray::Impl::ShowMenu(POINT anchor) {
    const HMENU menu = CreatePopupMenu();
    if (!menu) return;
    bool on = true;
    {
        std::lock_guard<std::mutex> lock(mu);
        on = engineOn;
    }
    AppendMenuW(menu, MF_STRING, kCmdSettings, Tr(Txt::kTraySettings));
    // Bold: the same thing a left click on the icon does.
    SetMenuDefaultItem(menu, kCmdSettings, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED), kCmdToggle, Tr(Txt::kTrayGeneration));
    AppendMenuW(menu, MF_STRING, kCmdMode, Tr(Txt::kTrayCycleMode));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, Tr(Txt::kTrayQuit));

    // Required, and the reason is not obvious: without a foreground window the menu never
    // receives its dismiss message and stays on screen after the user clicks away.
    SetForegroundWindow(hwnd);
    UINT flags = TPM_RIGHTBUTTON;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    menuOpen = true;
    TrackPopupMenuEx(menu, flags, anchor.x, anchor.y, hwnd, nullptr);
    menuOpen = false;
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK Tray::Impl::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* d = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);

    if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {
        d->iconAdded = false;
        d->addTries = 0;
        d->AddIcon();
        return 0;
    }

    switch (msg) {
        case WM_NSP_TRAY:
            switch (LOWORD(lp)) {
                // A left click opens Settings; it changes nothing by itself, so a stray click is
                // harmless. Toggling generation stays a deliberate menu choice or hotkey: this
                // program's effect is subtle, and a silent change of the picture would be
                // indistinguishable from a bug.
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    d->OpenSettings();
                    break;
                case WM_CONTEXTMENU:
                    d->ShowMenu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                    break;
                default:
                    break;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdSettings: d->OpenSettings(); break;
                case kCmdToggle: d->Push(Command::kToggle); break;
                case kCmdMode: d->Push(Command::kCycleMode); break;
                case kCmdQuit: d->Push(Command::kQuit); break;
                default: break;
            }
            return 0;

        case WM_TIMER:
            if (wp == kTimerAddIcon && !d->iconAdded) d->AddIcon();
            return 0;

        // This window is a real top-level one now (see nsp_tray.h), so anything that closes the
        // windows of a process — taskkill without /F, Restart Manager, a "close all" utility —
        // reaches it. That is a request to quit the program; letting DefWindowProc destroy the
        // window would end the UI thread and leave the engine running with no icon.
        case WM_CLOSE:
            d->Push(Command::kQuit);
            return 0;

        case WM_NSP_STATE:
            // Cleared before reading, so a change made while this runs posts again.
            d->statePosted.store(false);
            d->SyncIcon();
            return 0;

        case WM_NSP_SHUTDOWN:
            // Arriving from inside the menu's modal loop: end the menu first and come back, so
            // the window is not destroyed under TrackPopupMenuEx.
            if (d->menuOpen && d->shutdownRetries++ < 50) {
                EndMenu();
                PostMessageW(hwnd, WM_NSP_SHUTDOWN, 0, 0);
                return 0;
            }
            d->shuttingDown = true;
            KillTimer(hwnd, kTimerAddIcon);
            d->window.Close();
            d->RemoveIcon();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            // Destroyed by anything but the engine's own shutdown: the engine must not keep
            // running without the only UI that can stop it.
            if (!d->shuttingDown) d->Push(Command::kQuit);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

DWORD WINAPI Tray::Impl::ThreadMain(LPVOID param) {
    auto* d = static_cast<Impl*>(param);
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const ULONG_PTR gdiplus = StartGdiplus();
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Impl::WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // benign if already registered

    // Hidden top-level, never shown: WS_EX_TOOLWINDOW keeps it out of Alt+Tab even so.
    d->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"Nova SilkPlay", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, inst, d);
    if (!d->hwnd) {
        d->startErr = "tray: CreateWindowEx failed, GetLastError=" + std::to_string(GetLastError());
        SetEvent(d->ready);
        StopGdiplus(gdiplus);
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return 1;
    }

    // At the notification area's size for this DPI: LoadIcon would take the 32 px image and let
    // the shell shrink it, which blurs a 16 px ribbon.
    HRESULT hr = LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_SILK), LIM_SMALL, &d->iconOn);
    if (FAILED(hr)) {
        // A tray entry with no icon is a blank gap the user cannot click with any confidence, so
        // never leave it null — but say so, or a generic icon looks like a deliberate choice.
        LogErr("tray: loading the silk icon failed %s - using the stock application icon",
               HrString(hr).c_str());
        d->iconOn = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    }
    hr = LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_SILK_OFF), LIM_SMALL, &d->iconOff);
    if (FAILED(hr)) {
        LogErr("tray: loading the grey (off) icon failed %s - the icon will not change colour",
               HrString(hr).c_str());
        d->iconOff = d->iconOn;
    }
    d->AddIcon();
    SetEvent(d->ready);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (d->window.PreTranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    d->window.Close();
    d->RemoveIcon();
    // LoadIconMetric icons are the caller's to destroy; the stock fallback is not.
    const HICON stock = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    if (d->iconOff && d->iconOff != d->iconOn && d->iconOff != stock) DestroyIcon(d->iconOff);
    if (d->iconOn && d->iconOn != stock) DestroyIcon(d->iconOn);
    d->iconOn = d->iconOff = nullptr;
    StopGdiplus(gdiplus);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

Tray::Tray() : impl_(std::make_unique<Impl>()) {}

Tray::~Tray() { Destroy(); }

bool Tray::Create(const Settings& initial, const std::wstring& settingsPath, int uiMonitor,
                  std::string* err) {
    Impl& d = *impl_;
    if (d.thread) return d.hwnd != nullptr;
    d.settings = initial;
    d.settingsPath = settingsPath;
    d.uiMonitor = uiMonitor;
    d.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!d.ready) {
        if (err) *err = "tray: CreateEvent failed, GetLastError=" + std::to_string(GetLastError());
        return false;
    }
    d.thread = CreateThread(nullptr, 0, &Impl::ThreadMain, &d, 0, nullptr);
    if (!d.thread) {
        if (err) *err = "tray: CreateThread failed, GetLastError=" + std::to_string(GetLastError());
        CloseHandle(d.ready);
        d.ready = nullptr;
        return false;
    }
    WaitForSingleObject(d.ready, INFINITE);
    if (!d.hwnd) {
        WaitForSingleObject(d.thread, INFINITE);
        CloseHandle(d.thread);
        d.thread = nullptr;
        if (err) *err = d.startErr;
        return false;
    }
    return true;
}

void Tray::Destroy() {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.thread) {
        if (d.hwnd) PostMessageW(d.hwnd, WM_NSP_SHUTDOWN, 0, 0);
        if (WaitForSingleObject(d.thread, 5000) != WAIT_OBJECT_0) {
            // The UI thread is stuck (a modal loop that would not end). It still points into Impl,
            // so Impl must outlive it: leak it rather than free memory a running thread uses.
            // The process is on its way out anyway.
            LogErr("tray: the UI thread did not stop within 5 s - leaving it behind");
            impl_.release();
            return;
        }
        CloseHandle(d.thread);
        d.thread = nullptr;
    }
    d.hwnd = nullptr;
    if (d.ready) {
        CloseHandle(d.ready);
        d.ready = nullptr;
    }
}

bool Tray::Ok() const { return impl_ && impl_->hwnd != nullptr; }

Tray::Command Tray::Poll() {
    if (!impl_) return Command::kNone;
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->commands.empty()) return Command::kNone;
    const Command c = impl_->commands.front();
    impl_->commands.pop_front();
    return c;
}

Settings Tray::CurrentSettings() const {
    if (!impl_) return Settings{};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->settings;
}

void Tray::SetStatus(const std::wstring& text) {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->status == text) return;
        impl_->status = text;
    }
    impl_->PostState();
}

void Tray::SetEngineOn(bool on) {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->engineOn == on) return;
        impl_->engineOn = on;
    }
    impl_->PostState();
}

}  // namespace nsp
