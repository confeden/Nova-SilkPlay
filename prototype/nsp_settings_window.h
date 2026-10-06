// nsp_settings_window.h — the Settings window: which applications are captured, the frame rate
// counter, the language.
//
// It lives on the tray's UI thread, never on the engine's. Moving a window runs a modal loop
// inside DispatchMessage, and on the engine thread that loop would freeze the overlay on its
// last frame for as long as the user drags the window (a D22 defect).
//
// Changes apply the moment they are made and are reported through the callback; there is no
// OK / Cancel, the way Windows 11's own Settings behave.
//
// ONE ROW IS DIFFERENT. "Start with Windows" is not part of Settings, is never handed to
// onChange_ and never reaches settings.json: its truth is the HKCU Run value, and a copy in the
// JSON would be a second source of truth that disagrees the moment the user disables the entry in
// Task Manager or copies settings.json to another machine. That row reads the registry when the
// window opens and re-reads it after every write, so a failed write or a Task Manager veto snaps
// the switch back to reality instead of showing what the user wished for.
#pragma once

#include "nsp_autostart.h"
#include "nsp_common.h"
#include "nsp_settings.h"

#include <functional>
#include <string>

namespace nsp {

class SettingsWindow {
public:
    using ChangeFn = std::function<void(const Settings&)>;

    SettingsWindow() = default;
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Opens the window showing `current`, or brings the open one forward — its own state is the
    // newer one then, because it is the only thing that edits settings. `monitor`: 0 = the
    // monitor under the pointer, 1 = the primary, 2 = the next one (as tools/yt-check counts).
    // `activate` false shows it without taking the focus, for --ui-snapshot.
    bool Show(const Settings& current, ChangeFn onChange, int monitor, bool activate,
              std::string* err);
    void Close();

    // A DRAG OR AN OPEN DROPDOWN IS A MODAL LOOP OF THIS WINDOW'S OWN, and the
    // tray's shutdown message is dispatched BY that loop - so destroying the
    // window there would pull the ground out from under the loop still running
    // on it (beta audit, 2026-09-20; the popup menu already had this guard).
    // Returns true if a modal loop was found and asked to end; the caller must
    // then come back later rather than closing now.
    bool CancelModal();
    HWND Hwnd() const { return hwnd_; }

    // Tab, Space and Esc. Call for every message of the thread that owns the window.
    bool PreTranslate(MSG* msg);

private:
    // The toggles stay contiguous and the combobox stays last, so the WM_COMMAND range test
    // `id >= kCtlBase + kChrome && id <= kCtlBase + kCounter` keeps covering exactly the switches.
    enum Row { kChrome = 0, kPotPlayer, kAutostart, kCounter, kLanguage, kRowCount };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    bool OnCreate();
    // Fonts and icons for dpi_. ApplyGdi hands the new set to the window and the combobox
    // BEFORE releasing the old one, which they may still be drawing with until then.
    struct Gdi {
        HFONT title = nullptr, version = nullptr, section = nullptr;
        HFONT rowTitle = nullptr, note = nullptr, control = nullptr;
        HICON header = nullptr, smallIcon = nullptr, bigIcon = nullptr;
    };
    void ApplyGdi();
    static void FreeGdi(Gdi* g);
    void LayoutControls();
    void Relabel();
    void Paint(HDC hdc);
    void DrawToggle(const DRAWITEMSTRUCT& dis);
    void Flip(int row);
    void OnLanguage();
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
    RECT CardRect(int row) const;
    int RowAt(POINT pt) const;

    HWND hwnd_ = nullptr;
    HWND toggles_[kRowCount] = {};  // no toggle on the language row
    HWND combo_ = nullptr;
    // True between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE: the user is dragging or
    // sizing this window, and DefWindowProc is running its own message loop on it.
    bool inSizeMove_ = false;
    HWND lastFocus_ = nullptr;
    int pressedRow_ = -1;
    // The registry, cached so WM_PAINT does not hit it on every repaint. Refreshed when the
    // window is created and after every write in Flip() - nowhere else.
    AutostartState autostart_ = AutostartState::kOff;
    Settings s_;
    ChangeFn onChange_;
    UINT dpi_ = 96;
    Gdi gdi_;
};

// GDI+ draws the window's anti-aliased shapes. Start it before the first Show() on any thread
// and stop it once every window is gone.
ULONG_PTR StartGdiplus();
void StopGdiplus(ULONG_PTR token);

}  // namespace nsp
