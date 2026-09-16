// nsp_synth.cpp — the prototype synthesizer: pyramid block matching + warp.
//
// Shaders are compiled at runtime with D3DCompile (d3dcompiler_47.dll is in-box
// on Windows 11), so there is no fxc step and no .cso to keep in sync. The cost
// is a few milliseconds once, at engage.
//
// The matcher is SYMMETRIC: for a cell centre c it searches the vector v that
// makes A(c - v/2) match B(c + v/2). That gives the motion through the midpoint
// directly, which is what an interpolator needs — a forward field A->B would
// have to be projected onto the intermediate frame first, and the projection is
// where naive implementations produce their worst holes.

#include "nsp_synth.h"

#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nsp {
namespace {

// Full-resolution pixels per motion-field cell, unless Resize() is told
// otherwise. Smaller cells mean a narrower halo around a moving object and a
// matcher cost that grows with the square: 8 px is 4x the cells of 16 px.
constexpr UINT kDefaultCellPx = 8;
// Pyramid: level 0 is w/2, level 1 is w/4, level 2 is w/8.
constexpr int kPyramidLevels = 3;

constexpr char kShaderSource[] = R"HLSL(
// ---------------------------------------------------------------- resources
Texture2D<float4>   texA   : register(t0);
Texture2D<float4>   texB   : register(t1);
Texture2D<float2>   mvTex  : register(t2);
// The same motion, anchored on each source frame instead of on the intermediate
// one. mvATex says how the pixel that is at a given place in A moves; mvBTex
// says the same for B, already flipped into the A->B convention so the two are
// directly comparable with the vector the warp chose.
Texture2D<float2>   mvATex : register(t7);
Texture2D<float2>   mvBTex : register(t8);
SamplerState        smpPt  : register(s0);
SamplerState        smpLin : register(s1);

cbuffer Params : register(b0) {
    float2 gInvSize;    // 1 / source size, in pixels
    float  gT;          // phase in [0,1]
    float  gWarpCell;   // motion cell size in pixels, for the candidate offsets
    float  gBidir;      // 0 off, 1 anchored fields for occlusion, 2 also as candidates
    float3 gParamsPad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

// ------------------------------------------------------------- cross-fade
float4 PSBlend(VSOut i) : SV_Target {
    // Both SRVs are _SRGB views, so these samples are already linear light.
    float3 a = texA.SampleLevel(smpPt, i.uv, 0).rgb;
    float3 b = texB.SampleLevel(smpPt, i.uv, 0).rgb;
    return float4(lerp(a, b, gT), 1.0);
}

// --------------------------------------------------------- motion-compensated
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Cost of a candidate vector at this pixel: how well the two source frames
// agree when fetched with it. Three horizontal taps, because a vertical edge
// moving horizontally is the case a single centre tap cannot tell apart.
float CandCost(float2 vpx, float2 uv) {
    float2 duv = vpx * gInvSize;
    float c = 0.0;
    [unroll] for (int k = -1; k <= 1; ++k) {
        float2 o = float2(float(k) * 2.0 * gInvSize.x, 0.0);
        float3 a = texA.SampleLevel(smpLin, uv + o - duv * gT, 0).rgb;
        float3 b = texB.SampleLevel(smpLin, uv + o + duv * (1.0 - gT), 0).rgb;
        float3 e = abs(a - b);
        c += e.r + e.g + e.b;
    }
    return c;
}

float4 PSWarp(VSOut i) : SV_Target {
    // PER-PIXEL VECTOR CHOICE.
    //
    // A block field is smooth by construction, so at an object boundary the
    // bilinear value is a blend of two different motions and belongs to neither
    // — which is exactly where the torn edges and the bitten-out chunks come
    // from. Rather than trust it, take it as one CANDIDATE among the vectors of
    // the four neighbouring cells (plus zero, for a background that is not
    // moving) and let each pixel keep whichever one actually makes the two
    // source frames agree there. Boundaries then land on a whole vector instead
    // of an average of two, at the cost of a few texture samples.
    float2 vBil = mvTex.SampleLevel(smpLin, i.uv, 0);
    float2 cellUv = gWarpCell * gInvSize;

    float2 vBest = vBil;
    // A small bias in favour of the smooth field: on flat content every
    // candidate matches equally well and the field is the better answer.
    float  cBest = CandCost(vBil, i.uv) * 0.94;

    [unroll] for (int n = 0; n < 4; ++n) {
        float2 off = float2((n & 1) ? 1.0 : -1.0, (n & 2) ? 1.0 : -1.0) * cellUv;
        float2 cand = mvTex.SampleLevel(smpPt, i.uv + off, 0);
        float  c = CandCost(cand, i.uv) + 0.004 * length(cand - vBil);
        if (c < cBest) { cBest = c; vBest = cand; }
    }
    {
        float c = CandCost(float2(0.0, 0.0), i.uv) + 0.004 * length(vBil);
        if (c < cBest) { cBest = c; vBest = float2(0.0, 0.0); }
    }
    // The two anchored fields, read at THIS pixel, are candidates the
    // intermediate field cannot offer. Where an object has just uncovered the
    // background, every neighbouring intermediate cell carries the object's
    // vector — the object swept through here — while the A-anchored field still
    // holds the background's, because in A this pixel IS background. The
    // covering case is the mirror of it in B. Those are exactly the pixels the
    // occlusion test is arbitrating over, and without this it is arbitrating
    // between two fetches that are both made with the wrong vector.
    if (gBidir > 1.5) {
        [unroll] for (int s = 0; s < 2; ++s) {
            float2 cand = (s == 0) ? mvATex.SampleLevel(smpLin, i.uv, 0)
                                   : mvBTex.SampleLevel(smpLin, i.uv, 0);
            float c = CandCost(cand, i.uv) + 0.004 * length(cand - vBil);
            if (c < cBest) { cBest = c; vBest = cand; }
        }
    }

    float2 v = vBest * gInvSize;
    float2 uvA = i.uv - v * gT;
    float2 uvB = i.uv + v * (1.0 - gT);
    float3 a = texA.SampleLevel(smpLin, uvA, 0).rgb;
    float3 b = texB.SampleLevel(smpLin, uvB, 0).rgb;

    // OCCLUSION TEST. Even with the best candidate, a pixel in the halo of a
    // moving object has no correct vector at all: the background it should show
    // is hidden in one of the two frames. Sampling a field AT the fetch sites
    // tells that case apart from an ordinary one — where the pixel really
    // travels along v the field there agrees with v, in the halo it does not —
    // and decides which single frame to believe instead of averaging both into
    // a ghost.
    //
    // WHICH field is sampled is the whole difference between a halo heuristic
    // and a real occlusion mask. Reading the intermediate-anchored field twice
    // asks one texture the same question at two places: it can say "this
    // trajectory is inconsistent" but not which of the two frames stopped
    // seeing the pixel, because both answers come from the same estimate. The
    // A-anchored and B-anchored fields are independent evidence — in a covering
    // region the A side still tracks the background and the B side does not,
    // and in a disocclusion it is the other way round — so the two consistency
    // numbers can actually disagree, which is what makes the choice between the
    // frames mean something.
    float2 vAf = gBidir > 0.5 ? mvATex.SampleLevel(smpLin, uvA, 0)
                              : mvTex.SampleLevel(smpLin, uvA, 0);
    float2 vBf = gBidir > 0.5 ? mvBTex.SampleLevel(smpLin, uvB, 0)
                              : mvTex.SampleLevel(smpLin, uvB, 0);
    float  tol = 0.35 * length(vBest) + 1.5;
    float  cA  = saturate(1.0 - length(vAf - vBest) / tol);
    float  cB  = saturate(1.0 - length(vBf - vBest) / tol);

    float d = abs(Luma(a) - Luma(b));
    float k = saturate((d - 0.05) * 6.0);       // 0 the pair agrees .. 1 it does not

    float wA = (1.0 - gT) * (cA + 0.05);
    float wB = gT * (cB + 0.05);
    float w  = lerp(gT, wB / max(wA + wB, 1e-5), k);
    float3 outc = lerp(a, b, w);

    // Neither side consistent: one fixed-point step with the field found at the
    // fetch site, then the unwarped cross-fade. It ghosts, but it never invents
    // geometry.
    float bad = k * saturate(1.0 - 2.0 * max(cA, cB));
    if (bad > 0.01) {
        float3 a2 = texA.SampleLevel(smpLin, i.uv - vAf * gInvSize * gT, 0).rgb;
        float3 b2 = texB.SampleLevel(smpLin, i.uv + vBf * gInvSize * (1.0 - gT), 0).rgb;
        float3 a0 = texA.SampleLevel(smpPt, i.uv, 0).rgb;
        float3 b0 = texB.SampleLevel(smpPt, i.uv, 0).rgb;
        float  agree2 = saturate(1.0 - abs(Luma(a2) - Luma(b2)) * 8.0);
        float3 fallback = lerp(lerp(a0, b0, gT), lerp(a2, b2, gT), agree2);
        outc = lerp(outc, fallback, bad);
    }
    return float4(outc, 1.0);
}

// ------------------------------------------------------------- fps readout
// Plain 5x7 pixel-font text. The font table and the string arrive in the
// constant buffer, so the glyphs stay readable ASCII art in C++ instead of magic
// numbers in HLSL.
cbuffer TextParams : register(b3) {
    float4 gTRect;      // x, y of the text's top-left corner; w, h of the text box
    float4 gTInk;       // text colour, linear light, .a = opacity
    float4 gTMisc;      // y glyph scale, z glyph count
    float4 gTPad;
    uint4  gTGlyphs[4]; // up to 16 glyph indices into the font table
    uint4  gTFont[4];   // up to 16 glyphs, 5x7 bits each
};

float GlyphInk(float2 p) {
    float scale = max(gTMisc.y, 1.0);
    float count = gTMisc.z;
    float2 lp = (p - gTRect.xy) / scale;
    if (lp.x < 0.0 || lp.y < 0.0 || lp.y >= 7.0) return 0.0;
    int gi = (int)floor(lp.x / 6.0);
    if (gi < 0 || gi >= (int)count) return 0.0;
    int cx = (int)floor(lp.x - gi * 6.0);
    int cy = (int)floor(lp.y);
    if (cx < 0 || cx > 4) return 0.0;
    uint g = gTGlyphs[gi >> 2][gi & 3];
    uint bits = gTFont[g >> 2][g & 3];
    return ((bits >> (uint)(cy * 5 + (4 - cx))) & 1u) ? 1.0 : 0.0;
}

float4 PSFpsText(VSOut i) : SV_Target {
    float2 p = i.pos.xy;
    if (GlyphInk(p) > 0.0) return float4(gTInk.rgb, gTInk.a);
    // One pixel of dark outline around the glyphs: white text alone vanishes over
    // a bright sky, and a box behind it would be a banner again.
    float o = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            o = max(o, GlyphInk(p + float2(dx, dy)));
    if (o <= 0.0) discard;
    return float4(0.0, 0.0, 0.0, 0.75);
}

// ------------------------------------------------------------ luma pyramid
Texture2D<float4>   dsSrc : register(t0);
RWTexture2D<float>  dsDst : register(u0);

cbuffer DsParams : register(b1) {
    uint2 gDstSize;
    uint  gFromLuma;    // 1: source is already a luma texture
    uint  gDsPad;
    uint2 gSrcSize;     // the picture inside a possibly larger destination
    uint2 gDsPad2;
};

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gDstSize.x || id.y >= gDstSize.y) return;
    int2 p = int2(id.xy) * 2;
    float4 s0 = dsSrc.Load(int3(p, 0));
    float4 s1 = dsSrc.Load(int3(p + int2(1, 0), 0));
    float4 s2 = dsSrc.Load(int3(p + int2(0, 1), 0));
    float4 s3 = dsSrc.Load(int3(p + int2(1, 1), 0));
    float4 s = (s0 + s1 + s2 + s3) * 0.25;
    dsDst[id.xy] = gFromLuma ? s.r : Luma(s.rgb);
}

