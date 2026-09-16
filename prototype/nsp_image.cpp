// nsp_image.cpp — WIC PNG in/out, CNG SHA-256.

#include "nsp_image.h"

#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace nsp {
namespace {

// One factory for the process, DELIBERATELY LEAKED.
//
// It must not be a static ComPtr: a static's destructor runs at process exit,
// which is AFTER main() has called CoUninitialize, and releasing a COM object
// into a torn-down apartment crashes. The symptom is maximally confusing —
// every line of output is correct, every file is written, and the process still
// dies with an access violation on the way out, so the failure looks like it
// belongs to whatever ran last. Leaking one singleton at exit costs nothing:
// the OS reclaims the process anyway.
IWICImagingFactory* Factory() {
    static IWICImagingFactory* factory = [] {
        IWICImagingFactory* f = nullptr;
        const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&f));
        if (FAILED(hr))
            LogErr("WIC: CoCreateInstance(WICImagingFactory) failed %s", HrString(hr).c_str());
        return f;
    }();
    return factory;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

bool LoadImageBgra(const std::string& path, std::vector<uint8_t>* pixels, UINT* w, UINT* h,
                   std::string* err) {
    auto fail = [&](const char* what, HRESULT hr) {
        if (err) *err = std::string(what) + ": " + HrString(hr) + "  [" + path + "]";
        return false;
    };
    if (!pixels || !w || !h) return fail("LoadImageBgra: null out", E_POINTER);
    IWICImagingFactory* factory = Factory();
    if (!factory) return fail("LoadImageBgra: no WIC factory", E_FAIL);

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(Widen(path).c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return fail("CreateDecoderFromFilename", hr);

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return fail("GetFrame", hr);

    // 32bppBGRA is exactly what the capture ring holds (nsp_capture.cpp creates
    // B8G8R8A8_TYPELESS and views it as _SRGB), so the offline path feeds the
    // shaders the same bytes in the same order the live path does.
    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr)) return fail("CreateFormatConverter", hr);
    hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return fail("FormatConverter::Initialize", hr);

    UINT iw = 0, ih = 0;
    hr = conv->GetSize(&iw, &ih);
    if (FAILED(hr)) return fail("GetSize", hr);
    if (iw == 0 || ih == 0) return fail("image is empty", E_FAIL);

    const UINT stride = iw * 4;
    pixels->resize(static_cast<size_t>(stride) * ih);
    hr = conv->CopyPixels(nullptr, stride, static_cast<UINT>(pixels->size()), pixels->data());
    if (FAILED(hr)) return fail("CopyPixels", hr);

    *w = iw;
    *h = ih;
    return true;
}

bool SaveImageBgraPng(const std::string& path, const uint8_t* pixels, UINT w, UINT h, UINT stride,
                      std::string* err) {
    auto fail = [&](const char* what, HRESULT hr) {
        if (err) *err = std::string(what) + ": " + HrString(hr) + "  [" + path + "]";
        return false;
    };
    if (!pixels || w == 0 || h == 0) return fail("SaveImageBgraPng: bad input", E_INVALIDARG);
    IWICImagingFactory* factory = Factory();
    if (!factory) return fail("SaveImageBgraPng: no WIC factory", E_FAIL);

    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return fail("CreateStream", hr);
    hr = stream->InitializeFromFilename(Widen(path).c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return fail("InitializeFromFilename", hr);

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return fail("CreateEncoder(PNG)", hr);
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return fail("Encoder::Initialize", hr);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return fail("CreateNewFrame", hr);
    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) return fail("Frame::Initialize", hr);
    hr = frame->SetSize(w, h);
    if (FAILED(hr)) return fail("SetSize", hr);

    // BGRA in, BGRA out. If WIC ever answers with a different format the write
    // still succeeds but the bytes are converted, so the negotiated format is
    // checked rather than assumed.
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) return fail("SetPixelFormat", hr);
    if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA))
        LogErr("WIC: PNG encoder negotiated a different pixel format than 32bppBGRA for %s",
               path.c_str());

    hr = frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(pixels));
    if (FAILED(hr)) return fail("WritePixels", hr);
    hr = frame->Commit();
    if (FAILED(hr)) return fail("Frame::Commit", hr);
    hr = encoder->Commit();
    if (FAILED(hr)) return fail("Encoder::Commit", hr);
    return true;
}

std::string Sha256Hex(const void* data, size_t bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};
    uint8_t digest[32] = {};
    const NTSTATUS st = BCryptHash(alg, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)),
                                   static_cast<ULONG>(bytes), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) return {};

    static const char* kHex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

}  // namespace nsp
