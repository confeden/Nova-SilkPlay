// main.cpp — driver for the S-M2 overlay / composition measurement probe.
//
// What this process does, in order:
//   1. parses the CLI into np::Options;
//   2. finds the target top-level window (Chrome by default) by class / title /
//      pid, picking the largest client area among the matches;
//   3. creates ONE overlay HWND covering that client area (hidden), plus a
//      DirectComposition visual and a composition swapchain (overlay_win.cpp);
//   4. optionally starts a Windows.Graphics.Capture session on the *target*
//      window (wgc_capture.cpp) with its own D3D device;
//   5. runs the requested phases, pacing on the DWM compositor clock, showing
//      or hiding the overlay per phase and sampling both seams;
//   6. writes a CSV of raw samples plus a JSON report, and prints a summary.
//
// WHAT IT CANNOT ANSWER: whether *Chrome* kept its own hardware overlay plane.
// DXGI only reports the composition mode of the swapchain you own. Chrome's own
// GPU.DirectComposition / GPU.OutputPresenter histograms, sampled over CDP by
// chrome_cdp.py, are the only in-box source for that. This probe prints the
// phase markers that let the two runs be aligned after the fact.
//
// Exit codes: 0 completed run, 2 fatal setup error or aborted run (the target
// moved out from under the frozen overlay HWND), 3 overlay device loss.
// An aborted run still writes its CSV and JSON; target_moved is set in the JSON.

#include "probe_common.h"
#include "overlay_win.h"
#include "wgc_capture.h"

// probe_common.h pulls in <windows.h> with WIN32_LEAN_AND_MEAN, which leaves out
// the OLE/COM headers - objbase.h brings CoInitializeEx back.
#include <objbase.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace np {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitFatal = 2;
constexpr int kExitDeviceLost = 3;

// Verdict thresholds. These are reporting heuristics, not measurements.
constexpr double kBlackVerdictPct = 90.0;      // >= this share of black frames -> "black"
constexpr double kFrozenVerdictPct = 95.0;     // >= this share of duplicates  -> "frozen"

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// t0 for the CSV's t_ms column. main() calls Log() as its first statement so
// this baseline and probe_common's logging baseline are microseconds apart.
int64_t g_t0 = 0;

std::atomic<bool> g_interrupted{false};
std::atomic<uint32_t> g_stdinLines{0};
std::atomic<bool> g_stdinEof{false};

// Signalled by Run() once the CSV and the JSON are on disk (and by Run()'s exit
// guard on any other path). CTRL_CLOSE_EVENT blocks on it - see CtrlHandler.
HANDLE g_outputsWritten = nullptr;

// The console-close handler gets roughly 5 s before the system terminates the
// process regardless; stay under it so the wait itself is never the thing that
// kills us mid-fwrite.
constexpr DWORD kCloseFlushWaitMs = 4000;

// ------------------------------------------------------------------ small utils

const char* KindName(PhaseKind k) {
    return k == PhaseKind::Shown ? "shown" : "hidden";
}

const char* GeomName(GeomMode g) {
    switch (g) {
        case GeomMode::Client: return "client";
        case GeomMode::Rect:   return "rect";
        case GeomMode::Video:  return "video";
    }
    return "unknown";
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::wstring ToLowerW(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

bool ContainsCI(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return ToLowerW(haystack).find(ToLowerW(needle)) != std::wstring::npos;
}

double Mean(const std::vector<double>& v) {
    if (v.empty()) return kNaN;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

double StdDev(const std::vector<double>& v) {
    if (v.size() < 2) return kNaN;
    const double m = Mean(v);
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size() - 1));
}

// Linear-interpolated percentile over an ALREADY SORTED vector. p in [0,1].
double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return kNaN;
    if (sorted.size() == 1) return sorted[0];
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const double lo = std::floor(idx);
    const double hi = std::ceil(idx);
    const size_t li = static_cast<size_t>(lo);
    const size_t hi_i = static_cast<size_t>(hi);
    const double frac = idx - lo;
    return sorted[li] * (1.0 - frac) + sorted[hi_i] * frac;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// JSON has no NaN/Infinity literal; emit null so the Python side gets None.
std::string JsonNum(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

std::string CsvNum(double v) {
    if (!std::isfinite(v)) return std::string();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

// ------------------------------------------------------------------ CLI parsing

void PrintUsage() {
    std::printf(
"overlay_probe - S-M2 overlay / composition measurement probe (unelevated, in-box only)\n"
"\n"
"USAGE\n"
"  overlay_probe.exe [options]\n"
"  overlay_probe.exe --list-windows\n"
"\n"
"TARGET SELECTION\n"
"  --target-class STR      window class to match, case-insensitive exact.\n"
"                          Default \"Chrome_WidgetWin_1\". Empty string = any class.\n"
"  --target-title STR      substring of the window title, case-insensitive. Default: any.\n"
"  --target-pid N          exact process id. Default: any.\n"
"                          Among all matches the LARGEST client area wins.\n"
"\n"
"OVERLAY GEOMETRY (the HWND is created once and never moved/resized - I11)\n"
"  --geom client|rect      what the DComp visual covers. Default client.\n"
"  --rect X,Y,W,H          visual rect in TARGET-CLIENT pixels; implies --geom rect\n"
"                          unless --geom was given explicitly.\n"
"  --inset N               shrink the visual by N px per side (default 0) so the\n"
"                          probe can be told apart from the page underneath.\n"
"\n"
"OVERLAY STYLE\n"
"  --no-transparent        drop WS_EX_TRANSPARENT. S-M11 negative control: this is\n"
"                          expected to make Chrome throttle the occluded tab.\n"
"  --no-topmost            drop WS_EX_TOPMOST.\n"
"\n"
"WINDOWS.GRAPHICS.CAPTURE\n"
"  --no-wgc                do not start a capture session at all.\n"
"  --wgc-source window|monitor  capture target window or its monitor (default window).\n"
"  --wgc-min-update-ms F   GraphicsCaptureSession::MinUpdateInterval, ms. Default 1.0.\n"
"                          (G4: leaving it at 0 caps WGC near 50 fps on build 26100.)\n"
"  --no-borderless         do not attempt IsBorderRequired=false (G14: needs the\n"
"                          borderless capability + package identity; expected to fail).\n"
"  --wgc-cursor            enable IsCursorCaptureEnabled (default off).\n"
"\n"
"RUN CONTROL\n"
"  --phases SPEC           comma-separated phases. Long form NAME:KIND:SECONDS,\n"
"                          short form KIND:SECONDS (name defaults to KIND + its index).\n"
"                          KIND is hidden|shown. Default \"hidden:10,shown:20,hidden:10\".\n"
"                          Names may not contain spaces, commas or colons.\n"
"  --present-every N       present once every N compositor ticks (0 or 1 = every tick).\n"
"  --stdin-sync            advance phases on a line of stdin instead of the timer.\n"
"                          Ticking and presenting continue while waiting.\n"
"\n"
"OUTPUT\n"
"  --csv PATH              raw samples (default sm2_samples.csv).\n"
"  --json PATH             aggregated report (default sm2_report.json).\n"
"  --list-windows          dump every visible titled top-level window and exit.\n"
"  --help                  this text.\n"
"\n"
"EXIT CODES  0 completed run   2 fatal setup error / aborted run (target moved)\n"
"            3 overlay device loss\n");
    std::fflush(stdout);
}

bool ParseIntArg(const std::wstring& v, long long* out) {
    if (v.empty()) return false;
    wchar_t* end = nullptr;
    const long long n = _wcstoi64(v.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') return false;
    *out = n;
    return true;
}

bool ParseDoubleArg(const std::wstring& v, double* out) {
    if (v.empty()) return false;
    wchar_t* end = nullptr;
    const double d = wcstod(v.c_str(), &end);
    if (end == nullptr || *end != L'\0') return false;
    *out = d;
    return true;
}

bool ParseRectArg(const std::wstring& v, RECT* out, std::string* err) {
    std::vector<long long> nums;
    std::wstring cur;
    for (wchar_t c : v) {
        if (c == L',') {
            long long n = 0;
            if (!ParseIntArg(cur, &n)) { *err = "--rect: '" + Narrow(cur) + "' is not an integer"; return false; }
            nums.push_back(n);
            cur.clear();
        } else if (c != L' ') {
            cur.push_back(c);
        }
    }
    long long last = 0;
    if (!ParseIntArg(cur, &last)) { *err = "--rect: '" + Narrow(cur) + "' is not an integer"; return false; }
    nums.push_back(last);

    if (nums.size() != 4) {
        *err = "--rect expects exactly X,Y,W,H";
        return false;
    }
    if (nums[2] <= 0 || nums[3] <= 0) {
        *err = "--rect: W and H must both be > 0";
        return false;
    }
    out->left   = static_cast<LONG>(nums[0]);
    out->top    = static_cast<LONG>(nums[1]);
    out->right  = static_cast<LONG>(nums[0] + nums[2]);
    out->bottom = static_cast<LONG>(nums[1] + nums[3]);
    return true;
}

bool ParsePhases(const std::string& spec, std::vector<PhaseSpec>* out, std::string* err) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : spec) {
        if (c == ',') { tokens.push_back(cur); cur.clear(); }
        else          { cur.push_back(c); }
    }
    tokens.push_back(cur);

    out->clear();
    for (size_t i = 0; i < tokens.size(); ++i) {
        // Trim ASCII whitespace.
        std::string t = tokens[i];
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (t.empty()) { *err = "--phases: empty phase at position " + std::to_string(i); return false; }

        std::vector<std::string> f;
        std::string fc;
        for (char c : t) {
            if (c == ':') { f.push_back(fc); fc.clear(); }
            else          { fc.push_back(c); }
        }
        f.push_back(fc);

        PhaseSpec ps;
        std::string kindStr;
        std::string secsStr;
        if (f.size() == 3) {
            ps.name = f[0];
            kindStr = f[1];
            secsStr = f[2];
        } else if (f.size() == 2) {
            kindStr = f[0];
            secsStr = f[1];
            ps.name = ToLowerAscii(kindStr) + std::to_string(i);  // "hidden0", "shown1", ...
        } else {
            *err = "--phases: '" + t + "' is not NAME:KIND:SECONDS nor KIND:SECONDS";
            return false;
        }

        const std::string k = ToLowerAscii(kindStr);
        if (k == "hidden")      ps.kind = PhaseKind::Hidden;
        else if (k == "shown")  ps.kind = PhaseKind::Shown;
        else { *err = "--phases: unknown kind '" + kindStr + "' (want hidden|shown)"; return false; }

        if (ps.name.empty()) { *err = "--phases: empty phase name in '" + t + "'"; return false; }
        for (char c : ps.name) {
            if (c == ' ' || c == '\t' || c == ',' || c == ':') {
                *err = "--phases: name '" + ps.name + "' may not contain spaces, commas or colons "
                       "(the PHASE_BEGIN marker is whitespace-delimited)";
                return false;
            }
        }

        char* end = nullptr;
        ps.seconds = strtod(secsStr.c_str(), &end);
        if (end == nullptr || *end != '\0' || !(ps.seconds > 0.0)) {
            *err = "--phases: '" + secsStr + "' is not a positive number of seconds";
            return false;
        }
        out->push_back(ps);
    }
    return true;
}

struct CliFlags {
    bool listWindows = false;
    bool helpOnly = false;
    bool geomExplicit = false;
    bool rectGiven = false;
};

bool ParseArgs(int argc, wchar_t** argv, Options* opt, CliFlags* flags, std::string* err) {
    auto need = [&](int& i, const wchar_t* name, std::wstring* dst) -> bool {
        if (i + 1 >= argc) { *err = std::string(Narrow(name)) + " requires a value"; return false; }
        *dst = argv[++i];
        return true;
    };

    std::string phaseSpec;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        std::wstring v;

        if (a == L"--help" || a == L"-h" || a == L"/?") {
            flags->helpOnly = true;
            return true;
        } else if (a == L"--list-windows") {
            flags->listWindows = true;
        } else if (a == L"--target-class") {
            if (!need(i, L"--target-class", &v)) return false;
            opt->targetClass = v;
        } else if (a == L"--target-title") {
            if (!need(i, L"--target-title", &v)) return false;
            opt->targetTitle = v;
        } else if (a == L"--target-pid") {
            if (!need(i, L"--target-pid", &v)) return false;
            long long n = 0;
            if (!ParseIntArg(v, &n) || n < 0) { *err = "--target-pid: not a non-negative integer"; return false; }
            opt->targetPid = static_cast<DWORD>(n);
        } else if (a == L"--geom") {
            if (!need(i, L"--geom", &v)) return false;
            const std::string g = ToLowerAscii(Narrow(v));
            if (g == "client")     opt->geom = GeomMode::Client;
            else if (g == "rect")  opt->geom = GeomMode::Rect;
            else { *err = "--geom: want client|rect, got '" + g + "'"; return false; }
            flags->geomExplicit = true;
        } else if (a == L"--rect") {
            if (!need(i, L"--rect", &v)) return false;
            if (!ParseRectArg(v, &opt->rectClient, err)) return false;
            flags->rectGiven = true;
        } else if (a == L"--inset") {
            if (!need(i, L"--inset", &v)) return false;
            long long n = 0;
            if (!ParseIntArg(v, &n) || n < 0) { *err = "--inset: not a non-negative integer"; return false; }
            opt->insetPx = static_cast<int>(n);
        } else if (a == L"--no-transparent") {
            opt->styleTransparent = false;
        } else if (a == L"--no-topmost") {
            opt->styleTopmost = false;
        } else if (a == L"--no-wgc") {
            opt->wgc = false;
        } else if (a == L"--wgc-source") {
            if (!need(i, L"--wgc-source", &v)) return false;
            const std::string s = ToLowerAscii(Narrow(v));
            if (s == "window")      opt->wgcSource = Options::WgcSource::Window;
            else if (s == "monitor") opt->wgcSource = Options::WgcSource::Monitor;
            else { *err = "--wgc-source: want window|monitor, got '" + s + "'"; return false; }
        } else if (a == L"--wgc-min-update-ms") {
            if (!need(i, L"--wgc-min-update-ms", &v)) return false;
            double d = 0.0;
            if (!ParseDoubleArg(v, &d) || d < 0.0) { *err = "--wgc-min-update-ms: not a non-negative number"; return false; }
            opt->wgcMinUpdateIntervalMs = d;
        } else if (a == L"--no-borderless") {
            opt->wgcTryBorderless = false;
        } else if (a == L"--wgc-cursor") {
            opt->wgcCursor = true;
        } else if (a == L"--phases") {
            if (!need(i, L"--phases", &v)) return false;
            phaseSpec = Narrow(v);
        } else if (a == L"--csv") {
            if (!need(i, L"--csv", &v)) return false;
            opt->csvPath = Narrow(v);
        } else if (a == L"--json") {
            if (!need(i, L"--json", &v)) return false;
            opt->jsonPath = Narrow(v);
        } else if (a == L"--stdin-sync") {
            opt->stdinSync = true;
        } else if (a == L"--present-every") {
            if (!need(i, L"--present-every", &v)) return false;
            long long n = 0;
            if (!ParseIntArg(v, &n) || n < 0) { *err = "--present-every: not a non-negative integer"; return false; }
            opt->presentEveryNTicks = static_cast<int>(n);
        } else {
            *err = "unknown flag '" + Narrow(a) + "'";
            return false;
        }
    }

    if (flags->rectGiven && !flags->geomExplicit) {
        opt->geom = GeomMode::Rect;
    }
    if (opt->geom == GeomMode::Rect && !flags->rectGiven) {
        *err = "--geom rect requires --rect X,Y,W,H";
        return false;
    }

    if (phaseSpec.empty()) phaseSpec = "hidden:10,shown:20,hidden:10";
    if (!ParsePhases(phaseSpec, &opt->phases, err)) return false;

    return true;
}

// ------------------------------------------------------------- window discovery

struct WinInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring cls;
    std::wstring title;
    std::wstring image;       // base name of the process image, or a "<...>" marker
    RECT windowRect{};
    RECT clientScreen{};
    bool cloaked = false;
    DWORD cloakedValue = 0;
    HRESULT cloakedHr = S_OK;
    long long clientArea = 0;
};

