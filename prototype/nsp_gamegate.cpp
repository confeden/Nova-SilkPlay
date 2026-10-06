// nsp_gamegate.cpp — see nsp_gamegate.h for the three contracts this file exists to keep.

#include "nsp_gamegate.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>  // SHQueryUserNotificationState, QUERY_USER_NOTIFICATION_STATE
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <vector>

namespace nsp {
namespace {

// ----------------------------------------------------------------- tuning

// 5 Hz. Fast enough that two consecutive qualifying samples land inside the
// ~400 ms the architecture budgets for noticing a game, slow enough that a PDH
// wildcard collect over ~400 instances is a rounding error on one core.
constexpr DWORD  kSamplePeriodMs       = 200;
// Asymmetric thresholds: a game has to be clearly busy to stop a video, and
// clearly idle to let it start again, so a title hovering around one number
// cannot make the overlay blink.
constexpr double kPauseLoadPct         = 35.0;
constexpr double kResumeLoadPct        = 12.0;
constexpr int    kPauseSamples         = 2;     // ~400 ms on the timer, ~205 ms after a nudge
constexpr double kResumeSec            = 3.0;   // continuous all-clear before generation returns
constexpr double kTreeRefreshSec       = 2.0;   // a game's children are made at launch, not later
constexpr double kKnownGamesRefreshSec = 60.0;  // a game registers itself on its FIRST launch
// A nudged sample taken sooner than this after the last PDH collect reuses the
// previous utilisation instead of collecting again. A utilisation counter is a
// delta over time: a 5 ms window reads as noise and can print far over 100%,
// which is exactly the direction that would cause a FALSE pause. The
// classification still refreshes at once, so the nudge keeps its latency win.
constexpr double kMinCollectSec        = 0.100;
constexpr size_t kMaxTreePids          = 64;
constexpr int    kMaxTreeDepth         = 8;
// PDH instance names are not a documented contract. If a driver or OS update
// renames them, give up loudly rather than measure garbage (contract 1).
constexpr int    kMaxParseFailures     = 100;
constexpr int    kMaxPdhErrors         = 10;
constexpr size_t kCostWindow           = 256;

// ------------------------------------------------------------------- PDH

// PDH_STATUS is LONG and the PDH_* codes are DWORD; comparing them directly is a
// /W4 signed/unsigned mismatch. One helper instead of a cast at every site.
bool PdhIs(PDH_STATUS st, DWORD code) { return static_cast<DWORD>(st) == code; }

std::string PdhStr(PDH_STATUS st) {
    char b[32];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "0x%08lX", static_cast<unsigned long>(st));
    return b;
}

// ------------------------------------------------------------ known games

// The Game Bar's static list. MEASURED on this machine (2026-09-20): 59 children,
// only 4 of them carry MatchedExeFullPath, and GameDVR_Enabled is 0 — which is why
// this is a CORROBORATOR, never a primary signal. The sibling `Parent` value is a
// DPAPI blob: it is never read and never decrypted.
std::vector<std::wstring> LoadKnownGamePaths() {
    std::vector<std::wstring> out;
    HKEY children = nullptr;
    // Not an error when it is missing: a machine that never ran a Game Bar-aware
    // game has no such key, and the gate simply loses one corroborator.
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ,
                      &children) != ERROR_SUCCESS)
        return out;

    for (DWORD i = 0;; ++i) {
        wchar_t sub[256] = {};
        DWORD subLen = ARRAYSIZE(sub);
        if (RegEnumKeyExW(children, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD cb = sizeof(path);
        // RegGetValueW null-terminates for us and rejects a value that is not REG_SZ.
        if (RegGetValueW(children, sub, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, path,
                         &cb) != ERROR_SUCCESS)
            continue;
        if (path[0] == L'\0') continue;
        _wcslwr_s(path, ARRAYSIZE(path));  // matched against ProcessImageNameLower
        out.emplace_back(path);
    }
    RegCloseKey(children);

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// --------------------------------------------------------- process trees

// One toolhelp snapshot feeds BOTH trees in a tick (the capture target's, which is
// exempt, and the candidate's, which is measured). Taking two snapshots for two
// trees would double the only genuinely expensive call in the loop.
struct ProcessSnapshot {
    std::vector<DWORD> pid;
    std::vector<DWORD> parent;  // parallel to pid
};

bool TakeProcessSnapshot(ProcessSnapshot* out) {
    out->pid.clear();
    out->parent.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            out->pid.push_back(pe.th32ProcessID);
            out->parent.push_back(pe.th32ParentProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return !out->pid.empty();
}

// Breadth-first from `root`, capped at 64 pids and depth 8.
//
// The cap is not decoration. th32ParentProcessID is NOT reference counted: when a
// parent exits, its pid can be handed to an unrelated process, and that process's
// children then appear to descend from our root. The caps bound how much of the
// machine a stale parent link can drag in; the LUID and engine-type filters on the
// load side bound what that would cost even if it happened.
std::vector<DWORD> ProcessTreeOf(const ProcessSnapshot& snap, DWORD root) {
    std::vector<DWORD> tree;
    if (!root) return tree;
    tree.push_back(root);

    size_t layerBegin = 0;
    for (int depth = 0; depth < kMaxTreeDepth && tree.size() < kMaxTreePids; ++depth) {
        const size_t layerEnd = tree.size();
        if (layerBegin == layerEnd) break;  // nothing new was added last round
        for (size_t i = 0; i < snap.pid.size() && tree.size() < kMaxTreePids; ++i) {
            const DWORD child = snap.pid[i];
            if (!child) continue;
            bool descends = false;
            for (size_t j = layerBegin; j < layerEnd; ++j) {
                if (snap.parent[i] == tree[j]) {
                    descends = true;
                    break;
                }
            }
            if (!descends) continue;
            if (std::find(tree.begin(), tree.end(), child) != tree.end()) continue;
            tree.push_back(child);
        }
        layerBegin = layerEnd;
    }
    return tree;
}

bool InTree(DWORD pid, const std::vector<DWORD>& tree) {
    return std::find(tree.begin(), tree.end(), pid) != tree.end();
}

// A pid that exits with 259 reads as alive; that costs one extra 200 ms sample of
// a candidate that is already producing no load, so it is not worth a second API.
bool ProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;  // gone, or one we may not open: treat as gone, which fails OPEN
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) != FALSE && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// -------------------------------------------------------- classification

// Windows' own windows take the foreground constantly (the Start menu, Alt+Tab,
// the task view, a toast). None of them is a game and none may arm the gate.
// GetClassNameW does not send a message — it reads the class from the kernel side.
bool IsShellClass(HWND hwnd) {
    wchar_t cls[128] = {};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0) return false;
    static const wchar_t* const kShell[] = {
        L"Progman",
        L"WorkerW",
        L"Shell_TrayWnd",
        L"Windows.UI.Core.CoreWindow",
        L"XamlExplorerHostIslandWindow",
        L"MultitaskingViewFrame",
        L"ForegroundStaging",
        L"TaskListThumbnailWnd",
    };
    for (const wchar_t* s : kShell) {
        if (_wcsicmp(cls, s) == 0) return true;
    }
    return false;
}

RECT VirtualScreenRect() {
    RECT r{};
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

// Stage 1. Returns true when the foreground window is a positive game candidate.
// Sampler thread: every call below is a kernel-side or DWM query — none of them
// sends a message to the foreign window (contract 2).
bool ClassifyForeground(DWORD targetPid, const std::vector<DWORD>& targetTree,
                        const std::vector<std::wstring>& knownGames, DWORD* pid, GameSignal* sig,
                        std::wstring* exeName) {
    *pid = 0;
    *sig = GameSignal::kNone;
    exeName->clear();

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;  // nobody has the foreground (a lock screen, a desktop switch)

    DWORD fgPid = 0;
    GetWindowThreadProcessId(hwnd, &fgPid);
    if (!fgPid || fgPid == GetCurrentProcessId()) return false;
    // Contract 3: the capture target's whole tree can never be a candidate. Our own
    // overlay is what makes the browser's GPU process busy, and arming on it is N14.
    if (fgPid == targetPid || InTree(fgPid, targetTree)) return false;
    if (IsShellClass(hwnd)) return false;
    if (IsCloaked(hwnd)) return false;  // another virtual desktop, or a suspended UWP app
    if (IsIconic(hwnd)) return false;

    // Empty when the process may not be opened (a service, an elevated title). The
    // known-game test below then simply does not match — an unknown path is never
    // treated as a game, only as "not corroborated".
    const std::wstring path = ProcessImageNameLower(fgPid);

    GameSignal s = GameSignal::kNone;
    if (IsFullscreenWindow(hwnd)) {
        // The one signal that must carry the design: borderless-fullscreen is what
        // 2026 titles actually do, on either monitor (the owner's normal layout is
        // video on one and a game on the other, so this is deliberately NOT scoped
        // to the video's monitor).
        s = GameSignal::kFullscreenForeground;
    } else {
        // A failed query is not logged: it is asked five times a second, and the
        // fail-open answer — "no signal" — is already what an unset `qs` gives.
        QUERY_USER_NOTIFICATION_STATE qs = QUNS_NOT_PRESENT;
        if (SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
            (qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE)) {
            // The OS's own answer to "is some other process's swapchain fullscreen",
            // which no DXGI call can give us for a foreign process. A bonus path: it
            // only fires for EXCLUSIVE fullscreen, which most titles no longer use.
            s = GameSignal::kD3DExclusive;
        } else if (!path.empty() &&
                   std::binary_search(knownGames.begin(), knownGames.end(), path)) {
            s = GameSignal::kKnownGame;
        } else {
            // UNVERIFIED: this path has never been exercised against a real game. The
            // reasoning is that a windowed game clips the cursor to its client rect
            // while a browser or player does not; GetClipCursor is global, so the clip
            // is attributed to the foreground process, which we already know is foreign
            // and is not our capture target. Until a real title confirms it, treat a
            // kCursorClip verdict in a log as a hypothesis, not a measurement.
            RECT clip{};
            const RECT virt = VirtualScreenRect();
            if (GetClipCursor(&clip) && !RectEmpty(clip) && EqualRect(&clip, &virt) == FALSE)
                s = GameSignal::kCursorClip;
        }
    }
    if (s == GameSignal::kNone) return false;

    *pid = fgPid;
    *sig = s;
    if (!path.empty()) {
        const size_t slash = path.find_last_of(L'\\');
        *exeName = slash == std::wstring::npos ? path : path.substr(slash + 1);
    }
    return true;
}

// ---------------------------------------------------------------- the load

// Max over the candidate tree's 3D and compute engines, which is Task Manager's
// convention — a sum over engines double-counts work that is already serialised on
// one adapter and would cross 35% on a title that is barely moving.
//
// Instance names look like
//   pid_14280_luid_0x00000000_0x00010E1A_phys_0_eng_0_engtype_3D
// MEASURED on this machine with `typeperf -qx "GPU Engine"` (2026-09-20): the engtype
// is printed MIXED case — "3D", "Copy", "VideoDecode", "JPEG_Decode_0", "OFA_0",
// "Security_1", "VR" — so the comparison below is _wcsicmp, never ==. The same run
// shows TWO adapter LUIDs, 0x00000000_0x00010E1A with 700 instances and
// 0x00000000_0x0001254E with 126 from a second render device; without the LUID filter
// the second device's engines would count as load on ours. Copy, VideoDecode,
// VideoEncode, JPEG_Decode, OFA, VR and Security are excluded on purpose: DMA and
// video decode are what a VIDEO does, not what a game does.
double TreeUtilPct(const PDH_FMT_COUNTERVALUE_ITEM_W* items, DWORD n,
                   const std::vector<DWORD>& tree, LUID luid, int* consecutiveParseFails) {
    double best = 0.0;
    for (DWORD i = 0; i < n; ++i) {
        const wchar_t* name = items[i].szName;
        if (!name) {
            ++*consecutiveParseFails;
            continue;
        }
        unsigned int ipid = 0, hi = 0, lo = 0, phys = 0, eng = 0;
        wchar_t engtype[64] = {};
        const int got =
            swscanf_s(name, L"pid_%u_luid_0x%x_0x%x_phys_%u_eng_%u_engtype_%s", &ipid, &hi, &lo,
                      &phys, &eng, engtype, static_cast<unsigned>(ARRAYSIZE(engtype)));
        if (got != 6) {
            ++*consecutiveParseFails;
            continue;
        }
        *consecutiveParseFails = 0;

        if (!InTree(static_cast<DWORD>(ipid), tree)) continue;  // contract 3, the whole point
        if ((luid.LowPart != 0 || luid.HighPart != 0) &&
            (static_cast<DWORD>(lo) != luid.LowPart || static_cast<LONG>(hi) != luid.HighPart))
            continue;
        if (_wcsicmp(engtype, L"3d") != 0 && _wcsicmp(engtype, L"compute") != 0) continue;
        // An instance whose process vanished between the collect and the format
        // carries a status instead of a number.
        const DWORD cs = items[i].FmtValue.CStatus;
        if (cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA) continue;

        const double v = items[i].FmtValue.doubleValue;
        if (v > best) best = v;
    }
    return best;
}

// p is 0..1. Takes the vector by value: the ring must not be reordered.
double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    size_t k = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    if (k >= v.size()) k = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + static_cast<std::vector<double>::difference_type>(k),
                     v.end());
    return v[k];
}

}  // namespace