// --------------------------------------------------------------- matching
Texture2D<float>    lumaA : register(t3);
Texture2D<float>    lumaB : register(t4);
Texture2D<float2>   mvIn  : register(t5);
RWTexture2D<float2> mvOut : register(u0);

cbuffer McParams : register(b2) {
    uint2 gGrid;        // motion field dimensions, in cells
    uint2 gLevelSize;   // luma level dimensions, in pixels
    float gLevelScale;  // full-res pixels per level pixel
    uint  gUseIn;       // 1: refine mvIn instead of starting from zero
    float gStepLvl;     // search step, in level pixels (may be fractional)
    float gCellPx;      // full-res pixels per motion cell
    float gLambda;      // penalty per full-res pixel of deviation from the guess
    // Where the OUTPUT field is anchored: 0 on A, 0.5 on the intermediate
    // frame, 1 on B. It is what the scoring fetches are built around, and it is
    // NOT the same question as gReanchor — a field that already sits where it
    // belongs still has to be scored there.
    float gAnchor;
    // How far along its own vector the input field has to be traced to reach
    // that anchor, in units of the A->B interval. 0 leaves it where it is.
    float gReanchor;
    float gFlowSign;    // -1 flips NVOFA's backward field into the A->B convention
    float gMagPrior;    // bounded prior against long vectors, per full-res pixel
    float gMagCap;      // the bound, in the same units as the summed SAD
    float gMcPad;
    uint2 gHintSize;    // NVOFA hint grid, over the ALIGNED surface
    float2 gMcPad2;
};

// One matching pass. SEARCH and WINDOW are compile-time so the loops unroll;
// the step is a uniform, which is what lets the last pass search in whole
// FULL-RES pixels (gStepLvl = 0.5 at the /2 level) instead of level pixels.
//
// Sampling is bilinear on purpose: a vector v is evaluated at +-v/2 around the
// cell centre, so an odd v lands on half-pixels. Rounding those away is what
// quantised the field to 4 px and left every moving edge visibly ragged.
float2 MatchCell(uint2 cell, int SEARCH, int WINDOW) {
    float2 centreLvl = ((float2(cell) + 0.5) * gCellPx) / gLevelScale;
    float2 guessLvl  = gUseIn ? (mvIn[cell] / gLevelScale) : float2(0.0, 0.0);
    float2 invSize   = 1.0 / float2(gLevelSize);

    float  best  = 1e9;
    float2 bestV = guessLvl;
    for (int dy = -SEARCH; dy <= SEARCH; ++dy) {
        for (int dx = -SEARCH; dx <= SEARCH; ++dx) {
            float2 v = guessLvl + float2(dx, dy) * gStepLvl;
            float2 h = v * 0.5;
            float sad = 0.0;
            for (int wy = -WINDOW; wy <= WINDOW; ++wy) {
                for (int wx = -WINDOW; wx <= WINDOW; ++wx) {
                    float2 o = float2(wx, wy);
                    float a = lumaA.SampleLevel(smpLin, (centreLvl + o - h + 0.5) * invSize, 0);
                    float b = lumaB.SampleLevel(smpLin, (centreLvl + o + h + 0.5) * invSize, 0);
                    sad += abs(a - b);
                }
            }
            // Tie-break towards the smaller vector: on flat content every
            // candidate matches, and a random large vector there is a tear.
            sad += 0.0006 * length(v * gLevelScale);
            // Smoothness. Inside a large uniform object (the aperture problem)
            // every candidate matches equally well, and without this term the
            // cell picks an arbitrary one and tears the object apart. The guess
            // is the smoothed field, so this pulls ambiguous cells towards what
            // their neighbours decided.
            sad += gLambda * length((v - guessLvl) * gLevelScale);
            // A BOUNDED prior against long vectors. Unbounded (gLambda alone at
            // the coarse level, where guessLvl is zero) it fixes a period lock
            // and destroys fast motion, because the penalty grows without limit
            // in |v|. Capped, it can flip a photometric TIE — which is exactly
            // what a period alias is — but can never outvote a vector that wins
            // the match decisively, which is what a genuine fast pan does.
            sad += min(gMagPrior * length(v * gLevelScale), gMagCap);
            if (sad < best) { best = sad; bestV = v; }
        }
    }
    return bestV * gLevelScale;   // level pixels -> full-res pixels
}

[numthreads(8, 8, 1)]
void CSMatchCoarse(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gGrid.x || id.y >= gGrid.y) return;
    mvOut[id.xy] = MatchCell(id.xy, 6, 3);
}

[numthreads(8, 8, 1)]
void CSMatchMid(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gGrid.x || id.y >= gGrid.y) return;
    mvOut[id.xy] = MatchCell(id.xy, 3, 3);
}

[numthreads(8, 8, 1)]
void CSMatchFine(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gGrid.x || id.y >= gGrid.y) return;
    mvOut[id.xy] = MatchCell(id.xy, 3, 4);
}

// -------------------------------------------------- hardware flow plumbing
// Full-resolution 8-bit luma: what NVOFA takes as GRAYSCALE8 input.
[numthreads(8, 8, 1)]
void CSLumaFull(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gDstSize.x || id.y >= gDstSize.y) return;
    // The destination is the hardware's ALIGNED surface, which is larger than
    // the picture. Replicate the edge into the padding: a black border there
    // would be an enormous fake edge for a motion estimator to chase.
    int2 p = int2(min(id.x, gSrcSize.x - 1), min(id.y, gSrcSize.y - 1));
    float3 c = dsSrc.Load(int3(p, 0)).rgb;
    // The SRV is _SRGB, so this is linear light; the matcher wants something
    // perceptual, and a square root is a one-instruction stand-in for the
    // encode. Matching on linear luma crushes everything in the shadows.
    dsDst[id.xy] = sqrt(Luma(c));
}

// NVOFA writes S10.5 fixed point on its own grid; the warp reads float pixels
// on ours. With the cell size set equal to the flow grid this is a straight
// per-cell unit conversion and nothing else in the pipeline changes.
//
// gFlowSign carries the backward field's direction flip. NVOFA's backward
// output is B->A; negating it makes it describe the same A->B motion, so both
// directions are stored in one convention and the warp never has to remember
// which texture it is holding.
Texture2D<int2> flowIn : register(t6);

[numthreads(8, 8, 1)]
void CSFlowToField(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gGrid.x || id.y >= gGrid.y) return;
    int2 f = flowIn.Load(int3(id.xy, 0));
    mvOut[id.xy] = float2(f) * (gFlowSign / 32.0);
}

// ------------------------------------------------------- hardware field fix
// Two things the hardware field needs before the warp can use it.
//
// 1. RE-ANCHORING. NVOFA's vector belongs to frame A: it says where a pixel of A
//    went. The warp wants the vector passing through the INTERMEDIATE frame, and
//    at 42 px of motion the difference is a 21 px misplacement. Two fixed-point
//    steps move it there. The same pass also produces the fields that stay on
//    A and on B (gReanchor = 0) — those are the occlusion evidence, and they are
//    only worth anything if they carry the same smoothness prior as the field
//    the warp steers by.
// 2. A SMOOTHNESS PRIOR, which the hardware has none of. On a periodic pattern
//    it locks whole columns onto the wrong period (G24), and a median alone
//    cannot fix a column. Re-score the cell's own vector against the
//    neighbourhood median and against zero with our own SAD, and keep the
//    cheapest with a small penalty on length — which is exactly the tie-break
//    the block matcher had and the hardware lacks.
float FieldScore(float2 centreLvl, float2 vLvl, float2 invSize) {
    float sad = 0.0;
    [unroll] for (int wy = -2; wy <= 2; ++wy) {
        [unroll] for (int wx = -2; wx <= 2; ++wx) {
            float2 o = float2(wx, wy);
            float a = lumaA.SampleLevel(smpLin,
                                        (centreLvl + o - vLvl * gAnchor + 0.5) * invSize, 0);
            float b = lumaB.SampleLevel(smpLin,
                                        (centreLvl + o + vLvl * (1.0 - gAnchor) + 0.5) * invSize, 0);
            sad += abs(a - b);
        }
    }
    return sad;
}