std::wstring ProcessImageName(DWORD pid) {
    if (pid == 0) return L"<pid 0>";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"<OpenProcess failed, gle=%lu>", GetLastError());
        return buf;
    }
    wchar_t path[MAX_PATH * 2];
    DWORD n = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        result.assign(path, n);
        const size_t slash = result.find_last_of(L"\\/");
        if (slash != std::wstring::npos) result = result.substr(slash + 1);
    } else {
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"<QueryFullProcessImageNameW failed, gle=%lu>", GetLastError());
        result = buf;
    }
    CloseHandle(h);
    return result;
}

bool FillWinInfo(HWND hwnd, WinInfo* wi) {
    wi->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &wi->pid);

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    wi->cls = cls;

    const int len = GetWindowTextLengthW(hwnd);
    if (len > 0) {
        std::wstring t(static_cast<size_t>(len) + 1, L'\0');
        const int got = GetWindowTextW(hwnd, t.data(), len + 1);
        t.resize(got > 0 ? static_cast<size_t>(got) : 0);
        wi->title = t;
    }

    if (!GetWindowRect(hwnd, &wi->windowRect)) return false;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT p[2] = {{rc.left, rc.top}, {rc.right, rc.bottom}};
    SetLastError(0);
    if (MapWindowPoints(hwnd, HWND_DESKTOP, p, 2) == 0 && GetLastError() != 0) return false;
    wi->clientScreen.left = p[0].x;
    wi->clientScreen.top = p[0].y;
    wi->clientScreen.right = p[1].x;
    wi->clientScreen.bottom = p[1].y;
    wi->clientArea = static_cast<long long>(rc.right - rc.left) * static_cast<long long>(rc.bottom - rc.top);

    DWORD cloaked = 0;
    wi->cloakedHr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    wi->cloakedValue = cloaked;
    wi->cloaked = SUCCEEDED(wi->cloakedHr) && cloaked != 0;
    return true;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<WinInfo>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    WinInfo wi;
    if (FillWinInfo(hwnd, &wi)) out->push_back(wi);
    return TRUE;
}

std::vector<WinInfo> EnumerateVisibleWindows() {
    std::vector<WinInfo> v;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&v));
    return v;
}

void ListWindows() {
    const std::vector<WinInfo> all = EnumerateVisibleWindows();
    std::printf("%-18s %-8s %-24s %-30s %-26s %-26s %-8s %s\n",
                "HWND", "PID", "IMAGE", "CLASS", "WINDOW_RECT(l,t,r,b)",
                "CLIENT_RECT_SCREEN(l,t,r,b)", "CLOAKED", "TITLE");
    size_t shown = 0;
    for (const WinInfo& w : all) {
        if (w.title.empty()) continue;
        ++shown;
        char wr[64], cr[64];
        std::snprintf(wr, sizeof(wr), "%ld,%ld,%ld,%ld",
                      w.windowRect.left, w.windowRect.top, w.windowRect.right, w.windowRect.bottom);
        std::snprintf(cr, sizeof(cr), "%ld,%ld,%ld,%ld",
                      w.clientScreen.left, w.clientScreen.top, w.clientScreen.right, w.clientScreen.bottom);
        std::string cloak;
        if (FAILED(w.cloakedHr)) {
            cloak = "hr!";
        } else {
            cloak = w.cloaked ? ("yes(" + std::to_string(w.cloakedValue) + ")") : "no";
        }
        std::printf("%-18p %-8lu %-24s %-30s %-26s %-26s %-8s %s\n",
                    static_cast<void*>(w.hwnd),
                    static_cast<unsigned long>(w.pid),
                    Narrow(ProcessImageName(w.pid)).c_str(),
                    Narrow(w.cls).c_str(),
                    wr, cr, cloak.c_str(),
                    Narrow(w.title).c_str());
    }
    std::printf("\n%zu visible titled top-level window(s) of %zu enumerated.\n", shown, all.size());
    std::printf("Pick the Chrome window you want and pass e.g. --target-pid <PID> "
                "or --target-title \"<substring>\".\n");
    std::fflush(stdout);
}

