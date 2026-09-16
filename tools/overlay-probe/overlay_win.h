// overlay_win.h — the overlay HWND + DirectComposition visual + swapchain.
//
// Design constraints this MUST honour (they are project invariants, not taste):
//   I9  the window always carries WS_EX_TRANSPARENT | WS_POPUP; losing either is
//       a release blocker (Chromium's occlusion tracker throttles the tab otherwise).
//       Options::styleTransparent == false is the deliberate negative control.
//   I11 exactly one HWND, created once, NEVER moved or resized after creation.
//       All geometry is expressed as DComp visual SetOffsetX/Y + SetClip and
//       committed in a single IDCompositionDevice::Commit().
//   D13 engage-after-validate: the overlay starts hidden.
#pragma once

#include "probe_common.h"

#include <memory>
#include <string>

namespace np {

class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Creates the window (initially hidden) covering `hwndRectScreen`, the DComp
    // device/target/visual and a composition swapchain of that size.
    // Returns false and fills `err` on failure.
    bool Create(const Options& opt, HWND target, const RECT& hwndRectScreen, std::string* err);

    // Moves/clips the VISUAL only (never the HWND). Coordinates are screen-space
    // physical pixels; the implementation converts to visual-local space.
    // The OVERLAY owns Options::insetPx: `screenRect` is the un-inset rect and
    // the inset is applied here, exactly once. Callers must NOT pre-shrink it.
    bool SetVisualRect(const RECT& screenRect);

    // The rect the last successful SetVisualRect actually committed, in
    // screen-space physical pixels and with the inset already applied, so a
    // report can state what was on screen instead of what was requested.
    // All zeroes until the first successful SetVisualRect.
    const RECT& CommittedVisualRect() const;

    void Show();
    void Hide();
    bool IsShown() const;

    // Renders one frame of the test pattern and presents it. `frameIndex` drives
    // the pattern's animation so a captured stream can be checked for staleness.
    // Fills `out` with this present's statistics. Returns false on device loss.
    bool RenderAndPresent(uint32_t frameIndex, PresentSample* out);

    // Multi-line human-readable record of what creation actually produced:
    // final window styles read back with GetWindowLong, swapchain description,
    // whether IDXGISwapChainMedia was available, the output the window landed on
    // and that output's CheckHardwareCompositionSupport flags.
    const std::string& CreationReport() const;

    // Multi-line record of what the RUN did, as opposed to what creation
    // produced: presents that succeeded / failed / were skipped, and the
    // frame-latency wait statistics (how often the wait was not signalled and
    // the worst wait observed). Assembled on demand, so it may be read at any
    // point during or after the run.
    const std::string& RunReport() const;

    HWND Hwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace np
