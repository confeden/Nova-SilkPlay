// nsp_tray.h — the notification-area icon, its menu and the Settings window, on a UI thread
// of their own.
//
// Why a THREAD. TrackPopupMenu, and dragging any window, run modal loops inside the call that
// dispatches the message. On the engine's thread — which paces 165 frames a second — either one
// stops the loop, and the overlay keeps showing its last frame over a video that plays on
// underneath: a frozen picture for as long as the menu is open or the window is being moved.
// The menu is small enough to count as a widget, so the occlusion rule does not pause
// generation for it either; the engine has to keep running. Here the UI blocks only itself.
//
// Why a separate WINDOW rather than the overlay's. The overlay HWND carries WS_EX_TRANSPARENT |
// WS_EX_LAYERED and must keep them (I9), it is click-through by construction, and I11 says it is
// created once and never resized. The tray's window is a hidden top-level one: a message-only
// window (HWND_MESSAGE) cannot become the foreground window a popup menu needs to close when the
// user clicks elsewhere, and does not receive the TaskbarCreated broadcast that says the icon
// has to be added again after Explorer restarts.
//
// The engine thread talks to it through a mutex-guarded mailbox: the UI queues commands and
// keeps the latest Settings, the engine posts its status and on/off state.
#pragma once

#include "nsp_common.h"
#include "nsp_settings.h"

#include <memory>
#include <string>

namespace nsp {

class Tray {
public:
    // What the user did since the last Poll(). kNone most of the time.
    enum class Command { kNone, kToggle, kCycleMode, kQuit, kSettingsChanged };

    Tray();
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // Starts the UI thread and returns once its window exists. `initial` is what the Settings
    // window opens with; every change is saved to `settingsPath`, and a failed save is logged
    // (the change still applies to the run). `uiMonitor` is where Settings opens: 0 = under the
    // pointer (see SettingsWindow).
    // Returns false and fills `err` only if the thread or its window could not be created — the
    // caller should carry on without a tray, because the engine is useful without an icon and
    // useless without the engine. An icon the shell refuses at startup (Explorer not up yet at
    // logon) is added when Explorer announces itself.
    bool Create(const Settings& initial, const std::wstring& settingsPath, int uiMonitor,
                std::string* err);
    // Removes the icon, closes Settings and joins the thread. Engine thread.
    void Destroy();
    bool Ok() const;

    // Engine thread. One command per call, in the order the user gave them.
    Command Poll();
    // Any thread. The settings as the user last left them.
    Settings CurrentSettings() const;
    // Any thread. Hover text (~127 characters). Cheap to call every second: the shell is only
    // told when the text actually changes, because each modify repaints the notification area.
    void SetStatus(const std::wstring& text);
    // Any thread. Frame generation on or off: the icon's colour and the menu's check mark.
    void SetEngineOn(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