bool MatchesTarget(const WinInfo& w, const Options& opt,
                   bool* classOk, bool* titleOk, bool* pidOk) {
    *classOk = opt.targetClass.empty() || _wcsicmp(w.cls.c_str(), opt.targetClass.c_str()) == 0;
    *titleOk = ContainsCI(w.title, opt.targetTitle);
    *pidOk = (opt.targetPid == 0) || (w.pid == opt.targetPid);
    return *classOk && *titleOk && *pidOk && !w.cloaked;
}

// ------------------------------------------------------------------ aggregation

struct PresentStats {
    size_t count = 0;                 // VALID presents only
    double meanMs = kNaN, p50Ms = kNaN, p99Ms = kNaN;
    std::map<int, size_t> modes;      // compositionMode -> count
    double meanSyncIntervalMs = kNaN;
    // Missed vblanks, from SyncRefreshCount deltas between consecutive valid
    // presents. See AggregatePresents for what does and does not count.
    size_t missedVblanks = 0;         // sum of (delta - 1) over deltas > 1
    size_t missedVblankEvents = 0;    // how many deltas were > 1
    size_t syncIntervals = 0;         // deltas examined; 0 means "no evidence"
};

struct CaptureStats {
    size_t count = 0;              // frames that ARRIVED (fps is about arrival)
    size_t statsCount = 0;         // of those, frames whose pixels were readable
    size_t statsUnavailable = 0;   // count - statsCount; see CaptureSample::statsValid
    double fps = kNaN;
    double pctBlack = kNaN, pctDuplicate = kNaN;
    double meanReadbackMs = kNaN, p99ReadbackMs = kNaN;
    double meanLuma = kNaN, meanStdLuma = kNaN, stdOfMeanLuma = kNaN;
    uint32_t minW = 0, minH = 0, maxW = 0, maxH = 0;
};

// Only PresentSample::valid samples reach any aggregate. A phantom sample (the
// frame never reached Present1, or Present1 failed) is all zeroes, so counting it
// would pull mean/p50/p99 present-CPU time toward 0 ms and add a fake "n/a" entry
// to the composition-mode histogram - a number the run never observed.
PresentStats AggregatePresents(const std::vector<PresentSample>& all, size_t begin, size_t end) {
    PresentStats st;
    if (end <= begin) return st;

    std::vector<double> cpu;
    cpu.reserve(end - begin);

    // Missed vblanks. SyncRefreshCount is the compositor's refresh counter as of
    // the present. Between two consecutive VALID presents a delta of 1 means every
    // refresh carried one of our frames; a delta of D > 1 means D-1 refreshes went
    // by with nothing new from us. A delta of 0 (two presents inside one refresh,
    // or statistics that did not advance) and any decrease are not evidence of a
    // miss and are not counted either way - syncIntervals records how many deltas
    // actually backed the number.
    bool haveSync = false;
    uint32_t prevSync = 0;

    for (size_t i = begin; i < end; ++i) {
        const PresentSample& p = all[i];
        if (!p.valid) continue;
        ++st.count;
        cpu.push_back(p.presentCpuMs);
        st.modes[p.compositionMode] += 1;

        if (p.syncRefreshCount != 0) {
            if (haveSync && p.syncRefreshCount > prevSync) {
                const uint32_t d = p.syncRefreshCount - prevSync;
                ++st.syncIntervals;
                if (d > 1) {
                    ++st.missedVblankEvents;
                    st.missedVblanks += static_cast<size_t>(d - 1);
                }
            }
            prevSync = p.syncRefreshCount;
            haveSync = true;
        }
    }
    if (st.count == 0) return st;

    st.meanMs = Mean(cpu);
    std::sort(cpu.begin(), cpu.end());
    st.p50Ms = Percentile(cpu, 0.50);
    st.p99Ms = Percentile(cpu, 0.99);

    // Mean interval between DISTINCT SyncQPCTime values: several presents can
    // land on the same vblank and would otherwise contribute a bogus 0 ms.
    int64_t prev = 0;
    bool have = false;
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = begin; i < end; ++i) {
        if (!all[i].valid) continue;
        const int64_t t = all[i].syncQpcTime;
        if (t == 0) continue;
        if (have) {
            if (t == prev) continue;
            if (t > prev) { sum += QpcToMs(t - prev); ++n; }
        }
        prev = t;
        have = true;
    }
    if (n > 0) st.meanSyncIntervalMs = sum / static_cast<double>(n);
    return st;
}

CaptureStats AggregateCaptures(const std::vector<CaptureSample>& all, size_t begin, size_t end,
                               double phaseSeconds) {
    CaptureStats st;
    st.count = end - begin;
    if (st.count == 0) {
        if (phaseSeconds > 0.0) st.fps = 0.0;
        return st;
    }

    std::vector<double> readback, luma, lstd;
    readback.reserve(st.count);
    luma.reserve(st.count);
    lstd.reserve(st.count);
    size_t black = 0, dup = 0;
    st.minW = st.maxW = all[begin].width;
    st.minH = st.maxH = all[begin].height;
    for (size_t i = begin; i < end; ++i) {
        const CaptureSample& c = all[i];
        // Arrival facts are real for every sample, statsValid or not.
        st.minW = (std::min)(st.minW, c.width);
        st.maxW = (std::max)(st.maxW, c.width);
        st.minH = (std::min)(st.minH, c.height);
        st.maxH = (std::max)(st.maxH, c.height);

        // Pixel facts are not: on a !statsValid frame the map was skipped rather
        // than waited for, so meanLuma is NaN and isBlack/isDuplicate are just
        // their defaults. Counting them would report a black/duplicate rate the
        // run never observed, and readbackMs would mix two different costs.
        if (!c.statsValid) continue;
        ++st.statsCount;
        readback.push_back(c.readbackMs);
        luma.push_back(c.meanLuma);
        lstd.push_back(c.stdLuma);
        if (c.isBlack) ++black;
        if (c.isDuplicate) ++dup;
    }
    st.statsUnavailable = st.count - st.statsCount;

    if (phaseSeconds > 0.0) st.fps = static_cast<double>(st.count) / phaseSeconds;
    if (st.statsCount == 0) return st;   // frames arrived, but nothing to say about pixels

    const double n = static_cast<double>(st.statsCount);
    st.pctBlack = 100.0 * static_cast<double>(black) / n;
    st.pctDuplicate = 100.0 * static_cast<double>(dup) / n;
    st.meanReadbackMs = Mean(readback);
    std::sort(readback.begin(), readback.end());
    st.p99ReadbackMs = Percentile(readback, 0.99);
    st.meanLuma = Mean(luma);
    st.meanStdLuma = Mean(lstd);
    st.stdOfMeanLuma = StdDev(luma);
    return st;
}

const char* WgcVerdict(const CaptureStats& st) {
    if (st.count == 0) return "no frames";
    // Frames arrived but none of them could be inspected: the S-M2 question is
    // unanswered, which is NOT the same as "real pixels".
    if (st.statsCount == 0) return "no pixel data";
    if (std::isfinite(st.pctBlack) && st.pctBlack >= kBlackVerdictPct) return "black";
    if (std::isfinite(st.pctDuplicate) && st.pctDuplicate >= kFrozenVerdictPct) return "frozen";
    return "real pixels";
}

// ------------------------------------------------------------------ run context

struct PhaseRun {
    size_t presentBegin = 0, presentEnd = 0;
    size_t captureBegin = 0, captureEnd = 0;
    double actualSeconds = 0.0;
    uint64_t ticks = 0;
    // false when the phase never started (WM_QUIT, interrupt or a moved target
    // ended the run before it). Such a phase must not be read as "0 presents
    // observed" - nothing was observed at all.
    bool ran = false;
    size_t presentsSkipped = 0;   // RenderAndPresent never reached Present1
    size_t presentsFailed = 0;    // Present1 ran and returned a failure HRESULT
    PresentStats present;
    CaptureStats capture;
};

// Runs on a handler thread of its own, NOT on the run thread.
BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        // The process keeps running: the phase loop sees the flag, unwinds and
        // writes the files itself. Returning TRUE only suppresses the default
        // "terminate now" behaviour.
        g_interrupted.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    if (type == CTRL_CLOSE_EVENT) {
        // Different contract: for CTRL_CLOSE_EVENT the system terminates the
        // process AS SOON AS THIS HANDLER RETURNS, whatever it returns. Setting a
        // flag and returning would therefore throw the CSV and the JSON away.
        // So block here until the run thread has finished writing them.
        g_interrupted.store(true, std::memory_order_relaxed);
        if (g_outputsWritten != nullptr) {
            WaitForSingleObject(g_outputsWritten, kCloseFlushWaitMs);
        }
        return TRUE;
    }
    return FALSE;
}

// Every exit path out of Run() releases a console-close handler that is parked
// in the wait above, so closing the console during setup does not cost 4 s.
struct OutputsWrittenGuard {
    ~OutputsWrittenGuard() {
        if (g_outputsWritten != nullptr) SetEvent(g_outputsWritten);
    }
};

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// Re-reads a window's client rect in screen-space physical pixels.
// Deliberately NOT called from the tick path: two extra USER32 round-trips per
// compositor tick would perturb the very present cadence this probe measures.
bool SampleClientRectScreen(HWND hwnd, RECT* out) {
    if (!IsWindow(hwnd)) return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT p[2] = {{rc.left, rc.top}, {rc.right, rc.bottom}};
    SetLastError(0);
    if (MapWindowPoints(hwnd, HWND_DESKTOP, p, 2) == 0 && GetLastError() != 0) return false;
    out->left   = p[0].x;
    out->top    = p[0].y;
    out->right  = p[1].x;
    out->bottom = p[1].y;
    return true;
}

void StdinReaderThread() {
    std::string line;
    while (std::getline(std::cin, line)) {
        g_stdinLines.fetch_add(1, std::memory_order_relaxed);
    }
    g_stdinEof.store(true, std::memory_order_relaxed);
}

bool PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

using PfnWaitForCompositorClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