const char* GameSignalName(GameSignal s) {
    switch (s) {
        case GameSignal::kNone:                 return "none";
        case GameSignal::kFullscreenForeground: return "fullscreen";
        case GameSignal::kD3DExclusive:         return "d3d-exclusive";
        case GameSignal::kKnownGame:            return "known-game";
        case GameSignal::kCursorClip:           return "cursor-clip";
    }
    return "?";
}

// ------------------------------------------------------------------- Impl

struct GameGate::Impl {
    // ---- shared with the engine thread
    mutable std::mutex mu;
    GameGateStatus status;
    std::atomic<bool> ok{false};
    std::atomic<uint64_t> adapterLuid{0};        // HighPart << 32 | LowPart; 0 = accept any
    std::atomic<unsigned long> targetPid{0};     // DWORD

    // ---- owned by the thread that calls Start/Stop
    HANDLE thread = nullptr;
    HANDLE wake = nullptr;  // auto-reset: NudgeForeground()
    HANDLE stop = nullptr;  // manual-reset

    // ---- sampler thread only, from here down
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;  // owned by `query`, not closed separately
    std::vector<uint8_t> pdhBuf;

    std::vector<std::wstring> knownGames;
    int64_t knownGamesAt = 0;

    ProcessSnapshot snap;
    int64_t snapAt = 0;
    std::vector<DWORD> targetTree;
    DWORD targetTreeFor = 0;

