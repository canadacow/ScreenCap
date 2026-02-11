#pragma once

#include "capture/FrameData.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <optional>
#include <vector>

namespace screencap::capture {

// Persistent Desktop Duplication engine.
// Initialise once at startup; CaptureFullDesktop() then acquires frames
// with near-zero latency (no device / output re-creation).
class DesktopDuplicator final {
public:
    DesktopDuplicator() = default;
    ~DesktopDuplicator() = default;

    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;
    DesktopDuplicator(DesktopDuplicator&&) = default;
    DesktopDuplicator& operator=(DesktopDuplicator&&) = default;

    // Enumerate outputs and set up duplications using a shared device.
    // Returns false on fatal failure (no outputs, etc.).
    [[nodiscard]] bool Init(ID3D11Device* device);

    // Acquire the current desktop frame from all monitors.
    // Returns std::nullopt on failure (DEVICE_LOST, no frames, etc.).
    [[nodiscard]] std::optional<FrameData> CaptureFullDesktop();

    // Virtual-desktop bounding rect (union of all monitors).
    struct Bounds {
        int left{};
        int top{};
        int right{};
        int bottom{};
        [[nodiscard]] uint32_t Width() const noexcept { return static_cast<uint32_t>(right - left); }
        [[nodiscard]] uint32_t Height() const noexcept { return static_cast<uint32_t>(bottom - top); }
    };

private:
    struct DuplInfo {
        Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl;
        DXGI_OUTPUT_DESC desc{};
    };

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  ctx_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> convertCS_;
    std::vector<DuplInfo>                        dupls_;
    Bounds                                       bounds_{};
    bool                                         ready_{false};
};

} // namespace screencap::capture