// ------------------------------------------------------------------ CSV / JSON

bool WriteCsv(const Options& opt,
              const std::vector<PresentSample>& presents,
              const std::vector<CaptureSample>& captures,
              std::string* err) {
    FILE* f = nullptr;
    const errno_t e = fopen_s(&f, opt.csvPath.c_str(), "wb");
    if (e != 0 || f == nullptr) {
        *err = "fopen_s(\"" + opt.csvPath + "\") failed, errno=" + std::to_string(e);
        return false;
    }

    std::fprintf(f,
        "rec,phase_index,phase_name,qpc,t_ms,"
        "present_count,present_refresh_count,sync_refresh_count,sync_qpc_time,"
        "composition_mode,composition_mode_name,approved_present_duration,present_cpu_ms,"
        "system_relative_time_100ns,width,height,mean_luma,std_luma,grid_hash,"
        "is_black,is_duplicate,readback_ms\n");

    auto phaseName = [&](uint32_t idx) -> const char* {
        return idx < opt.phases.size() ? opt.phases[idx].name.c_str() : "?";
    };

    // rec is "present" for a real present and "present_invalid" for a sample the
    // aggregates deliberately drop (never reached Present1, or Present1 failed).
    // The raw file stays complete; only the statistics are filtered.
    for (const PresentSample& p : presents) {
        std::fprintf(f,
            "%s,%u,%s,%lld,%s,"
            "%u,%u,%u,%lld,"
            "%d,%s,%u,%s,"
            ",,,,,,,,\n",
            p.valid ? "present" : "present_invalid",
            p.phaseIndex, phaseName(p.phaseIndex),
            static_cast<long long>(p.qpc), CsvNum(QpcToMs(p.qpc - g_t0)).c_str(),
            p.presentCount, p.presentRefreshCount, p.syncRefreshCount,
            static_cast<long long>(p.syncQpcTime),
            p.compositionMode, CompositionModeName(p.compositionMode),
            p.approvedPresentDuration, CsvNum(p.presentCpuMs).c_str());
    }

    // rec is "capture_nostats" when the frame arrived but its pixels could not be
    // mapped: the arrival columns are real, every pixel column is blank/meaningless.
    for (const CaptureSample& c : captures) {
        std::fprintf(f,
            "%s,%u,%s,%lld,%s,"
            ",,,,"
            ",,,,"
            "%lld,%u,%u,%s,%s,%llu,%d,%d,%s\n",
            c.statsValid ? "capture" : "capture_nostats",
            c.phaseIndex, phaseName(c.phaseIndex),
            static_cast<long long>(c.qpc), CsvNum(QpcToMs(c.qpc - g_t0)).c_str(),
            static_cast<long long>(c.systemRelativeTime100ns),
            c.width, c.height,
            CsvNum(c.meanLuma).c_str(), CsvNum(c.stdLuma).c_str(),
            static_cast<unsigned long long>(c.gridHash),
            c.isBlack ? 1 : 0, c.isDuplicate ? 1 : 0,
            CsvNum(c.readbackMs).c_str());
    }

    const bool bad = std::ferror(f) != 0;
    if (std::fclose(f) != 0 || bad) {
        *err = "writing \"" + opt.csvPath + "\" failed";
        return false;
    }
    return true;
}

struct RunMeta {
    HWND target = nullptr;
    DWORD targetPid = 0;
    std::wstring targetClass, targetTitle, targetImage;
    RECT hwndRectScreen{};
    RECT visualRectRequested{};   // what --geom/--rect asked for, before clipping
    RECT visualRectScreen{};      // what DirectComposition actually committed
    bool visualRectClipped = false;
    RECT targetRectLastSeen{};    // client rect at the last phase boundary
    bool targetMoved = false;
    std::string targetMovedDetail;
    UINT targetDpi = 0;
    UINT targetDpiLastSeen = 0;
    std::string clockPath;
    std::string creationReport;
    std::string wgcReport;
    bool wgcStarted = false;
    std::string wgcError;
    bool deviceLost = false;
    bool interrupted = false;
    bool quitPosted = false;
    size_t phasesRun = 0;
    size_t presentsMissingQpc = 0;   // VALID samples the overlay left unstamped
    size_t capturesMissingQpc = 0;
    size_t presentsSkipped = 0;
    size_t presentsFailed = 0;
    size_t wgcFramesWithoutStats = 0;
    std::string coInitHr;
    std::string dpiAwareness;
    double totalSeconds = 0.0;
};

std::string RectJson(const RECT& r) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "{\"left\":%ld,\"top\":%ld,\"right\":%ld,\"bottom\":%ld,\"w\":%ld,\"h\":%ld}",
                  r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    return buf;
}