    DWORD armedPid = 0;
    std::vector<DWORD> armedTree;
    GameSignal armedSignal = GameSignal::kNone;
    std::wstring armedExe;

    GateVerdict verdict = GateVerdict::kClear;
    int64_t pausedAt = 0;
    int pauseRun = 0;
    bool resuming = false;
    int64_t resumeAt = 0;

    double lastUtil = 0.0;
    int64_t collectAt = 0;
    int parseFails = 0;
    int pdhErrors = 0;
    bool gaveUp = false;
    bool snapErrLogged = false;
    uint64_t samples = 0;

    std::vector<double> cost;  // rolling PDH collect+format cost, milliseconds
    size_t costNext = 0;

    static DWORD WINAPI ThreadMain(void* p);
    void Run();
    void Sample();
    void Publish(double util);
    void PushCost(double ms);
    LUID CurrentLuid() const;
};

DWORD WINAPI GameGate::Impl::ThreadMain(void* p) {
    static_cast<Impl*>(p)->Run();
    return 0;
}

LUID GameGate::Impl::CurrentLuid() const {
    const uint64_t packed = adapterLuid.load(std::memory_order_relaxed);
    LUID l{};
    l.LowPart = static_cast<DWORD>(packed & 0xFFFFFFFFull);
    l.HighPart = static_cast<LONG>(static_cast<uint32_t>(packed >> 32));
    return l;
}

