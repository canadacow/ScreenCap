# ScreenCap

A Windows screen capture tool that correctly captures HDR monitors.

Windows Snipping Tool, ShareX, and everything else either clip HDR content to SDR at capture time or produce washed-out results. ScreenCap captures the full scRGB FP16 framebuffer via Desktop Duplication and tone-maps to SDR PNG on output, preserving what you actually see on screen.

## Features

- **Region capture** (PrtScn) -- drag to select
- **Window capture** (Alt+PrtScn) -- hover to pick
- **Full desktop capture** (Ctrl+PrtScn) -- all monitors composited
- Save to file (PNG) or copy to clipboard
- Toast notification with thumbnail on save/copy
- System tray app, single-instance

## Requirements

- Windows 10 1903+
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.24+

## Building

```bat
git submodule update --init --recursive
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64 --config RelWithDebInfo
```

Or open the folder in Visual Studio 2022 and select the **vs2022-x64** preset.

## License

MIT-0. See [LICENSE](LICENSE).
