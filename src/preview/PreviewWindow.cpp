#include "preview/PreviewWindow.h"
#include "preview/Shaders.h"
#include "capture/SaveImage.h"
#include "capture/WindowCapture.h"

#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM, GET_Y_LPARAM
#include <dwmapi.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace screencap::preview {
namespace {

constexpr UINT kFrameCount = 2;

// ── Helpers ─────────────────────────────────────────────────────────

struct WindowInfo {
    HWND hwnd;
    RECT rect; // DWM extended frame bounds (visible area, no shadow).
};

struct PreviewState {
    capture::FrameData frame{};
    bool userClickedSave{false};
    bool done{false};

    // Region selection (only active when regionMode == true).
    bool regionMode{false};
    bool dragging{false};
    bool selectionComplete{false};
    bool needsRedraw{false};
    POINT dragStart{};
    POINT dragEnd{};
    RECT selection{};

    // Window capture mode (only active when windowMode == true).
    bool windowMode{false};
    RECT desktopRect{};                 // virtual desktop origin for coord mapping
    std::vector<WindowInfo> windows;    // pre-enumerated, in Z-order
    int hoveredWindowIndex{-1};
    HWND selectedHwnd{};                // HWND chosen by the user
};

RECT GetVirtualDesktopRect() noexcept
{
    RECT r{};
    r.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

// Normalise a drag rect so left < right and top < bottom.
RECT NormaliseDragRect(POINT a, POINT b) noexcept
{
    RECT r{};
    r.left = (std::min)(a.x, b.x);
    r.top = (std::min)(a.y, b.y);
    r.right = (std::max)(a.x, b.x);
    r.bottom = (std::max)(a.y, b.y);
    return r;
}

// ── Window enumeration for window-capture mode ──────────────────────

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    if (!::IsWindowVisible(hwnd)) return TRUE;
    if (::IsIconic(hwnd)) return TRUE;

    // Skip cloaked windows (UWP background apps, etc.).
    DWORD cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }

    // Use DWM extended frame bounds (visible area without shadow).
    RECT r{};
    if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
        if (!::GetWindowRect(hwnd, &r)) return TRUE;
    }

    if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return TRUE;

    out->push_back({hwnd, r});
    return TRUE;
}