void GameGate::Impl::PushCost(double ms) {
    if (cost.size() < kCostWindow) cost.push_back(ms);
    else cost[costNext] = ms;
    costNext = (costNext + 1) % kCostWindow;
}

void GameGate::Impl::Run() {
    cost.reserve(kCostWindow);

    // The FIRST collect after the query opens carries no delta and PDH answers the
    // next format call with PDH_INVALID_DATA by design. Prime it here so the first
    // real sample 200 ms later already has a baseline, and do not count it.
    (void)PdhCollectQueryData(query);
    collectAt = QpcNow();

    for (;;) {
        HANDLE waits[2] = {stop, wake};  // stop is index 0, so it wins a simultaneous signal
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, kSamplePeriodMs);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_FAILED) {
            LogErr("game gate: WaitForMultipleObjects failed, GetLastError=%lu - sampler stopping "
                   "(generation will NOT pause for games)",
                   GetLastError());
            ok.store(false, std::memory_order_relaxed);
            break;
        }
        // WAIT_OBJECT_0 + 1 is the foreground nudge, WAIT_TIMEOUT the 5 Hz tick.
        Sample();
    }
}

void GameGate::Impl::Sample() {
    const int64_t now = QpcNow();

    // ---- the Game Bar list, on a slow timer: a title registers itself the first
    //      time it runs, which can happen in the middle of our session.
    if (!knownGamesAt || QpcToSec(now - knownGamesAt) >= kKnownGamesRefreshSec) {
        knownGames = LoadKnownGamePaths();
        knownGamesAt = now;
    }

    // ---- process snapshot, shared by both trees
    const DWORD tpid = static_cast<DWORD>(targetPid.load(std::memory_order_relaxed));
    const bool needSnap = !snapAt || tpid != targetTreeFor ||
                          QpcToSec(now - snapAt) >= kTreeRefreshSec ||
                          (armedPid != 0 && armedTree.empty());
    if (needSnap) {
        if (TakeProcessSnapshot(&snap)) {
            snapAt = now;
            targetTree = ProcessTreeOf(snap, tpid);
            targetTreeFor = tpid;
            if (armedPid) armedTree = ProcessTreeOf(snap, armedPid);
        } else if (!snapErrLogged) {
            // Fail open: with no snapshot the target tree may be stale, so the safest
            // reading is "no candidate" — which the classifier reaches on its own once
            // the foreground window is inside a tree we can no longer confirm.
            snapErrLogged = true;
            LogErr("game gate: CreateToolhelp32Snapshot failed, GetLastError=%lu - process trees "
                   "are stale until it recovers",
                   GetLastError());
        }
    }

    // ---- an armed candidate that turns out to be inside the capture target's tree
    //      must be disarmed. This happens for real: Chrome can be armed as a foreign
    //      fullscreen window BEFORE Engage() names it as our target, and keeping it
    //      armed afterwards is exactly N14 (our overlay drives its GPU process).
    if (armedPid && (armedPid == tpid || InTree(armedPid, targetTree))) {
        armedPid = 0;
        armedTree.clear();
        armedSignal = GameSignal::kNone;
        armedExe.clear();
    }
    // ---- an armed candidate whose root exited
    if (armedPid && !ProcessAlive(armedPid)) {
        armedPid = 0;
        armedTree.clear();
        armedSignal = GameSignal::kNone;
        armedExe.clear();
    }

    // ---- stage 1
    DWORD candPid = 0;
    GameSignal sigNow = GameSignal::kNone;
    std::wstring exeNow;
    ClassifyForeground(tpid, targetTree, knownGames, &candPid, &sigNow, &exeNow);
    if (candPid && candPid != armedPid) {
        armedPid = candPid;
        // Arming is the one moment the tree has to be right NOW: a game that just
        // launched is younger than the 2 s refresh, and the whole pause decision
        // hangs on which pids we are about to measure. Every later rebuild can wait.
        if (QpcToSec(now - snapAt) > 0.0 && TakeProcessSnapshot(&snap)) {
            snapAt = now;
            targetTree = ProcessTreeOf(snap, tpid);
            targetTreeFor = tpid;
        }
        armedTree = ProcessTreeOf(snap, armedPid);
        armedSignal = sigNow;
        armedExe = exeNow;
        pauseRun = 0;  // a new candidate starts its own run of qualifying samples
    } else if (candPid && candPid == armedPid) {
        armedSignal = sigNow;
        armedExe = exeNow;
    }
    // A candidate stays armed while its root lives even when it loses the foreground:
    // alt-tabbing out of a fullscreen game does not stop the game rendering.

    // ---- stage 2: load, attributed to the armed tree ONLY
    double util = lastUtil;
    if (ok.load(std::memory_order_relaxed) && !gaveUp) {
        if (QpcToSec(now - collectAt) >= kMinCollectSec) {
            const int64_t t0 = QpcNow();
            PDH_STATUS st = PdhCollectQueryData(query);
            bool measured = false;
            if (st == ERROR_SUCCESS) {
                DWORD cb = static_cast<DWORD>(pdhBuf.size());
                DWORD count = 0;
                auto* arr = pdhBuf.empty()
                                ? nullptr
                                : reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhBuf.data());
                st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &cb, &count, arr);
                if (PdhIs(st, PDH_MORE_DATA)) {
                    // Instances come and go with every process, so the buffer grows on
                    // demand and is then reused for the life of the gate.
                    pdhBuf.resize(cb);
                    cb = static_cast<DWORD>(pdhBuf.size());
                    arr = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhBuf.data());
                    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &cb, &count, arr);
                }
                if (st == ERROR_SUCCESS && arr) {
                    util = armedPid ? TreeUtilPct(arr, count, armedTree, CurrentLuid(), &parseFails)
                                    : 0.0;
                    measured = true;
                }
            }
            collectAt = QpcNow();
            PushCost(QpcToMs(collectAt - t0));

            if (measured) {
                pdhErrors = 0;
            } else {
                // FAIL OPEN: a sample we could not measure counts as NO load, never as
                // the last (possibly high) one — a stale 90% would hold the pause down.
                util = 0.0;
                if (++pdhErrors == 1)
                    LogErr("game gate: PDH sample failed (%s) - treating this sample as idle",
                           PdhStr(st).c_str());
                if (pdhErrors >= kMaxPdhErrors && !gaveUp) {
                    gaveUp = true;
                    LogErr("game gate: %d PDH samples in a row failed - turning the gate OFF for "
                           "this run (generation will NOT pause for games)",
                           pdhErrors);
                }
            }
            if (parseFails >= kMaxParseFailures && !gaveUp) {
                gaveUp = true;
                LogErr("game gate: %d GPU Engine instance names in a row did not parse - the "
                       "counter format changed; turning the gate OFF for this run (generation "
                       "will NOT pause for games)",
                       parseFails);
            }
        }
    } else {
        util = 0.0;
    }
    lastUtil = util;

    // Giving up forces a clear verdict, published BEFORE Ok() goes false, so a
    // caller that stops reading Status() at that moment is not left holding a
    // stale "paused" (contract 1).
    if (gaveUp && verdict == GateVerdict::kPaused) {
        verdict = GateVerdict::kClear;
        resuming = false;
        Log("game gate: giving up - generation resumes");
    }

    // ---- the state machine
    //
    // The D3D fast path uses THIS sample's classification rather than the armed
    // signal: QUNS is a global shell state, and a stale kD3DExclusive on a game
    // that has gone to the background would pause us forever with no load to show.
    const bool d3dNow = candPid != 0 && candPid == armedPid && sigNow == GameSignal::kD3DExclusive;
    const bool qualifying = !gaveUp && armedPid != 0 && (d3dNow || util >= kPauseLoadPct);
    const bool clearing = gaveUp || armedPid == 0 || util < kResumeLoadPct;

    if (verdict == GateVerdict::kClear) {
        pauseRun = qualifying ? pauseRun + 1 : 0;
        if (pauseRun >= kPauseSamples) {
            verdict = GateVerdict::kPaused;
            pausedAt = now;
            pauseRun = 0;
            resuming = false;
            Log("game gate: PAUSED - %s pid %lu %ls, %.0f%% on the 3D engine",
                GameSignalName(armedSignal), armedPid,
                armedExe.empty() ? L"?" : armedExe.c_str(), util);
        }
    } else {
        pauseRun = 0;
        if (clearing) {
            if (!resuming) {
                resuming = true;
                resumeAt = now;
            } else if (QpcToSec(now - resumeAt) >= kResumeSec) {
                Log("game gate: clear after %.1f s - generation resumes",
                    QpcToSec(now - pausedAt));
                verdict = GateVerdict::kClear;
                resuming = false;
                armedPid = 0;
                armedTree.clear();
                armedSignal = GameSignal::kNone;
                armedExe.clear();
            }
        } else {
            resuming = false;  // the run of clearing samples has to be continuous
        }
    }

    ++samples;
    Publish(util);
    if (gaveUp) ok.store(false, std::memory_order_relaxed);
}