bool WriteJson(const Options& opt, const RunMeta& meta,
               const std::vector<PhaseRun>& runs,
               const CaptureStats& overall,
               std::string* err) {
    FILE* f = nullptr;
    const errno_t e = fopen_s(&f, opt.jsonPath.c_str(), "wb");
    if (e != 0 || f == nullptr) {
        *err = "fopen_s(\"" + opt.jsonPath + "\") failed, errno=" + std::to_string(e);
        return false;
    }

    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"tool\": \"overlay_probe\",\n");
    std::fprintf(f, "  \"spike\": \"S-M2\",\n");

    std::fprintf(f, "  \"options\": {\n");
    std::fprintf(f, "    \"target_class\": \"%s\",\n", JsonEscape(Narrow(opt.targetClass)).c_str());
    std::fprintf(f, "    \"target_title\": \"%s\",\n", JsonEscape(Narrow(opt.targetTitle)).c_str());
    std::fprintf(f, "    \"target_pid\": %lu,\n", static_cast<unsigned long>(opt.targetPid));
    std::fprintf(f, "    \"geom\": \"%s\",\n", GeomName(opt.geom));
    std::fprintf(f, "    \"rect_client\": %s,\n", RectJson(opt.rectClient).c_str());
    std::fprintf(f, "    \"inset_px\": %d,\n", opt.insetPx);
    std::fprintf(f, "    \"style_transparent\": %s,\n", opt.styleTransparent ? "true" : "false");
    std::fprintf(f, "    \"style_topmost\": %s,\n", opt.styleTopmost ? "true" : "false");
    std::fprintf(f, "    \"wgc\": %s,\n", opt.wgc ? "true" : "false");
    std::fprintf(f, "    \"wgc_source\": \"%s\",\n", opt.wgcSource == Options::WgcSource::Monitor ? "monitor" : "window");
    std::fprintf(f, "    \"wgc_min_update_interval_ms\": %s,\n", JsonNum(opt.wgcMinUpdateIntervalMs).c_str());
    std::fprintf(f, "    \"wgc_try_borderless\": %s,\n", opt.wgcTryBorderless ? "true" : "false");
    std::fprintf(f, "    \"wgc_cursor\": %s,\n", opt.wgcCursor ? "true" : "false");
    std::fprintf(f, "    \"csv_path\": \"%s\",\n", JsonEscape(opt.csvPath).c_str());
    std::fprintf(f, "    \"json_path\": \"%s\",\n", JsonEscape(opt.jsonPath).c_str());
    std::fprintf(f, "    \"stdin_sync\": %s,\n", opt.stdinSync ? "true" : "false");
    std::fprintf(f, "    \"present_every_n_ticks\": %d,\n", opt.presentEveryNTicks);
    std::fprintf(f, "    \"phases\": [");
    for (size_t i = 0; i < opt.phases.size(); ++i) {
        std::fprintf(f, "%s{\"name\":\"%s\",\"kind\":\"%s\",\"seconds\":%s}",
                     i ? ", " : "",
                     JsonEscape(opt.phases[i].name).c_str(),
                     KindName(opt.phases[i].kind),
                     JsonNum(opt.phases[i].seconds).c_str());
    }
    std::fprintf(f, "]\n  },\n");

    std::fprintf(f, "  \"environment\": {\n");
    std::fprintf(f, "    \"dpi_awareness\": \"%s\",\n", JsonEscape(meta.dpiAwareness).c_str());
    std::fprintf(f, "    \"com_init\": \"%s\",\n", JsonEscape(meta.coInitHr).c_str());
    std::fprintf(f, "    \"compositor_clock\": \"%s\"\n", JsonEscape(meta.clockPath).c_str());
    std::fprintf(f, "  },\n");

    std::fprintf(f, "  \"target\": {\n");
    std::fprintf(f, "    \"hwnd\": \"0x%llX\",\n",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(meta.target)));
    std::fprintf(f, "    \"pid\": %lu,\n", static_cast<unsigned long>(meta.targetPid));
    std::fprintf(f, "    \"image\": \"%s\",\n", JsonEscape(Narrow(meta.targetImage)).c_str());
    std::fprintf(f, "    \"class\": \"%s\",\n", JsonEscape(Narrow(meta.targetClass)).c_str());
    std::fprintf(f, "    \"title\": \"%s\",\n", JsonEscape(Narrow(meta.targetTitle)).c_str());
    std::fprintf(f, "    \"dpi\": %u,\n", meta.targetDpi);
    std::fprintf(f, "    \"dpi_last_seen\": %u,\n", meta.targetDpiLastSeen);
    std::fprintf(f, "    \"hwnd_rect_screen\": %s,\n", RectJson(meta.hwndRectScreen).c_str());
    std::fprintf(f, "    \"client_rect_last_seen\": %s,\n", RectJson(meta.targetRectLastSeen).c_str());
    // requested = what --geom/--rect asked for; screen = what DComp committed
    // (clipped to the HWND, inset applied by the overlay). They differ whenever
    // visual_rect_clipped or inset_px is set - never quote "requested" as coverage.
    std::fprintf(f, "    \"visual_rect_requested\": %s,\n", RectJson(meta.visualRectRequested).c_str());
    std::fprintf(f, "    \"visual_rect_clipped\": %s,\n", meta.visualRectClipped ? "true" : "false");
    std::fprintf(f, "    \"visual_rect_screen\": %s\n", RectJson(meta.visualRectScreen).c_str());
    std::fprintf(f, "  },\n");

    std::fprintf(f, "  \"creation_report\": \"%s\",\n", JsonEscape(meta.creationReport).c_str());
    std::fprintf(f, "  \"wgc_started\": %s,\n", meta.wgcStarted ? "true" : "false");
    std::fprintf(f, "  \"wgc_error\": \"%s\",\n", JsonEscape(meta.wgcError).c_str());
    std::fprintf(f, "  \"wgc_startup_report\": \"%s\",\n", JsonEscape(meta.wgcReport).c_str());
    // Two different losses. dropped_no_sample: the frame was dequeued and produced
    // no CaptureSample at all. without_pixel_stats (per phase, and summed here):
    // the sample exists and its arrival fields are real, but its pixels were never
    // mapped, so it is excluded from every pixel statistic.
    std::fprintf(f, "  \"wgc_frames_dropped_no_sample\": %zu,\n", meta.wgcFramesWithoutStats);
    std::fprintf(f, "  \"wgc_frames_without_pixel_stats\": %zu,\n", overall.statsUnavailable);
    std::fprintf(f, "  \"device_lost\": %s,\n", meta.deviceLost ? "true" : "false");
    std::fprintf(f, "  \"interrupted\": %s,\n", meta.interrupted ? "true" : "false");
    std::fprintf(f, "  \"quit_posted\": %s,\n", meta.quitPosted ? "true" : "false");
    std::fprintf(f, "  \"target_moved\": %s,\n", meta.targetMoved ? "true" : "false");
    std::fprintf(f, "  \"target_moved_detail\": \"%s\",\n", JsonEscape(meta.targetMovedDetail).c_str());
    std::fprintf(f, "  \"phases_requested\": %zu,\n", opt.phases.size());
    std::fprintf(f, "  \"phases_run\": %zu,\n", meta.phasesRun);
    std::fprintf(f, "  \"presents_skipped_total\": %zu,\n", meta.presentsSkipped);
    std::fprintf(f, "  \"presents_failed_total\": %zu,\n", meta.presentsFailed);
    std::fprintf(f, "  \"present_samples_missing_qpc\": %zu,\n", meta.presentsMissingQpc);
    std::fprintf(f, "  \"capture_samples_missing_qpc\": %zu,\n", meta.capturesMissingQpc);
    std::fprintf(f, "  \"total_seconds\": %s,\n", JsonNum(meta.totalSeconds).c_str());

    std::fprintf(f, "  \"phases\": [\n");
    for (size_t i = 0; i < runs.size(); ++i) {
        const PhaseRun& r = runs[i];
        const PhaseSpec& ps = opt.phases[i];
        std::fprintf(f, "    {\n");
        std::fprintf(f, "      \"index\": %zu,\n", i);
        std::fprintf(f, "      \"name\": \"%s\",\n", JsonEscape(ps.name).c_str());
        std::fprintf(f, "      \"kind\": \"%s\",\n", KindName(ps.kind));
        std::fprintf(f, "      \"seconds_requested\": %s,\n", JsonNum(ps.seconds).c_str());
        std::fprintf(f, "      \"seconds_actual\": %s,\n", JsonNum(r.actualSeconds).c_str());
        std::fprintf(f, "      \"ticks\": %llu,\n", static_cast<unsigned long long>(r.ticks));
        std::fprintf(f, "      \"ran\": %s,\n", r.ran ? "true" : "false");

        std::fprintf(f, "      \"presents\": {\n");
        std::fprintf(f, "        \"count\": %zu,\n", r.present.count);
        std::fprintf(f, "        \"skipped\": %zu,\n", r.presentsSkipped);
        std::fprintf(f, "        \"failed\": %zu,\n", r.presentsFailed);
        std::fprintf(f, "        \"ticks\": %llu,\n", static_cast<unsigned long long>(r.ticks));
        std::fprintf(f, "        \"missed_vblanks\": %zu,\n", r.present.missedVblanks);
        std::fprintf(f, "        \"missed_vblank_events\": %zu,\n", r.present.missedVblankEvents);
        std::fprintf(f, "        \"sync_refresh_intervals\": %zu,\n", r.present.syncIntervals);
        std::fprintf(f, "        \"mean_present_cpu_ms\": %s,\n", JsonNum(r.present.meanMs).c_str());
        std::fprintf(f, "        \"p50_present_cpu_ms\": %s,\n", JsonNum(r.present.p50Ms).c_str());
        std::fprintf(f, "        \"p99_present_cpu_ms\": %s,\n", JsonNum(r.present.p99Ms).c_str());
        std::fprintf(f, "        \"mean_sync_qpc_interval_ms\": %s,\n",
                     JsonNum(r.present.meanSyncIntervalMs).c_str());
        std::fprintf(f, "        \"composition_mode_histogram\": {");
        bool first = true;
        for (const auto& kv : r.present.modes) {
            std::fprintf(f, "%s\"%s\": %zu", first ? "" : ", ",
                         CompositionModeName(kv.first), kv.second);
            first = false;
        }
        std::fprintf(f, "}\n");
        std::fprintf(f, "      },\n");

        std::fprintf(f, "      \"captures\": {\n");
        std::fprintf(f, "        \"count\": %zu,\n", r.capture.count);
        // count = frames that arrived; the pct_/luma_/readback_ figures below are
        // over with_pixel_stats only.
        std::fprintf(f, "        \"with_pixel_stats\": %zu,\n", r.capture.statsCount);
        std::fprintf(f, "        \"without_pixel_stats\": %zu,\n", r.capture.statsUnavailable);
        std::fprintf(f, "        \"fps\": %s,\n", JsonNum(r.capture.fps).c_str());
        std::fprintf(f, "        \"pct_black\": %s,\n", JsonNum(r.capture.pctBlack).c_str());
        std::fprintf(f, "        \"pct_duplicate\": %s,\n", JsonNum(r.capture.pctDuplicate).c_str());
        std::fprintf(f, "        \"mean_readback_ms\": %s,\n", JsonNum(r.capture.meanReadbackMs).c_str());
        std::fprintf(f, "        \"p99_readback_ms\": %s,\n", JsonNum(r.capture.p99ReadbackMs).c_str());
        std::fprintf(f, "        \"mean_luma\": %s,\n", JsonNum(r.capture.meanLuma).c_str());
        std::fprintf(f, "        \"mean_std_luma\": %s,\n", JsonNum(r.capture.meanStdLuma).c_str());
        std::fprintf(f, "        \"std_of_mean_luma\": %s,\n", JsonNum(r.capture.stdOfMeanLuma).c_str());
        std::fprintf(f, "        \"content_size_min\": {\"w\": %u, \"h\": %u},\n",
                     r.capture.minW, r.capture.minH);
        std::fprintf(f, "        \"content_size_max\": {\"w\": %u, \"h\": %u},\n",
                     r.capture.maxW, r.capture.maxH);
        std::fprintf(f, "        \"verdict\": \"%s\"\n", WgcVerdict(r.capture));
        std::fprintf(f, "      }\n");
        std::fprintf(f, "    }%s\n", (i + 1 < runs.size()) ? "," : "");
    }
    std::fprintf(f, "  ],\n");

    std::fprintf(f, "  \"wgc_verdict\": \"%s\",\n", WgcVerdict(overall));
    std::fprintf(f, "  \"caveat\": \"%s\"\n",
        JsonEscape("composition_mode_histogram describes THIS PROBE'S OWN swapchain "
                   "(IDXGISwapChainMedia::GetFrameStatisticsMedia). It says nothing about whether "
                   "Chrome kept its own MPO plane - that answer comes only from chrome_cdp.py's "
                   "GPU.DirectComposition / GPU.OutputPresenter histograms for the same phases.").c_str());
    std::fprintf(f, "}\n");

    const bool bad = std::ferror(f) != 0;
    if (std::fclose(f) != 0 || bad) {
        *err = "writing \"" + opt.jsonPath + "\" failed";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------- summary

std::string ModeColumn(const PresentStats& st) {
    auto get = [&](int m) -> size_t {
        auto it = st.modes.find(m);
        return it == st.modes.end() ? 0 : it->second;
    };
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%zu/%zu/%zu/%zu/%zu",
                  get(1), get(0), get(2), get(3), get(-1));
    return buf;
}

