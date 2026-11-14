// Exercise the production shader with identical, deterministic inputs on both paths.
#include "../nsp_synth.cpp"
#include <cstdio>
#include <random>
#include <stdexcept>

static void Require(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error(nsp::HrString(hr));
}

int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        Require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                D3D11_SDK_VERSION, &device, nullptr, &ctx));
        ComPtr<ID3DBlob> code, errors;
        const HRESULT compiled = D3DCompile(nsp::kShaderSource, sizeof(nsp::kShaderSource)-1,
            "nsp_synth.hlsl", nullptr, nullptr, "CSFieldCohere", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        Require(compiled);
        ComPtr<ID3D11ComputeShader> shader, visibility;
        Require(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        code.Reset(); errors.Reset();
        Require(D3DCompile(nsp::kShaderSource, sizeof(nsp::kShaderSource)-1,
            "nsp_synth.hlsl", nullptr, nullptr, "CSFieldVisibility", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors));
        Require(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&visibility));
        ComPtr<ID3D11Buffer> cb;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(nsp::McCb);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Require(device->CreateBuffer(&bd, nullptr, &cb));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        Require(device->CreateSamplerState(&sd, &sampler));
        std::mt19937 rng(0x53504c59);
        int cases = 0;
        for (const auto dims : {std::pair<UINT,UINT>{17,13}, {64,48}}) {
            const UINT gw = dims.first, gh = dims.second, lw = gw*2, lh = gh*2;
            auto texture = [&](UINT w, UINT h, DXGI_FORMAT format, UINT bind, const void* pixels, UINT pitch) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width=w; td.Height=h; td.MipLevels=td.ArraySize=1;
                td.Format=format; td.SampleDesc.Count=1; td.BindFlags=bind;
                D3D11_SUBRESOURCE_DATA initial{pixels,pitch,0};
                ComPtr<ID3D11Texture2D> tex;
                Require(device->CreateTexture2D(&td, pixels ? &initial : nullptr, &tex));
                return tex;
            };
            std::vector<float> a(lw*lh), b(lw*lh), field(gw*gh*2);
            for (auto& value : a) value = static_cast<float>(rng()%4096)/4095.0f;
            for (auto& value : b) value = static_cast<float>(rng()%4096)/4095.0f;
            auto ta=texture(lw,lh,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,a.data(),lw*4);
            auto tb=texture(lw,lh,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,b.data(),lw*4);
            auto mv=texture(gw,gh,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE,nullptr,0);
            auto out=texture(gw,gh,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_UNORDERED_ACCESS,nullptr,0);
            ComPtr<ID3D11ShaderResourceView> sa,sb,sm;
            Require(device->CreateShaderResourceView(ta.Get(),nullptr,&sa));
            Require(device->CreateShaderResourceView(tb.Get(),nullptr,&sb));
            Require(device->CreateShaderResourceView(mv.Get(),nullptr,&sm));
            ComPtr<ID3D11UnorderedAccessView> uav;
            Require(device->CreateUnorderedAccessView(out.Get(),nullptr,&uav));
            D3D11_TEXTURE2D_DESC td{}; out->GetDesc(&td);
            td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            Require(device->CreateTexture2D(&td,nullptr,&staging));
            for (int pattern=0; pattern<6; ++pattern) {
                ctx->UpdateSubresource(tb.Get(),0,nullptr,(pattern==0 || pattern==5) ? a.data() : b.data(),lw*4,0);
                for (UINT y=0;y<gh;++y) for (UINT x=0;x<gw;++x) {
                    const UINT k=(y*gw+x)*2;
                    if (pattern==0) field[k]=field[k+1]=0.0f;
                    if (pattern==1) { field[k]=4.0f; field[k+1]=-3.0f; }
                    if (pattern==2) { field[k]=static_cast<float>((x/4)*3); field[k+1]=static_cast<float>(y/5); }
                    if (pattern==3) { field[k]=(x%2)?12.0f:-12.0f; field[k+1]=(y%2)?8.0f:-8.0f; }
                    if (pattern==4) { field[k]=static_cast<float>(static_cast<int>(rng()%513)-256)/8.0f;
                                     field[k+1]=static_cast<float>(static_cast<int>(rng()%513)-256)/8.0f; }
                    if (pattern==5) { field[k]=(x%4)?0.0f:8.0f; field[k+1]=(y%3)?0.0f:-4.0f; }
                }
                ctx->UpdateSubresource(mv.Get(),0,nullptr,field.data(),gw*8,0);
                for (int radius : {1,2}) for (float anchor : {0.0f,0.5f,1.0f}) for (int side : {0,1}) {
                    std::vector<uint8_t> reference(gw*gh*8);
                    for (int skip : {0,1}) {
                        nsp::McCb c{};
                        c.gridW=gw; c.gridH=gh; c.levelW=lw; c.levelH=lh; c.levelScale=2.0f;
                        c.cellPx=4.0f; c.lambda=0.05f; c.anchor=anchor; c.magPrior=1.0f;
                        c.magCap=8.0f; c.pad3=static_cast<float>(radius);
                        c.pad4[0]=static_cast<float>(side); c.pad4[1]=static_cast<float>(skip);
                        ctx->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
                        ID3D11Buffer* buffers[]={cb.Get()};
                        ID3D11ShaderResourceView* views[]={sa.Get(),sb.Get(),sm.Get()};
                        ID3D11UnorderedAccessView* targets[]={uav.Get()};
                        ID3D11SamplerState* samplers[]={sampler.Get()};
                        ctx->CSSetShader(skip && side ? visibility.Get() : shader.Get(),nullptr,0);
                        ctx->CSSetConstantBuffers(2,1,buffers);
                        ctx->CSSetShaderResources(3,3,views);
                        ctx->CSSetSamplers(1,1,samplers);
                        ctx->CSSetUnorderedAccessViews(0,1,targets,nullptr);
                        ctx->Dispatch((gw+7)/8,(gh+7)/8,1);
                        ID3D11UnorderedAccessView* none[]={nullptr};
                        ctx->CSSetUnorderedAccessViews(0,1,none,nullptr);
                        ctx->CopyResource(staging.Get(),out.Get());
                        D3D11_MAPPED_SUBRESOURCE map{};
                        Require(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
                        bool same=true;
                        for (UINT y=0;y<gh;++y) {
                            const auto* row=static_cast<const uint8_t*>(map.pData)+y*map.RowPitch;
                            if (!skip) std::memcpy(reference.data()+y*gw*8,row,gw*8);
                            else same = same && std::memcmp(reference.data()+y*gw*8,row,gw*8)==0;
                        }
                        ctx->Unmap(staging.Get(),0);
                        if (!same) {
                            std::printf("FAIL %ux%u pattern=%d radius=%d anchor=%.1f side=%d\n",gw,gh,pattern,radius,anchor,side);
                            return 1;
                        }
                    }
                    ++cases;
                }
            }
        }
        std::printf("PASS: %d deterministic GPU coherence cases are bit-exact\n",cases);
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr,"FAIL: %s\n",ex.what());
        return 1;
    }
}
