// nsp_drm.h — recognise DRM-protected video so the engine SKIPS it (ROADMAP P11, I7/D30).
//
// The module answers exactly one question about the browser we are capturing: is the
// video it is showing protected? It NEVER captures, decodes, defeats or works around
// protection. It reads process metadata — the target's direct children and their
// command lines — and, optionally, one scalar summary of a picture the ENGINE already
// captured for its own use. Nothing here ever touches a protected surface, and there is
// no code path in this file that could be turned into one: I7/D30 is that protected
// video is recognised and skipped, never circumvented.
//
// Two independent signals, either of which is enough:
//
//   (a) A Content Decryption Module process in the target's tree. Chrome runs the CDM
//       out of process, and the utility process names itself on its own command line:
//         --utility-sub-type=media.mojom.MediaFoundationService[Broker]
//                                         -> HARDWARE DRM (Widevine L1 / PlayReady
//                                            SL3000). Capture over such a video is
//                                            expected to come back black.
//         --utility-sub-type=media.mojom.CdmService[Broker], or a `widevinecdm` path
//                                         -> the library CDM (Widevine L3). Capture
//                                            returns REAL PIXELS, so this process is
//                                            the ONLY thing between the engine and
//                                            generating over protected content.
//       The two are reported apart (kHardwareCdm / kLibraryCdm) because they mean very
//       different things for signal (b): with a library CDM only, the picture half has
//       zero coverage and the process scan is the entire gate.
//
//   (b) A dead video rect. A hardware CDM's protected surface reads back as black
//       through Windows.Graphics.Capture. The engine already summarises the picture it
//       captured; it hands that summary in through OnPictureSummary, and a rect that
//       stays effectively black for kDeadSec is the hardware-DRM signature.
//       `unverified` — see the constants in nsp_drm.cpp. The owner has no subscription
//       to protected content on this machine (stated 2026-09-20), so NOTHING about this
//       signal has been measured here; it is reasoning, not a measurement.
//
// THREE CONTRACTS, in the order they must survive a refactor:
//
// (1) FAIL CLOSED — THE EXACT OPPOSITE OF THE GAME GATE (nsp_gamegate.h contract 1).
//     If this module cannot tell, the verdict is kUnknown, and THE CALLER MUST TREAT
//     kUnknown EXACTLY LIKE kProtected AND NOT GENERATE. Same for DrmStatus::ok being
//     false, which means the scanner is not measuring at all: that is a fail-closed
//     state too, and it must be checked FIRST because a default-constructed DrmStatus
//     carries verdict == kClear.
//     Operationally: kClear is published ONLY by a scan that completed, found the
//     target, walked every one of its browser children and read every one of their
//     command lines. A snapshot that failed, a target pid we could not find, a child we
//     could not open or whose command line we could not read, a `media.mojom.*` utility
//     subtype we do not recognise, or no target set at all — every one of those is
//     kUnknown. Generating over protected content is the failure this module exists to
//     prevent; refusing to generate over a video we were unsure about costs the user
//     smoothness on one page, and that is the cheaper of the two mistakes.
//
// (2) THE SCAN IS SLOW AND BELONGS OFF THE ENGINE THREAD. A cold full scan of a
//     29-child Chrome MEASURED 11 ms (2026-09-20, tools/drm-probe) — nearly two whole
//     frames at 165 Hz. So it runs on a thread of its own at 1 Hz while a target is set
//     and sleeps on an event otherwise, and the engine only ever reads one
//     mutex-guarded snapshot through Status(). OnPictureSummary() is the one call the
//     engine makes into this module per frame, and it does nothing but store three
//     atomics.
//
// (3) RECOVERY, WITH A DWELL. When the CDM process goes away the verdict returns to
//     kClear only after kCdmGoneDwellSec (nsp_drm.cpp; the probe's README calls it that
//     and the design assumes 10 s). Chrome keeps a CDM utility process alive for a
//     while after the user navigates away, and it also tears one down and builds
//     another between two protected items — a verdict that tracked the process exactly
//     would open a generation window inside that gap.
#pragma once