std::string Fmt(double v, const char* spec = "%.3f") {
    if (!std::isfinite(v)) return "-";
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

void PrintSummary(const Options& opt, const RunMeta& meta,
                  const std::vector<PhaseRun>& runs, const CaptureStats& overall) {
    std::printf("\n");
    std::printf("================================ S-M2 SUMMARY ================================\n");
    std::printf("target hwnd=0x%llX pid=%lu image=%s class=%s dpi=%u\n",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(meta.target)),
                static_cast<unsigned long>(meta.targetPid),
                Narrow(meta.targetImage).c_str(), Narrow(meta.targetClass).c_str(),
                meta.targetDpi);
    std::printf("overlay hwnd rect  = %ld,%ld %ldx%ld\n",
                meta.hwndRectScreen.left, meta.hwndRectScreen.top,
                meta.hwndRectScreen.right - meta.hwndRectScreen.left,
                meta.hwndRectScreen.bottom - meta.hwndRectScreen.top);
    // The COMMITTED rect, read back from the overlay - not the one we asked for.
    std::printf("overlay visual rect= %ld,%ld %ldx%ld   (committed to DComp; geom=%s inset=%d)\n",
                meta.visualRectScreen.left, meta.visualRectScreen.top,
                meta.visualRectScreen.right - meta.visualRectScreen.left,
                meta.visualRectScreen.bottom - meta.visualRectScreen.top,
                GeomName(opt.geom), opt.insetPx);
    if (meta.visualRectClipped) {
        std::printf("  !! REQUESTED   = %ld,%ld %ldx%ld  -- clipped to the overlay HWND; "
                    "coverage below is the CLIPPED rect\n",
                    meta.visualRectRequested.left, meta.visualRectRequested.top,
                    meta.visualRectRequested.right - meta.visualRectRequested.left,
                    meta.visualRectRequested.bottom - meta.visualRectRequested.top);
    }
    if (meta.targetMoved) {
        std::printf("  !! TARGET MOVED: %s\n", meta.targetMovedDetail.c_str());
        std::printf("  !! the run was ABORTED; phases marked ran=no below produced no data at all\n");
    }
    if (meta.phasesRun != opt.phases.size()) {
        std::printf("  !! only %zu of %zu requested phase(s) actually ran%s%s\n",
                    meta.phasesRun, opt.phases.size(),
                    meta.quitPosted ? " (WM_QUIT)" : "",
                    meta.interrupted ? " (console signal)" : "");
    }
    std::printf("styles: WS_EX_TRANSPARENT=%s WS_EX_TOPMOST=%s   clock=%s\n",
                opt.styleTransparent ? "on" : "OFF(S-M11 negative control)",
                opt.styleTopmost ? "on" : "OFF",
                meta.clockPath.c_str());

    std::printf("\n-- our overlay swapchain (presents) ------------------------------------------\n");
    std::printf("%-16s %-4s %-7s %7s %7s %8s %8s %5s %5s %9s %9s %9s %8s %10s  %s\n",
                "PHASE", "RAN", "KIND", "REQ_S", "ACT_S", "TICKS", "PRESENTS", "SKIP", "FAIL",
                "CPU_MEAN", "CPU_P50", "CPU_P99", "SYNC_MS", "MISSED_VBL",
                "OVL/CMP/NONE/FAIL/NA");
    for (size_t i = 0; i < runs.size(); ++i) {
        const PhaseRun& r = runs[i];
        // "missed / intervals that backed the number": 0/0 means no evidence, not
        // a clean run.
        char missed[48];
        std::snprintf(missed, sizeof(missed), "%zu/%zu",
                      r.present.missedVblanks, r.present.syncIntervals);
        std::printf("%-16s %-4s %-7s %7.2f %7.2f %8llu %8zu %5zu %5zu %9s %9s %9s %8s %10s  %s\n",
                    opt.phases[i].name.c_str(), r.ran ? "yes" : "NO",
                    KindName(opt.phases[i].kind),
                    opt.phases[i].seconds, r.actualSeconds,
                    static_cast<unsigned long long>(r.ticks), r.present.count,
                    r.presentsSkipped, r.presentsFailed,
                    Fmt(r.present.meanMs).c_str(), Fmt(r.present.p50Ms).c_str(),
                    Fmt(r.present.p99Ms).c_str(),
                    Fmt(r.present.meanSyncIntervalMs).c_str(),
                    missed,
                    ModeColumn(r.present).c_str());
    }
    std::printf("PRESENTS counts only real presents; SKIP never reached Present1, FAIL returned a\n"
                "failure HRESULT - both are excluded from CPU_* and the mode histogram and are\n"
                "written to the CSV as rec=present_invalid. MISSED_VBL is missed/intervals from\n"
                "consecutive SyncRefreshCount deltas; compare it against TICKS.\n");

    std::printf("\n-- Windows.Graphics.Capture of the TARGET window ----------------------------\n");
    std::printf("%-16s %-7s %8s %8s %8s %8s %8s %9s %9s %-12s %-12s %s\n",
                "PHASE", "KIND", "FRAMES", "NOSTATS", "FPS", "%BLACK", "%DUP",
                "LUMA_MEAN", "LUMA_STD", "SIZE_MIN", "SIZE_MAX", "VERDICT");
    for (size_t i = 0; i < runs.size(); ++i) {
        const PhaseRun& r = runs[i];
        char smin[32], smax[32];
        std::snprintf(smin, sizeof(smin), "%ux%u", r.capture.minW, r.capture.minH);
        std::snprintf(smax, sizeof(smax), "%ux%u", r.capture.maxW, r.capture.maxH);
        std::printf("%-16s %-7s %8zu %8zu %8s %8s %8s %9s %9s %-12s %-12s %s\n",
                    opt.phases[i].name.c_str(), KindName(opt.phases[i].kind),
                    r.capture.count, r.capture.statsUnavailable,
                    Fmt(r.capture.fps, "%.2f").c_str(),
                    Fmt(r.capture.pctBlack, "%.1f").c_str(),
                    Fmt(r.capture.pctDuplicate, "%.1f").c_str(),
                    Fmt(r.capture.meanLuma, "%.2f").c_str(),
                    Fmt(r.capture.meanStdLuma, "%.2f").c_str(),
                    smin, smax, WgcVerdict(r.capture));
    }
    std::printf("\n%-16s %-7s %8zu %8zu %8s %8s %8s %9s %9s\n",
                "ALL", "-", overall.count, overall.statsUnavailable,
                Fmt(overall.fps, "%.2f").c_str(),
                Fmt(overall.pctBlack, "%.1f").c_str(),
                Fmt(overall.pctDuplicate, "%.1f").c_str(),
                Fmt(overall.meanLuma, "%.2f").c_str(),
                Fmt(overall.meanStdLuma, "%.2f").c_str());
    std::printf("FRAMES is what arrived (so FPS is arrival rate); NOSTATS is how many of those had\n"
                "no readable pixels - %%BLACK / %%DUP / LUMA_* are over FRAMES-NOSTATS only, and are\n"
                "written to the CSV as rec=capture_nostats.\n");

    if (meta.wgcFramesWithoutStats > 0) {
        std::printf("\n%zu WGC frame(s) were dequeued but produced NO sample at all (surface access,\n"
                    "staging allocation or an exception). They are not in the FRAMES counts above.\n",
                    meta.wgcFramesWithoutStats);
    }

    std::printf("\nWGC VERDICT: %s\n", WgcVerdict(overall));
    std::printf("  (thresholds: >=%.0f%% black frames -> \"black\"; >=%.0f%% duplicate frames -> \"frozen\";\n"
                "   zero frames -> \"no frames\"; frames but no readable pixels -> \"no pixel data\";\n"
                "   otherwise \"real pixels\". These are reporting heuristics chosen here, not\n"
                "   measured constants.)\n",
                kBlackVerdictPct, kFrozenVerdictPct);

    std::printf("\nNOTE: the OVL/CMP/NONE/FAIL/NA column is the composition mode of *this probe's own*\n"
                "      swapchain (IDXGISwapChainMedia::GetFrameStatisticsMedia). DXGI cannot report\n"
                "      another process's swapchain, so this says NOTHING about whether Chrome kept its\n"
                "      hardware overlay plane while our overlay was shown. That question is answered\n"
                "      only by chrome_cdp.py's GPU.DirectComposition / GPU.OutputPresenter histogram\n"
                "      deltas for the SAME phase names. Align the two runs on the PHASE_BEGIN /\n"
                "      PHASE_END markers both tools print.\n");
    std::printf("=============================================================================\n");
    std::fflush(stdout);
}

// --------------------------------------------------------------------- the run

