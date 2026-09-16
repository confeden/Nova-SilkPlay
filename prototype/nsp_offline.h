// nsp_offline.h — the quality instrument: frames in from disk, frames out to
// disk, through the SAME Synth object the live path uses.
//
// Why this exists at all, in one paragraph. Quality cannot be measured through
// the live capture path: the two frames the engine holds are not guaranteed to
// be adjacent source frames (the prototype's own logs show 78-182 ms arrival
// gaps against a 42 ms median, i.e. skipped frames), the phase t is estimated
// rather than known, and Chrome changes its pixel format underneath the run
// (G9/G17). Every one of those turns into an error attributed to the
// synthesizer. Offline replaces the estimated pair with an array index and the
// estimated phase with a number we chose, so the only variable left is the code
// under test — and run-to-run identity becomes a hash comparison instead of a
// tolerance.
//
// What this deliberately does NOT touch: Capture, Overlay, DirectComposition,
// the hotkeys, the cadence estimator, the watermark. Those are measured by the
// live instrument, and no number crosses between the two.
#pragma once

#include "nsp_common.h"

#include <string>
#include <vector>

namespace nsp {

struct OfflineOptions {
    std::vector<std::string> frames;  // ordered source frames; pairs are (i, i+1)
    std::string outDir;               // synthesized PNGs go here; empty = none
    std::string mode = "mc";          // mc | blend | passthrough
    std::vector<double> ts{0.5};      // phases synthesized for every pair

    bool useOfa = false;
    int  cellPx = 8;
    int  ofaGrid = 4;
    int  occMode = 2;        // Synth::SetOcclusionMode: 0 self, 1 bidir, 2 bidir+cand
    bool ofaHints = true;    // seed NVOFA's search with our own coarse field (G30)
    int  warpLab = 0;        // Synth::SetWarpLab: branch maps and ablations (P21)
    float warpLabParam = 0.05f;

    int  repeat = 1;         // run the whole sequence N times and compare hashes
    bool checkEndpoints = false;  // assert mc(t=0)==A and mc(t=1)==B bit-exactly
    // Estimate motion for the first pair once and throw the result away before
    // the measured run starts. MEASURED on GB206: the FIRST nvOFExecute of a
    // session returns a different field from every later one on the same input,
    // so without this the first pair of every run is scored on a field no other
    // pair is scored on.
    bool warmup = true;

    // THE ORACLE-FLOW ARM. A directory of raw little-endian float32 fields, one
    // per (pair, phase), named pair%04zu_t%03d.f32 to match the output PNGs —
    // gridH*gridW*2 floats, row-major, x then y, in full-resolution pixels of
    // A->B displacement over one source interval.
    //
    // When set, the engine's own estimate is REPLACED by the file for that pair
    // and phase, so the run measures the warp with a perfect field. That splits
    // the score into motion-estimation error and the architecture's resampling
    // ceiling, which `kb/quality-harness.md` names as the thing the harness was
    // still missing. A missing file is a hard failure, never a silent fallback
    // to the estimator: an oracle arm that quietly measured the estimator would
    // poison every comparison it appears in, exactly as a silent NVOFA fallback
    // would. Since the injected field is phase-specific, it is uploaded per t
    // rather than per pair — that breaks I6's once-per-pair rule on purpose,
    // and only here, because this is an instrument and not a shipping path.
    std::string injectDir;
};

// Returns 0 on success, non-zero on a setup failure or a failed assertion.
int RunOffline(const OfflineOptions& opt);

}  // namespace nsp
