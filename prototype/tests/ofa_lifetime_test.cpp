#include "../nsp_ofa.h"
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;

static void Require(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error(nsp::HrString(hr)); }
static void Check(bool yes,const char* message) { if (!yes) throw std::runtime_error(message); }

int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        Require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
                                 D3D11_SDK_VERSION,&device,nullptr,&ctx));
        nsp::OfaFlow ofa;
        for (bool hints : {true,false,true,false,true,false}) {
            std::string error;
            const bool created=ofa.Create(device.Get(),ctx.Get(),320,180,4,&error,hints);
            Check(created,error.c_str());
            Check(ofa.Ready() && ofa.HintsEnabled()==hints,"wrong ready/hint mode after recreate");
            Check((ofa.HintUav()!=nullptr)==hints,"stale hint view");
            if (hints) {
                ComPtr<ID3D11Resource> resource;
                ofa.HintUav()->GetResource(&resource);
                ComPtr<ID3D11Texture2D> tex;
                Require(resource.As(&tex));
                D3D11_TEXTURE2D_DESC desc{};tex->GetDesc(&desc);
                desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;
                desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> stage;
                Require(device->CreateTexture2D(&desc,nullptr,&stage));
                ctx->CopyResource(stage.Get(),tex.Get());
                D3D11_MAPPED_SUBRESOURCE map{};
                Require(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));
                bool zero=true;
                for (UINT y=0;y<desc.Height;++y) {
                    const auto* row=static_cast<const unsigned char*>(map.pData)+y*map.RowPitch;
                    for (UINT x=0;x<desc.Width*4;++x) zero=zero && row[x]==0;
                }
                ctx->Unmap(stage.Get(),0);
                Check(zero,"hint texture is not explicitly zero");
            }
            const UINT width=ofa.SurfaceW(),height=ofa.SurfaceH();
            std::vector<unsigned char> a(width*height),b(width*height);
            unsigned state=523;
            for (auto& pixel:a) {
                state=1664525*state+1013904223;
                pixel=static_cast<unsigned char>(state>>24);
            }
            for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x)
                b[y*width+x]=a[y*width+(x>=8?x-8:0)];
            ctx->UpdateSubresource(ofa.InputPrev(),0,nullptr,a.data(),width,0);
            ctx->UpdateSubresource(ofa.InputNext(),0,nullptr,b.data(),width,0);
            ctx->Flush();
            Check(ofa.Execute(),"Execute failed after changing hint mode");
            for (ID3D11ShaderResourceView* view : {ofa.FlowFwdSrv(),ofa.FlowBwdSrv()}) {
                ComPtr<ID3D11Resource> resource;
                view->GetResource(&resource);
                ComPtr<ID3D11Texture2D> tex;
                Require(resource.As(&tex));
                D3D11_TEXTURE2D_DESC desc{};tex->GetDesc(&desc);
                desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;
                desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> stage;
                Require(device->CreateTexture2D(&desc,nullptr,&stage));
                ctx->CopyResource(stage.Get(),tex.Get());
                D3D11_MAPPED_SUBRESOURCE map{};
                Require(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));
                const int sign=view==ofa.FlowFwdSrv()?1:-1;
                unsigned correct=0,total=0;
                for (UINT y=4;y+4<desc.Height;++y) for (UINT x=4;x+4<desc.Width;++x) {
                    const auto* row=reinterpret_cast<const short*>(
                        static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);
                    if (std::abs(row[2*x]-sign*256)<=32 && std::abs(row[2*x+1])<=32) ++correct;
                    ++total;
                }
                ctx->Unmap(stage.Get(),0);
                Check(total>0 && correct*10>total*9,"bidirectional translation is not within 1 px");
            }
            ofa.Destroy();
            Check(!ofa.Ready() && !ofa.HintsEnabled() && !ofa.HintUav(),"Destroy retained hint state");
            Check(!ofa.FlowFwdSrv() && !ofa.FlowBwdSrv(),"Destroy retained flow views");
            Require(device->GetDeviceRemovedReason());
        }
        std::puts("PASS: six hardware OFA lifetime cycles, zero hints, no stale mode, bidirectional 8 px translation");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr,"FAIL: %s\n",ex.what());
        return 1;
    }
}