[[nodiscard]] std::vector<WindowInfo> EnumerateVisibleWindows()
{
    std::vector<WindowInfo> windows;
    ::EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

// Find the topmost window whose rect contains the given screen-space point.
// EnumWindows enumerates in Z-order (front to back), so the first hit is topmost.
[[nodiscard]] int FindWindowAtPoint(const std::vector<WindowInfo>& windows, POINT screenPt)
{
    for (int i = 0; i < static_cast<int>(windows.size()); ++i) {
        const auto& r = windows[i].rect;
        if (screenPt.x >= r.left && screenPt.x < r.right &&
            screenPt.y >= r.top && screenPt.y < r.bottom) {
            return i;
        }
    }
    return -1;
}

// ── WndProc ─────────────────────────────────────────────────────────

// IMPORTANT: This WndProc must NEVER call PostQuitMessage.
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* state = reinterpret_cast<PreviewState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && state) {
            state->done = true;
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && state) {
            LPCWSTR cur = IDC_ARROW;
            if (state->regionMode) cur = IDC_CROSS;
            else if (state->windowMode) cur = IDC_HAND;
            ::SetCursor(::LoadCursorW(nullptr, cur));
            return TRUE;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);

    // ── Mouse handling ──────────────────────────────────────────────
    case WM_LBUTTONDOWN:
        if (state && state->regionMode) {
            state->dragging = true;
            state->dragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            state->dragEnd = state->dragStart;
            state->needsRedraw = true;
            ::SetCapture(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (state && state->windowMode) {
            POINT screenPt;
            screenPt.x = GET_X_LPARAM(lp) + state->desktopRect.left;
            screenPt.y = GET_Y_LPARAM(lp) + state->desktopRect.top;
            const int idx = FindWindowAtPoint(state->windows, screenPt);
            if (idx != state->hoveredWindowIndex) {
                state->hoveredWindowIndex = idx;
                state->needsRedraw = true;
            }
        } else if (state && state->regionMode && state->dragging) {
            state->dragEnd = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            state->needsRedraw = true;
        }
        return 0;

    case WM_LBUTTONUP:
        // Region mode: finalise drag selection.
        if (state && state->regionMode && state->dragging) {
            ::ReleaseCapture();
            state->dragging = false;
            state->dragEnd = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            state->selection = NormaliseDragRect(state->dragStart, state->dragEnd);
            if (state->selection.right - state->selection.left > 1 &&
                state->selection.bottom - state->selection.top > 1) {
                state->selectionComplete = true;
                state->done = true;
            }
            return 0;
        }
        // Window mode: select hovered window.
        if (state && state->windowMode && state->hoveredWindowIndex >= 0) {
            const auto& wi = state->windows[state->hoveredWindowIndex];
            state->selectedHwnd = wi.hwnd;
            // Also store the rect as fallback for CropFrame.
            state->selection.left = wi.rect.left - state->desktopRect.left;
            state->selection.top = wi.rect.top - state->desktopRect.top;
            state->selection.right = wi.rect.right - state->desktopRect.left;
            state->selection.bottom = wi.rect.bottom - state->desktopRect.top;
            state->selectionComplete = true;
            state->done = true;
            return 0;
        }
        // Full-desktop mode: any click saves.
        if (state && !state->regionMode && !state->windowMode) {
            state->userClickedSave = true;
            state->done = true;
        }
        return 0;

    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (state && !state->regionMode && !state->windowMode) {
            state->userClickedSave = true;
            state->done = true;
        } else if (state) {
            // In region/window mode, right-click cancels.
            state->done = true;
        }
        return 0;

    case WM_DESTROY:
        if (state) {
            state->done = true;
        }
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ── DX11 pipeline objects ───────────────────────────────────────────

struct DX11Context {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11ShaderResourceView> textureSRV;
    DXGI_FORMAT backBufferFormat{DXGI_FORMAT_B8G8R8A8_UNORM};
    DXGI_COLOR_SPACE_TYPE swapChainColorSpace{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
};

// ── D2D overlay (native D3D11 interop) ──────────────────────────────

struct D2DOverlay {
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dCtx;
    ComPtr<IDWriteFactory> dwriteFactory;
    ComPtr<IDWriteTextFormat> textFormat;
    ComPtr<ID2D1Bitmap1> d2dRenderTarget;
};

bool CompileShader(const char* source, const char* target, const char* entry, ComPtr<ID3DBlob>& blob)
{
    ComPtr<ID3DBlob> errors;
    HRESULT hr = ::D3DCompile(
        source, std::strlen(source),
        nullptr, nullptr, nullptr,
        entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &blob, &errors);
    return SUCCEEDED(hr);
}

bool InitDX11(DX11Context& dx, ID3D11Device* sharedDevice, HWND hwnd, UINT width, UINT height)
{
    HRESULT hr{};

    // Use the shared device.
    dx.device = sharedDevice;
    sharedDevice->GetImmediateContext(&dx.ctx);

    // Factory
    hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&dx.factory));
    if (FAILED(hr)) return false;

    // Prefer scRGB (FP16) swapchain so HDR preview is passthrough.
    dx.backBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dx.swapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

    // Swap chain
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = dx.backBufferFormat;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> sc1;
    hr = dx.factory->CreateSwapChainForHwnd(dx.device.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        dx.backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        dx.swapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        scDesc.Format = dx.backBufferFormat;
        hr = dx.factory->CreateSwapChainForHwnd(dx.device.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1);
        if (FAILED(hr)) return false;
    }
    hr = sc1.As(&dx.swapChain);
    if (FAILED(hr)) return false;

    {
        UINT support = 0;
        if (SUCCEEDED(dx.swapChain->CheckColorSpaceSupport(dx.swapChainColorSpace, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
            (void)dx.swapChain->SetColorSpace1(dx.swapChainColorSpace);
        }
    }

    dx.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Render target view for the back buffer.
    ComPtr<ID3D11Texture2D> backBuf;
    hr = dx.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    if (FAILED(hr)) return false;
    hr = dx.device->CreateRenderTargetView(backBuf.Get(), nullptr, &dx.rtv);
    if (FAILED(hr)) return false;

    // Compile and create shaders.
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileShader(kVertexShaderHLSL, "vs_5_0", "VSMain", vsBlob)) return false;
    if (!CompileShader(kPixelShaderHLSL, "ps_5_0", "PSMain", psBlob)) return false;

    hr = dx.device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &dx.vs);
    if (FAILED(hr)) return false;
    hr = dx.device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &dx.ps);
    if (FAILED(hr)) return false;

    // Sampler state (linear, clamp).
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = dx.device->CreateSamplerState(&sampDesc, &dx.sampler);
    if (FAILED(hr)) return false;

    return true;
}

