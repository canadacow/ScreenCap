#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace screencap::capture {

struct FrameData {
    // CPU pixel buffer (may be empty when gpuTexture is set).
    // - SDR: format = DXGI_FORMAT_B8G8R8A8_UNORM, bytesPerPixel = 4, pixels are BGRA8.
    // - HDR/scRGB: format = DXGI_FORMAT_R16G16B16A16_FLOAT, bytesPerPixel = 8, pixels are RGBA16F (linear).
    std::vector<uint8_t> pixels;

    // GPU-resident texture (may be null for CPU-only frames such as crops).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuTexture;

    uint32_t width{};
    uint32_t height{};
    uint32_t format{};        // DXGI_FORMAT
    uint32_t bytesPerPixel{}; // 4 or 8
};

// Ensure frame.pixels is populated.  If pixels are already present this is
// a no-op.  Otherwise reads back from gpuTexture via a staging copy.
// Returns true when CPU pixels are available afterwards.
[[nodiscard]] bool ReadbackPixels(FrameData& frame, ID3D11DeviceContext* ctx);

} // namespace screencap::capture
