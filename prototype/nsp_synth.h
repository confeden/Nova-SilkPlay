// nsp_synth.h — frame synthesis: what actually goes on the overlay for a phase
// t in (0,1) between two source frames.
//
// The split matters more than any individual shader: motion is estimated ONCE
// per source pair (PrepareMotion), and only Synthesize runs per generated frame
// (I6). At 24 fps into 165 Hz that is 24 estimates and 165 syntheses per second,
// which is why the cost scales with the output rate only through the cheap half.
//
// v0/v1 estimate motion with a pyramid block matcher written here. The engine's
// v1.0 answer is NVOFA hardware optical flow (D11) — it replaces PrepareMotion's
// internals and nothing else: the motion field's format and the warp are the
// same either way.
#pragma once

#include "nsp_common.h"
#include "nsp_ofa.h"

#include <d3d11_1.h>

#include <memory>
#include <string>

namespace nsp {

class Synth {
public:
    Synth();
    ~Synth();
    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;

    bool Create(ID3D11Device* device, ID3D11DeviceContext* ctx, std::string* err);

    // Allocates the luma pyramid and the motion field for this source size.
    // `cellPx` is the motion field's cell size in full-res pixels (4..64).
    bool Resize(UINT w, UINT h, std::string* err, UINT cellPx = 8);

    // Switches the motion source to the hardware engine. `gridSize` must equal
    // the cell size Resize() was given, so the field lands on the same grid the
    // warp already samples. Returns false (and leaves the block matcher in
    // place) when the hardware path cannot be brought up.
    // `seedHints` additionally turns on NVOFA's EXTERNAL HINT input and feeds
    // it our own coarse field every pair. The hardware has no pyramid of ours to
    // refine in, so it carries G30's period lock in full (A4: 18.90 dB against
    // the block matcher's 43.74); seeding is the only route to the fix that does
    // not require changing the hardware's internals. A zero hint buffer is worse
    // than no hints (G32), so this is all-or-nothing per pair.
    //
    // ON by default: the seed is computed on its own COARSE grid (16 px cells,
    // 16x fewer than the hardware's 4 px grid), which put the cost under the
    // measurement floor -- hints-on and hints-off time identically at 1080p --
    // while the quality is bit-identical to seeding at the fine grid. A search
    // seed does not need per-4-px detail, and paying for it cost 11.2 ms/pair.
    bool EnableOfa(UINT gridSize, std::string* err, bool seedHints = true);
    bool OfaActive() const;
    const std::string& OfaReport() const;

    // What the warp does with the two independently anchored fields — one on A,
    // one on B — that the hardware's BOTH-direction execute already pays for.
    // Three settings, because they are two separate claims and this project
    // measures them separately:
    //   0 self       the intermediate field sampled twice. It can say "this
    //                trajectory is inconsistent" but never WHICH frame lost
    //                sight of the pixel, because both answers come from one
    //                estimate. This is what the engine shipped with.
    //   1 bidir      the occlusion test reads the A-anchored field at the A
    //                fetch site and the B-anchored one at the B fetch site, so
    //                the two answers are independent evidence.
    //   2 bidir+cand as 1, and both fields are also offered to the per-pixel
    //                vector search as candidates. Where an object has just
    //                uncovered background, every neighbouring intermediate cell
    //                carries the object's vector and only the anchored fields
    //                still hold the background's.
    // Values above 2 clamp to 2; the hardware path is required for anything but 0.
    void SetOcclusionMode(int mode);
    // The mode actually in force for the pair just prepared: 0 whenever the
    // fields do not exist, whatever was asked for.
    int OcclusionModeActive() const;

    // The warp laboratory (P21): 0 = the shipping PSWarp; any other value switches
    // to PSWarpLab in that mode - a branch map or a one-at-a-time ablation, listed
    // in the shader. 99 runs the lab copy unchanged and must match PSWarp bit for
    // bit. Compiles the lab shader on first use.
    bool SetWarpLab(int mode, std::string* err);
    void SetWarpLabParam(float p);  // the lab's free parameter (a softmin sigma, ...)

    // Estimates the motion field between two source frames. Call once per new
    // source frame, not per generated frame.
    bool PrepareMotion(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b);

    // THE ORACLE-FLOW REFERENCE. Replaces the estimated field with one supplied
    // from outside — in practice the analytic corpus's TRUE field — so that a
    // run measures the warp alone.
    //
    // This is the instrument that splits one number into two. Scoring the
    // engine against ground truth conflates motion-estimation error with the
    // architecture's own resampling ceiling, and those two have completely
    // different fixes. engine-vs-oracle is the estimator's error; oracle-vs-
    // truth is the ceiling, which is a property of the cell grid and the warp
    // and is not something tuning can move.
    //
    // `xy` is gw*gh*2 floats, row-major, x then y, in FULL-RESOLUTION PIXELS of
    // A->B displacement over one whole source interval, anchored on the
    // intermediate frame — the same thing the warp's own field means. gw/gh
    // must equal the grid Resize() produced.
    //
    // Deliberately NOT supplied: the A- and B-anchored occlusion fields. An
    // oracle that filled those would be measuring a different engine from the
    // one that ships, so the occlusion mode drops to `self` for an injected
    // pair and OcclusionModeActive() reports 0. Call this INSTEAD of
    // PrepareMotion, and once per phase if the injected field is phase-specific.
    bool InjectField(const float* xy, UINT gw, UINT gh, std::string* err);

    // Cross-fade a -> b at phase t, in linear light. No motion: ghosts on motion,
    // and is the reference the motion-compensated path has to beat.
    bool Blend(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11ShaderResourceView* a,
               ID3D11ShaderResourceView* b, float t);

    // Motion-compensated synthesis at phase t, using the field from the last
    // PrepareMotion — or, with `held`, the field HoldFields() kept from the pair
    // before it. Falls back to Blend when that field does not exist.
    bool Warp(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11ShaderResourceView* a,
              ID3D11ShaderResourceView* b, float t, bool held = false);

    // Keeps a copy of the current fields (the pair PrepareMotion last estimated)
    // so the warp can still draw that pair after the next PrepareMotion. Call it
    // right before PrepareMotion for a new pair. Three grid-sized copies.
    void HoldFields();
    // Forgets the held fields, e.g. when more than one source frame arrived at
    // once and the held pair no longer precedes the current one.
    void DropHeldFields();
    bool HasHeldFields() const;

    bool HasMotion() const;
    // Block grid of the motion field and its cell size, for logging.
    void MotionGrid(UINT* gw, UINT* gh, UINT* cellPx = nullptr) const;

    // One-shot, blocking: percentiles of |v| over the current field, in pixels.
    // For diagnosing a motion source against known ground truth.
    bool DebugFieldStats(float* p50, float* p95, float* maxAbs, float* zeroFrac);

    // The frame-rate readout, the way Lossless Scaling shows its own: plain text
    // "<source fps>/<output fps>", white with a one-pixel dark outline so it
    // reads over any picture, at a FIXED place — the top-right corner of the video
    // rect (owner, 2026-09-13). No chip, no hover, nothing that moves.
    // Draws `text` with its top-right corner at (right, top) in render-target
    // pixels; characters outside 0-9 and '/' are skipped.
    bool DrawFpsText(ID3D11RenderTargetView* rtv, UINT w, UINT h, float right, float top, int scale,
                     const std::string& text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