bool InitD2DOverlay(D2DOverlay& ov, DX11Context& dx)
{
    HRESULT hr{};

    // D2D factory + device from the existing D3D11 device.
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&ov.d2dFactory));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDev;
    hr = dx.device.As(&dxgiDev);
    if (FAILED(hr)) return false;

    hr = ov.d2dFactory->CreateDevice(dxgiDev.Get(), &ov.d2dDevice);
    if (FAILED(hr)) return false;

    hr = ov.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ov.d2dCtx);
    if (FAILED(hr)) return false;

    // Create a D2D render target from the swap chain back buffer.
    ComPtr<IDXGISurface> surface;
    hr = dx.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(dx.backBufferFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = ov.d2dCtx->CreateBitmapFromDxgiSurface(surface.Get(), &bmpProps, &ov.d2dRenderTarget);
    if (FAILED(hr)) return false;

    // DWrite factory for dimension labels.
    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(ov.dwriteFactory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = ov.dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        22.0f,
        L"en-us",
        &ov.textFormat);
    if (FAILED(hr)) return false;

    return true;
}

bool UploadTexture(DX11Context& dx, const capture::FrameData& frame)
{
    HRESULT hr{};

    const auto texFormat = static_cast<DXGI_FORMAT>(frame.format);
    const UINT bpp = frame.bytesPerPixel;
    if ((texFormat != DXGI_FORMAT_B8G8R8A8_UNORM && texFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (bpp != 4 && bpp != 8)) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = texFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    // Fast path: the frame already has a GPU texture (from DesktopDuplicator).
    if (frame.gpuTexture) {
        hr = dx.device->CreateShaderResourceView(frame.gpuTexture.Get(), &srvDesc, &dx.textureSRV);
        return SUCCEEDED(hr);
    }

    // Fallback: upload from CPU pixels (e.g. WindowCapture frames).
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = frame.width;
    texDesc.Height = frame.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = texFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    const UINT rowPitch = frame.width * bpp;
    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = frame.pixels.data();
    initData.SysMemPitch = rowPitch;

    ComPtr<ID3D11Texture2D> texture;
    hr = dx.device->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr)) return false;

    hr = dx.device->CreateShaderResourceView(texture.Get(), &srvDesc, &dx.textureSRV);
    if (FAILED(hr)) return false;

    return true;
}