int Run(int argc, wchar_t** argv) {
    Log("overlay_probe starting (S-M2 overlay/composition probe)");
    g_t0 = QpcNow();

    Options opt;
    CliFlags flags;
    std::string err;
    if (!ParseArgs(argc, argv, &opt, &flags, &err)) {
        LogErr("FATAL: %s", err.c_str());
        PrintUsage();
        return kExitFatal;
    }
    if (flags.helpOnly) {
        PrintUsage();
        return kExitOk;
    }

    RunMeta meta;

    // Physical pixels everywhere. Without this a scaled monitor makes every
    // GetClientRect/MapWindowPoints result silently virtualised.
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        meta.dpiAwareness = "PER_MONITOR_AWARE_V2";
    } else {
        const DWORD gle = GetLastError();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "SetProcessDpiAwarenessContext failed, gle=%lu", gle);
        meta.dpiAwareness = buf;
        LogErr("WARN: %s - screen rects may be DPI-virtualised", buf);
    }

    if (flags.listWindows) {
        ListWindows();
        return kExitOk;
    }

    // Manual-reset, created BEFORE the handler is installed so CtrlHandler never
    // sees a null handle. The guard releases a parked console-close handler on
    // every exit path out of Run(), including the fatal ones below.
    g_outputsWritten = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_outputsWritten == nullptr) {
        LogErr("WARN: CreateEvent failed (gle=%lu); closing the console window will lose the "
               "CSV and JSON of this run.", GetLastError());
    }
    const OutputsWrittenGuard outputsWrittenGuard;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // MTA: wgc_capture uses the FREE-THREADED WGC frame pool and polls it from
    // this thread, and DirectComposition has no apartment requirement. Recorded
    // rather than asserted - S_FALSE and RPC_E_CHANGED_MODE are both survivable.
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        meta.coInitHr = "CoInitializeEx(MTA) -> " + HrString(hr);
        Log("%s", meta.coInitHr.c_str());
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            LogErr("FATAL: COM initialisation failed");
            return kExitFatal;
        }
    }

    // ---- target resolution
    const std::vector<WinInfo> all = EnumerateVisibleWindows();
    const WinInfo* best = nullptr;
    std::vector<const WinInfo*> nearMisses;
    for (const WinInfo& w : all) {
        bool c = false, t = false, p = false;
        if (MatchesTarget(w, opt, &c, &t, &p)) {
            if (best == nullptr || w.clientArea > best->clientArea) best = &w;
        } else if ((c || t || p) && !w.title.empty()) {
            nearMisses.push_back(&w);
        }
    }

    if (best == nullptr) {
        LogErr("FATAL: no visible, non-cloaked top-level window matched "
               "class=\"%s\" title~=\"%s\" pid=%lu",
               Narrow(opt.targetClass).c_str(), Narrow(opt.targetTitle).c_str(),
               static_cast<unsigned long>(opt.targetPid));
        LogErr("Near misses (%zu). C=class T=title P=pid K=not-cloaked:", nearMisses.size());
        for (const WinInfo* w : nearMisses) {
            bool c = false, t = false, p = false;
            MatchesTarget(*w, opt, &c, &t, &p);
            LogErr("  [%s%s%s%s] hwnd=0x%llX pid=%-6lu %-24s %-28s \"%s\"",
                   c ? "C" : "-", t ? "T" : "-", p ? "P" : "-", w->cloaked ? "-" : "K",
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(w->hwnd)),
                   static_cast<unsigned long>(w->pid),
                   Narrow(ProcessImageName(w->pid)).c_str(),
                   Narrow(w->cls).c_str(),
                   Narrow(w->title).c_str());
        }
        LogErr("Run with --list-windows to see every candidate.");
        return kExitFatal;
    }

    const HWND target = best->hwnd;
    meta.target = target;
    meta.targetPid = best->pid;
    meta.targetClass = best->cls;
    meta.targetTitle = best->title;
    meta.targetImage = ProcessImageName(best->pid);

    // ---- geometry
    meta.hwndRectScreen = best->clientScreen;
    meta.targetDpi = GetDpiForWindow(target);
    if (meta.targetDpi == 0) {
        LogErr("WARN: GetDpiForWindow returned 0 (gle=%lu)", GetLastError());
    }

    meta.targetRectLastSeen = meta.hwndRectScreen;
    meta.targetDpiLastSeen = meta.targetDpi;

    // The requested visual rect. NOTE: --inset is NOT applied here. The overlay
    // owns Options::insetPx and applies (and clamps) it inside SetVisualRect;
    // applying it a second time here would shrink the visual twice and make the
    // report quote a rect nothing ever drew.
    RECT visual = meta.hwndRectScreen;
    if (opt.geom == GeomMode::Rect) {
        visual.left   = meta.hwndRectScreen.left + opt.rectClient.left;
        visual.top    = meta.hwndRectScreen.top  + opt.rectClient.top;
        visual.right  = meta.hwndRectScreen.left + opt.rectClient.right;
        visual.bottom = meta.hwndRectScreen.top  + opt.rectClient.bottom;
    } else if (flags.rectGiven) {
        LogErr("WARN: --rect was given but --geom is 'client'; the rect is ignored.");
    }

    // Degenerate-rect guard, on the REQUESTED rect.
    if (visual.right <= visual.left || visual.bottom <= visual.top) {
        LogErr("FATAL: the requested visual rect is degenerate: %ldx%ld (client area is %ldx%ld)",
               visual.right - visual.left, visual.bottom - visual.top,
               meta.hwndRectScreen.right - meta.hwndRectScreen.left,
               meta.hwndRectScreen.bottom - meta.hwndRectScreen.top);
        return kExitFatal;
    }
    meta.visualRectRequested = visual;

    // The overlay HWND is frozen at the target's client rect (I11) and the
    // swapchain is exactly that size, so anything outside it is clipped away by
    // DirectComposition and never reaches the screen. Reporting the requested
    // rect as coverage would claim pixels the run never put anywhere.
    {
        RECT effective{};
        if (!IntersectRect(&effective, &visual, &meta.hwndRectScreen)) {
            LogErr("FATAL: --rect %ld,%ld,%ld,%ld (screen px) lies entirely outside the overlay "
                   "HWND rect %ld,%ld,%ld,%ld - nothing would be drawn.",
                   visual.left, visual.top, visual.right, visual.bottom,
                   meta.hwndRectScreen.left, meta.hwndRectScreen.top,
                   meta.hwndRectScreen.right, meta.hwndRectScreen.bottom);
            return kExitFatal;
        }
        if (!SameRect(effective, visual)) {
            meta.visualRectClipped = true;
            LogErr("*****************************************************************************");
            LogErr("WARN: the requested visual rect does NOT fit the overlay HWND and was clipped.");
            LogErr("      requested (screen px): %ld,%ld,%ld,%ld  (%ldx%ld)",
                   visual.left, visual.top, visual.right, visual.bottom,
                   visual.right - visual.left, visual.bottom - visual.top);
            LogErr("      overlay HWND rect    : %ld,%ld,%ld,%ld  (%ldx%ld)",
                   meta.hwndRectScreen.left, meta.hwndRectScreen.top,
                   meta.hwndRectScreen.right, meta.hwndRectScreen.bottom,
                   meta.hwndRectScreen.right - meta.hwndRectScreen.left,
                   meta.hwndRectScreen.bottom - meta.hwndRectScreen.top);
            LogErr("      effective (used)     : %ld,%ld,%ld,%ld  (%ldx%ld)",
                   effective.left, effective.top, effective.right, effective.bottom,
                   effective.right - effective.left, effective.bottom - effective.top);
            LogErr("      The report quotes the EFFECTIVE rect; visual_rect_clipped is true in the JSON.");
            LogErr("*****************************************************************************");
        }
        visual = effective;
    }

    Log("target hwnd=0x%llX pid=%lu image=%s class=%s dpi=%u title=\"%s\"",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)),
        static_cast<unsigned long>(meta.targetPid), Narrow(meta.targetImage).c_str(),
        Narrow(meta.targetClass).c_str(), meta.targetDpi, Narrow(meta.targetTitle).c_str());
    Log("target window rect (screen px) = %ld,%ld,%ld,%ld  (%ldx%ld)",
        best->windowRect.left, best->windowRect.top, best->windowRect.right, best->windowRect.bottom,
        best->windowRect.right - best->windowRect.left,
        best->windowRect.bottom - best->windowRect.top);
    Log("target client rect (screen px) = %ld,%ld,%ld,%ld  (%ldx%ld)  <- overlay HWND rect",
        meta.hwndRectScreen.left, meta.hwndRectScreen.top,
        meta.hwndRectScreen.right, meta.hwndRectScreen.bottom,
        meta.hwndRectScreen.right - meta.hwndRectScreen.left,
        meta.hwndRectScreen.bottom - meta.hwndRectScreen.top);
    Log("requested visual rect (screen px)= %ld,%ld,%ld,%ld  (%ldx%ld)  geom=%s inset=%d "
        "(the inset is applied by the overlay, not here)",
        visual.left, visual.top, visual.right, visual.bottom,
        visual.right - visual.left, visual.bottom - visual.top,
        GeomName(opt.geom), opt.insetPx);

    // ---- overlay
    Overlay overlay;
    std::string createErr;
    if (!overlay.Create(opt, target, meta.hwndRectScreen, &createErr)) {
        LogErr("FATAL: Overlay::Create failed: %s", createErr.c_str());
        meta.creationReport = overlay.CreationReport();
        if (!meta.creationReport.empty()) {
            std::printf("%s\n", meta.creationReport.c_str());
            std::fflush(stdout);
        }
        return kExitFatal;
    }
    meta.creationReport = overlay.CreationReport();
    std::printf("---- Overlay::CreationReport ----\n%s\n--------------------------------\n",
                meta.creationReport.c_str());
    std::fflush(stdout);

    if (!overlay.SetVisualRect(visual)) {
        LogErr("FATAL: Overlay::SetVisualRect failed for %ld,%ld,%ld,%ld",
               visual.left, visual.top, visual.right, visual.bottom);
        return kExitFatal;
    }
    // Record what DirectComposition actually got, not what we asked it for: the
    // overlay clamps the inset and owns the final geometry.
    meta.visualRectScreen = overlay.CommittedVisualRect();
    Log("committed visual rect (screen px)= %ld,%ld,%ld,%ld  (%ldx%ld)  <- what the report quotes",
        meta.visualRectScreen.left, meta.visualRectScreen.top,
        meta.visualRectScreen.right, meta.visualRectScreen.bottom,
        meta.visualRectScreen.right - meta.visualRectScreen.left,
        meta.visualRectScreen.bottom - meta.visualRectScreen.top);

    // ---- capture
    WgcCapture capture;
    if (opt.wgc) {
        std::string wgcErr;
        if (capture.Start(target, opt, &wgcErr)) {
            meta.wgcStarted = true;
        } else {
            meta.wgcError = wgcErr;
            LogErr("WARN: WgcCapture::Start failed: %s - continuing WITHOUT capture; "
                   "the S-M2 capture half of this run is void.", wgcErr.c_str());
        }
        meta.wgcReport = capture.StartupReport();
        std::printf("---- WgcCapture::StartupReport ----\n%s\n----------------------------------\n",
                    meta.wgcReport.c_str());
        std::fflush(stdout);
    } else {
        meta.wgcError = "disabled by --no-wgc";
        Log("WGC disabled by --no-wgc");
    }

    // ---- compositor clock
    PfnWaitForCompositorClock waitClock = nullptr;
    HMODULE dcomp = LoadLibraryW(L"dcomp.dll");
    if (dcomp != nullptr) {
        waitClock = reinterpret_cast<PfnWaitForCompositorClock>(
            GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"));
    }
    if (waitClock != nullptr) {
        meta.clockPath = "DCompositionWaitForCompositorClock";
    } else {
        const DWORD gle = GetLastError();
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Sleep(1) fallback (dcomp.dll %s, DCompositionWaitForCompositorClock gle=%lu)",
                      dcomp ? "loaded" : "NOT loaded", static_cast<unsigned long>(gle));
        meta.clockPath = buf;
        LogErr("WARN: %s - pacing is no longer locked to the compositor", buf);
    }
    Log("compositor clock path: %s", meta.clockPath.c_str());

    // ---- stdin sync thread
    if (opt.stdinSync) {
        std::thread(StdinReaderThread).detach();
        Log("stdin-sync enabled: each phase ends on one line of stdin (EOF falls back to the timer)");
    }

    // ---- phase loop
    std::vector<PresentSample> presents;
    std::vector<CaptureSample> captures;
    std::vector<PhaseRun> runs(opt.phases.size());

    const uint32_t presentEvery = (opt.presentEveryNTicks <= 1)
                                      ? 1u : static_cast<uint32_t>(opt.presentEveryNTicks);
    uint32_t frameIndex = 0;
    uint32_t stdinConsumed = 0;
    const int64_t runStart = QpcNow();

    // Phase-boundary check that the target is still where the overlay HWND was
    // frozen (I11). Off the tick path by construction: it runs at most once per
    // phase plus once at the end of the run. Returns false when the run can no
    // longer be qualified and must be aborted.
    auto targetStillAligned = [&](const char* when) -> bool {
        RECT now{};
        if (!SampleClientRectScreen(target, &now)) {
            meta.targetMoved = true;
            meta.targetMovedDetail =
                std::string("target client rect unreadable at ") + when +
                " (window closed, destroyed or no longer accessible)";
            LogErr("FATAL: %s", meta.targetMovedDetail.c_str());
            return false;
        }
        meta.targetRectLastSeen = now;
        const UINT dpiNow = GetDpiForWindow(target);
        if (dpiNow != 0) meta.targetDpiLastSeen = dpiNow;

        const bool rectChanged = !SameRect(now, meta.hwndRectScreen);
        const bool dpiChanged = (dpiNow != 0) && (meta.targetDpi != 0) && (dpiNow != meta.targetDpi);
        if (!rectChanged && !dpiChanged) return true;

        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "target %s at %s: client rect was %ld,%ld,%ld,%ld (%ldx%ld) dpi=%u, "
                      "now %ld,%ld,%ld,%ld (%ldx%ld) dpi=%u",
                      rectChanged ? (dpiChanged ? "moved/resized AND changed DPI" : "moved or resized")
                                  : "changed DPI",
                      when,
                      meta.hwndRectScreen.left, meta.hwndRectScreen.top,
                      meta.hwndRectScreen.right, meta.hwndRectScreen.bottom,
                      meta.hwndRectScreen.right - meta.hwndRectScreen.left,
                      meta.hwndRectScreen.bottom - meta.hwndRectScreen.top,
                      meta.targetDpi,
                      now.left, now.top, now.right, now.bottom,
                      now.right - now.left, now.bottom - now.top, dpiNow);
        meta.targetMoved = true;
        meta.targetMovedDetail = buf;
        LogErr("FATAL: %s", buf);
        LogErr("       The overlay HWND is frozen at its creation-time rect (I11), so from here on "
               "the visual sits over stale coordinates. Aborting: a 'shown' phase measured against "
               "the wrong geometry is worse than no phase at all.");
        return false;
    };

    for (size_t pi = 0; pi < opt.phases.size() && !meta.deviceLost && !meta.quitPosted; ++pi) {
        const PhaseSpec& ph = opt.phases[pi];
        PhaseRun& run = runs[pi];
        run.presentBegin = presents.size();
        run.captureBegin = captures.size();
        // Kept equal to begin until the phase really ends, so an abort below can
        // never leave end < begin for the aggregates.
        run.presentEnd = run.presentBegin;
        run.captureEnd = run.captureBegin;

        // Phase boundary: has the target moved under us since creation?
        {
            char when[96];
            std::snprintf(when, sizeof(when), "the start of phase '%s'", ph.name.c_str());
            if (!targetStillAligned(when)) break;
        }

        if (ph.kind == PhaseKind::Shown) overlay.Show(); else overlay.Hide();
        // A WM_QUIT seen here means the phase must NOT start: printing PHASE_BEGIN
        // and running it would put a whole extra phase in the report as if it had
        // completed normally.
        if (!PumpMessages()) {
            meta.quitPosted = true;
            LogErr("WARN: WM_QUIT observed at the boundary before phase '%s'; that phase does not run",
                   ph.name.c_str());
            break;
        }

        run.ran = true;
        ++meta.phasesRun;

        // Exact marker text (after the Log() timestamp prefix) that chrome_cdp.py
        // aligns on: "PHASE_BEGIN <name> <kind> <seconds>".
        Log("PHASE_BEGIN %s %s %.3f", ph.name.c_str(), KindName(ph.kind), ph.seconds);

        const int64_t phaseStart = QpcNow();
        const double phaseMs = ph.seconds * 1000.0;
        bool useStdin = opt.stdinSync && !g_stdinEof.load(std::memory_order_relaxed);
        uint64_t tick = 0;

        for (;;) {
            if (g_interrupted.load(std::memory_order_relaxed)) { meta.interrupted = true; break; }

            if (waitClock != nullptr) {
                const DWORD wr = waitClock(0, nullptr, 100);
                if (wr != WAIT_OBJECT_0 && wr != WAIT_TIMEOUT) {
                    // Never silently: a failed wait degrades the pacing.
                    LogErr("WARN: DCompositionWaitForCompositorClock returned %lu (gle=%lu)",
                           static_cast<unsigned long>(wr), GetLastError());
                }
            } else {
                Sleep(1);
            }

            if (!PumpMessages()) { meta.quitPosted = true; break; }

            if (ph.kind == PhaseKind::Shown && (tick % presentEvery) == 0) {
                PresentSample ps{};
                if (!overlay.RenderAndPresent(frameIndex, &ps)) {
                    LogErr("FATAL: Overlay::RenderAndPresent reported device loss at frame %u", frameIndex);
                    meta.deviceLost = true;
                    break;
                }
                ++frameIndex;
                ps.phaseIndex = static_cast<uint32_t>(pi);
                // A sample that never reached Present1 carries no timestamp of its
                // own; one that reached Present1 and failed does. That is what
                // separates "skipped" from "failed" here.
                const bool reachedPresent1 = (ps.qpc != 0);
                if (ps.qpc == 0) {
                    ps.qpc = QpcNow();
                    if (ps.valid) ++meta.presentsMissingQpc;
                }
                if (!ps.valid) {
                    if (reachedPresent1) { ++run.presentsFailed;  ++meta.presentsFailed;  }
                    else                 { ++run.presentsSkipped; ++meta.presentsSkipped; }
                }
                // Pushed either way: the CSV stays complete (rec=present_invalid),
                // AggregatePresents is what filters on `valid`.
                presents.push_back(ps);
            }

            if (meta.wgcStarted) {
                const size_t before = captures.size();
                capture.Poll(static_cast<uint32_t>(pi), &captures);
                for (size_t i = before; i < captures.size(); ++i) {
                    if (captures[i].qpc == 0) { captures[i].qpc = QpcNow(); ++meta.capturesMissingQpc; }
                }
            }

            ++tick;

            if (useStdin) {
                if (g_stdinLines.load(std::memory_order_relaxed) > stdinConsumed) { ++stdinConsumed; break; }
                if (g_stdinEof.load(std::memory_order_relaxed)) {
                    // Do not cut the phase short: drop back to the timer so a
                    // closed pipe degrades into an ordinary timed run.
                    LogErr("WARN: stdin reached EOF; phase '%s' falls back to its %.3f s timer",
                           ph.name.c_str(), ph.seconds);
                    useStdin = false;
                }
            }
            if (!useStdin) {
                if (QpcToMs(QpcNow() - phaseStart) >= phaseMs) break;
            }
        }

        run.actualSeconds = QpcToMs(QpcNow() - phaseStart) / 1000.0;
        run.ticks = tick;
        run.presentEnd = presents.size();
        run.captureEnd = captures.size();

        Log("PHASE_END %s", ph.name.c_str());

        if (run.presentsSkipped > 0 || run.presentsFailed > 0) {
            LogErr("WARN: phase '%s': %zu present(s) never reached Present1 and %zu failed; "
                   "they are in the CSV as rec=present_invalid but not in any statistic",
                   ph.name.c_str(), run.presentsSkipped, run.presentsFailed);
        }
        if (meta.quitPosted) LogErr("WARN: WM_QUIT received; stopping after phase '%s'", ph.name.c_str());
        if (meta.interrupted) LogErr("WARN: interrupted by console signal during phase '%s'", ph.name.c_str());
        if (meta.interrupted || meta.quitPosted) break;
    }

    meta.totalSeconds = QpcToMs(QpcNow() - runStart) / 1000.0;

    // Final phase boundary: qualify the last phase the same way as the others.
    // Skipped when the run already aborted for this reason.
    if (!meta.targetMoved && meta.phasesRun > 0) {
        targetStillAligned("the end of the run");
    }

    overlay.Hide();
    PumpMessages();
    // Read the capture counters BEFORE Stop(), which resets the session state.
    if (meta.wgcStarted) {
        meta.wgcFramesWithoutStats = capture.FramesWithoutPixelStats();
        capture.Stop();
    }

    // ---- aggregate
    CaptureStats overall;
    {
        double capturedSeconds = 0.0;
        for (size_t i = 0; i < runs.size(); ++i) {
            runs[i].present = AggregatePresents(presents, runs[i].presentBegin, runs[i].presentEnd);
            runs[i].capture = AggregateCaptures(captures, runs[i].captureBegin, runs[i].captureEnd,
                                                runs[i].actualSeconds);
            capturedSeconds += runs[i].actualSeconds;
        }
        overall = AggregateCaptures(captures, 0, captures.size(), capturedSeconds);
    }

    // ---- output
    int exitCode = kExitOk;
    std::string writeErr;
    if (!WriteCsv(opt, presents, captures, &writeErr)) {
        LogErr("FATAL: %s", writeErr.c_str());
        exitCode = kExitFatal;
    } else {
        Log("wrote %zu present + %zu capture rows to %s",
            presents.size(), captures.size(), opt.csvPath.c_str());
    }
    if (!WriteJson(opt, meta, runs, overall, &writeErr)) {
        LogErr("FATAL: %s", writeErr.c_str());
        exitCode = kExitFatal;
    } else {
        Log("wrote report to %s", opt.jsonPath.c_str());
    }

    // Both files are on disk: release a console-close handler parked in
    // CtrlHandler's wait. (The guard would do it too, but only after the summary.)
    if (g_outputsWritten != nullptr) SetEvent(g_outputsWritten);

    PrintSummary(opt, meta, runs, overall);

    if (meta.deviceLost) return kExitDeviceLost;
    // An aborted run is not a completed run: the target moved out from under the
    // frozen overlay HWND, so the phases that did run cannot be qualified.
    if (meta.targetMoved && exitCode == kExitOk) return kExitFatal;
    return exitCode;
}

}  // namespace
}  // namespace np

int wmain(int argc, wchar_t** argv) {
    int code = np::kExitFatal;
    try {
        code = np::Run(argc, argv);
    } catch (const std::exception& e) {
        np::LogErr("FATAL: unhandled std::exception: %s", e.what());
        code = np::kExitFatal;
    } catch (...) {
        np::LogErr("FATAL: unhandled non-standard exception (a C++/WinRT hresult_error most likely)");
        code = np::kExitFatal;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    // A --stdin-sync run leaves a detached reader thread parked inside
    // std::getline. Returning from wmain would run the CRT's static destructors
    // underneath it; _Exit skips them. Everything we own is already flushed.
    std::_Exit(code);
}
