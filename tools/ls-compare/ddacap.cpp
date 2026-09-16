// ddacap — record what a monitor actually shows, frame by frame.
//
// The one instrument in which Lossless Scaling and our engine can be compared on
// the same content (kb/quality-harness.md, "Instrument B"): DXGI Desktop
// Duplication of the composed desktop, so whatever window presents the picture —
// LS's, ours, or the browser's own — is what gets recorded.
//
// Every frame whose desktop image changed is copied (a crop of it) to one raw
// BGRA file; an index CSV carries, per frame, the QPC time it was acquired, DXGI's
// LastPresentTime, AccumulatedFrames (> 1 means presents were coalesced and the
// recording MISSED frames), and a 64-bit hash of the crop so repeated pictures
// can be counted without reading the pixels back.
//
//   ddacap.exe --at X,Y --rect L,T,W,H --seconds S --out PREFIX
//     --at     a desktop point on the monitor to record (physical px)
//     --rect   crop in that monitor's coordinates (default: whole monitor)
//   writes PREFIX.raw (W*H*4 bytes per frame, BGRA, top-down) and PREFIX.csv

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int64_t Qpc() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    POINT at{0, 0};
    LONG cl = 0, ct = 0, cw = 0, ch = 0;
    double seconds = 3.0;
    std::string out = "ddacap";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            sscanf_s(argv[++i], "%ld,%ld", &at.x, &at.y);
        } else if (!strcmp(argv[i], "--rect") && i + 1 < argc) {
            sscanf_s(argv[++i], "%ld,%ld,%ld,%ld", &cl, &ct, &cw, &ch);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        }
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    RECT mon{};
    for (UINT a = 0; !output; ++a) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> op;
            if (ad->EnumOutputs(o, &op) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            op->GetDesc(&od);
            if (PtInRect(&od.DesktopCoordinates, at)) {
                adapter = ad;
                output = op;
                mon = od.DesktopCoordinates;
                break;
            }
        }
    }
    if (!output) {
        fprintf(stderr, "no output contains %ld,%ld\n", at.x, at.y);
        return 3;
    }
    const LONG monW = mon.right - mon.left, monH = mon.bottom - mon.top;
    if (cw <= 0 || ch <= 0) {
        cl = 0;
        ct = 0;
        cw = monW;
        ch = monH;
    }
    if (cl < 0 || ct < 0 || cl + cw > monW || ch + ct > monH) {
        fprintf(stderr, "crop %ld,%ld %ldx%ld outside the %ldx%ld monitor\n", cl, ct, cw, ch, monW, monH);
        return 3;
    }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return 3;
    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = E_FAIL;
    ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(output.As(&out5))) {
        DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = out5->DuplicateOutput1(dev.Get(), 0, 1, fmts, &dup);
    }
    if (!dup) {
        fprintf(stderr, "DuplicateOutput1 failed 0x%08lX\n", hr);
        return 3;
    }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = static_cast<UINT>(cw);
    sd.Height = static_cast<UINT>(ch);
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return 3;

    FILE* raw = nullptr;
    FILE* csv = nullptr;
    fopen_s(&raw, (out + ".raw").c_str(), "wb");
    fopen_s(&csv, (out + ".csv").c_str(), "w");
    if (!raw || !csv) return 4;
    fprintf(csv, "# monitor %ld,%ld %ldx%ld crop %ld,%ld %ldx%ld\n", mon.left, mon.top, monW, monH, cl,
            ct, cw, ch);
    fprintf(csv, "index,qpc_ms,last_present_qpc_ms,accumulated,hash\n");

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const auto toMs = [&](int64_t q) { return static_cast<double>(q) * 1000.0 / freq.QuadPart; };
    // In memory during the recording, to disk after it: writing 8 MB per frame
    // inside the loop capped the recorder near 22 frames a second and lost 176 of
    // LS's presents in 3 s (measured, first probe run).
    const size_t frameBytes = static_cast<size_t>(cw) * ch * 4;
    struct Rec {
        double qpcMs, presentMs;
        UINT accumulated;
        uint64_t hash;
    };
    std::vector<std::vector<uint8_t>> frames;
    std::vector<Rec> recs;
    frames.reserve(static_cast<size_t>(seconds * 180.0) + 16);
    double worstWorkMs = 0.0;

    const int64_t t0 = Qpc();
    uint64_t index = 0, missed = 0;
    while (toMs(Qpc() - t0) < seconds * 1000.0) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        hr = dup->AcquireNextFrame(50, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            fprintf(stderr, "AcquireNextFrame 0x%08lX\n", hr);
            break;
        }
        const int64_t acquired = Qpc();
        if (fi.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> tex;
            res.As(&tex);
            D3D11_BOX box{static_cast<UINT>(cl), static_cast<UINT>(ct), 0, static_cast<UINT>(cl + cw),
                          static_cast<UINT>(ct + ch), 1};
            ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<uint8_t> buf(frameBytes);
                for (LONG y = 0; y < ch; ++y)
                    memcpy(buf.data() + static_cast<size_t>(y) * cw * 4,
                           static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                           static_cast<size_t>(cw) * 4);
                ctx->Unmap(staging.Get(), 0);
                if (fi.AccumulatedFrames > 1) missed += fi.AccumulatedFrames - 1;
                recs.push_back({toMs(acquired), toMs(fi.LastPresentTime.QuadPart), fi.AccumulatedFrames,
                                Fnv1a(buf.data(), buf.size())});
                frames.push_back(std::move(buf));
                ++index;
                worstWorkMs = (std::max)(worstWorkMs, toMs(Qpc() - acquired));
            }
        }
        dup->ReleaseFrame();
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        fwrite(frames[i].data(), 1, frames[i].size(), raw);
        fprintf(csv, "%zu,%.3f,%.3f,%u,%016llx\n", i, recs[i].qpcMs, recs[i].presentMs,
                recs[i].accumulated, static_cast<unsigned long long>(recs[i].hash));
    }
    fclose(raw);
    fclose(csv);
    printf("%llu frames in %.1f s, %llu coalesced presents (missed), %ldx%ld crop, worst per-frame work %.1f ms\n",
           static_cast<unsigned long long>(index), seconds, static_cast<unsigned long long>(missed), cw, ch,
           worstWorkMs);
    return 0;
}