// Render the desktop texture but do not present -- D2D draws on top next.
void RenderFrameNoPresent(DX11Context& dx, UINT width, UINT height)
{
    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    dx.ctx->ClearRenderTargetView(dx.rtv.Get(), clearColor);
    dx.ctx->OMSetRenderTargets(1, dx.rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    dx.ctx->RSSetViewports(1, &vp);

    dx.ctx->VSSetShader(dx.vs.Get(), nullptr, 0);
    dx.ctx->PSSetShader(dx.ps.Get(), nullptr, 0);
    dx.ctx->PSSetShaderResources(0, 1, dx.textureSRV.GetAddressOf());
    dx.ctx->PSSetSamplers(0, 1, dx.sampler.GetAddressOf());

    dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dx.ctx->Draw(3, 0);

    // Unbind the render target so D2D can draw on the back buffer.
    ID3D11RenderTargetView* nullRTV = nullptr;
    dx.ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
}

// Render path for full-desktop mode (no D2D overlay).
void RenderFrame(DX11Context& dx, UINT width, UINT height)
{
    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    dx.ctx->ClearRenderTargetView(dx.rtv.Get(), clearColor);
    dx.ctx->OMSetRenderTargets(1, dx.rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    dx.ctx->RSSetViewports(1, &vp);

    dx.ctx->VSSetShader(dx.vs.Get(), nullptr, 0);
    dx.ctx->PSSetShader(dx.ps.Get(), nullptr, 0);
    dx.ctx->PSSetShaderResources(0, 1, dx.textureSRV.GetAddressOf());
    dx.ctx->PSSetSamplers(0, 1, dx.sampler.GetAddressOf());

    dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dx.ctx->Draw(3, 0);

    dx.swapChain->Present(1, 0);
}

// ── Shared overlay building blocks ──────────────────────────────────

struct OverlayBrushes {
    ComPtr<ID2D1SolidColorBrush> dim;
    ComPtr<ID2D1SolidColorBrush> black;
    ComPtr<ID2D1SolidColorBrush> green;
};

OverlayBrushes CreateOverlayBrushes(ID2D1DeviceContext* ctx)
{
    OverlayBrushes b{};
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &b.dim);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.9f), &b.black);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f), &b.green);
    return b;
}

// Dim everything outside a cutout rect (4 strips).
void DimAroundRect(ID2D1DeviceContext* ctx, ID2D1Brush* brush,
                   float l, float t, float r, float b, float sw, float sh)
{
    if (!brush) return;
    if (t > 0.0f) ctx->FillRectangle(D2D1::RectF(0, 0, sw, t), brush);
    if (b < sh)    ctx->FillRectangle(D2D1::RectF(0, b, sw, sh), brush);
    if (l > 0.0f) ctx->FillRectangle(D2D1::RectF(0, t, l, b), brush);
    if (r < sw)    ctx->FillRectangle(D2D1::RectF(r, t, sw, b), brush);
}

// Draw the standard border (4px black outer + 3px green inner) and
// an optional W×H dimension label in the lower-right corner.
void DrawBorderAndLabel(ID2D1DeviceContext* ctx, const OverlayBrushes& br,
                        IDWriteTextFormat* textFmt,
                        float l, float t, float r, float b,
                        int pixelW, int pixelH)
{
    const auto rect = D2D1::RectF(l, t, r, b);
    if (br.black) ctx->DrawRectangle(rect, br.black.Get(), 4.0f);
    if (br.green) {
        const auto inner = D2D1::RectF(l + 4.0f, t + 4.0f, r - 4.0f, b - 4.0f);
        ctx->DrawRectangle(inner, br.green.Get(), 3.0f);
    }

    if (pixelW > 0 && pixelH > 0 && textFmt) {
        wchar_t buf[64]{};
        (void)std::swprintf(buf, _countof(buf), L"%d \u00D7 %d", pixelW, pixelH);

        constexpr float labelW = 200.0f;
        constexpr float labelH = 30.0f;
        constexpr float pad = 10.0f;

        const float labelX = r - pad - labelW;
        const float labelY = b - pad - labelH;

        if (br.black) {
            const auto bgRect = D2D1::RectF(labelX - 4.0f, labelY - 2.0f,
                                             labelX + labelW + 4.0f, labelY + labelH + 2.0f);
            ctx->FillRectangle(bgRect, br.black.Get());
        }
        if (br.green) {
            textFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            const auto textRect = D2D1::RectF(labelX, labelY, labelX + labelW, labelY + labelH);
            ctx->DrawText(buf, static_cast<UINT32>(wcslen(buf)), textFmt, textRect, br.green.Get());
        }
    }
}