void GameGate::Impl::Publish(double util) {
    GameGateStatus s;
    s.verdict = verdict;
    s.signal = armedSignal;
    s.pid = armedPid;
    s.exe = armedExe;
    s.utilPct = util;
    s.pausedSec = verdict == GateVerdict::kPaused ? QpcToSec(QpcNow() - pausedAt) : 0.0;
    s.samples = samples;
    s.ok = ok.load(std::memory_order_relaxed) && !gaveUp;
    s.collectMsP50 = Percentile(cost, 0.50);
    s.collectMsP99 = Percentile(cost, 0.99);

    std::lock_guard<std::mutex> lock(mu);
    status = std::move(s);
}

// --------------------------------------------------------------- GameGate

GameGate::GameGate() = default;

GameGate::~GameGate() { Stop(); }

bool GameGate::Start(std::string* err) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;
    if (d.thread) return d.ok.load(std::memory_order_relaxed);

    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &d.query);
    if (st != ERROR_SUCCESS) {
        d.query = nullptr;
        if (err) *err = "PdhOpenQueryW failed, status=" + PdhStr(st);
        return false;
    }
    // PdhAddEnglishCounterW, not PdhAddCounterW: the counter set's display names are
    // MUI-localised (this is a Russian-capable machine), and the English API takes the
    // English path on any locale. That is what removes the PdhLookupPerfNameByIndex /
    // Perflib index-table dependency the architecture originally planned for.
    st = PdhAddEnglishCounterW(d.query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &d.counter);
    if (st != ERROR_SUCCESS) {
        if (err) *err = "PdhAddEnglishCounterW(\\GPU Engine(*)\\Utilization Percentage) failed, "
                        "status=" + PdhStr(st);
        PdhCloseQuery(d.query);
        d.query = nullptr;
        d.counter = nullptr;
        return false;
    }

    d.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
    d.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    if (!d.stop || !d.wake) {
        if (err) *err = "CreateEvent failed, GetLastError=" + std::to_string(GetLastError());
        if (d.stop) { CloseHandle(d.stop); d.stop = nullptr; }
        if (d.wake) { CloseHandle(d.wake); d.wake = nullptr; }
        PdhCloseQuery(d.query);
        d.query = nullptr;
        d.counter = nullptr;
        return false;
    }

    d.ok.store(true, std::memory_order_relaxed);
    d.thread = CreateThread(nullptr, 0, &Impl::ThreadMain, &d, 0, nullptr);
    if (!d.thread) {
        if (err) *err = "CreateThread failed, GetLastError=" + std::to_string(GetLastError());
        d.ok.store(false, std::memory_order_relaxed);
        CloseHandle(d.stop);
        d.stop = nullptr;
        CloseHandle(d.wake);
        d.wake = nullptr;
        PdhCloseQuery(d.query);
        d.query = nullptr;
        d.counter = nullptr;
        return false;
    }
    return true;
}

