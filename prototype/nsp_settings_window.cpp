// nsp_settings_window.cpp — see nsp_settings_window.h.

#include "nsp_settings_window.h"

#include "nsp_resource.h"
#include "nsp_ui_text.h"
#include "nsp_version.h"

#if __has_include("nsp_build.h")
#include "nsp_build.h"  // generated into obj\ by build.cmd: the commit this binary was built from
#endif
#ifndef NSP_BUILD_COMMIT
#define NSP_BUILD_COMMIT L"local"
#endif

#include <commctrl.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>
#include <vector>

// GDI+ spells min/max as unqualified calls, and nsp_common.h defines NOMINMAX.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

namespace nsp {
namespace {

const wchar_t* kClassName = L"NovaSilkPlaySettings";
constexpr int kCtlBase = 1000;
constexpr UINT kMsgRelabel = WM_APP + 1;
// A small fixed-size window: the close button only, no greyed-out maximize beside it.
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT;

// Layout in device-independent pixels (96 DPI). Header, then the three sections: Browsers
// (Chrome), Players (PotPlayer), General (counter, language), then the hotkey footer.
constexpr int kClientW = 520;
constexpr int kClientH = 546;
constexpr int kMargin = 24;
constexpr int kCardH = 64;
constexpr int kCardTop[] = {122, 228, 334, 402};  // indexed by Row
constexpr int kSectionTop[] = {96, 202, 308};
constexpr int kFooterTop = 486;

// Windows 11 light Settings colours, with the ribbon's pink as the accent. The pink is darker
// than the icon's so the white knob of a switched-on toggle keeps its contrast.
constexpr COLORREF kBg = RGB(243, 243, 243);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kCardBorder = RGB(229, 229, 229);
constexpr COLORREF kText = RGB(26, 26, 26);
constexpr COLORREF kTextSecondary = RGB(96, 96, 96);
constexpr COLORREF kTextDisabled = RGB(160, 160, 160);
constexpr COLORREF kAccent = RGB(214, 51, 108);
constexpr COLORREF kAccentPressed = RGB(186, 38, 92);
constexpr COLORREF kSwitchOff = RGB(128, 128, 128);
constexpr COLORREF kSwitchDisabled = RGB(198, 198, 198);

Gdiplus::Color GpColor(COLORREF c) {
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

void AddRoundRect(Gdiplus::GraphicsPath* p, Gdiplus::REAL x, Gdiplus::REAL y, Gdiplus::REAL w,
                  Gdiplus::REAL h, Gdiplus::REAL r) {
    const Gdiplus::REAL d = r * 2.0f;
    p->AddArc(x, y, d, d, 180.0f, 90.0f);
    p->AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p->AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p->AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p->CloseFigure();
}

// Paints into a memory bitmap and copies it out in one blit: drawing straight onto the window
// DC shows each card and line being drawn when the window is resized across monitors.
class BufferedDc {
public:
    BufferedDc(HDC target, int w, int h) : target_(target), w_(w), h_(h) {
        mem_ = CreateCompatibleDC(target);
        bmp_ = CreateCompatibleBitmap(target, (std::max)(1, w), (std::max)(1, h));
        old_ = SelectObject(mem_, bmp_);
    }
    ~BufferedDc() {
        BitBlt(target_, 0, 0, w_, h_, mem_, 0, 0, SRCCOPY);
        SelectObject(mem_, old_);
        DeleteObject(bmp_);
        DeleteDC(mem_);
    }
    HDC Dc() const { return mem_; }

private:
    HDC target_, mem_;
    HBITMAP bmp_;
    HGDIOBJ old_;
    int w_, h_;
};

void DrawLine(HDC dc, HFONT font, COLORREF color, const RECT& r, const wchar_t* text) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT rr = r;
    DrawTextW(dc, text, -1, &rr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

HMONITOR PickMonitor(int index) {
    if (index > 0) {
        std::vector<std::pair<HMONITOR, bool>> mons;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
                MONITORINFO mi = {sizeof(mi)};
                const bool primary = GetMonitorInfoW(m, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
                reinterpret_cast<std::vector<std::pair<HMONITOR, bool>>*>(lp)->emplace_back(m, primary);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&mons));
        std::stable_sort(mons.begin(), mons.end(),
                         [](const auto& a, const auto& b) { return a.second && !b.second; });
        if (static_cast<size_t>(index) <= mons.size()) return mons[static_cast<size_t>(index) - 1].first;
    }
    POINT pt{};
    GetCursorPos(&pt);
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

SIZE OuterSize(UINT dpi) {
    RECT r{0, 0, MulDiv(kClientW, static_cast<int>(dpi), 96), MulDiv(kClientH, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, dpi);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

HFONT MakeFont(int px, bool semibold) {
    // "Segoe UI Semibold" is its own family in GDI; asking for FW_SEMIBOLD from "Segoe UI" gets
    // synthesised bold instead.
    return CreateFontW(-px, 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, semibold ? L"Segoe UI Semibold" : L"Segoe UI");
}

}  // namespace

ULONG_PTR StartGdiplus() {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) {
        LogErr("GdiplusStartup failed - the Settings window will draw without its switches");
        return 0;
    }
    return token;
}

void StopGdiplus(ULONG_PTR token) {
    if (token) Gdiplus::GdiplusShutdown(token);
}

SettingsWindow::~SettingsWindow() { Close(); }

bool SettingsWindow::Show(const Settings& current, ChangeFn onChange, int monitor, bool activate,
                          std::string* err) {
    if (hwnd_) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        if (activate) SetForegroundWindow(hwnd_);
        return true;
    }
    s_ = current;
    onChange_ = std::move(onChange);

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    // Once per process: the class owns its two icons for as long as the process lives. Only one
    // thread ever opens this window in a run (the tray's, or main for --ui-snapshot).
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SettingsWindow::WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_SILK), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
        wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_SILK), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0));
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            if (err) *err = "RegisterClassEx failed, GetLastError=" + std::to_string(GetLastError());
            return false;
        }
        registered = true;
    }

    // Sized for the DPI of the monitor it opens on, centred in that monitor's work area.
    const HMONITOR mon = PickMonitor(monitor);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
    const SIZE outer = OuterSize(dpiX);
    const RECT& wa = mi.rcWork;
    const int x = wa.left + ((wa.right - wa.left) - outer.cx) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - outer.cy) / 2;

    if (!CreateWindowExW(kExStyle, kClassName, Tr(Txt::kWindowTitle), kStyle, x, y, outer.cx, outer.cy,
                         nullptr, nullptr, inst, this)) {
        if (err) *err = "CreateWindowEx failed, GetLastError=" + std::to_string(GetLastError());
        return false;
    }
    ShowWindow(hwnd_, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    if (activate) SetForegroundWindow(hwnd_);
    return true;
}

void SettingsWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool SettingsWindow::PreTranslate(MSG* msg) {
    if (!hwnd_ || !msg || (msg->hwnd != hwnd_ && !IsChild(hwnd_, msg->hwnd))) return false;
    return IsDialogMessageW(hwnd_, msg) != FALSE;
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->Handle(msg, wp, lp);
}

LRESULT SettingsWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            return OnCreate() ? 0 : -1;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            ApplyGdi();
            // Windows' suggested rectangle scales the old one; take its position and our own
            // size, so the client area stays exactly the layout's.
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            const SIZE outer = OuterSize(dpi_);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, outer.cx, outer.cy,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutControls();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            const HDC hdc = BeginPaint(hwnd_, &ps);
            Paint(hdc);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_PRINTCLIENT:
            Paint(reinterpret_cast<HDC>(wp));
            return 0;

        case kMsgRelabel:
            Relabel();
            return 0;

        case WM_DRAWITEM:
            DrawToggle(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
            return TRUE;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDCANCEL) {
                DestroyWindow(hwnd_);
            } else if (id >= kCtlBase + kChrome && id <= kCtlBase + kCounter &&
                       (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
                // A double click on a switch is two flips, not one: owner-drawn buttons report
                // the second click as BN_DOUBLECLICKED.
                Flip(id - kCtlBase);
            } else if (id == kCtlBase + kLanguage && code == CBN_SELCHANGE) {
                OnLanguage();
            }
            return 0;
        }

        // The whole card is the target, not just the switch drawn on it.
        case WM_LBUTTONDOWN:
            pressedRow_ = RowAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_LBUTTONUP: {
            const int row = RowAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (row >= 0 && row == pressedRow_) {
                if (row == kLanguage) {
                    SetFocus(combo_);
                    SendMessageW(combo_, CB_SHOWDROPDOWN, TRUE, 0);
                } else if (IsWindowEnabled(toggles_[row])) {
                    SetFocus(toggles_[row]);
                    Flip(row);
                }
            }
            pressedRow_ = -1;
            return 0;
        }

        case WM_ACTIVATE:
            // Keyboard focus goes back to the control that had it, as in a dialog.
            if (LOWORD(wp) == WA_INACTIVE) {
                const HWND f = GetFocus();
                if (f && IsChild(hwnd_, f)) lastFocus_ = f;
            } else if (lastFocus_ && IsWindow(lastFocus_)) {
                SetFocus(lastFocus_);
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            FreeGdi(&gdi_);
            for (auto& t : toggles_) t = nullptr;
            combo_ = nullptr;
            lastFocus_ = nullptr;
            return 0;

        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

bool SettingsWindow::OnCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    ApplyGdi();

    // The title bar in the window's own background colour, as Windows 11's apps draw it. Older
    // builds do not know the attribute and keep the default caption, which is fine.
    COLORREF caption = kBg;
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    for (int row : {static_cast<int>(kChrome), static_cast<int>(kPotPlayer), static_cast<int>(kCounter)}) {
        toggles_[row] = CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtlBase + row)),
                                        inst, nullptr);
        if (!toggles_[row]) return false;
    }
    // PotPlayer is listed so the choice is visible, and disabled because the player path does
    // not exist yet (D30: after the beta).
    EnableWindow(toggles_[kPotPlayer], FALSE);

    combo_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                             0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtlBase + kLanguage)),
                             inst, nullptr);
    if (!combo_) return false;
    SendMessageW(combo_, WM_SETFONT, reinterpret_cast<WPARAM>(gdi_.control), FALSE);

    Relabel();
    // The window was sized for the DPI of the monitor it was aimed at; match the DPI it got.
    const SIZE outer = OuterSize(dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, outer.cx, outer.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutControls();
    lastFocus_ = toggles_[kChrome];
    return true;
}