// ── Overlay functions (thin wrappers around the shared primitives) ───

void DrawSelectionOverlay(D2DOverlay& ov, const RECT& sel, UINT screenW, UINT screenH)
{
    ov.d2dCtx->SetTarget(ov.d2dRenderTarget.Get());
    ov.d2dCtx->BeginDraw();

    const float sw = static_cast<float>(screenW);
    const float sh = static_cast<float>(screenH);
    const float l = static_cast<float>(sel.left);
    const float t = static_cast<float>(sel.top);
    const float r = static_cast<float>(sel.right);
    const float b = static_cast<float>(sel.bottom);

    auto br = CreateOverlayBrushes(ov.d2dCtx.Get());
    DimAroundRect(ov.d2dCtx.Get(), br.dim.Get(), l, t, r, b, sw, sh);
    DrawBorderAndLabel(ov.d2dCtx.Get(), br, ov.textFormat.Get(),
                       l, t, r, b, sel.right - sel.left, sel.bottom - sel.top);

    (void)ov.d2dCtx->EndDraw();
    ov.d2dCtx->SetTarget(nullptr);
}

void DrawWindowOverlay(D2DOverlay& ov,
                       int hoveredIndex, const std::vector<WindowInfo>& windows,
                       const RECT& desktopRect, UINT screenW, UINT screenH)
{
    ov.d2dCtx->SetTarget(ov.d2dRenderTarget.Get());
    ov.d2dCtx->BeginDraw();

    const float sw = static_cast<float>(screenW);
    const float sh = static_cast<float>(screenH);
    auto br = CreateOverlayBrushes(ov.d2dCtx.Get());

    if (hoveredIndex < 0) {
        // No window hovered -- dim the entire screen.
        if (br.dim) {
            ov.d2dCtx->FillRectangle(D2D1::RectF(0, 0, sw, sh), br.dim.Get());
        }
    } else {
        const auto& wr = windows[hoveredIndex].rect;
        float l = static_cast<float>(wr.left - desktopRect.left);
        float t = static_cast<float>(wr.top - desktopRect.top);
        float r = static_cast<float>(wr.right - desktopRect.left);
        float b = static_cast<float>(wr.bottom - desktopRect.top);
        l = (std::max)(0.0f, l);  t = (std::max)(0.0f, t);
        r = (std::min)(sw, r);    b = (std::min)(sh, b);

        DimAroundRect(ov.d2dCtx.Get(), br.dim.Get(), l, t, r, b, sw, sh);
        DrawBorderAndLabel(ov.d2dCtx.Get(), br, ov.textFormat.Get(),
                           l, t, r, b, wr.right - wr.left, wr.bottom - wr.top);
    }

    (void)ov.d2dCtx->EndDraw();
    ov.d2dCtx->SetTarget(nullptr);
}

// ── Monitor enumeration for full-desktop border overlay ─────────────

BOOL CALLBACK MonitorEnumCallback(HMONITOR /*hmon*/, HDC /*hdc*/, LPRECT rect, LPARAM lParam)
{
    auto* out = reinterpret_cast<std::vector<RECT>*>(lParam);
    out->push_back(*rect);
    return TRUE;
}

[[nodiscard]] std::vector<RECT> EnumerateMonitorRects()
{
    std::vector<RECT> rects;
    ::EnumDisplayMonitors(nullptr, nullptr, MonitorEnumCallback, reinterpret_cast<LPARAM>(&rects));
    return rects;
}

void DrawMonitorBorders(D2DOverlay& ov, const std::vector<RECT>& monitors,
                        const RECT& desktopRect)
{
    ov.d2dCtx->SetTarget(ov.d2dRenderTarget.Get());
    ov.d2dCtx->BeginDraw();

    auto br = CreateOverlayBrushes(ov.d2dCtx.Get());

    for (const auto& mon : monitors) {
        const float l = static_cast<float>(mon.left - desktopRect.left);
        const float t = static_cast<float>(mon.top - desktopRect.top);
        const float r = static_cast<float>(mon.right - desktopRect.left);
        const float b = static_cast<float>(mon.bottom - desktopRect.top);

        DrawBorderAndLabel(ov.d2dCtx.Get(), br, ov.textFormat.Get(),
                           l, t, r, b, mon.right - mon.left, mon.bottom - mon.top);
    }

    (void)ov.d2dCtx->EndDraw();
    ov.d2dCtx->SetTarget(nullptr);
}