[numthreads(8, 8, 1)]
void CSFieldFix(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gGrid.x || id.y >= gGrid.y) return;

    float2 centreFull = (float2(id.xy) + 0.5) * gCellPx;
    float2 gridExtent = gCellPx * float2(gGrid);

    float2 v = mvIn[id.xy];
    [unroll] for (int it = 0; it < 2; ++it) {
        v = mvIn.SampleLevel(smpLin, (centreFull - v * gReanchor) / gridExtent, 0);
    }

    float xs[9], ys[9];
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 q = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(gGrid) - 1);
            float2 nv = mvIn[q];
            xs[n] = nv.x; ys[n] = nv.y; ++n;
        }
    }
    for (int i = 0; i < 9; ++i) {
        for (int j = i + 1; j < 9; ++j) {
            if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { float t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
    }
    float2 med = float2(xs[4], ys[4]);

    float2 centreLvl = centreFull / gLevelScale;
    float2 invSize = 1.0 / float2(gLevelSize);

    // The prior that matters is COHERENCE, not shortness. Preferring the shorter
    // vector on equal evidence fragments the field inside a moving object — half
    // its cells take the object's vector and half take zero — and the warp then
    // pulls background through the middle of it. Start from the neighbourhood
    // median and depart from it only on clear evidence.
    float cSelf = FieldScore(centreLvl, v / gLevelScale, invSize);
    float cMed  = FieldScore(centreLvl, med / gLevelScale, invSize);
    float cZero = FieldScore(centreLvl, float2(0.0, 0.0), invSize);

    float2 best = med;
    float  bestCost = cMed;
    if (cSelf < bestCost * 0.92) { bestCost = cSelf; best = v; }
    // Zero only where the two frames genuinely agree in absolute terms, not
    // merely better than the alternatives: 25 taps, mean |dLuma| under 2 %.
    if (cZero < bestCost * 0.85 && cZero < 0.5) { best = float2(0.0, 0.0); }

    mvOut[id.xy] = best;
}

// ------------------------------------------------------- NVOFA external hint
// Our field, rewritten in the hardware's own units so it can be handed back as a
// search seed. NVOFA has no pyramid of ours to refine in, so it carries G30's
// period lock in full; this is the only way the fix reaches it.
//
// Three conversions, each of which is a way to get this silently wrong:
//   * units — the hint is S10.5 FIXED POINT, so a pixel is 32 (same as the flow
//     output, which nsp_ofa.h documents as "divide by 32 for pixels");
//   * grid — the hint is sized over the ALIGNED surface, which is larger than
//     the picture, so cells past the right/bottom edge clamp to our last cell
//     rather than reading garbage;
//   * direction — the hint seeds the FORWARD search, which is A->B, and our
//     field already means A->B, so no sign flip. The backward field is derived
//     by the hardware, not seeded.
RWTexture2D<int2> hintOut : register(u0);

[numthreads(8, 8, 1)]
void CSFillHint(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gHintSize.x || id.y >= gHintSize.y) return;
    // gCellPx carries the hint-cell to seed-cell ratio here, because the seed
    // lives on a COARSER grid than the hint: one seed cell covers several hint
    // cells and every one of them takes its value.
    int2 src = clamp(int2(float2(id.xy) * gCellPx), int2(0, 0), int2(gGrid) - 1);
    float2 v = mvIn[src];
    hintOut[id.xy] = int2(round(v * 32.0));
}

// ---------------------------------------------------------------- smoothing
// Component-wise median of the 3x3 neighbourhood. A mean would smear a wrong
// vector into eight good cells; the median throws it away.
// ------------------------------------------------- coarse period disambiguation
// The coarse pyramid level cannot decide a period ambiguity and never could:
// downsampling to /8 removes exactly the frequencies that distinguish the true
// vector from the alias one texture period away, while the repeating component
// itself survives. Measured on A4, symmetric SAD: at full resolution the truth
// is 2.9x cheaper than the alias; at /8 the alias is 6.9x cheaper than the
// truth. So the coarse winner has to be re-examined where the evidence still
// exists.
//
// Two things this needs that CSFieldFix cannot give it. It must score at FULL
// resolution, and its window must be WIDER THAN THE PERIOD it is trying to
// resolve -- CSFieldFix scores a 5x5 window on the /2 luma, which spans 10 px
// against a 43 px period and therefore sees the alias and the truth as equally
// good. Here the taps are spread over +-4 steps of gCellPx (64 px at the
// default 8 px cell), which covers a period up to ~64 px.
//
// It deliberately has NO magnitude prior. A prior against long vectors also
// fixes A4, and was rejected on measurement: it caps the motion the matcher can
// track, collapsing a rigid pan from 80 dB to 16 dB at 64 px/interval. The test
// here is photometric at a resolution that can actually see the difference, so a
// GENUINE long vector beats zero and is kept.
float WideScore(float2 centreLvl, float2 vLvl, float2 invSize) {
    float sad = 0.0;
    // Tap spacing is one cell in LEVEL pixels, so the 9x5 grid spans +-4 cells
    // horizontally: 64 full-res px at the default 8 px cell, wide enough to
    // contain a period of that order. A window narrower than the period sees
    // the alias and the truth as equally good, which is exactly why CSFieldFix's
    // 5x5 could not do this job.
    float sp = gCellPx / gLevelScale;
    for (int wy = -2; wy <= 2; ++wy) {
        for (int wx = -4; wx <= 4; ++wx) {
            float2 o = float2(float(wx) * sp, float(wy) * sp);
            float a = lumaA.SampleLevel(smpLin, (centreLvl + o - vLvl * 0.5 + 0.5) * invSize, 0);
            float b = lumaB.SampleLevel(smpLin, (centreLvl + o + vLvl * 0.5 + 0.5) * invSize, 0);
            sad += abs(a - b);
        }
    }
    return sad;
}

[numthreads(8, 8, 1)]
void CSCoarseFix(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gGrid.x || id.y >= gGrid.y) return;
    float2 centreLvl = ((float2(id.xy) + 0.5) * gCellPx) / gLevelScale;
    float2 invSize = 1.0 / float2(gLevelSize);

    float2 v = mvIn[id.xy];

    float xs[9], ys[9];
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 q = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(gGrid) - 1);
            float2 nv = mvIn[q];
            xs[n] = nv.x; ys[n] = nv.y; ++n;
        }
    }
    for (int i = 0; i < 9; ++i) {
        for (int j = i + 1; j < 9; ++j) {
            if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { float t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
    }
    float2 med = float2(xs[4], ys[4]);

    // EACH CANDIDATE IS REFINED BEFORE IT IS SCORED. That one piece of structure
    // is the fix for G30, and its absence is why the first two versions of this
    // pass changed nothing. The coarse search reaches only multiples of 16
    // full-res px, so on a periodic pattern the ALIAS lands almost exactly on a
    // grid point -- on A4 the alias is -47.95 and the grid offers -48 -- while
    // the TRUTH lands between two (-4.75, with 0 and -16 either side). Scoring
    // the RAW candidates therefore pits a sharp alias against a blurred stand-in
    // for the truth and picks the alias every time. The score was never wrong;
    // the truth was simply never among the things being scored.
    //
    // Measured, which is what makes this more than a story: with the truth in
    // the candidate set this window rates the alias 2.7x more expensive than the
    // truth on 100 % of 90 A4 cells, and on a real photograph panned 32 px it
    // rates the wrong candidate ~1e7 times more expensive -- so a genuine fast
    // vector is in no danger, which is exactly what a magnitude prior could not
    // promise (N23).
    float2 cands[3] = { v, med, float2(0.0, 0.0) };
    float2 best = v;
    float  bestCost = 1e9;
    [unroll] for (int ci = 0; ci < 3; ++ci) {
        for (int sx = -10; sx <= 10; ++sx) {
            float2 t2 = cands[ci] + float2(float(sx), 0.0);
            float sc = WideScore(centreLvl, t2 / gLevelScale, invSize);
            if (sc < bestCost) { bestCost = sc; best = t2; }
        }
        for (int sy = -10; sy <= 10; ++sy) {
            float2 t2 = cands[ci] + float2(0.0, float(sy));
            float sc = WideScore(centreLvl, t2 / gLevelScale, invSize);
            if (sc < bestCost) { bestCost = sc; best = t2; }
        }
    }

    mvOut[id.xy] = best;
}

[numthreads(8, 8, 1)]
void CSSmooth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gGrid.x || id.y >= gGrid.y) return;
    float xs[9], ys[9];
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 p = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(gGrid) - 1);
            float2 v = mvIn[p];
            xs[n] = v.x; ys[n] = v.y; ++n;
        }
    }
    for (int i = 0; i < 9; ++i) {
        for (int j = i + 1; j < 9; ++j) {
            if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { float t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
    }
    mvOut[id.xy] = float2(xs[4], ys[4]);
}
)HLSL";

struct ParamsCb {
    float invW, invH;
    float t;
    float cellPx;
    float bidir;
    float pad[3];
};

struct DsCb {
    UINT dstW, dstH;
    UINT fromLuma;
    UINT pad;
    UINT srcW, srcH;
    UINT pad2[2];
};

// The 5x7 pixel font, as ASCII art so it stays editable. Row 0 is the top; a
// '#' is ink. Packed into one uint per glyph, bit (row*5 + (4-col)).
struct Glyph {
    char ch;
    const char* rows[7];
};

constexpr Glyph kGlyphs[] = {
    {'0', {"#####", "#   #", "#   #", "#   #", "#   #", "#   #", "#####"}},
    // The other digits are drawn with full-width bars, so a thin 1 next to them
    // reads as an I (or, without a base, as a stray tick). A two-pixel stem
    // gives it the same visual weight as its neighbours.
    {'1', {"  ## ", " ### ", "  ## ", "  ## ", "  ## ", "  ## ", " ####"}},
    {'2', {"#####", "    #", "    #", "#####", "#    ", "#    ", "#####"}},
    {'3', {"#####", "    #", "    #", "#####", "    #", "    #", "#####"}},
    {'4', {"#   #", "#   #", "#   #", "#####", "    #", "    #", "    #"}},
    {'5', {"#####", "#    ", "#    ", "#####", "    #", "    #", "#####"}},
    {'6', {"#####", "#    ", "#    ", "#####", "#   #", "#   #", "#####"}},
    {'7', {"#####", "    #", "    #", "   # ", "  #  ", "  #  ", "  #  "}},
    {'8', {"#####", "#   #", "#   #", "#####", "#   #", "#   #", "#####"}},
    {'9', {"#####", "#   #", "#   #", "#####", "    #", "    #", "#####"}},
    {'/', {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}},
};
constexpr int kGlyphCount = static_cast<int>(std::size(kGlyphs));
static_assert(kGlyphCount <= 16, "the text constant buffer holds 16 glyphs");

struct TextCb {
    float rect[4];
    float ink[4];
    float misc[4];
    float pad[4];
    UINT glyphs[16];
    UINT font[16];
};