void GameGate::Stop() {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.thread) {
        SetEvent(d.stop);
        if (WaitForSingleObject(d.thread, 5000) != WAIT_OBJECT_0) {
            // The sampler still points into Impl, so Impl must outlive it: leak it
            // rather than free memory a running thread reads. Same call as the tray's.
            LogErr("game gate: the sampler thread did not stop within 5 s - leaving it behind");
            impl_.release();
            return;
        }
        CloseHandle(d.thread);
        d.thread = nullptr;
    }
    d.ok.store(false, std::memory_order_relaxed);
    if (d.query) {
        PdhCloseQuery(d.query);  // closes every counter added to it
        d.query = nullptr;
        d.counter = nullptr;
    }
    if (d.wake) { CloseHandle(d.wake); d.wake = nullptr; }
    if (d.stop) { CloseHandle(d.stop); d.stop = nullptr; }
}

bool GameGate::Ok() const { return impl_ && impl_->ok.load(std::memory_order_relaxed); }

void GameGate::SetAdapterLuid(LUID luid) {
    if (!impl_) return;  // never started: nothing to configure, and the gate is open
    const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
                            static_cast<uint64_t>(luid.LowPart);
    impl_->adapterLuid.store(packed, std::memory_order_relaxed);
}

void GameGate::SetTargetPid(DWORD pid) {
    if (!impl_) return;
    impl_->targetPid.store(pid, std::memory_order_relaxed);
}

void GameGate::NudgeForeground() {
    if (impl_ && impl_->wake) SetEvent(impl_->wake);
}

GameGateStatus GameGate::Status() const {
    if (!impl_) return GameGateStatus{};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->status;
}

}  // namespace nsp
