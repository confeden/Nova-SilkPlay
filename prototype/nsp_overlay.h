// nsp_overlay.h — the D3D11 device, the overlay HWND, its DirectComposition
// visual and the composition swapchain the synthesized frames are presented on.
//
// Invariants (ROADMAP): I9 the window always carries WS_EX_TRANSPARENT|WS_POPUP.
// I11 exactly one HWND, created once over the target's client area, never moved
// or resized — the video rect lives in the DComp visual offset + clip only.
// I13 the swapchain is exactly the size of the video rect and the visual is not
// scaled, so the picture is 1:1 over the source pixels.
#pragma once

#include "nsp_common.h"

#include <d3d11_1.h>

#include <memory>
#include <string>

namespace nsp {

class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Creates the device, the (hidden) HWND covering `hwndRectScreen`, the DComp
    // tree and a composition swapchain the size of `videoRectScreen`.
    // `topmost` restores the old WS_EX_TOPMOST placement; the default keeps the
    // window one slot above `target` instead (see KeepAbove). `layered = false`
    // exists only to measure what WS_EX_LAYERED costs: that window swallows input.
    bool Create(HWND target, const RECT& hwndRectScreen, const RECT& videoRectScreen,
                std::string* err, bool topmost = false, bool layered = true);

    // Puts the overlay back directly above the target in z-order if anything moved
    // it (activating the target raises the target above us). One GetWindow call
    // when nothing changed; call it every loop iteration. True if it had to move.
    bool KeepAbove();
    uint64_t ZOrderFixes() const;
    // True once KeepAbove gave up and made the window topmost for this engagement.
    bool ZOrderDegraded() const;

    // WM_NCHITTEST messages this window has received. Stays 0 while the window is
    // genuinely click-through; anything else means mouse input is being swallowed.
    uint32_t HitTestsSeen() const;

    ID3D11Device*        Device() const;
    ID3D11DeviceContext* Context() const;
    HWND                 Hwnd() const;

    // Moves the visual, resizing the swapchain when the size changed. Screen-space
    // physical pixels. The HWND never moves (I11).
    bool SetVideoRect(const RECT& videoRectScreen);
    const RECT& VideoRect() const;

    // Waits up to `timeoutMs` for the swapchain to accept another present.
    // Returns true when it will (or when there is no waitable object). Exposed
    // separately from AcquireBackBuffer so the caller can spend that idle time
    // draining the capture queue instead of sleeping through it.
    bool WaitPresentable(DWORD timeoutMs);

    // Hands out this frame's back buffer. Does NOT wait — call WaitPresentable
    // first.
    // Both pointers are BORROWED: valid until the next Present(). `rtvSrgb` writes
    // through an _SRGB view, so shaders work in linear light for free.
    bool AcquireBackBuffer(ID3D11Texture2D** tex, ID3D11RenderTargetView** rtvSrgb);

    // Presents the acquired back buffer. Returns false only on device loss.
    bool Present();

    void Show();
    void Hide();
    bool IsShown() const;

    // DXGI_FRAME_STATISTICS_MEDIA::CompositionMode of the last present:
    // 0 COMPOSED, 1 OVERLAY, 2 NONE, 3 FAILURE, -1 unavailable.
    int  LastCompositionMode() const;

    const std::string& CreationReport() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