// The defaults are the midpoint field's, because that is what every pass except
// the two occlusion fields wants: a call site that forgets them still gets the
// behaviour the block matcher and the old hardware path had.
struct McCb {
    UINT gridW, gridH;
    UINT levelW, levelH;
    float levelScale;
    UINT useIn;
    float stepLvl;
    float cellPx;
    float lambda;
    float anchor = 0.5f;
    float reanchor = 0.5f;
    float flowSign = 1.0f;
    float magPrior = 0.0f;
    float magCap = 0.0f;
    float pad3 = 0.0f;
    UINT hintW = 0, hintH = 0;
    float pad4[2] = {};
};

bool CompileOne(const char* entry, const char* target, ID3DBlob** blob, std::string* err) {
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "nsp_synth.hlsl",
                                  nullptr, nullptr, entry, target, flags, 0, blob, &errors);
    if (FAILED(hr)) {
        std::string msg = std::string("D3DCompile(") + entry + ") failed: " + HrString(hr);
        if (errors && errors->GetBufferPointer()) {
            msg += " - ";
            msg.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        if (err) *err = msg;
        return false;
    }
    return true;
}

UINT DivUp(UINT a, UINT b) { return (a + b - 1) / b; }

// R16G16_FLOAT readback: the motion field is stored as halves.
float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h >> 15) & 1u;
    const uint32_t exp = static_cast<uint32_t>(h >> 10) & 0x1Fu;
    const uint32_t man = static_cast<uint32_t>(h) & 0x3FFu;
    uint32_t f = 0;
    if (exp == 0) {
        if (man != 0) {
            int e = -1;
            uint32_t m = man;
            do {
                m <<= 1;
                ++e;
            } while ((m & 0x400u) == 0);
            f = (sign << 31) | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        } else {
            f = sign << 31;
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7F800000u | (man << 13);
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float out = 0.0f;
    memcpy(&out, &f, sizeof(out));
    return out;
}

}  // namespace

struct Synth::Impl {
    ID3D11Device* device = nullptr;      // borrowed
    ID3D11DeviceContext* ctx = nullptr;  // borrowed

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psBlend;
    ComPtr<ID3D11PixelShader> psWarp;
    ComPtr<ID3D11ComputeShader> csDownsample;
    ComPtr<ID3D11ComputeShader> csMatchCoarse;
    ComPtr<ID3D11ComputeShader> csMatchMid;
    ComPtr<ID3D11ComputeShader> csMatchFine;
    ComPtr<ID3D11ComputeShader> csSmooth;
    ComPtr<ID3D11ComputeShader> csLumaFull;
    ComPtr<ID3D11ComputeShader> csFlowToField;
    ComPtr<ID3D11ComputeShader> csFieldFix;
    ComPtr<ID3D11ComputeShader> csFillHint;
    ComPtr<ID3D11ComputeShader> csCoarseFix;
    OfaFlow ofa;
    bool ofaOn = false;

    ComPtr<ID3D11SamplerState> smpPoint;
    ComPtr<ID3D11SamplerState> smpLinear;
    ComPtr<ID3D11Buffer> cbParams;
    ComPtr<ID3D11Buffer> cbDs;
    ComPtr<ID3D11Buffer> cbMc;
    ComPtr<ID3D11PixelShader> psText;
    ComPtr<ID3D11Buffer> cbBadge;
    ComPtr<ID3D11BlendState> blendOff;
    ComPtr<ID3D11BlendState> blendAlpha;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11RasterizerState> rasterScissor;
    ComPtr<ID3D11DepthStencilState> depthOff;
    UINT fontPacked[16] = {};

    UINT srcW = 0, srcH = 0;
    UINT gridW = 0, gridH = 0;
    UINT cellPx = kDefaultCellPx;

    struct Level {
        UINT w = 0, h = 0;
        ComPtr<ID3D11Texture2D> texA, texB;
        ComPtr<ID3D11ShaderResourceView> srvA, srvB;
        ComPtr<ID3D11UnorderedAccessView> uavA, uavB;
    };
    Level levels[kPyramidLevels];

    // 0 and 1 are scratch, ping-ponged between passes. 2 is the intermediate-
    // anchored field the warp steers by. 3 and 4 are the same motion anchored on
    // A and on B, which only the hardware path can fill and which exist for the
    // occlusion test — a heuristic without them, real evidence with them.
    static constexpr int kFieldCount = 5;
    ComPtr<ID3D11Texture2D> mv[kFieldCount];
    ComPtr<ID3D11ShaderResourceView> mvSrv[kFieldCount];
    ComPtr<ID3D11UnorderedAccessView> mvUav[kFieldCount];
    // THE SEED GRID. The hint NVOFA is handed is only a search seed, so it does
    // not need the engine's per-4-px detail — and computing it there is what made
    // hints cost 11.2 ms/pair, because the cell-4 grid has 16x the cells of this
    // one and the candidate refinement dominates. Its own coarse grid costs the
    // same work per cell over far fewer cells; CSFillHint upsamples on the way in.
    static constexpr UINT kSeedCellPx = 16;
    UINT seedGridW = 0, seedGridH = 0;
    ComPtr<ID3D11Texture2D> seedMv[2];
    ComPtr<ID3D11ShaderResourceView> seedSrv[2];
    ComPtr<ID3D11UnorderedAccessView> seedUav[2];

    // The previous pair's fields, kept for the pacing's older pair (HoldFields):
    // 0 intermediate, 1 A-anchored, 2 B-anchored.
    ComPtr<ID3D11Texture2D> held[3];
    ComPtr<ID3D11ShaderResourceView> heldSrv[3];
    bool heldValid = false;
    int heldOccMode = 0;

    int mvFinal = -1;  // index of the field the warp reads; -1 = none yet
    int mvFwd = -1;    // A-anchored field, -1 until a bidirectional pair is ready
    int mvBwd = -1;    // B-anchored field, in the A->B convention
    int occMode = 2;   // 0 self, 1 bidirectional occlusion, 2 also as candidates
    int OccModeNow() const { return (occMode > 0 && mvFwd >= 0 && mvBwd >= 0) ? occMode : 0; }

    // Blocking readback of the warp's field, for DebugFieldStats (--dump) only.
    ComPtr<ID3D11Texture2D> mvStaging;

    bool hasMotion = false;

    void SetParams(float t, int occModeOverride = -1);
    void RunSmoothPass(int inIdx, int outIdx);
    void RunFieldFix(int inIdx, int outIdx, float anchor, float reanchor);
    void RunCoarseFix(int inIdx, int outIdx);
    void RunCoarseFixOn(ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out,
                        UINT gw, UINT gh, UINT cell);
    void RunMatchCoarseOn(ID3D11UnorderedAccessView* out, UINT gw, UINT gh, UINT cell);
    bool EnsureSeedGrid(std::string* err);
    void RunFillHint(ID3D11ShaderResourceView* in);
    void Downsample(ID3D11ShaderResourceView* src, const Level& dst, bool intoA, bool fromLuma);
    void ClearSrvs();
};

void Synth::Impl::ClearSrvs() {
    ID3D11ShaderResourceView* none[9] = {};
    ctx->PSSetShaderResources(0, 9, none);
    ctx->CSSetShaderResources(0, 9, none);
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
}

void Synth::Impl::SetParams(float t, int occModeOverride) {
    const UINT cellPx = this->cellPx;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ParamsCb p{};
        p.invW = srcW ? 1.0f / static_cast<float>(srcW) : 0.0f;
        p.invH = srcH ? 1.0f / static_cast<float>(srcH) : 0.0f;
        p.t = t;
        p.cellPx = static_cast<float>(cellPx);
        p.bidir = static_cast<float>(occModeOverride >= 0 ? occModeOverride : OccModeNow());
        memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(cbParams.Get(), 0);
    }
}

void Synth::Impl::Downsample(ID3D11ShaderResourceView* src, const Level& dst, bool intoA,
                             bool fromLuma) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbDs.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        DsCb c{};
        c.dstW = dst.w;
        c.dstH = dst.h;
        c.fromLuma = fromLuma ? 1u : 0u;
        c.srcW = dst.w * 2;
        c.srcH = dst.h * 2;
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cbDs.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[1] = {src};
    ID3D11UnorderedAccessView* uavs[1] = {intoA ? dst.uavA.Get() : dst.uavB.Get()};
    ID3D11Buffer* cbs[1] = {cbDs.Get()};
    ctx->CSSetShader(csDownsample.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetConstantBuffers(1, 1, cbs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(DivUp(dst.w, 8), DivUp(dst.h, 8), 1);
    ID3D11UnorderedAccessView* none[1] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, none, nullptr);
    ID3D11ShaderResourceView* noSrv[1] = {nullptr};
    ctx->CSSetShaderResources(0, 1, noSrv);
}

void Synth::Impl::RunFieldFix(int inIdx, int outIdx, float anchor, float reanchor) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        McCb c{};
        c.gridW = gridW;
        c.gridH = gridH;
        c.levelW = levels[0].w;
        c.levelH = levels[0].h;
        c.levelScale = 2.0f;      // scoring runs on the /2 luma
        c.useIn = 1;
        c.stepLvl = 1.0f;
        c.cellPx = static_cast<float>(cellPx);
        c.lambda = 0.004f;        // per pixel of vector length, against the SAD
        c.anchor = anchor;
        c.reanchor = reanchor;
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cbMc.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {levels[0].srvA.Get(), levels[0].srvB.Get(),
                                         mvSrv[inIdx].Get()};
    ID3D11UnorderedAccessView* uavs[1] = {mvUav[outIdx].Get()};
    ID3D11Buffer* cbs[1] = {cbMc.Get()};
    ID3D11SamplerState* samplers[2] = {smpPoint.Get(), smpLinear.Get()};
    ctx->CSSetShader(csFieldFix.Get(), nullptr, 0);
    ctx->CSSetSamplers(0, 2, samplers);
    ctx->CSSetShaderResources(3, 3, srvs);
    ctx->CSSetConstantBuffers(2, 1, cbs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(DivUp(gridW, 8), DivUp(gridH, 8), 1);
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ctx->CSSetShaderResources(3, 3, noSrv);
}

