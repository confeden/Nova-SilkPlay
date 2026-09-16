// nsp_tray.cpp — see nsp_tray.h for why this owns its own window.

#include "nsp_tray.h"

#include "nsp_common.h"

#include <shellapi.h>

namespace nsp {
namespace {

constexpr UINT WM_NSP_TRAY = WM_APP + 11;
constexpr UINT kIconId = 1;

constexpr UINT kCmdToggle = 100;
constexpr UINT kCmdMode = 101;
constexpr UINT kCmdQuit = 102;

// The shell re-creates the notification area when Explorer restarts, and every
// icon added before that is gone with no notification other than this broadcast
// message. Registering for it is the difference between a tray icon and a tray
// icon that quietly disappears the first time Explorer crashes.
UINT g_taskbarCreated = 0;

const wchar_t* kClassName = L"NovaSilkPlayTray";

}  // namespace

Tray::Tray() = default;

Tray::~Tray() { Destroy(); }

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        // Explorer came back. Re-add rather than modify: the icon the shell knew
        // about no longer exists, so NIM_MODIFY would simply fail.
        self->iconAdded_ = false;
        std::string ignored;
        self->Create(self->tip_, &ignored);
        return 0;
    }

    switch (msg) {
        case WM_NSP_TRAY:
            // Both buttons open the menu. A left-click toggle is tempting and is
            // the wrong default here: this program's visible effect is subtle, so
            // an accidental click that silently changed the picture would be
            // indistinguishable from a bug.
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP) self->ShowMenu();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdToggle: self->pending_ = Command::kToggle; break;
                case kCmdMode: self->pending_ = Command::kCycleMode; break;
                case kCmdQuit: self->pending_ = Command::kQuit; break;
                default: break;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool Tray::Create(const std::wstring& tip, std::string* err) {
    tip_ = tip;
    if (!hwnd_) {
        if (g_taskbarCreated == 0) g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Tray::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);  // benign if already registered

        // HWND_MESSAGE: never visible, never in the taskbar, never in Alt-Tab,
        // and it still receives the broadcast above.
        hwnd_ = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (!hwnd_) {
            if (err) *err = "tray: CreateWindowEx failed, GetLastError=" + std::to_string(GetLastError());
            return false;
        }
    }

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_NSP_TRAY;
    // The application icon if the binary has one, otherwise a stock icon. A tray
    // entry with no icon is a blank gap the user cannot click with any
    // confidence, so never leave hIcon null.
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        if (err) *err = "tray: Shell_NotifyIcon(NIM_ADD) failed, GetLastError=" +
                        std::to_string(GetLastError());
        return false;
    }
    iconAdded_ = true;
    return true;
}

void Tray::Destroy() {
    if (iconAdded_ && hwnd_) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        iconAdded_ = false;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Tray::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (engineOn_ ? MF_CHECKED : MF_UNCHECKED), kCmdToggle,
                L"Frame generation\tCtrl+Alt+Q");
    AppendMenuW(menu, MF_STRING, kCmdMode, L"Cycle mode\tCtrl+Alt+M");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quit\tCtrl+Alt+X");

    POINT pt{};
    GetCursorPos(&pt);
    // Required, and the reason is not obvious: without a foreground window the
    // menu never receives its dismiss message and stays on screen after the user
    // clicks away.
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

Tray::Command Tray::Poll() {
    if (!hwnd_) return Command::kNone;
    MSG msg;
    while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const Command c = pending_;
    pending_ = Command::kNone;
    return c;
}

void Tray::SetStatus(const std::wstring& text) {
    if (!iconAdded_ || !hwnd_ || text == tip_) return;
    tip_ = text;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::SetEngineOn(bool on) { engineOn_ = on; }

}  // namespace nsp
