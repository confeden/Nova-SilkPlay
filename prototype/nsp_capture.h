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
#include "nsp_rectfind.h"

#include <d3d11_1.h>

#include <memory>
#include <string>
#include <vector>

namespace nsp {

// One capture's damage as DWM reported it, in WGC surface coordinates, stamped with the
// capture's SystemRelativeTime in seconds (the QPC clock, as QpcToSec).
struct TimedDamage {
    double tSec = 0.0;
    DamageSig d;
};

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

// Engine-thread lifetime, shared by scouting and capture across re-engagements.
// WGC broker allocations persist per CreateForWindow / access request on this OS.
class CaptureRuntime {
public:
    CaptureRuntime();
    ~CaptureRuntime();
    CaptureRuntime(const CaptureRuntime&) = delete;
    CaptureRuntime& operator=(const CaptureRuntime&) = delete;
private:
    friend class Capture;
    friend class DamageScout;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Capture {
public:
    explicit Capture(std::shared_ptr<CaptureRuntime> runtime = {});
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

    // Every capture's damage since the last call, oldest first (P11: the geometry guard of a
    // windowed video). Empty on a build without dirty regions. Capped: a caller that stops
    // draining loses the oldest, never blocks the capture.
    void TakeDamage(std::vector<TimedDamage>* out);
    // Off by default: the fullscreen path never reads it, and an undrained queue would cost a
    // shift of 256 entries per capture.
    void EnableDamage(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// SCOUTING a windowed browser for its video (P11): a capture session that copies nothing
// and reads only where DWM says the window changed. Its own small device, so it costs no
// overlay, no synthesis and no ring - only what WGC itself does per frame. Borderless and
// cursorless like the real capture: scouting must not show either (D22, I12).
class DamageScout {
public:
    explicit DamageScout(std::shared_ptr<CaptureRuntime> runtime = {});
    ~DamageScout();
    DamageScout(const DamageScout&) = delete;
    DamageScout& operator=(const DamageScout&) = delete;

    bool Start(HWND target, std::string* err);
    void Stop();
    // Drains the frames that arrived: their damage, nothing else. false once the window
    // closed or resized (the session is then useless; Stop and start again).
    bool Poll(std::vector<TimedDamage>* out);
    HWND Target() const;
    LONG SurfaceW() const;
    LONG SurfaceH() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
