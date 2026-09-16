// nsp_dump.cpp — texture -> PPM.

#include "nsp_dump.h"

#include <wrl/client.h>

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nsp {

bool DumpTextureToPpm(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                      const char* path) {
    if (!device || !ctx || !tex || !path) return false;

    D3D11_TEXTURE2D_DESC sd{};
    tex->GetDesc(&sd);
    if (sd.Width == 0 || sd.Height == 0) return false;

    D3D11_TEXTURE2D_DESC td = sd;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &staging);
    if (FAILED(hr)) {
        LogErr("dump: CreateTexture2D(staging %ux%u fmt=%d) failed %s", sd.Width, sd.Height,
               static_cast<int>(sd.Format), HrString(hr).c_str());
        return false;
    }

    ctx->CopyResource(staging.Get(), tex);
    ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);  // blocking: one-shot path
    if (FAILED(hr)) {
        LogErr("dump: Map failed %s", HrString(hr).c_str());
        return false;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        ctx->Unmap(staging.Get(), 0);
        LogErr("dump: could not open %s for writing", path);
        return false;
    }
    fprintf(f, "P6\n%u %u\n255\n", sd.Width, sd.Height);

    std::vector<uint8_t> row(static_cast<size_t>(sd.Width) * 3);
    const auto* base = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < sd.Height; ++y) {
        const uint8_t* src = base + static_cast<size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < sd.Width; ++x) {
            // BGRA -> RGB. Every texture this dumps is a B8G8R8A8 family format.
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    ctx->Unmap(staging.Get(), 0);
    Log("dump: wrote %s (%ux%u)", path, sd.Width, sd.Height);
    return true;
}

}  // namespace nsp
