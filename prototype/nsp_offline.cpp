// nsp_offline.cpp — the offline quality instrument.

#include "nsp_offline.h"

#include "nsp_image.h"
#include "nsp_synth.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdio>
#include <map>

using Microsoft::WRL::ComPtr;

namespace nsp {
namespace {

// A source frame on the GPU, in exactly the layout the capture ring uses:
// B8G8R8A8_TYPELESS viewed as _SRGB, so the shaders see linear light (G21).
struct GpuFrame {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
};

bool MakeSourceTexture(ID3D11Device* dev, UINT w, UINT h, GpuFrame* out, std::string* err) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &out->tex);
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = dev->CreateShaderResourceView(out->tex.Get(), &sd, &out->srv);
    }
    if (FAILED(hr)) {
        if (err) *err = "source texture creation failed: " + HrString(hr);
        return false;
    }
    return true;
}

// Reads a texture back into tightly packed BGRA. Blocking on purpose: this is
// the measurement path, not the hot path.
bool ReadbackBgra(ID3D11DeviceContext* ctx, ID3D11Texture2D* staging, ID3D11Texture2D* src, UINT w,
                  UINT h, std::vector<uint8_t>* out, std::string* err) {
    ctx->CopyResource(staging, src);
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        if (err) *err = "readback Map failed: " + HrString(hr);
        return false;
    }
    out->resize(static_cast<size_t>(w) * h * 4);
    const auto* base = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < h; ++y)
        memcpy(out->data() + static_cast<size_t>(y) * w * 4, base + static_cast<size_t>(y) * m.RowPitch,
               static_cast<size_t>(w) * 4);
    ctx->Unmap(staging, 0);
    return true;
}

size_t MaxAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return 256;
    size_t worst = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        // Alpha is not part of the picture: the overlay is opaque and the
        // shaders write 1.0, while a PNG may carry anything there.
        if ((i & 3) == 3) continue;
        const size_t d = static_cast<size_t>(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

std::string TName(double t) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "t%03d", static_cast<int>(t * 100.0 + 0.5));
    return buf;
}

// Raw float32 in, with the count asserted. The count check is the whole point:
// a field written for a different cell size is exactly the kind of mistake that
// produces a picture rather than an error, and it would be read as a quality
// result.
bool ReadRawFloats(const char* path, size_t wantCount, std::vector<float>* out, std::string* err) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        if (err) *err = "cannot open";
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes < 0 || static_cast<size_t>(bytes) != wantCount * sizeof(float)) {
        char buf[160];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "expected %zu floats (%zu bytes), file has %ld bytes",
                    wantCount, wantCount * sizeof(float), bytes);
        if (err) *err = buf;
        fclose(f);
        return false;
    }
    out->resize(wantCount);
    const size_t got = fread(out->data(), sizeof(float), wantCount, f);
    fclose(f);
    if (got != wantCount) {
        if (err) *err = "short read";
        return false;
    }
    return true;
}

}  // namespace