// Crop a FrameData to a sub-rectangle.
capture::FrameData CropFrame(const capture::FrameData& src, RECT sel)
{
    // Clamp selection to frame bounds.
    sel.left = (std::max)(0L, (std::min)(sel.left, static_cast<LONG>(src.width)));
    sel.top = (std::max)(0L, (std::min)(sel.top, static_cast<LONG>(src.height)));
    sel.right = (std::max)(0L, (std::min)(sel.right, static_cast<LONG>(src.width)));
    sel.bottom = (std::max)(0L, (std::min)(sel.bottom, static_cast<LONG>(src.height)));

    const uint32_t cropW = static_cast<uint32_t>(sel.right - sel.left);
    const uint32_t cropH = static_cast<uint32_t>(sel.bottom - sel.top);

    capture::FrameData out;
    out.width = cropW;
    out.height = cropH;
    out.format = src.format;
    out.bytesPerPixel = src.bytesPerPixel;
    out.pixels.resize(static_cast<size_t>(cropW) * src.bytesPerPixel * cropH);

    const uint32_t srcStride = src.width * src.bytesPerPixel;
    const uint32_t dstStride = cropW * src.bytesPerPixel;

    for (uint32_t row = 0; row < cropH; ++row) {
        const size_t srcOff = static_cast<size_t>(sel.top + row) * srcStride + static_cast<size_t>(sel.left) * src.bytesPerPixel;
        const size_t dstOff = static_cast<size_t>(row) * dstStride;
        std::memcpy(out.pixels.data() + dstOff, src.pixels.data() + srcOff, dstStride);
    }

    return out;
}

// ── Shared window creation helper ───────────────────────────────────

HWND CreatePreviewWindow(PreviewState& state, UINT& winW, UINT& winH)
{
    const auto hinst = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, state.regionMode ? IDC_CROSS : IDC_ARROW);
    wc.lpszClassName = L"ScreenCap.Preview";

    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    const RECT desk = GetVirtualDesktopRect();
    winW = static_cast<UINT>(desk.right - desk.left);
    winH = static_cast<UINT>(desk.bottom - desk.top);

    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName,
        L"ScreenCap Preview",
        WS_POPUP,
        desk.left, desk.top,
        static_cast<int>(winW), static_cast<int>(winH),
        nullptr,
        nullptr,
        hinst,
        &state);

    return hwnd;
}

void TeardownDX11(DX11Context& dx)
{
    if (dx.ctx) {
        dx.ctx->ClearState();
        dx.ctx->Flush();
    }
}

} // namespace

// ── Helper: save or clipboard ───────────────────────────────────────

bool OutputImage(const capture::FrameData& frame, bool copyToClipboard)
{
    const bool ok = copyToClipboard
        ? capture::CopyImageToClipboard(frame)
        : capture::SaveImageInteractive(frame);
    if (ok) {
        capture::WriteThumbnailPng(frame);
    }
    return ok;
}

// ── Full-desktop preview (existing behavior) ────────────────────────

