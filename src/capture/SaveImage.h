#pragma once

#include "capture/FrameData.h"

#include <string>

namespace screencap::capture {

// Shows a Save dialog (NFD) and writes the frame as PNG via WIC.
// Returns true if saved successfully, false if cancelled or error.
[[nodiscard]] bool SaveImageInteractive(const FrameData& frame);

// Copies the frame to the Windows clipboard as a CF_DIB bitmap.
// Returns true on success.
[[nodiscard]] bool CopyImageToClipboard(const FrameData& frame);

// Write a small thumbnail PNG to %TEMP% for toast notifications.
// Returns true on success.  The output path is GetThumbnailTempPath().
bool WriteThumbnailPng(const FrameData& frame);

// Deterministic temp path for the toast thumbnail.
[[nodiscard]] std::wstring GetThumbnailTempPath();

} // namespace screencap::capture