int RunOffline(const OfflineOptions& opt) {
    if (opt.frames.size() < 2) {
        LogErr("offline: need at least two frames, got %zu", opt.frames.size());
        return 2;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    std::string err;
    if (!CreateRenderDevice(&device, &ctx, &err)) {
        LogErr("offline: %s", err.c_str());
        return 2;
    }

    // First frame decides the size; every other frame must match, or the
    // comparison would be between different pictures.
    std::vector<uint8_t> pixA, pixB;
    UINT w = 0, h = 0;
    if (!LoadImageBgra(opt.frames[0], &pixA, &w, &h, &err)) {
        LogErr("offline: %s", err.c_str());
        return 2;
    }
    static const char* kOccNames[3] = {"self", "bidir", "bidir+cand"};
    Log("offline: %zu frames, %ux%u, mode=%s, motion=%s, cell=%d, occ=%s", opt.frames.size(), w, h,
        opt.mode.c_str(), opt.injectDir.empty() ? (opt.useOfa ? "ofa" : "blocks") : "ORACLE",
        opt.useOfa ? opt.ofaGrid : opt.cellPx, kOccNames[opt.occMode < 0 ? 0 : (opt.occMode > 2 ? 2 : opt.occMode)]);
    if (!opt.injectDir.empty()) {
        // Say it loudly. A run whose field came from disk is not a measurement
        // of the estimator, and a log that did not distinguish the two would let
        // an oracle number be quoted as an engine number.
        Log("offline: ORACLE FLOW from %s - the estimator is NOT under test in this run; "
            "occlusion mode drops to 'self' because no anchored fields are injected",
            opt.injectDir.c_str());
    }

    Synth synth;
    if (!synth.Create(device.Get(), ctx.Get(), &err)) {
        LogErr("offline: synth create failed: %s", err.c_str());
        return 2;
    }
    synth.SetOcclusionMode(opt.occMode);
    if (opt.warpLab != 0) {
        if (!synth.SetWarpLab(opt.warpLab, &err)) {
            LogErr("offline: warp lab: %s", err.c_str());
            return 2;
        }
        synth.SetWarpLabParam(opt.warpLabParam);
        Log("offline: WARP LAB mode %d (p %.4f) - PSWarpLab, not the shipping warp", opt.warpLab,
            opt.warpLabParam);
    }
    const UINT cell = opt.useOfa ? static_cast<UINT>(opt.ofaGrid) : static_cast<UINT>(opt.cellPx);
    if (!synth.Resize(w, h, &err, cell)) {
        LogErr("offline: synth resize failed: %s", err.c_str());
        return 2;
    }
    if (opt.useOfa) {
        std::string ofaErr;
        if (!synth.EnableOfa(cell, &ofaErr, opt.ofaHints)) {
            LogErr("offline: hardware flow unavailable (%s) - REFUSING to silently fall back, "
                   "because a run labelled 'ofa' that measured the block matcher would poison "
                   "every comparison it appears in",
                   ofaErr.c_str());
            printf("%s", synth.OfaReport().c_str());
            return 2;
        }
        printf("--- NVOFA ---\n%s-------------\n", synth.OfaReport().c_str());
    }

    GpuFrame texA, texB;
    if (!MakeSourceTexture(device.Get(), w, h, &texA, &err) ||
        !MakeSourceTexture(device.Get(), w, h, &texB, &err)) {
        LogErr("offline: %s", err.c_str());
        return 2;
    }

    ComPtr<ID3D11Texture2D> rt, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &rt);
        if (SUCCEEDED(hr)) {
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            hr = device->CreateRenderTargetView(rt.Get(), &rd, &rtv);
        }
        if (SUCCEEDED(hr)) {
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = device->CreateTexture2D(&td, nullptr, &staging);
        }
        if (FAILED(hr)) {
            LogErr("offline: render target creation failed %s", HrString(hr).c_str());
            return 2;
        }
    }

    // Create the output directory rather than failing one PNG at a time on
    // stderr, which is exactly how the first real run lost its images.
    if (!opt.outDir.empty()) {
        std::string acc;
        for (size_t i = 0; i <= opt.outDir.size(); ++i) {
            if (i == opt.outDir.size() || opt.outDir[i] == '/' || opt.outDir[i] == '\\') {
                if (!acc.empty() && acc != "." && acc != "..") CreateDirectoryA(acc.c_str(), nullptr);
            }
            if (i < opt.outDir.size()) acc += opt.outDir[i];
        }
    }

    const bool isMc = opt.mode == "mc";
    const bool isBlend = opt.mode == "blend";
    const bool isPass = opt.mode == "passthrough";
    if (!isMc && !isBlend && !isPass) {
        LogErr("offline: unknown mode '%s'", opt.mode.c_str());
        return 2;
    }

    // The warm-up, on the LAST pair, which is not a detail. MEASURED on GB206:
    // NVOFA's field for a pair depends on the content of the pair executed
    // before it, so the first pair of a run is the only one estimated without a
    // predecessor — and on a repeat pass it acquires one (the run's last pair)
    // and changes. Priming with the last pair gives pass 0 the same predecessor
    // every later pass has, which is what makes the repeat gate compare two
    // identical experiments instead of two different ones.
    if (opt.warmup && isMc) {
        UINT aw = 0, ah = 0, bw = 0, bh = 0;
        const size_t last = opt.frames.size() - 1;
        if (LoadImageBgra(opt.frames[last - 1], &pixA, &aw, &ah, &err) &&
            LoadImageBgra(opt.frames[last], &pixB, &bw, &bh, &err) && aw == w && bw == w) {
            ctx->UpdateSubresource(texA.tex.Get(), 0, nullptr, pixA.data(), w * 4, 0);
            ctx->UpdateSubresource(texB.tex.Get(), 0, nullptr, pixB.data(), w * 4, 0);
            synth.PrepareMotion(texA.srv.Get(), texB.srv.Get());
        }
    }

    // hash -> the label that produced it, so a repeat run can name what changed.
    std::map<std::string, std::string> firstPass;
    int failures = 0;
    const UINT rowBytes = w * 4;

    for (int rep = 0; rep < (std::max)(1, opt.repeat); ++rep) {
        for (size_t i = 0; i + 1 < opt.frames.size(); ++i) {
            UINT aw = 0, ah = 0, bw = 0, bh = 0;
            if (!LoadImageBgra(opt.frames[i], &pixA, &aw, &ah, &err) ||
                !LoadImageBgra(opt.frames[i + 1], &pixB, &bw, &bh, &err)) {
                LogErr("offline: %s", err.c_str());
                return 2;
            }
            if (aw != w || ah != h || bw != w || bh != h) {
                LogErr("offline: frame %zu is %ux%u but the sequence is %ux%u", i, aw, ah, w, h);
                return 2;
            }
            ctx->UpdateSubresource(texA.tex.Get(), 0, nullptr, pixA.data(), rowBytes, 0);
            ctx->UpdateSubresource(texB.tex.Get(), 0, nullptr, pixB.data(), rowBytes, 0);

            if (isMc && opt.injectDir.empty()) synth.PrepareMotion(texA.srv.Get(), texB.srv.Get());

            // The endpoint identity is DERIVED from the warp shader, not hoped
            // for: at t=0 it fetches A at the identity offset and weights B by
            // zero, so mc(t=0) must be A byte for byte. A non-zero difference
            // here is a plumbing fault — a colour-managed PNG, a 16-bit
            // promotion, an SRV built UNORM instead of UNORM_SRGB — and every
            // other number in the run would inherit it.
            std::vector<double> ts = opt.ts;
            if (opt.checkEndpoints && isMc) {
                ts.insert(ts.begin(), 0.0);
                ts.push_back(1.0);
            }

            for (double t : ts) {
                if (isMc && !opt.injectDir.empty()) {
                    char fn[160];
                    _snprintf_s(fn, sizeof(fn), _TRUNCATE, "%s/pair%04zu_%s.f32",
                                opt.injectDir.c_str(), i, TName(t).c_str());
                    UINT gw = 0, gh = 0;
                    synth.MotionGrid(&gw, &gh);
                    std::vector<float> field;
                    // The endpoints carry no ground-truth frame, so a corpus has
                    // no field for them — and needs none. At t=0 the warp fetches
                    // A at uv - v*0 and at t=1 it fetches B at uv + v*0, so the
                    // output is A (or B) for ANY field whatsoever. Injecting
                    // zeros there is not a fudge, it is the one value that is
                    // provably as correct as every other, and it keeps G0
                    // checkable on the oracle arm. G0 is the gate that catches a
                    // colour-managed PNG or a non-SRGB view, and an arm exempt
                    // from it is an arm with no floor under it.
                    const bool endpoint = (t == 0.0 || t == 1.0);
                    if (!ReadRawFloats(fn, static_cast<size_t>(gw) * gh * 2, &field, &err)) {
                        if (!endpoint) {
                            LogErr("offline: oracle field %s: %s", fn, err.c_str());
                            return 2;
                        }
                        field.assign(static_cast<size_t>(gw) * gh * 2, 0.0f);
                    }
                    if (!synth.InjectField(field.data(), gw, gh, &err)) {
                        LogErr("offline: %s", err.c_str());
                        return 2;
                    }
                }
                if (isPass) {
                    ctx->CopyResource(rt.Get(), texB.tex.Get());
                } else if (isBlend) {
                    synth.Blend(rtv.Get(), w, h, texA.srv.Get(), texB.srv.Get(),
                                static_cast<float>(t));
                } else {
                    synth.Warp(rtv.Get(), w, h, texA.srv.Get(), texB.srv.Get(),
                               static_cast<float>(t));
                }

                std::vector<uint8_t> out;
                if (!ReadbackBgra(ctx.Get(), staging.Get(), rt.Get(), w, h, &out, &err)) {
                    LogErr("offline: %s", err.c_str());
                    return 2;
                }

                char label[128];
                _snprintf_s(label, sizeof(label), _TRUNCATE, "pair%04zu_%s", i, TName(t).c_str());
                const std::string hash = Sha256Hex(out.data(), out.size());

                if (opt.checkEndpoints && isMc && (t == 0.0 || t == 1.0)) {
                    const std::vector<uint8_t>& ref = (t == 0.0) ? pixA : pixB;
                    const size_t diff = MaxAbsDiff(out, ref);
                    if (diff != 0) {
                        ++failures;
                        LogErr("G0 FAIL %s: mc(t=%.0f) differs from %s by %zu levels (must be 0)",
                               label, t, t == 0.0 ? "A" : "B", diff);
                    } else if (rep == 0 && i == 0) {
                        Log("G0 ok: mc(t=%.0f) reproduces %s bit-exactly", t, t == 0.0 ? "A" : "B");
                    }
                    continue;  // endpoints are a gate, not an output frame
                }
                if (isPass) {
                    const size_t diff = MaxAbsDiff(out, pixB);
                    if (diff != 0) {
                        ++failures;
                        LogErr("G0 FAIL %s: passthrough differs from its input by %zu levels", label,
                               diff);
                    } else if (rep == 0 && i == 0) {
                        Log("G0 ok: passthrough reproduces the input bit-exactly");
                    }
                }

                if (rep == 0) {
                    firstPass[label] = hash;
                    if (!opt.outDir.empty()) {
                        const std::string path = opt.outDir + "/" + label + ".png";
                        if (!SaveImageBgraPng(path, out.data(), w, h, rowBytes, &err))
                            LogErr("offline: %s", err.c_str());
                    }
                    printf("%s  %s\n", label, hash.c_str());
                } else {
                    const auto it = firstPass.find(label);
                    if (it == firstPass.end() || it->second != hash) {
                        ++failures;
                        LogErr("G1 FAIL %s: repeat %d produced a different image (%s vs %s)", label,
                               rep, hash.c_str(), it == firstPass.end() ? "<missing>"
                                                                        : it->second.c_str());
                    }
                }
            }
        }
        if (rep > 0 && failures == 0)
            Log("G1 ok: repeat %d reproduced every frame bit-exactly (%zu frames)", rep,
                firstPass.size());
    }

    fflush(stdout);
    if (failures) {
        LogErr("offline: %d assertion(s) FAILED", failures);
        return 1;
    }
    Log("offline: %zu synthesized frames, all assertions passed", firstPass.size());
    return 0;
}

}  // namespace nsp
