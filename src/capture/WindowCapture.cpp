#include "capture/WindowCapture.h"
#include "capture/PixelFormats.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

// Raw COM interop headers (must come after <d3d11.h> for IUnknown / MIDL_INTERFACE).
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.interop.h>

// C++/WinRT projection headers.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace winrt_cap  = winrt::Windows::Graphics::Capture;
namespace winrt_dx   = winrt::Windows::Graphics::DirectX;
namespace winrt_d3d  = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace screencap::capture {
namespace {

// ── Helpers ─────────────────────────────────────────────────────────

// Wrap a raw IDXGIDevice as a WinRT IDirect3DDevice.
[[nodiscard]] winrt_d3d::IDirect3DDevice CreateWinRTDevice(IDXGIDevice* dxgiDevice)
{
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put()));
    return inspectable.as<winrt_d3d::IDirect3DDevice>();
}

// Create a GraphicsCaptureItem from an HWND via COM interop.
[[nodiscard]] winrt_cap::GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd)
{
    auto interop = winrt::get_activation_factory<
        winrt_cap::GraphicsCaptureItem,
        ::IGraphicsCaptureItemInterop>();

    winrt_cap::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(
        interop->CreateForWindow(
            hwnd,
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item)));
    return item;
}

// Copy a D3D11 texture to CPU-accessible FrameData, respecting the actual
// texture format (BGRA8, RGBA16F, etc.) rather than assuming a fixed format.
[[nodiscard]] bool CopyTextureToFrame(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* srcTex,
    FrameData& frame)
{
    D3D11_TEXTURE2D_DESC desc{};
    srcTex->GetDesc(&desc);

    const uint32_t bpp = BytesPerPixel(desc.Format);
    if (bpp == 0) return false; // Unsupported format.

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = desc.Width;
    stagingDesc.Height = desc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = desc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) return false;

    ctx->CopyResource(staging.Get(), srcTex);
    ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    frame.width = desc.Width;
    frame.height = desc.Height;
    frame.format = static_cast<uint32_t>(desc.Format);
    frame.bytesPerPixel = bpp;
    frame.pixels.resize(static_cast<size_t>(desc.Width) * bpp * desc.Height);

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const uint32_t dstStride = desc.Width * bpp;
    for (uint32_t row = 0; row < desc.Height; ++row) {
        std::memcpy(
            frame.pixels.data() + static_cast<size_t>(row) * dstStride,
            src + static_cast<size_t>(row) * mapped.RowPitch,
            dstStride);
    }

    ctx->Unmap(staging.Get(), 0);
    return true;
}

} // namespace

std::optional<FrameData> CaptureWindow(HWND hwnd, ID3D11Device* device)
{
    if (!hwnd || !::IsWindow(hwnd) || !device) {
        return std::nullopt;
    }

    try {
        ComPtr<ID3D11DeviceContext> d3dCtx;
        device->GetImmediateContext(&d3dCtx);

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return std::nullopt;

        auto winrtDevice = CreateWinRTDevice(dxgiDevice.Get());

        // ── Create capture item from HWND ───────────────────────────
        auto item = CreateCaptureItemForWindow(hwnd);
        auto itemSize = item.Size();
        if (itemSize.Width <= 0 || itemSize.Height <= 0) {
            return std::nullopt;
        }

        // ── Frame pool + session ────────────────────────────────────
        // Try R16G16B16A16_FLOAT first (preserves HDR content from windows
        // like Edge/Chrome that use scRGB float swapchains).  Fall back to
        // B8G8R8A8_UNORM if the driver/OS doesn't support FP16 pools.
        winrt_cap::Direct3D11CaptureFramePool framePool{nullptr};
        try {
            framePool = winrt_cap::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice,
                winrt_dx::DirectXPixelFormat::R16G16B16A16Float,
                1,   // single buffer
                itemSize);
        } catch (...) {
            framePool = nullptr;
        }
        if (!framePool) {
            framePool = winrt_cap::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice,
                winrt_dx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1,
                itemSize);
        }

        auto session = framePool.CreateCaptureSession(item);

        // Suppress the yellow capture border and cursor (requires Windows 11 / 10 2104+).
        try {
            session.IsBorderRequired(false);
        } catch (...) { /* Not available on this OS version. */ }

        try {
            session.IsCursorCaptureEnabled(false);
        } catch (...) { /* Not available on this OS version. */ }

        // ── Wait for one frame via event ────────────────────────────
        HANDLE frameEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!frameEvent) return std::nullopt;

        std::atomic<bool> gotFrame{false};

        auto token = framePool.FrameArrived(
            [&](winrt_cap::Direct3D11CaptureFramePool const&, winrt::Windows::Foundation::IInspectable const&) {
                if (!gotFrame.exchange(true)) {
                    ::SetEvent(frameEvent);
                }
            });

        session.StartCapture();

        // Wait up to 2 seconds for the first frame.
        ::WaitForSingleObject(frameEvent, 2000);
        ::CloseHandle(frameEvent);

        if (!gotFrame.load()) {
            session.Close();
            framePool.Close();
            return std::nullopt;
        }

        // ── Read the captured frame ─────────────────────────────────
        auto frame = framePool.TryGetNextFrame();
        if (!frame) {
            session.Close();
            framePool.Close();
            return std::nullopt;
        }

        auto surface = frame.Surface();

        // Get the underlying D3D11 texture via the interop interface.
        // IDirect3DDxgiInterfaceAccess lives in ::Windows::Graphics::DirectX::Direct3D11.
        using DxgiAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
        ComPtr<DxgiAccess> access;
        winrt::check_hresult(
            reinterpret_cast<IInspectable*>(winrt::get_abi(surface))
                ->QueryInterface(IID_PPV_ARGS(&access)));

        ComPtr<ID3D11Texture2D> surfaceTex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&surfaceTex)));

        FrameData result;
        bool ok = CopyTextureToFrame(device, d3dCtx.Get(), surfaceTex.Get(), result);

        frame.Close();
        session.Close();
        framePool.Close();

        if (!ok || result.width == 0 || result.height == 0) {
            return std::nullopt;
        }

        return result;
    }
    catch (...) {
        // WinRT or interop failure — not supported on this OS, or window was closed.
        return std::nullopt;
    }
}

} // namespace screencap::capture
