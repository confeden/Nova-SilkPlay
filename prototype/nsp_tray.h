// nsp_tray.h — the notification-area icon, and the message-only window it needs.
//
// Why a SEPARATE window rather than the overlay's. The overlay HWND carries
// WS_EX_TRANSPARENT | WS_EX_LAYERED and must keep them (I9 — losing either is a
// release blocker), it is click-through by construction, and I11 says it is
// created once and never resized. Hanging a tray icon and a popup menu off it
// would put user-interaction concerns on the one window whose whole job is to be
// invisible and inert. A message-only window (HWND_MESSAGE) costs nothing, never
// appears anywhere, and keeps the two roles apart.
//
// The menu is deliberately modal-free at the call site: TrackPopupMenu with
// TPM_RETURNCMD blocks until the user picks something, and this program paces
// 165 frames a second on the thread that pumps its messages. Tray::Poll()
// therefore reports what was clicked and lets the caller act between ticks; the
// only stall is while a menu is actually open, which is a moment the user is
// looking at the menu rather than the video.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace nsp {

class Tray {
public:
    // What the user picked since the last Poll(). kNone most of the time.
    enum class Command { kNone, kToggle, kCycleMode, kQuit };

    Tray();
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // `tip` is the hover text, replaced later by SetStatus(). Returns false and
    // fills `err` if the icon could not be added — the caller should carry on
    // without a tray rather than refuse to run, because the engine is useful
    // without an icon and useless without the engine.
    bool Create(const std::wstring& tip, std::string* err);
    void Destroy();
    bool Ok() const { return hwnd_ != nullptr; }

    // Pumps this window's messages and returns what was clicked. Must be called
    // from the thread that called Create().
    Command Poll();

    // Hover text, ~127 chars max (Windows truncates). Cheap enough to call once
    // a second; it skips the Shell_NotifyIcon when the text has not changed,
    // because a modify per frame makes the shell repaint the notification area.
    void SetStatus(const std::wstring& text);

    // Drives the icon and the menu's check mark.
    void SetEngineOn(bool on);

private:
    HWND hwnd_ = nullptr;
    bool iconAdded_ = false;
    bool engineOn_ = true;
    std::wstring tip_;
    Command pending_ = Command::kNone;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void ShowMenu();
};

}  // namespace nsp
