// nsp_gamegate.h — pause frame generation while a game has the GPU (ROADMAP P6).
//
// The gate is two-stage and ASYMMETRIC. Stage 1 classifies the FOREGROUND window
// into a positive game candidate (a foreign fullscreen window on any monitor, the
// shell reporting exclusive-fullscreen D3D, the foreground image path listed in
// the Game Bar's HKCU\System\GameConfigStore\Children, or the cursor clipped to
// less than the virtual screen). Stage 2 confirms it with GPU load attributed
// ONLY to that candidate's process tree. Load is never consulted without a
// candidate, so a positive game signal always comes first and a busy GPU on its
// own can never stop a video.
//
// It samples on a thread of its own at 5 Hz. The engine only ever reads one
// mutex-guarded snapshot through Status(), because PDH wildcard collection over
// ~400 GPU Engine instances and toolhelp process walking have no business in a
// loop that paces 165 frames a second.
//
// THREE CONTRACTS, in the order they must survive a refactor:
//
// (1) FAIL OPEN. A gate that cannot measure must never pause. Every failure path
//     — PDH refusing the counter set, an instance name that no longer parses, a
//     process we may not open, a toolhelp snapshot that fails — logs through
//     LogErr and leaves the verdict kClear. Ok() says whether the gate is
//     measuring at all, and the caller must keep generating when it is false.
//     Callers must also treat !Ok() as kClear: when the gate gives up mid-run it
//     publishes a clear verdict first, but a caller that stops reading Status()
//     the moment Ok() turns false would otherwise hold a stale "paused" forever.
//     Stopping a video the user is watching because our own instrument broke is
//     the worse of the two errors (I2).
//
// (2) THE SAMPLER THREAD NEVER TOUCHES A FOREIGN MESSAGE LOOP (I15). It creates
//     no window and runs no modal loop, and it must never call GetWindowTextW,
//     nsp::DescribeWindow (which calls GetWindowTextW at nsp_common.cpp:291),
//     SendMessage, CreateWindow, TrackPopupMenu, DialogBox or MessageBox. Those
//     SEND to the window's owning thread, so a hung game would block the sampler
//     for the whole SendMessageTimeout — seconds, during which the gate is blind.
//     Classification uses only GetForegroundWindow, GetClassNameW, GetWindowRect,
//     GetWindowLongW, IsIconic, DwmGetWindowAttribute and GetClipCursor, none of
//     which cross into another process's message loop. A grep in the verification
//     steps enforces this; do not weaken it for a nicer window description.
//
// (3) LOAD IS MEASURED PER PROCESS TREE, NEVER DEVICE-WIDE (N14). The historical
//     failure mode is gating on total GPU utilisation: our own overlay forces the
//     browser's GPU process and dwm.exe to composite at the refresh rate, so a
//     device-wide threshold crosses as a direct consequence of engaging and then
//     oscillates. Here the only tree ever summed is the CANDIDATE's, and the
//     capture target's whole tree is excluded by construction (SetTargetPid), so
//     the browser and DWM are structurally unreachable by the measurement.
#pragma once

#include "nsp_common.h"

#include <cstdint>
#include <memory>
#include <string>

namespace nsp {

enum class GateVerdict { kClear, kPaused };

// Which stage-1 test named the candidate. Reported so a log or the probe can say
// WHY something was treated as a game, and so a beta note can be honest about
// which signals were actually exercised.
enum class GameSignal { kNone, kFullscreenForeground, kD3DExclusive, kKnownGame, kCursorClip };

struct GameGateStatus {
    GateVerdict  verdict = GateVerdict::kClear;
    GameSignal   signal = GameSignal::kNone;
    DWORD        pid = 0;        // the armed candidate's root process, 0 when nothing is armed
    std::wstring exe;            // its lower-case image name, empty when we may not open it
    double       utilPct = 0.0;  // max over the candidate tree's 3d/compute engines, this sample
    double       pausedSec = 0.0;// wall seconds since the kPaused transition, 0 when clear
    uint64_t     samples = 0;    // completed samples since Start(), the priming collect excluded
    bool         ok = false;     // same as GameGate::Ok() at the moment of the sample
    // PDH collect + format cost over a rolling window of the last 256 samples, in
    // milliseconds. Published so the probe mode can settle "is PDH cheap enough"
    // with a measurement instead of an inherited estimate.
    double       collectMsP50 = 0.0;
    double       collectMsP99 = 0.0;
};

// Short ASCII name for a log line ("fullscreen", "d3d-exclusive", …). Log lines
// stay English and do not go through nsp_ui_text.
const char* GameSignalName(GameSignal s);

class GameGate {
public:
    // Both are out of line on purpose: `Impl` is incomplete here, and a defaulted
    // constructor in the class body would need its destructor to build the
    // unique_ptr's exception cleanup. Same reason Tray declares its own.
    GameGate();
    ~GameGate();  // calls Stop()

    GameGate(const GameGate&) = delete;
    GameGate& operator=(const GameGate&) = delete;

    // Opens the PDH query and starts the sampler thread. Returns false and fills
    // `err` when PDH refuses the counter set; the caller must carry on WITHOUT a
    // gate in that case, never by pausing (contract 1).
    bool Start(std::string* err);

    // Signals the sampler thread and joins it. Safe to call more than once.
    void Stop();

    // False when the gate is not measuring — never started, PDH refused, or it
    // gave up on unparseable instance names mid-run. The caller must FAIL OPEN.
    bool Ok() const;

    // Our render device's adapter. Instances belonging to any other LUID are
    // ignored: this machine also exposes a second render device whose engines
    // would otherwise be counted. {0,0} (the default) accepts any adapter.
    // Any thread; takes effect on the next sample.
    void SetAdapterLuid(LUID luid);

    // The captured browser. Its WHOLE process tree is exempt from being a game
    // candidate and from the load measurement — that exemption is contract 3.
    // 0 means "nothing captured", which is also the right value while disengaged.
    // Any thread.
    void SetTargetPid(DWORD pid);

    // Wakes the sampler for an out-of-band sample, so a foreground change is
    // classified within milliseconds instead of at the next 200 ms tick. Any
    // thread; cheap enough to call on every EVENT_SYSTEM_FOREGROUND.
    void NudgeForeground();

    // Engine thread. One mutex-guarded copy of the last published sample.
    GameGateStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
