// wgc_capture.h — Windows.Graphics.Capture of the target window, with per-frame
// statistics good enough to answer "are these real pixels or a black/frozen frame".
//
// Why this exists: S-M2's second half. If WGC returns black (or a frozen frame)
// while Chrome's video sits on an MPO plane, the whole browser path dies and no
// amount of overlay tuning saves it. Related known hazards:
//   G4  GraphicsCaptureSession::MinUpdateInterval defaults to 0 and anything
//       below 1000 us caps WGC at ~50 fps on build 26100 — set it explicitly.
//   G14 IsBorderRequired = false needs the borderless capability, whose policy
//       allowlist takes Package Family Names only; the setter succeeds and is
//       silently ignored without consent. Record what actually happened.
//
// Uses the FREE-THREADED frame pool and polling (TryGetNextFrame) so the probe
// needs no DispatcherQueue and no message-pump reentrancy.
#pragma once

#include "probe_common.h"

#include <memory>
#include <string>
#include <vector>

namespace np {

class WgcCapture {
public:
    WgcCapture();
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    // Creates its OWN D3D11 device (the capture seam is deliberately separate
    // from the presentation device — see directives §4 "device topology hazard").
    bool Start(HWND target, const Options& opt, std::string* err);

    // Drains every frame that arrived since the last call, appends one
    // CaptureSample per frame to `out`, and returns how many were appended.
    // Never blocks. `phaseIndex` is stamped into each sample.
    size_t Poll(uint32_t phaseIndex, std::vector<CaptureSample>* out);

    void Stop();

    // Frames that were pulled out of the pool but for which NO CaptureSample
    // could be produced (surface access, staging allocation, Map or statistics
    // failed). Those frames really arrived, so a report that only counts samples
    // silently understates the capture rate. Cumulative since Start(); read it
    // BEFORE Stop(), which resets the session state.
    size_t FramesWithoutPixelStats() const;

    // What the session actually accepted: capability request result, the
    // IsBorderRequired / IsCursorCaptureEnabled / MinUpdateInterval setter
    // HRESULTs, the item's initial size and the pixel format in use.
    const std::string& StartupReport() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace np