bool Synth::Impl::EnsureSeedGrid(std::string* err) {
    const UINT gw = DivUp(srcW, kSeedCellPx), gh = DivUp(srcH, kSeedCellPx);
    if (seedGridW == gw && seedGridH == gh && seedMv[0]) return true;
    seedGridW = gw;
    seedGridH = gh;
    for (int i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = gw;
        td.Height = gh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, seedMv[i].ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
            hr = device->CreateShaderResourceView(seedMv[i].Get(), nullptr,
                                                  seedSrv[i].ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
            hr = device->CreateUnorderedAccessView(seedMv[i].Get(), nullptr,
                                                   seedUav[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            if (err) *err = "seed field allocation failed: " + HrString(hr);
            seedGridW = seedGridH = 0;
            return false;
        }
    }
    return true;
}

void Synth::Impl::RunMatchCoarseOn(ID3D11UnorderedAccessView* out, UINT gw, UINT gh, UINT cell) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        McCb c{};
        c.gridW = gw;
        c.gridH = gh;
        c.levelW = levels[2].w;
        c.levelH = levels[2].h;
        c.levelScale = 8.0f;
        c.useIn = 0;
        c.stepLvl = 2.0f;
        c.cellPx = static_cast<float>(cell);
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cbMc.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {levels[2].srvA.Get(), levels[2].srvB.Get(), nullptr};
    ID3D11UnorderedAccessView* uavs[1] = {out};
    ID3D11Buffer* cbs[1] = {cbMc.Get()};
    ctx->CSSetShader(csMatchCoarse.Get(), nullptr, 0);
    ctx->CSSetShaderResources(3, 3, srvs);
    ctx->CSSetConstantBuffers(2, 1, cbs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(DivUp(gw, 8), DivUp(gh, 8), 1);
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ctx->CSSetShaderResources(3, 3, noSrv);
}

void Synth::Impl::RunFillHint(ID3D11ShaderResourceView* in) {
    if (!ofa.HintsEnabled() || !ofa.HintUav()) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        McCb c{};
        c.gridW = seedGridW;
        c.gridH = seedGridH;
        c.hintW = ofa.GridW();
        c.hintH = ofa.GridH();
        // One hint cell spans ofa.GridSize() px; one seed cell spans kSeedCellPx.
        c.cellPx = static_cast<float>(ofa.GridSize()) / static_cast<float>(kSeedCellPx);
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cbMc.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, in};
    ID3D11UnorderedAccessView* uavs[1] = {ofa.HintUav()};
    ID3D11Buffer* cbs[1] = {cbMc.Get()};
    ctx->CSSetShader(csFillHint.Get(), nullptr, 0);
    ctx->CSSetShaderResources(3, 3, srvs);
    ctx->CSSetConstantBuffers(2, 1, cbs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(DivUp(ofa.GridW(), 8), DivUp(ofa.GridH(), 8), 1);
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ctx->CSSetShaderResources(3, 3, noSrv);
}

void Synth::Impl::RunCoarseFixOn(ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out,
                                 UINT gw, UINT gh, UINT cell) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        McCb c{};
        c.gridW = gw;
        c.gridH = gh;
        // The /2 luma, not /8: the frequencies that tell the truth from its
        // period alias survive here and are gone by /8.
        c.levelW = levels[0].w;
        c.levelH = levels[0].h;
        c.levelScale = 2.0f;   // the /2 luma: 11 px and 23 px detail survive here
        c.useIn = 1;
        c.cellPx = static_cast<float>(cell);
        c.lambda = 0.0f;
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cbMc.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {levels[0].srvA.Get(), levels[0].srvB.Get(), in};
    ID3D11UnorderedAccessView* uavs[1] = {out};
    ID3D11Buffer* cbs[1] = {cbMc.Get()};
    ID3D11SamplerState* samplers[2] = {smpPoint.Get(), smpLinear.Get()};
    ctx->CSSetShader(csCoarseFix.Get(), nullptr, 0);
    ctx->CSSetSamplers(0, 2, samplers);
    ctx->CSSetShaderResources(3, 3, srvs);
    ctx->CSSetConstantBuffers(2, 1, cbs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(DivUp(gw, 8), DivUp(gh, 8), 1);
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ctx->CSSetShaderResources(3, 3, noSrv);
}

void Synth::Impl::RunCoarseFix(int inIdx, int outIdx) {
    RunCoarseFixOn(mvSrv[inIdx].Get(), mvUav[outIdx].Get(), gridW, gridH, cellPx);
}

void Synth::Impl::RunSmoothPass(int inIdx, int outIdx) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        McCb c{};
        c.gridW = gridW;
        c.gridH = gridH;
        c.levelW = levels[0].w;
        c.levelH = levels[0].h;
        c.levelScale = 1.0f;
        c.useIn = 1;
        c.stepLvl = 1.0f;
        c.cellPx = static_cast<float>(cellPx);
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cbMc.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, mvSrv[inIdx].Get()};
    ID3D11UnorderedAccessView* uavs[1] = {mvUav[outIdx].Get()};
    ID3D11Buffer* cbs[1] = {cbMc.Get()};
    ctx->CSSetShader(csSmooth.Get(), nullptr, 0);
    ctx->CSSetShaderResources(3, 3, srvs);
    ctx->CSSetConstantBuffers(2, 1, cbs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(DivUp(gridW, 8), DivUp(gridH, 8), 1);
    ID3D11UnorderedAccessView* noUav[1] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ctx->CSSetShaderResources(3, 3, noSrv);
}

Synth::Synth() : impl_(std::make_unique<Impl>()) {}
Synth::~Synth() = default;

bool Synth::HasMotion() const { return impl_->hasMotion && impl_->mvFinal >= 0; }

void Synth::MotionGrid(UINT* gw, UINT* gh, UINT* cellPx) const {
    if (gw) *gw = impl_->gridW;
    if (gh) *gh = impl_->gridH;
    if (cellPx) *cellPx = impl_->cellPx;
}

bool Synth::Create(ID3D11Device* device, ID3D11DeviceContext* ctx, std::string* err) {
    Impl& d = *impl_;
    d.device = device;
    d.ctx = ctx;
    if (!device || !ctx) {
        if (err) *err = "Synth::Create: no device";
        return false;
    }

    auto build = [&](const char* entry, const char* target, auto makeShader) {
        ComPtr<ID3DBlob> blob;
        if (!CompileOne(entry, target, &blob, err)) return false;
        const HRESULT hr = makeShader(blob.Get());
        if (FAILED(hr)) {
            if (err) *err = std::string("Create shader ") + entry + " failed: " + HrString(hr);
            return false;
        }
        return true;
    };
    auto vsOut = [&](ID3DBlob* b) {
        return device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
                                          d.vs.ReleaseAndGetAddressOf());
    };
    auto psOut = [&](ComPtr<ID3D11PixelShader>* out) {
        return [&, out](ID3DBlob* b) {
            return device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
                                             out->ReleaseAndGetAddressOf());
        };
    };
    auto csOut = [&](ComPtr<ID3D11ComputeShader>* out) {
        return [&, out](ID3DBlob* b) {
            return device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
                                               out->ReleaseAndGetAddressOf());
        };
    };

    if (!build("VSMain", "vs_5_0", vsOut)) return false;
    if (!build("PSBlend", "ps_5_0", psOut(&d.psBlend))) return false;
    if (!build("PSWarp", "ps_5_0", psOut(&d.psWarp))) return false;
    if (!build("PSFpsText", "ps_5_0", psOut(&d.psText))) return false;
    if (!build("CSDownsample", "cs_5_0", csOut(&d.csDownsample))) return false;
    if (!build("CSMatchCoarse", "cs_5_0", csOut(&d.csMatchCoarse))) return false;
    if (!build("CSMatchMid", "cs_5_0", csOut(&d.csMatchMid))) return false;
    if (!build("CSMatchFine", "cs_5_0", csOut(&d.csMatchFine))) return false;
    if (!build("CSSmooth", "cs_5_0", csOut(&d.csSmooth))) return false;
    if (!build("CSLumaFull", "cs_5_0", csOut(&d.csLumaFull))) return false;
    if (!build("CSFlowToField", "cs_5_0", csOut(&d.csFlowToField))) return false;
    if (!build("CSFieldFix", "cs_5_0", csOut(&d.csFieldFix))) return false;
    if (!build("CSFillHint", "cs_5_0", csOut(&d.csFillHint))) return false;
    if (!build("CSCoarseFix", "cs_5_0", csOut(&d.csCoarseFix))) return false;

    // Point for the 1:1 paths (I13: nothing is scaled), linear for the warped
    // fetches, whose coordinates are genuinely sub-pixel.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = device->CreateSamplerState(&sd, &d.smpPoint);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sd, &d.smpLinear);
    if (FAILED(hr)) {
        if (err) *err = "CreateSamplerState failed: " + HrString(hr);
        return false;
    }

    auto makeCb = [&](UINT bytes, ComPtr<ID3D11Buffer>* out) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (bytes + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return device->CreateBuffer(&bd, nullptr, out->ReleaseAndGetAddressOf());
    };
    if (FAILED(makeCb(sizeof(ParamsCb), &d.cbParams)) || FAILED(makeCb(sizeof(DsCb), &d.cbDs)) ||
        FAILED(makeCb(sizeof(McCb), &d.cbMc)) || FAILED(makeCb(sizeof(TextCb), &d.cbBadge))) {
        if (err) *err = "constant buffer creation failed";
        return false;
    }

    // Pack the ASCII-art font once.
    for (int g = 0; g < kGlyphCount; ++g) {
        UINT bits = 0;
        for (int row = 0; row < 7; ++row) {
            const char* r = kGlyphs[g].rows[row];
            for (int col = 0; col < 5; ++col) {
                if (r[col] != ' ' && r[col] != '\0')
                    bits |= 1u << static_cast<UINT>(row * 5 + (4 - col));
            }
        }
        d.fontPacked[g] = bits;
    }

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bl, &d.blendOff);

    // Straight-alpha "over" for the watermark. The render target view is _SRGB,
    // so the hardware blends in linear light, which is where our colours are.
    D3D11_BLEND_DESC ba = {};
    ba.RenderTarget[0].BlendEnable = TRUE;
    ba.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&ba, &d.blendAlpha);

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rs, &d.raster);
    rs.ScissorEnable = TRUE;  // the badge shades its own few hundred pixels only
    device->CreateRasterizerState(&rs, &d.rasterScissor);

    D3D11_DEPTH_STENCIL_DESC ds = {};
    device->CreateDepthStencilState(&ds, &d.depthOff);
    return true;
}

