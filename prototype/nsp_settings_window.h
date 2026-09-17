// nsp_settings_window.h — the Settings window: which applications are captured, the frame rate
// counter, the language.
//
// It lives on the tray's UI thread, never on the engine's. Moving a window runs a modal loop
// inside DispatchMessage, and on the engine thread that loop would freeze the overlay on its
// last frame for as long as the user drags the window (a D22 defect).
//
// Changes apply the moment they are made and are reported through the callback; there is no
// OK / Cancel, the way Windows 11's own Settings behave.
#pragma once

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
    HWND Hwnd() const { return hwnd_; }

    // Tab, Space and Esc. Call for every message of the thread that owns the window.
    bool PreTranslate(MSG* msg);

private:
    enum Row { kChrome = 0, kPotPlayer, kCounter, kLanguage, kRowCount };

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
    HWND lastFocus_ = nullptr;
    int pressedRow_ = -1;
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