bool Show(capture::FrameData frame, ID3D11Device* device, bool copyToClipboard)
{
    PreviewState state;
    state.frame = std::move(frame);
    state.regionMode = false;
    state.desktopRect = GetVirtualDesktopRect();

    UINT winW{}, winH{};
    HWND hwnd = CreatePreviewWindow(state, winW, winH);
    if (!hwnd) {
        return false;
    }

    DX11Context dx;
    if (!InitDX11(dx, device, hwnd, winW, winH)) {
        ::DestroyWindow(hwnd);
        return false;
    }

    if (!UploadTexture(dx, state.frame)) {
        TeardownDX11(dx);
        ::DestroyWindow(hwnd);
        return false;
    }

    // D2D overlay for monitor borders.
    D2DOverlay ov;
    const bool hasOverlay = InitD2DOverlay(ov, dx);
    auto monitors = EnumerateMonitorRects();

    ::ShowWindow(hwnd, SW_SHOW);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

    // Render desktop texture + monitor border overlay.
    RenderFrameNoPresent(dx, winW, winH);
    if (hasOverlay && !monitors.empty()) {
        DrawMonitorBorders(ov, monitors, state.desktopRect);
    }
    dx.swapChain->Present(1, 0);

    MSG msg{};
    while (!state.done) {
        const BOOL ret = ::GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0) {
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (hasOverlay) {
        ov.d2dRenderTarget.Reset();
        ov.d2dCtx.Reset();
        ov.d2dDevice.Reset();
        ov.d2dFactory.Reset();
    }

    TeardownDX11(dx);
    ::DestroyWindow(hwnd);

    if (state.userClickedSave) {
        // Lazy readback: populate CPU pixels from the GPU texture.
        ComPtr<ID3D11DeviceContext> readbackCtx;
        device->GetImmediateContext(&readbackCtx);
        if (!capture::ReadbackPixels(state.frame, readbackCtx.Get())) {
            return false;
        }
        return OutputImage(state.frame, copyToClipboard);
    }

    return false;
}

// ── Region selection preview ────────────────────────────────────────

bool ShowRegion(capture::FrameData frame, ID3D11Device* device, bool copyToClipboard)
{
    PreviewState state;
    state.frame = std::move(frame);
    state.regionMode = true;

    UINT winW{}, winH{};
    HWND hwnd = CreatePreviewWindow(state, winW, winH);
    if (!hwnd) {
        return false;
    }

    DX11Context dx;
    if (!InitDX11(dx, device, hwnd, winW, winH)) {
        ::DestroyWindow(hwnd);
        return false;
    }

    if (!UploadTexture(dx, state.frame)) {
        TeardownDX11(dx);
        ::DestroyWindow(hwnd);
        return false;
    }

    D2DOverlay ov;
    if (!InitD2DOverlay(ov, dx)) {
        TeardownDX11(dx);
        ::DestroyWindow(hwnd);
        return false;
    }

    ::ShowWindow(hwnd, SW_SHOW);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

    // Initial render: desktop texture + full dim (no selection yet).
    {
        RenderFrameNoPresent(dx, winW, winH);
        ov.d2dCtx->SetTarget(ov.d2dRenderTarget.Get());
        ov.d2dCtx->BeginDraw();
        ComPtr<ID2D1SolidColorBrush> dimBrush;
        ov.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &dimBrush);
        if (dimBrush) {
            ov.d2dCtx->FillRectangle(
                D2D1::RectF(0, 0, static_cast<float>(winW), static_cast<float>(winH)),
                dimBrush.Get());
        }
        (void)ov.d2dCtx->EndDraw();
        ov.d2dCtx->SetTarget(nullptr);
        dx.swapChain->Present(1, 0);
    }

    // ── PeekMessage loop: re-render when selection changes ──────────

    MSG msg{};
    while (!state.done) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                state.done = true;
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (state.done) {
            break;
        }

        if (state.needsRedraw && state.dragging) {
            state.needsRedraw = false;

            const RECT sel = NormaliseDragRect(state.dragStart, state.dragEnd);

            // Render desktop texture, then overlay selection via D2D.
            RenderFrameNoPresent(dx, winW, winH);
            DrawSelectionOverlay(ov, sel, winW, winH);
            dx.swapChain->Present(1, 0);
        } else {
            // Avoid busy-wait when nothing is happening.
            ::WaitMessage();
        }
    }

    // Release D2D resources before DX11 teardown.
    ov.d2dRenderTarget.Reset();
    ov.d2dCtx.Reset();
    ov.d2dDevice.Reset();
    ov.d2dFactory.Reset();

    TeardownDX11(dx);
    ::DestroyWindow(hwnd);

    // ── Save cropped region ────────────────────────────────────────

    if (state.selectionComplete) {
        // Lazy readback: CropFrame needs CPU pixels.
        ComPtr<ID3D11DeviceContext> readbackCtx;
        device->GetImmediateContext(&readbackCtx);
        if (!capture::ReadbackPixels(state.frame, readbackCtx.Get())) {
            return false;
        }
        auto cropped = CropFrame(state.frame, state.selection);
        if (cropped.width > 0 && cropped.height > 0) {
            return OutputImage(cropped, copyToClipboard);
        }
    }

    return false;
}