bool Synth::DrawFpsText(ID3D11RenderTargetView* rtv, UINT w, UINT h, float right, float top,
                        int scale, const std::string& text) {
    Impl& d = *impl_;
    if (!d.ctx || !rtv || !d.psText || scale < 1) return false;

    TextCb cb{};
    int count = 0;
    for (char ch : text) {
        if (count >= 16) break;
        for (int g = 0; g < kGlyphCount; ++g) {
            if (kGlyphs[g].ch == ch) {
                cb.glyphs[count++] = static_cast<UINT>(g);
                break;
            }
        }
    }
    if (count == 0) return false;
    const float tw = static_cast<float>((count * 5 + (count - 1)) * scale);
    const float th = static_cast<float>(7 * scale);
    // Whole pixels: a glyph edge on a half pixel would shimmer as the width changes.
    cb.rect[0] = std::floor(right - tw);
    cb.rect[1] = std::floor(top);
    cb.rect[2] = tw;
    cb.rect[3] = th;
    cb.ink[0] = cb.ink[1] = cb.ink[2] = 0.92f;  // near-white, linear light
    cb.ink[3] = 1.0f;
    cb.misc[1] = static_cast<float>(scale);
    cb.misc[2] = static_cast<float>(count);
    memcpy(cb.font, d.fontPacked, sizeof(cb.font));

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(d.ctx->Map(d.cbBadge.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        d.ctx->Unmap(d.cbBadge.Get(), 0);
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MaxDepth = 1.0f;

    // The text box plus its one-pixel outline, and nothing else is touched.
    D3D11_RECT sc = {};
    sc.left = (std::max)(0L, static_cast<LONG>(cb.rect[0]) - 1);
    sc.top = (std::max)(0L, static_cast<LONG>(cb.rect[1]) - 1);
    sc.right = (std::min)(static_cast<LONG>(w), static_cast<LONG>(cb.rect[0] + tw) + 1);
    sc.bottom = (std::min)(static_cast<LONG>(h), static_cast<LONG>(cb.rect[1] + th) + 1);
    if (sc.right <= sc.left || sc.bottom <= sc.top) return false;

    ID3D11Buffer* cbs[1] = {d.cbBadge.Get()};
    ID3D11RenderTargetView* rtvs[1] = {rtv};
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->IASetInputLayout(nullptr);
    d.ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    d.ctx->PSSetShader(d.psText.Get(), nullptr, 0);
    d.ctx->PSSetConstantBuffers(3, 1, cbs);
    d.ctx->RSSetViewports(1, &vp);
    d.ctx->RSSetScissorRects(1, &sc);
    d.ctx->RSSetState(d.rasterScissor.Get());
    d.ctx->OMSetBlendState(d.blendAlpha.Get(), nullptr, 0xFFFFFFFF);
    d.ctx->OMSetDepthStencilState(d.depthOff.Get(), 0);
    d.ctx->OMSetRenderTargets(1, rtvs, nullptr);
    d.ctx->Draw(3, 0);

    d.ctx->RSSetState(d.raster.Get());
    d.ctx->OMSetBlendState(d.blendOff.Get(), nullptr, 0xFFFFFFFF);
    ID3D11RenderTargetView* noRt[1] = {nullptr};
    d.ctx->OMSetRenderTargets(1, noRt, nullptr);
    return true;
}

bool Synth::Resize(UINT w, UINT h, std::string* err, UINT cellPx) {
    Impl& d = *impl_;
    if (w == 0 || h == 0) {
        if (err) *err = "Synth::Resize: empty size";
        return false;
    }
    d.srcW = w;
    d.srcH = h;
    d.cellPx = (cellPx >= 4 && cellPx <= 64) ? cellPx : kDefaultCellPx;
    d.gridW = DivUp(w, d.cellPx);
    d.gridH = DivUp(h, d.cellPx);
    d.mvFinal = -1;
    d.mvFwd = -1;
    d.mvBwd = -1;
    d.hasMotion = false;

    auto makeLuma = [&](UINT lw, UINT lh, ComPtr<ID3D11Texture2D>* tex,
                        ComPtr<ID3D11ShaderResourceView>* srv,
                        ComPtr<ID3D11UnorderedAccessView>* uav) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (std::max)(1u, lw);
        td.Height = (std::max)(1u, lh);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = d.device->CreateTexture2D(&td, nullptr, tex->ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
            hr = d.device->CreateShaderResourceView(tex->Get(), nullptr,
                                                    srv->ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
            hr = d.device->CreateUnorderedAccessView(tex->Get(), nullptr,
                                                     uav->ReleaseAndGetAddressOf());
        return hr;
    };

    UINT lw = w, lh = h;
    for (int i = 0; i < kPyramidLevels; ++i) {
        lw = (std::max)(1u, lw / 2);
        lh = (std::max)(1u, lh / 2);
        Impl::Level& L = d.levels[i];
        L.w = lw;
        L.h = lh;
        if (FAILED(makeLuma(lw, lh, &L.texA, &L.srvA, &L.uavA)) ||
            FAILED(makeLuma(lw, lh, &L.texB, &L.srvB, &L.uavB))) {
            if (err) *err = "luma pyramid allocation failed";
            return false;
        }
    }

    d.heldValid = false;
    for (int i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = d.gridW;
        td.Height = d.gridH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = d.device->CreateTexture2D(&td, nullptr, d.held[i].ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
            hr = d.device->CreateShaderResourceView(d.held[i].Get(), nullptr,
                                                    d.heldSrv[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            if (err) *err = "held field allocation failed: " + HrString(hr);
            return false;
        }
    }

    for (int i = 0; i < Impl::kFieldCount; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = d.gridW;
        td.Height = d.gridH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16_FLOAT;  // filterable: the warp samples it bilinearly
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = d.device->CreateTexture2D(&td, nullptr, d.mv[i].ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
            hr = d.device->CreateShaderResourceView(d.mv[i].Get(), nullptr,
                                                    d.mvSrv[i].ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr))
            hr = d.device->CreateUnorderedAccessView(d.mv[i].Get(), nullptr,
                                                     d.mvUav[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            if (err) *err = "motion field allocation failed: " + HrString(hr);
            return false;
        }
    }

    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = d.gridW;
        td.Height = d.gridH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hr = d.device->CreateTexture2D(&td, nullptr, d.mvStaging.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            // Not fatal: only DebugFieldStats reads it.
            LogErr("Synth: motion readback allocation failed %s - --dump field stats are unavailable",
                   HrString(hr).c_str());
            d.mvStaging = nullptr;
        }
    }
    return true;
}

bool Synth::OfaActive() const { return impl_->ofaOn && impl_->ofa.Ready(); }

void Synth::SetOcclusionMode(int mode) {
    impl_->occMode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

int Synth::OcclusionModeActive() const { return impl_->OccModeNow(); }
const std::string& Synth::OfaReport() const { return impl_->ofa.Report(); }

bool Synth::EnableOfa(UINT gridSize, std::string* err, bool seedHints) {
    Impl& d = *impl_;
    d.ofaOn = false;
    if (d.srcW == 0 || d.srcH == 0) {
        if (err) *err = "EnableOfa before Resize";
        return false;
    }
    if (gridSize != d.cellPx) {
        // Not a nicety: the conversion shader writes one flow vector per cell,
        // so a mismatch would silently scramble the field.
        if (err)
            *err = "the flow grid must equal the motion cell size (" + std::to_string(d.cellPx) +
                   " px)";
        return false;
    }
    if (seedHints && !d.EnsureSeedGrid(err)) return false;
    if (!d.ofa.Create(d.device, d.ctx, d.srcW, d.srcH, gridSize, err, seedHints)) return false;
    if (d.ofa.GridW() < d.gridW || d.ofa.GridH() < d.gridH) {
        if (err)
            *err = "flow grid " + std::to_string(d.ofa.GridW()) + "x" +
                   std::to_string(d.ofa.GridH()) + " does not cover the field " +
                   std::to_string(d.gridW) + "x" + std::to_string(d.gridH);
        d.ofa.Destroy();
        return false;
    }
    d.ofaOn = true;
    return true;
}

bool Synth::DebugFieldStats(float* p50, float* p95, float* maxAbs, float* zeroFrac) {
    Impl& d = *impl_;
    if (!d.ctx || !d.mvStaging || d.mvFinal < 0) return false;
    d.ctx->CopyResource(d.mvStaging.Get(), d.mv[d.mvFinal].Get());
    d.ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(d.ctx->Map(d.mvStaging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::vector<float> mags;
    mags.reserve(static_cast<size_t>(d.gridW) * d.gridH);
    size_t zeros = 0;
    const auto* base = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < d.gridH; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + y * m.RowPitch);
        for (UINT x = 0; x < d.gridW; ++x) {
            const float vx = HalfToFloat(row[x * 2]);
            const float vy = HalfToFloat(row[x * 2 + 1]);
            const float mag = std::sqrt(vx * vx + vy * vy);
            if (mag < 0.25f) ++zeros;
            mags.push_back(mag);
        }
    }
    // Where the bad vectors are matters more than how many: a bad EDGE is an
    // alignment or padding bug, bad patches inside the picture are the
    // estimator failing on the content.
    {
        std::string map;
        const UINT cols = 48, rows = 20;
        for (UINT ry = 0; ry < rows; ++ry) {
            for (UINT rx = 0; rx < cols; ++rx) {
                const UINT x = rx * d.gridW / cols;
                const UINT y = ry * d.gridH / rows;
                const auto* row = reinterpret_cast<const uint16_t*>(base + y * m.RowPitch);
                const float vx = HalfToFloat(row[x * 2]);
                const float vy = HalfToFloat(row[x * 2 + 1]);
                const float mag = std::sqrt(vx * vx + vy * vy);
                map += (mag < 0.5f)    ? '.'
                       : (mag < 8.0f)  ? '1'
                       : (mag < 24.0f) ? '2'
                       : (mag < 48.0f) ? '3'
                       : (mag < 100.0f) ? '4'
                                        : '#';
            }
            map += '\n';
        }
        Log("field map (. static  1 <8px  2 <24px  3 <48px  4 <100px  # huge):\n%s", map.c_str());
    }
    d.ctx->Unmap(d.mvStaging.Get(), 0);
    if (mags.empty()) return false;
    std::sort(mags.begin(), mags.end());
    if (p50) *p50 = mags[mags.size() / 2];
    if (p95) *p95 = mags[mags.size() * 95 / 100];
    if (maxAbs) *maxAbs = mags.back();
    if (zeroFrac) *zeroFrac = static_cast<float>(zeros) / static_cast<float>(mags.size());
    return true;
}

bool Synth::InjectField(const float* xy, UINT gw, UINT gh, std::string* err) {
    Impl& d = *impl_;
    d.heldValid = false;
    if (!d.device || d.mvFinal < 0 || !d.mv[2]) {
        if (err) *err = "InjectField before Resize()";
        return false;
    }
    // A silent grid mismatch would upload the wrong rows and still produce a
    // plausible picture, which is the one failure this harness exists to make
    // impossible. Refuse instead.
    if (gw != d.gridW || gh != d.gridH) {
        char buf[160];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "InjectField: field is %ux%u but the engine's grid is %ux%u (cell %u px)",
                    gw, gh, d.gridW, d.gridH, d.cellPx);
        if (err) *err = buf;
        return false;
    }
    if (!xy) {
        if (err) *err = "InjectField: null field";
        return false;
    }

    // The field is stored as halves because the warp samples it bilinearly and
    // R16G16_FLOAT is the filterable format. Half has 11 bits of mantissa, so a
    // displacement up to 2048 px is represented to better than 1/1024 px — far
    // finer than anything the warp can act on, and the same precision the
    // estimated field already lives at, which is what keeps the oracle arm and
    // the engine arm comparable.
    std::vector<uint16_t> halves(static_cast<size_t>(gw) * gh * 2);
    for (size_t i = 0; i < halves.size(); ++i)
        halves[i] = DirectX::PackedVector::XMConvertFloatToHalf(xy[i]);

    d.ctx->UpdateSubresource(d.mv[2].Get(), 0, nullptr, halves.data(), gw * 2 * sizeof(uint16_t), 0);
    d.mvFinal = 2;
    // No anchored fields exist for an injected pair, and inventing them by
    // copying the intermediate one would hand the occlusion test two copies of
    // the same estimate while labelling them independent evidence — the exact
    // mistake `occ=self` already makes and that `occ=bidir` was built to stop.
    d.mvFwd = -1;
    d.mvBwd = -1;
    d.hasMotion = true;
    return true;
}

bool Synth::PrepareMotion(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b) {
    Impl& d = *impl_;
    if (!d.ctx || !a || !b || d.srcW == 0) return false;

    // The /2 level is built either way: the hardware path's field fix scores
    // at it and the seed pyramid starts from it, and so does the block matcher.
    d.Downsample(a, d.levels[0], true, false);
    d.Downsample(b, d.levels[0], false, false);

    if (d.ofaOn && d.ofa.Ready()) {
        // ---- hardware path: full-res luma -> NVOFA -> our field
        auto lumaFull = [&](ID3D11ShaderResourceView* src, ID3D11UnorderedAccessView* dst) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(d.ctx->Map(d.cbDs.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                DsCb c{};
                c.dstW = d.ofa.SurfaceW();
                c.dstH = d.ofa.SurfaceH();
                c.fromLuma = 0;
                c.srcW = d.srcW;
                c.srcH = d.srcH;
                memcpy(m.pData, &c, sizeof(c));
                d.ctx->Unmap(d.cbDs.Get(), 0);
            }
            ID3D11ShaderResourceView* srvs[1] = {src};
            ID3D11UnorderedAccessView* uavs[1] = {dst};
            ID3D11Buffer* cbs[1] = {d.cbDs.Get()};
            d.ctx->CSSetShader(d.csLumaFull.Get(), nullptr, 0);
            d.ctx->CSSetShaderResources(0, 1, srvs);
            d.ctx->CSSetConstantBuffers(1, 1, cbs);
            d.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            d.ctx->Dispatch(DivUp(d.ofa.SurfaceW(), 8), DivUp(d.ofa.SurfaceH(), 8), 1);
            ID3D11UnorderedAccessView* noUav[1] = {nullptr};
            d.ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
            ID3D11ShaderResourceView* noSrv[1] = {nullptr};
            d.ctx->CSSetShaderResources(0, 1, noSrv);
        };
        // SEED THE HARDWARE SEARCH with our own coarse field (G30). The hardware
        // has no pyramid of ours, so it cannot be given the candidate refinement
        // that fixed the period lock on the block-matcher path; handing it the
        // answer as an external hint is the only route in. Only the coarse pass
        // and its refinement are run — the mid and fine passes exist to reach
        // sub-pixel accuracy, and NVOFA does that part itself.
        if (d.ofa.HintsEnabled()) {
            for (int lv = 1; lv < kPyramidLevels; ++lv) {
                d.Downsample(d.levels[lv - 1].srvA.Get(), d.levels[lv], true, true);
                d.Downsample(d.levels[lv - 1].srvB.Get(), d.levels[lv], false, true);
            }
            ID3D11SamplerState* seedSamplers[2] = {d.smpPoint.Get(), d.smpLinear.Get()};
            d.ctx->CSSetSamplers(0, 2, seedSamplers);
            // On the SEED grid, not the engine's: same work per cell over 16x
            // fewer cells, because a search seed does not need per-4-px detail.
            d.RunMatchCoarseOn(d.seedUav[0].Get(), d.seedGridW, d.seedGridH, Impl::kSeedCellPx);
            d.RunCoarseFixOn(d.seedSrv[0].Get(), d.seedUav[1].Get(), d.seedGridW, d.seedGridH,
                             Impl::kSeedCellPx);
            d.RunFillHint(d.seedSrv[1].Get());
        }

        lumaFull(a, d.ofa.InputPrevUav());
        lumaFull(b, d.ofa.InputNextUav());
        // NVOFA reads the textures on its own engine, so the writes above have
        // to be submitted before Execute rather than sit in the context.
        d.ctx->Flush();

        if (d.ofa.Execute()) {
            // One Execute produced both directions. Converting each into our
            // field format costs a dispatch over the grid — nothing next to the
            // flow itself — and the backward one is negated on the way in so
            // that from here on every field means the same thing: A -> B.
            auto flowToField = [&](ID3D11ShaderResourceView* flowSrv, int outIdx, float sign) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(d.ctx->Map(d.cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    McCb c{};
                    c.gridW = d.gridW;
                    c.gridH = d.gridH;
                    c.levelW = d.srcW;
                    c.levelH = d.srcH;
                    c.levelScale = 1.0f;
                    c.cellPx = static_cast<float>(d.cellPx);
                    c.flowSign = sign;
                    memcpy(m.pData, &c, sizeof(c));
                    d.ctx->Unmap(d.cbMc.Get(), 0);
                }
                ID3D11ShaderResourceView* flow[1] = {flowSrv};
                ID3D11UnorderedAccessView* uavs[1] = {d.mvUav[outIdx].Get()};
                ID3D11Buffer* cbs[1] = {d.cbMc.Get()};
                d.ctx->CSSetShader(d.csFlowToField.Get(), nullptr, 0);
                d.ctx->CSSetShaderResources(6, 1, flow);
                d.ctx->CSSetConstantBuffers(2, 1, cbs);
                d.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
                d.ctx->Dispatch(DivUp(d.gridW, 8), DivUp(d.gridH, 8), 1);
                ID3D11UnorderedAccessView* noUav[1] = {nullptr};
                d.ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
                ID3D11ShaderResourceView* noSrv[1] = {nullptr};
                d.ctx->CSSetShaderResources(6, 1, noSrv);
            };

            // The forward flow, three times over: re-anchored to the
            // intermediate frame for the warp to steer by, and left on A as the
            // occlusion evidence. Both get the smoothness prior the hardware
            // lacks (G24) — an unfixed field would make the occlusion test
            // disagree wherever the hardware locked onto the wrong period,
            // which is precisely where it must not.
            flowToField(d.ofa.FlowFwdSrv(), 0, 1.0f);
            d.RunFieldFix(0, 1, 0.5f, 0.5f);
            d.RunSmoothPass(1, 2);
            d.mvFinal = 2;

            const bool wantSides = d.occMode > 0 && d.ofa.FlowBwdSrv() != nullptr;
            if (wantSides) {
                d.RunFieldFix(0, 1, 0.0f, 0.0f);
                d.RunSmoothPass(1, 3);
                flowToField(d.ofa.FlowBwdSrv(), 0, -1.0f);
                d.RunFieldFix(0, 1, 1.0f, 0.0f);
                d.RunSmoothPass(1, 4);
                d.mvFwd = 3;
                d.mvBwd = 4;
            } else {
                d.mvFwd = -1;
                d.mvBwd = -1;
            }

            d.ctx->CSSetShader(nullptr, nullptr, 0);
            d.hasMotion = true;
            return true;
        }
        // Execute failed: fall through to the block matcher for this pair.
    }

    for (int i = 1; i < kPyramidLevels; ++i) {
        d.Downsample(d.levels[i - 1].srvA.Get(), d.levels[i], true, true);
        d.Downsample(d.levels[i - 1].srvB.Get(), d.levels[i], false, true);
    }

    ID3D11SamplerState* csSamplers[2] = {d.smpPoint.Get(), d.smpLinear.Get()};
    d.ctx->CSSetSamplers(0, 2, csSamplers);

    auto runMatch = [&](ID3D11ComputeShader* cs, const Impl::Level& L, float levelScale,
                        float stepLvl, bool useIn, int inIdx, int outIdx, float lambda = 0.0f,
                        float magPrior = 0.0f, float magCap = 0.0f) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(d.ctx->Map(d.cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            McCb c{};
            c.gridW = d.gridW;
            c.gridH = d.gridH;
            c.levelW = L.w;
            c.levelH = L.h;
            c.levelScale = levelScale;
            c.useIn = useIn ? 1u : 0u;
            c.stepLvl = stepLvl;
            c.cellPx = static_cast<float>(d.cellPx);
            c.lambda = lambda;
            c.magPrior = magPrior;
            c.magCap = magCap;
            memcpy(m.pData, &c, sizeof(c));
            d.ctx->Unmap(d.cbMc.Get(), 0);
        }
        ID3D11ShaderResourceView* srvs[3] = {L.srvA.Get(), L.srvB.Get(),
                                             (useIn && inIdx >= 0) ? d.mvSrv[inIdx].Get() : nullptr};
        ID3D11UnorderedAccessView* uavs[1] = {d.mvUav[outIdx].Get()};
        ID3D11Buffer* cbs[1] = {d.cbMc.Get()};
        d.ctx->CSSetShader(cs, nullptr, 0);
        d.ctx->CSSetShaderResources(3, 3, srvs);  // t3 lumaA, t4 lumaB, t5 mvIn
        d.ctx->CSSetConstantBuffers(2, 1, cbs);
        d.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        d.ctx->Dispatch(DivUp(d.gridW, 8), DivUp(d.gridH, 8), 1);
        ID3D11UnorderedAccessView* noUav[1] = {nullptr};
        d.ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
        ID3D11ShaderResourceView* noSrv[3] = {};
        d.ctx->CSSetShaderResources(3, 3, noSrv);
    };

    auto runSmooth = [&](int inIdx, int outIdx) { d.RunSmoothPass(inIdx, outIdx); };
    auto runSmoothUnused = [&](int inIdx, int outIdx) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(d.ctx->Map(d.cbMc.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            McCb c{};
            c.gridW = d.gridW;
            c.gridH = d.gridH;
            c.levelW = d.levels[0].w;
            c.levelH = d.levels[0].h;
            c.levelScale = 1.0f;
            c.useIn = 1;
            c.stepLvl = 1.0f;
            c.cellPx = static_cast<float>(d.cellPx);
            memcpy(m.pData, &c, sizeof(c));
            d.ctx->Unmap(d.cbMc.Get(), 0);
        }
        ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, d.mvSrv[inIdx].Get()};
        ID3D11UnorderedAccessView* uavs[1] = {d.mvUav[outIdx].Get()};
        ID3D11Buffer* cbs[1] = {d.cbMc.Get()};
        d.ctx->CSSetShader(d.csSmooth.Get(), nullptr, 0);
        d.ctx->CSSetShaderResources(3, 3, srvs);
        d.ctx->CSSetConstantBuffers(2, 1, cbs);
        d.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        d.ctx->Dispatch(DivUp(d.gridW, 8), DivUp(d.gridH, 8), 1);
        ID3D11UnorderedAccessView* noUav[1] = {nullptr};
        d.ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
        ID3D11ShaderResourceView* noSrv[3] = {};
        d.ctx->CSSetShaderResources(3, 3, noSrv);
    };

    // Each pass covers the granularity the previous one left:
    //   /8, step 16 full-res px, +-6 steps -> +-96 px of motion
    //   /4, step  4 full-res px, +-3 steps -> +-12 px  (needs +-8)
    //   /2, step  1 full-res px, +-3 steps -> +-3 px   (needs +-2)
    // The last step is a whole FULL-RES pixel: a hard moving edge placed on a
    // 4-px grid is visibly ragged, and this is what removes that.
    // The median smoothing between the last two passes is not cosmetic — it is
    // what the final pass regularises against (gLambda), so an ambiguous cell
    // inside a uniform object follows its neighbours instead of tearing.
    // G30, THE PERIOD LOCK, AND WHY THE FIX IS NOT A MAGNITUDE PRIOR.
    //
    // On content that repeats along its own direction of travel the true vector
    // v and the alias v +- P (P the texture period) are both photometrically
    // perfect, so the matching cost cannot separate them — and the alias is
    // perfect at t=0 and t=1 while being catastrophically wrong at every t in
    // between, which is the only frame this engine emits. MEASURED on A4: the
    // engine warped the background by -48.2 px/interval against a true -4.75,
    // an offset of -43.45 that matches the texture's self-similarity lag of
    // 43.44 px to 0.01 px.
    //
    // The failure is committed HERE, at the /8 coarse level, and the reason is
    // not the search range. Downsampling to /8 destroys exactly the frequencies
    // that disambiguate the period — A4's background carries 11 px and 23 px
    // components, both sub-Nyquist at /8 — while the 43 px square wave survives
    // as 5.4 px. Measured symmetric SAD: at FULL resolution the truth wins
    // (0.0090 against the alias's 0.0262, 2.9x), at /8 it INVERTS (0.0163
    // against 0.0024, 6.9x the wrong way).
    //
    // A magnitude prior at this level does fix A4 — and it was tried and
    // REJECTED on measurement. It works by refusing long vectors, so it caps
    // the motion the matcher can track. On a rigid pan of a real photograph,
    // lambda = 0.12 scored 83.82 dB at 8 px/interval and then collapsed: 38.07
    // at 16, 22.35 at 32, 17.55 at 48, 15.90 at 64, against 61.38 / 75.69 /
    // 79.46 / 80.40 with no prior at all. That is a 65 dB regression on fast
    // pans, which the analytic scenes could not see because every one of them
    // moves slower than 12 px/interval, and which the photographic corpus could
    // not see because Meridian is a STATIC camera (measured: 0.0 px of global
    // displacement at every stride) whose large vectors are a 1 % tail.
    //
    // What works instead: re-score the coarse winner at the /2 luma, where the
    // disambiguating frequencies are still present, against zero and the
    // neighbourhood median. CSFieldFix already does exactly that, so this is a
    // reuse and not a new mechanism. The alias loses to zero because it is only
    // periodic in the component that survived downsampling; a GENUINE long
    // vector beats zero on the same test and is kept, which is precisely the
    // property a magnitude prior cannot have.
    // gMagPrior stays ZERO. Both a bounded and an unbounded prior against long
    // vectors were built and measured here, and both were rejected: see N23.
    // The mechanism is kept in the shader because it costs nothing when zero and
    // because the next attempt at G30 will want to A/B against it.
    runMatch(d.csMatchCoarse.Get(), d.levels[2], 8.0f, 2.0f, false, -1, 0);
    d.RunCoarseFix(0, 1);
    runMatch(d.csMatchMid.Get(), d.levels[1], 4.0f, 1.0f, true, 1, 0);
    runSmooth(0, 2);
    runMatch(d.csMatchFine.Get(), d.levels[0], 2.0f, 0.5f, true, 2, 0, 0.08f);
    runSmooth(0, 2);

    d.ctx->CSSetShader(nullptr, nullptr, 0);
    d.mvFinal = 2;
    // The matcher produces one field and no second opinion, so the occlusion
    // test has to fall back to the self-consistency heuristic. Clearing these
    // matters on the path where a hardware Execute failed mid-stream: the warp
    // would otherwise read fields belonging to the previous pair.
    d.mvFwd = -1;
    d.mvBwd = -1;
    d.hasMotion = true;
    return true;
}

bool Synth::Blend(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11ShaderResourceView* a,
                  ID3D11ShaderResourceView* b, float t) {
    Impl& d = *impl_;
    if (!d.ctx || !rtv || !a || !b) return false;
    d.SetParams(t);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* srvs[3] = {a, b, nullptr};
    ID3D11SamplerState* samplers[2] = {d.smpPoint.Get(), d.smpLinear.Get()};
    ID3D11Buffer* cbs[1] = {d.cbParams.Get()};
    ID3D11RenderTargetView* rtvs[1] = {rtv};

    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->IASetInputLayout(nullptr);
    d.ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    d.ctx->PSSetShader(d.psBlend.Get(), nullptr, 0);
    d.ctx->PSSetShaderResources(0, 3, srvs);
    d.ctx->PSSetSamplers(0, 2, samplers);
    d.ctx->PSSetConstantBuffers(0, 1, cbs);
    d.ctx->RSSetViewports(1, &vp);
    d.ctx->RSSetState(d.raster.Get());
    d.ctx->OMSetBlendState(d.blendOff.Get(), nullptr, 0xFFFFFFFF);
    d.ctx->OMSetDepthStencilState(d.depthOff.Get(), 0);
    d.ctx->OMSetRenderTargets(1, rtvs, nullptr);
    d.ctx->Draw(3, 0);

    d.ClearSrvs();
    ID3D11RenderTargetView* noRt[1] = {nullptr};
    d.ctx->OMSetRenderTargets(1, noRt, nullptr);
    return true;
}

void Synth::HoldFields() {
    Impl& d = *impl_;
    d.heldValid = false;
    if (!d.ctx || d.mvFinal < 0 || !d.held[0]) return;
    d.ctx->CopyResource(d.held[0].Get(), d.mv[d.mvFinal].Get());
    d.heldOccMode = d.OccModeNow();
    if (d.heldOccMode > 0) {
        d.ctx->CopyResource(d.held[1].Get(), d.mv[d.mvFwd].Get());
        d.ctx->CopyResource(d.held[2].Get(), d.mv[d.mvBwd].Get());
    }
    d.heldValid = true;
}

void Synth::DropHeldFields() { impl_->heldValid = false; }
bool Synth::HasHeldFields() const { return impl_->heldValid; }

bool Synth::Warp(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11ShaderResourceView* a,
                 ID3D11ShaderResourceView* b, float t, bool held) {
    Impl& d = *impl_;
    if (!d.ctx || !rtv || !a || !b) return false;
    if (held && !d.heldValid) return Blend(rtv, w, h, a, b, t);
    if (!held && d.mvFinal < 0) return Blend(rtv, w, h, a, b, t);
    const int occ = held ? d.heldOccMode : d.OccModeNow();
    d.SetParams(t, occ);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MaxDepth = 1.0f;

    const bool bidir = occ > 0;
    ID3D11ShaderResourceView* srvs[3] = {a, b, held ? d.heldSrv[0].Get() : d.mvSrv[d.mvFinal].Get()};
    ID3D11ShaderResourceView* sides[2] = {
        bidir ? (held ? d.heldSrv[1].Get() : d.mvSrv[d.mvFwd].Get()) : nullptr,
        bidir ? (held ? d.heldSrv[2].Get() : d.mvSrv[d.mvBwd].Get()) : nullptr};
    ID3D11SamplerState* samplers[2] = {d.smpPoint.Get(), d.smpLinear.Get()};
    ID3D11Buffer* cbs[1] = {d.cbParams.Get()};
    ID3D11RenderTargetView* rtvs[1] = {rtv};

    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->IASetInputLayout(nullptr);
    d.ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    d.ctx->PSSetShader(d.psWarp.Get(), nullptr, 0);
    d.ctx->PSSetShaderResources(0, 3, srvs);
    d.ctx->PSSetShaderResources(7, 2, sides);
    d.ctx->PSSetSamplers(0, 2, samplers);
    d.ctx->PSSetConstantBuffers(0, 1, cbs);
    d.ctx->RSSetViewports(1, &vp);
    d.ctx->RSSetState(d.raster.Get());
    d.ctx->OMSetBlendState(d.blendOff.Get(), nullptr, 0xFFFFFFFF);
    d.ctx->OMSetDepthStencilState(d.depthOff.Get(), 0);
    d.ctx->OMSetRenderTargets(1, rtvs, nullptr);
    d.ctx->Draw(3, 0);

    d.ClearSrvs();
    ID3D11RenderTargetView* noRt[1] = {nullptr};
    d.ctx->OMSetRenderTargets(1, noRt, nullptr);
    return true;
}

}  // namespace nsp
