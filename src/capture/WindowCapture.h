#pragma once

#include "capture/FrameData.h"

#include <windows.h>

#include <optional>

struct ID3D11Device;

namespace screencap::capture {

// Capture a single window using the Windows Graphics Capture API.
// Works even when the target window is occluded by other windows.
// Returns std::nullopt on failure (unsupported OS, window closed, etc.).
[[nodiscard]] std::optional<FrameData> CaptureWindow(HWND hwnd, ID3D11Device* device);

} // namespace screencap::capture
