#pragma once

#include "capture/FrameData.h"

struct ID3D11Device;

namespace screencap::preview {

// Displays a captured frame in a borderless fullscreen DX11 window.
// Blocks until the user clicks (save) or presses Esc (discard).
// If copyToClipboard is true, the image goes to the clipboard; otherwise
// a Save dialog is shown.
[[nodiscard]] bool Show(capture::FrameData frame, ID3D11Device* device, bool copyToClipboard = false);

// Same as Show(), but lets the user drag-select a region.
[[nodiscard]] bool ShowRegion(capture::FrameData frame, ID3D11Device* device, bool copyToClipboard = false);

// Shows the desktop with a window-picker overlay.
// Hovering highlights a window; clicking captures it.
[[nodiscard]] bool ShowWindowCapture(capture::FrameData frame, ID3D11Device* device, bool copyToClipboard = false);

} // namespace screencap::preview
