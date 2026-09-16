// nsp_capture.h — Windows.Graphics.Capture of the target window, cropped to the
// video rect and kept ON THE GPU.
//
// The probe's wgc_capture.cpp reads frames back to the CPU for statistics; this
// one never does. It keeps a four-slot ring of video-rect-sized textures — the
// newest source frame and the one before it — which is exactly what a frame
// interpolator consumes (I6: motion once per source pair, synthesis per t).
//
// Known hazards carried over: G4 MinUpdateInterval must be set explicitly or WGC
// caps near 50 fps; G14 IsBorderRequired needs the Borderless capability (which
// this machine grants unpackaged — measured, see the startup report); G17 any
// WGC session costs Chrome its hardware overlay plane and latches BGRA8.
#pragma once

#include "nsp_common.h"

#include <d3d11_1.h>

#include <memory>
#include <string>

namespace nsp {

struct CaptureOptions {
    double minUpdateIntervalMs = 1.0;  // G4
    bool   tryBorderless = true;       // G14
    bool   captureCursor = false;      // I12: never, the OS cursor is the only cursor
    bool   videoFrameTest = true;      // false: every capture is a video frame (the old behaviour, for A/B)
    bool   logDirty = false;           // one log line per capture: damage, content test, verdict
};

// What the video-frame test did since the last TakeStats().
struct CaptureStats {
    uint64_t uiRefreshes = 0;    // captures that were the same video frame with newer UI
    uint64_t contentTests = 0;   // GPU middle-band comparisons run
    double   testWaitMsSum = 0;  // time blocked reading their answers back
    double   testWaitMsMax = 0;
};

class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // `device`/`ctx` are the presentation device: one device, no cross-device seam.
    // `srcRect` is in CAPTURE space (origin = DWM extended frame bounds) and fixes
    // the ring texture size.
    bool Start(ID3D11Device* device, ID3D11DeviceContext* ctx, HWND target, const RECT& srcRect,
               const CaptureOptions& opt, std::string* err);
    void Stop();

    // Drains every frame that arrived since the last call. A capture that carries
    // a new VIDEO frame advances the pair; one that only changed player UI
    // (controls fading, a clock ticking) replaces the newest slot in place without
    // moving time - see "WHICH CAPTURED FRAMES ARE VIDEO FRAMES" in the .cpp.
    // Returns how many video frames arrived. The content test reads one integer
    // back from the GPU per ambiguous capture; the wait is in TakeStats().
    int Poll();
    CaptureStats TakeStats();

    // Same size as the current source rect; only the offset moves. Used by the
    // alignment nudge keys.
    bool SetSourceOffset(LONG x, LONG y);
    const RECT& SourceRect() const;

    // The two most recent source frames. Newest()==nullptr until the first frame
    // lands; Previous()==nullptr until the second. Borrowed pointers, valid until
    // the next Poll() that ingests a frame.
    ID3D11ShaderResourceView* NewestSrv() const;
    ID3D11ShaderResourceView* PreviousSrv() const;
    ID3D11Texture2D*          NewestTex() const;
    ID3D11Texture2D*          PreviousTex() const;

    // SystemRelativeTime of those frames, in QPC-based 100 ns units.
    double NewestTime100ns() const;
    double PreviousTime100ns() const;

    // The video frame before Previous(): nullptr/0 until three have arrived.
    ID3D11ShaderResourceView* OlderSrv() const;
    double OlderTime100ns() const;

    // Qpc100nsNow() at the last capture that repainted the picture itself — a
    // new video frame OR the same one recomposited (a still shot keeps arriving
    // and is not "stalled"). 0 until the first. Small UI damage does not count.
    double LastPictureActivity100ns() const;

    uint64_t FramesIngested() const;
    // The capture surface changed size (window resized) or the item closed: the
    // caller must re-engage. Sticky until Stop().
    bool NeedsReengage() const;

    const std::string& StartupReport() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