void SettingsWindow::ApplyGdi() {
    Gdi fresh;
    fresh.title = MakeFont(Px(20), true);
    fresh.version = MakeFont(Px(12), false);
    fresh.section = MakeFont(Px(14), true);
    fresh.rowTitle = MakeFont(Px(14), false);
    fresh.note = MakeFont(Px(12), false);
    fresh.control = MakeFont(Px(14), false);

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    fresh.header = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_SILK), IMAGE_ICON, Px(48), Px(48), 0));
    // The class icons were loaded at the system DPI; the title bar and taskbar want this one's.
    fresh.smallIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_SILK), IMAGE_ICON,
                                                    GetSystemMetricsForDpi(SM_CXSMICON, dpi_),
                                                    GetSystemMetricsForDpi(SM_CYSMICON, dpi_), 0));
    fresh.bigIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_SILK), IMAGE_ICON,
                                                  GetSystemMetricsForDpi(SM_CXICON, dpi_),
                                                  GetSystemMetricsForDpi(SM_CYICON, dpi_), 0));
    if (hwnd_) {
        if (fresh.smallIcon) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(fresh.smallIcon));
        if (fresh.bigIcon) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(fresh.bigIcon));
    }
    if (combo_) SendMessageW(combo_, WM_SETFONT, reinterpret_cast<WPARAM>(fresh.control), FALSE);

    Gdi old = gdi_;
    gdi_ = fresh;
    FreeGdi(&old);
}

