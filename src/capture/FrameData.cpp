#include "capture/FrameData.h"
#include "capture/PixelFormats.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace screencap::capture {

bool ReadbackPixels(FrameData& frame, ID3D11DeviceContext* ctx)
{
    if (!frame.pixels.empty()) return true;          // Already populated.
    if (!frame.gpuTexture || !ctx) return false;

    D3D11_TEXTURE2D_DESC desc{};
    frame.gpuTexture->GetDesc(&desc);

    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);

    // Create a staging texture for readback.
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width          = desc.Width;
    stagingDesc.Height         = desc.Height;
    stagingDesc.MipLevels      = 1;
    stagingDesc.ArraySize      = 1;
    stagingDesc.Format         = desc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage          = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) return false;

    ctx->CopyResource(staging.Get(), frame.gpuTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    const uint32_t bpp       = BytesPerPixel(desc.Format);
    const uint32_t dstStride = desc.Width * bpp;
    frame.pixels.resize(static_cast<size_t>(dstStride) * desc.Height);

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t row = 0; row < desc.Height; ++row) {
        std::memcpy(
            frame.pixels.data() + static_cast<size_t>(row) * dstStride,
            src + static_cast<size_t>(row) * mapped.RowPitch,
            dstStride);
    }

    ctx->Unmap(staging.Get(), 0);
    return true;
}

} // namespace screencap::capture