#include "nsp_common.h"

#include <cstdint>
#include <memory>
#include <string>

namespace nsp {

// kUnknown is NOT a third, softer answer: the caller treats it as kProtected
// (contract 1). It exists so a log line can say we did not know, rather than
// claiming we found protection we did not find.
enum class DrmVerdict { kClear, kProtected, kUnknown };

// Which signal produced the verdict. Reported so a log, or the beta notes, can be
// honest about which half of the design was actually exercised on this machine.
enum class DrmSignal { kNone, kHardwareCdm, kLibraryCdm, kDeadPicture };

struct DrmStatus {
    DrmVerdict verdict = DrmVerdict::kClear;
    DrmSignal  signal  = DrmSignal::kNone;
    // The CDM utility process, as last seen. Kept during the kCdmGoneDwellSec hold,
    // where the process is already gone, so a log line can still say where the verdict
    // came from; 0 once the hold expires or when the signal is kDeadPicture.
    DWORD      cdmPid  = 0;
    int        unreadableChildren = 0;   // children whose command line we could not read
    double     darkFrac = 0.0;           // the last picture summary the caller fed in
    uint64_t   scans = 0;
    bool       ok = false;               // false: the scanner is not measuring - FAIL CLOSED here
};

// Short ASCII name for a log line ("hardware-cdm", "library-cdm", …). Log lines stay
// English and do not go through nsp_ui_text.
const char* DrmSignalName(DrmSignal s);

class DrmWatch {
public:
    // Both out of line: `Impl` is incomplete here, so the unique_ptr's cleanup needs a
    // visible destructor. Same reason GameGate declares its own.
    DrmWatch();
    ~DrmWatch();  // calls Stop()

    DrmWatch(const DrmWatch&) = delete;
    DrmWatch& operator=(const DrmWatch&) = delete;

    // Starts the scanner thread and publishes an immediate kUnknown, so a caller that
    // reads Status() before the first scan lands cannot see a clear verdict it did not
    // earn. Returns false and fills `err` when ntdll's NtQueryInformationProcess cannot
    // be resolved or a thread/event cannot be created; the caller must then treat every
    // target as protected — NOT carry on generating (contract 1).
    bool Start(std::string* err);

    // Signals the scanner thread and joins it. Safe to call more than once. Afterwards
    // Status() reports ok == false, which is fail-closed.
    void Stop();

    // The captured browser's process — for Chrome that is the BROWSER process, the one
    // that owns the window, which is what GetWindowThreadProcessId on the target HWND
    // returns. 0 = nothing is being captured; the scanner then idles on its event and
    // publishes kUnknown, because an idle scanner has not scanned anything (contract 1).
    // Changing the target publishes kUnknown at once and wakes the scanner, so the stale
    // verdict of the PREVIOUS target can never be read as this one's.
    // Engine thread — see OnPictureSummary.
    void SetTargetPid(DWORD pid);

    // Called by the engine once it has a cheap summary of the captured video rect:
    // `darkFrac` is the fraction of sampled blocks below the dark threshold, 0..1.
    //
    // "Block" and "dark" mean what tools/drm-probe/drm_probe.py `band` means by them,
    // and the engine's summary must agree with it or the constants are measuring
    // something else: 16 px blocks tiling the middle band x 0.06..0.94, y 0.20..0.72 of
    // the video rect (chosen so no player UI — progress bar, captions, title card —
    // reaches it), a block being dark when the largest channel of every pixel sampled in
    // it is <= 16/255. The probe prints the BRIGHT fraction, which is 1 - darkFrac.
    //
    // Engine thread, once per captured frame at most. It stores three atomics and
    // returns; it takes no lock and makes no system call. It must be called from the
    // SAME thread as SetTargetPid — that is what makes the read-modify-write of the
    // dwell timestamp safe without one.
    void OnPictureSummary(double darkFrac);

    // Engine thread. One mutex-guarded copy of the last published scan.
    DrmStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