void SettingsWindow::FreeGdi(Gdi* g) {
    for (HFONT* f : {&g->title, &g->version, &g->section, &g->rowTitle, &g->note, &g->control}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    for (HICON* i : {&g->header, &g->smallIcon, &g->bigIcon}) {
        if (*i) DestroyIcon(*i);
        *i = nullptr;
    }
}

RECT SettingsWindow::CardRect(int row) const {
    return RECT{Px(kMargin), Px(kCardTop[row]), Px(kClientW - kMargin), Px(kCardTop[row] + kCardH)};
}

int SettingsWindow::RowAt(POINT pt) const {
    for (int row = 0; row < kRowCount; ++row) {
        const RECT r = CardRect(row);
        if (PtInRect(&r, pt)) return row;
    }
    return -1;
}

void SettingsWindow::LayoutControls() {
    for (int row : {static_cast<int>(kChrome), static_cast<int>(kPotPlayer), static_cast<int>(kCounter)}) {
        const RECT c = CardRect(row);
        // 56x32 around a 40x20 switch: room for the focus ring, with the switch's right edge
        // 16 px inside the card like the combobox's.
        SetWindowPos(toggles_[row], nullptr, c.right - Px(64), c.top + Px(16), Px(56), Px(32),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const RECT c = CardRect(kLanguage);
    const int w = Px(190);
    const int x = c.right - Px(16) - w;
    // A combobox's height follows its font; the height passed in is the drop-down list's.
    SetWindowPos(combo_, nullptr, x, c.top, w, Px(160), SWP_NOZORDER | SWP_NOACTIVATE);
    RECT cr{};
    GetWindowRect(combo_, &cr);
    const int h = cr.bottom - cr.top;
    SetWindowPos(combo_, nullptr, x, c.top + (Px(kCardH) - h) / 2, w, Px(160), SWP_NOZORDER | SWP_NOACTIVATE);
}

void SettingsWindow::Relabel() {
    SetWindowTextW(hwnd_, Tr(Txt::kWindowTitle));
    // Owner-drawn buttons show no text; the names are for screen readers.
    SetWindowTextW(toggles_[kChrome], Tr(Txt::kChromeTitle));
    SetWindowTextW(toggles_[kPotPlayer], Tr(Txt::kPotPlayerTitle));
    SetWindowTextW(toggles_[kCounter], Tr(Txt::kCounterTitle));

    wchar_t autoText[96];
    _snwprintf_s(autoText, _TRUNCATE, Tr(Txt::kLanguageAuto), SystemLanguageName());
    SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(autoText));
    SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Русский"));
    SendMessageW(combo_, CB_SETCURSEL, static_cast<WPARAM>(s_.language), 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::Paint(HDC hdc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    BufferedDc buf(hdc, client.right, client.bottom);
    const HDC dc = buf.Dc();

    const HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush cardBrush(GpColor(kCard));
        Gdiplus::Pen border(GpColor(kCardBorder), static_cast<Gdiplus::REAL>((std::max)(1, Px(1))));
        for (int row = 0; row < kRowCount; ++row) {
            const RECT c = CardRect(row);
            Gdiplus::GraphicsPath path;
            // Half a pixel in, so a one-pixel border lands on whole pixels instead of two blurry ones.
            AddRoundRect(&path, c.left + 0.5f, c.top + 0.5f, static_cast<Gdiplus::REAL>(c.right - c.left - 1),
                         static_cast<Gdiplus::REAL>(c.bottom - c.top - 1), static_cast<Gdiplus::REAL>(Px(6)));
            g.FillPath(&cardBrush, &path);
            g.DrawPath(&border, &path);
        }
    }

    if (gdi_.header) DrawIconEx(dc, Px(kMargin), Px(24), gdi_.header, Px(48), Px(48), 0, nullptr, DI_NORMAL);

    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ oldFont = SelectObject(dc, gdi_.note);
    const int textLeft = Px(kMargin + 48 + 16);
    const int right = client.right - Px(kMargin);
    DrawLine(dc, gdi_.title, kText, RECT{textLeft, Px(24), right, Px(54)}, L"Nova SilkPlay");
    wchar_t version[128];
    _snwprintf_s(version, _TRUNCATE, Tr(Txt::kVersionLine), NSP_VERSION_WTEXT, NSP_BUILD_COMMIT);
    DrawLine(dc, gdi_.version, kTextSecondary, RECT{textLeft, Px(54), right, Px(74)}, version);

    const Txt sections[] = {Txt::kSectionBrowsers, Txt::kSectionPlayers, Txt::kSectionGeneral};
    for (int i = 0; i < 3; ++i)
        DrawLine(dc, gdi_.section, kText, RECT{Px(kMargin + 2), Px(kSectionTop[i]), right, Px(kSectionTop[i] + 20)},
                 Tr(sections[i]));

    struct RowText {
        Txt title, note;
    };
    const RowText rows[kRowCount] = {{Txt::kChromeTitle, Txt::kChromeNote},
                                     {Txt::kPotPlayerTitle, Txt::kPotPlayerNote},
                                     {Txt::kCounterTitle, Txt::kCounterNote},
                                     {Txt::kLanguageTitle, Txt::kLanguageNote}};
    for (int row = 0; row < kRowCount; ++row) {
        const RECT c = CardRect(row);
        const bool enabled = row != kPotPlayer;
        // Leave the control's column free: the switch is 56 px wide, the combobox 190.
        const int textRight = c.right - Px(row == kLanguage ? 16 + 190 + 12 : 64 + 8);
        DrawLine(dc, gdi_.rowTitle, enabled ? kText : kTextDisabled,
                 RECT{c.left + Px(16), c.top + Px(11), textRight, c.top + Px(32)}, Tr(rows[row].title));
        DrawLine(dc, gdi_.note, enabled ? kTextSecondary : kTextDisabled,
                 RECT{c.left + Px(16), c.top + Px(33), textRight, c.top + Px(52)}, Tr(rows[row].note));
    }

    DrawLine(dc, gdi_.note, kTextSecondary, RECT{Px(kMargin + 2), Px(kFooterTop), right, Px(kFooterTop + 18)},
             Tr(Txt::kHotkeyToggle));
    DrawLine(dc, gdi_.note, kTextSecondary, RECT{Px(kMargin + 2), Px(kFooterTop + 18), right, Px(kFooterTop + 36)},
             Tr(Txt::kHotkeyQuit));
    SelectObject(dc, oldFont);
}

void SettingsWindow::DrawToggle(const DRAWITEMSTRUCT& dis) {
    const int row = static_cast<int>(dis.CtlID) - kCtlBase;
    const bool enabled = (dis.itemState & ODS_DISABLED) == 0;
    const bool on = enabled && (row == kChrome ? s_.captureChrome : row == kCounter && s_.showFpsCounter);
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;
    const bool focus = (dis.itemState & ODS_FOCUS) && !(dis.itemState & ODS_NOFOCUSRECT);

    const int w = dis.rcItem.right - dis.rcItem.left;
    const int h = dis.rcItem.bottom - dis.rcItem.top;
    BufferedDc buf(dis.hDC, w, h);
    RECT all{0, 0, w, h};
    const HBRUSH card = CreateSolidBrush(kCard);
    FillRect(buf.Dc(), &all, card);
    DeleteObject(card);

    using Gdiplus::REAL;
    Gdiplus::Graphics g(buf.Dc());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const REAL pw = static_cast<REAL>(Px(40)), ph = static_cast<REAL>(Px(20));
    const REAL x = (w - pw) / 2.0f, y = (h - ph) / 2.0f;

    if (focus) {
        const REAL o = static_cast<REAL>(Px(3));
        Gdiplus::GraphicsPath ring;
        AddRoundRect(&ring, x - o, y - o, pw + 2 * o, ph + 2 * o, (ph + 2 * o) / 2.0f);
        Gdiplus::Pen pen(GpColor(kText), static_cast<REAL>((std::max)(1, Px(2))));
        g.DrawPath(&pen, &ring);
    }

    Gdiplus::GraphicsPath pill;
    AddRoundRect(&pill, x + 0.5f, y + 0.5f, pw - 1.0f, ph - 1.0f, (ph - 1.0f) / 2.0f);
    const REAL cy = y + ph / 2.0f;
    if (on) {
        Gdiplus::SolidBrush fill(GpColor(pressed ? kAccentPressed : kAccent));
        g.FillPath(&fill, &pill);
        const REAL r = static_cast<REAL>(Px(pressed ? 7 : 6));
        const REAL cx = x + pw - static_cast<REAL>(Px(10));
        Gdiplus::SolidBrush knob(Gdiplus::Color(255, 255, 255, 255));
        g.FillEllipse(&knob, cx - r, cy - r, 2 * r, 2 * r);
    } else {
        const COLORREF line = enabled ? kSwitchOff : kSwitchDisabled;
        Gdiplus::SolidBrush fill(GpColor(kCard));
        g.FillPath(&fill, &pill);
        Gdiplus::Pen pen(GpColor(line), static_cast<REAL>((std::max)(1, Px(1))));
        g.DrawPath(&pen, &pill);
        const REAL r = static_cast<REAL>(Px(pressed && enabled ? 6 : 5));
        const REAL cx = x + static_cast<REAL>(Px(10));
        Gdiplus::SolidBrush knob(GpColor(enabled ? RGB(92, 92, 92) : kSwitchDisabled));
        g.FillEllipse(&knob, cx - r, cy - r, 2 * r, 2 * r);
    }
}

void SettingsWindow::Flip(int row) {
    if (row == kChrome) {
        s_.captureChrome = !s_.captureChrome;
    } else if (row == kCounter) {
        s_.showFpsCounter = !s_.showFpsCounter;
    } else {
        return;
    }
    InvalidateRect(toggles_[row], nullptr, FALSE);
    NotifyWinEvent(EVENT_OBJECT_STATECHANGE, toggles_[row], OBJID_CLIENT, CHILDID_SELF);
    if (onChange_) onChange_(s_);
}

void SettingsWindow::OnLanguage() {
    const LRESULT sel = SendMessageW(combo_, CB_GETCURSEL, 0, 0);
    const UiLanguage lang = sel == 1 ? UiLanguage::kEnglish : sel == 2 ? UiLanguage::kRussian : UiLanguage::kAuto;
    if (lang == s_.language) return;
    s_.language = lang;
    SetUiLanguage(lang);
    if (onChange_) onChange_(s_);
    // Relabelling rebuilds the combobox's items, and this runs inside the combobox's own
    // CBN_SELCHANGE — with its list possibly still open under the pointer. Let it finish first.
    PostMessageW(hwnd_, kMsgRelabel, 0, 0);
}

}  // namespace nsp