// ── Window capture preview ──────────────────────────────────────────

bool ShowWindowCapture(capture::FrameData frame, ID3D11Device* device, bool copyToClipboard)
{
    // Enumerate visible windows BEFORE creating the overlay so our own
    // fullscreen window is not in the list.
    auto windows = EnumerateVisibleWindows();
    if (windows.empty()) {
        return false;
    }

    PreviewState state;
    state.frame = std::move(frame);
    state.windowMode = true;
    state.desktopRect = GetVirtualDesktopRect();
    state.windows = std::move(windows);

    UINT winW{}, winH{};
    HWND hwnd = CreatePreviewWindow(state, winW, winH);
    if (!hwnd) {
        return false;
    }

    DX11Context dx;
    if (!InitDX11(dx, device, hwnd, winW, winH)) {
        ::DestroyWindow(hwnd);
        return false;
    }

    if (!UploadTexture(dx, state.frame)) {
        TeardownDX11(dx);
        ::DestroyWindow(hwnd);
        return false;
    }

    D2DOverlay ov;
    if (!InitD2DOverlay(ov, dx)) {
        TeardownDX11(dx);
        ::DestroyWindow(hwnd);
        return false;
    }

    ::ShowWindow(hwnd, SW_SHOW);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

    // Initial render: desktop texture + full grey dim (no window hovered yet).
    RenderFrameNoPresent(dx, winW, winH);
    DrawWindowOverlay(ov, -1, state.windows, state.desktopRect, winW, winH);
    dx.swapChain->Present(1, 0);

    // ── PeekMessage loop: re-render when hovered window changes ─────

    MSG msg{};
    while (!state.done) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                state.done = true;
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (state.done) {
            break;
        }

        if (state.needsRedraw) {
            state.needsRedraw = false;

            RenderFrameNoPresent(dx, winW, winH);
            DrawWindowOverlay(ov, state.hoveredWindowIndex,
                              state.windows, state.desktopRect, winW, winH);
            dx.swapChain->Present(1, 0);
        } else {
            ::WaitMessage();
        }
    }

    // Release D2D resources before DX11 teardown.
    ov.d2dRenderTarget.Reset();
    ov.d2dCtx.Reset();
    ov.d2dDevice.Reset();
    ov.d2dFactory.Reset();

    TeardownDX11(dx);
    ::DestroyWindow(hwnd);

    // ── Capture the selected window via WinRT Graphics Capture ──────

    if (state.selectionComplete && state.selectedHwnd) {
        auto windowFrame = capture::CaptureWindow(state.selectedHwnd, device);
        if (windowFrame && windowFrame->width > 0 && windowFrame->height > 0) {
            return OutputImage(*windowFrame, copyToClipboard);
        }
        // Fallback: crop from the desktop capture if WinRT capture failed.
        ComPtr<ID3D11DeviceContext> readbackCtx;
        device->GetImmediateContext(&readbackCtx);
        if (!capture::ReadbackPixels(state.frame, readbackCtx.Get())) {
            return false;
        }
        auto cropped = CropFrame(state.frame, state.selection);
        if (cropped.width > 0 && cropped.height > 0) {
            return OutputImage(cropped, copyToClipboard);
        }
    }

    return false;
}

} // namespace screencap::preview
