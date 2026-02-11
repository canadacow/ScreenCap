#include "capture/SaveImage.h"
#include "capture/PixelFormats.h"

#include <nfd.h>

#include <dxgiformat.h>
#include <windows.h>
#include <wingdi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdlib>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace screencap::capture {
namespace {

// ── Per-monitor SDR white level ─────────────────────────────────────
//
// On an HDR desktop, the DWM composes everything into linear scRGB where
// 1.0 = 80 nits.  The user's "SDR content brightness" slider (paper white)
// boosts SDR white content in the captured buffer:
//
//   scRGB_value_of_SDR_white = paperWhiteNits / 80
//
// To produce a correct SDR PNG we simply divide by that ratio so SDR white
// maps back to 1.0 (linear) → 255 (sRGB 8-bit).
// HDR highlights above 1.0 after normalisation are clipped, which is the
// same thing an SDR display does.

[[nodiscard]] float GetSdrWhiteNitsForMonitor(HMONITOR mon) noexcept
{
    if (!mon) {
        return 80.0f;
    }

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) {
        return 80.0f;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG st = ::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (st != ERROR_SUCCESS || pathCount == 0) {
        return 80.0f;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    st = ::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (st != ERROR_SUCCESS) {
        return 80.0f;
    }

    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto& p = paths[i];

        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = p.sourceInfo.adapterId;
        srcName.header.id = p.sourceInfo.id;

        if (::DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) {
            continue;
        }

        if (::lstrcmpiW(srcName.viewGdiDeviceName, mi.szDevice) != 0) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL sdr{};
        sdr.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        sdr.header.size = sizeof(sdr);
        sdr.header.adapterId = p.targetInfo.adapterId;
        sdr.header.id = p.targetInfo.id;

        if (::DisplayConfigGetDeviceInfo(&sdr.header) != ERROR_SUCCESS) {
            return 80.0f;
        }

        // SDRWhiteLevel is "a multiplier of 80 nits, multiplied by 1000".
        // nits = (SDRWhiteLevel / 1000.0) * 80
        const float nits = (static_cast<float>(sdr.SDRWhiteLevel) / 1000.0f) * 80.0f;
        return (nits > 0.0f) ? nits : 80.0f;
    }

    return 80.0f;
}

[[nodiscard]] float GetSdrWhiteNitsForPrimaryMonitor() noexcept
{
    // Use the primary monitor (where the tray icon lives) as the reference.
    const HMONITOR mon = ::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return GetSdrWhiteNitsForMonitor(mon);
}

// ── scRGB FP16 → BGRA8 for PNG ─────────────────────────────────────
//
// The captured scRGB buffer has:
//   SDR white  = paperWhiteNits / 80   (e.g. 2.5 at 200 nits)
//   HDR peak   = monitorPeakNits / 80  (e.g. 12.5 at 1000 nits)
//
// To get a correct SDR PNG:
//   1. Divide by (paperWhiteNits / 80) → SDR white = 1.0
//   2. Clamp to [0, 1]  (clips HDR highlights — same as an SDR display)
//   3. Apply sRGB gamma
//   4. Quantise to 8-bit

[[nodiscard]] bool ScRgb16fToBgra8(const FrameData& in, std::vector<uint8_t>& outBgra8) noexcept
{
    if (in.width == 0 || in.height == 0) {
        return false;
    }
    if (static_cast<DXGI_FORMAT>(in.format) != DXGI_FORMAT_R16G16B16A16_FLOAT || in.bytesPerPixel != 8) {
        return false;
    }

    const float paperWhite = GetSdrWhiteNitsForPrimaryMonitor();
    // scRGB value that corresponds to SDR white on this monitor:
    const float sdrWhiteScRgb = paperWhite / 80.0f;
    // We divide by this to bring SDR white back to 1.0 linear.
    const float scale = 1.0f / sdrWhiteScRgb; // = 80 / paperWhite

    outBgra8.resize(static_cast<size_t>(in.width) * 4 * in.height);

    const auto* src = reinterpret_cast<const uint16_t*>(in.pixels.data());
    auto* dst = outBgra8.data();

    for (uint32_t y = 0; y < in.height; ++y) {
        for (uint32_t x = 0; x < in.width; ++x) {
            const size_t si = (static_cast<size_t>(y) * in.width + x) * 4;

            // Read linear scRGB, normalise by paper white, clamp negatives + HDR.
            float r = (std::max)(0.0f, HalfToFloat(src[si + 0]) * scale);
            float g = (std::max)(0.0f, HalfToFloat(src[si + 1]) * scale);
            float b = (std::max)(0.0f, HalfToFloat(src[si + 2]) * scale);

            r = (std::min)(1.0f, r);
            g = (std::min)(1.0f, g);
            b = (std::min)(1.0f, b);

            // Linear → sRGB gamma → 8-bit.  Note: output is BGRA order.
            const size_t di = (static_cast<size_t>(y) * in.width + x) * 4;
            dst[di + 0] = FloatToUNorm8(LinearToSrgb(b));
            dst[di + 1] = FloatToUNorm8(LinearToSrgb(g));
            dst[di + 2] = FloatToUNorm8(LinearToSrgb(r));
            dst[di + 3] = 255;
        }
    }

    return true;
}

// ── WIC PNG writer ──────────────────────────────────────────────────

bool WritePng(const FrameData& frame, const wchar_t* path)
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) {
        return false;
    }

    hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) {
        return false;
    }

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    hr = encoder->CreateNewFrame(&frameEncode, nullptr);
    if (FAILED(hr)) {
        return false;
    }

    hr = frameEncode->Initialize(nullptr);
    if (FAILED(hr)) {
        return false;
    }

    hr = frameEncode->SetSize(frame.width, frame.height);
    if (FAILED(hr)) {
        return false;
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    hr = frameEncode->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) {
        return false;
    }

    // Convert to 8-bit BGRA for WIC.
    std::vector<uint8_t> bgra8;
    const auto fmt = static_cast<DXGI_FORMAT>(frame.format);
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && frame.bytesPerPixel == 8) {
        if (!ScRgb16fToBgra8(frame, bgra8)) {
            return false;
        }
    } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM && frame.bytesPerPixel == 4) {
        // Already BGRA8 — use directly.
        bgra8 = frame.pixels;
    } else {
        return false;
    }

    const UINT stride = frame.width * 4;
    const UINT bufferSize = stride * frame.height;
    hr = frameEncode->WritePixels(
        frame.height,
        stride,
        bufferSize,
        const_cast<BYTE*>(bgra8.data()));
    if (FAILED(hr)) {
        return false;
    }

    hr = frameEncode->Commit();
    if (FAILED(hr)) {
        return false;
    }

    hr = encoder->Commit();
    if (FAILED(hr)) {
        return false;
    }

    return true;
}

} // namespace

bool SaveImageInteractive(const FrameData& frame)
{
    nfdchar_t* outPath = nullptr;
    const nfdresult_t result = NFD_SaveDialog("png", nullptr, &outPath);

    if (result != NFD_OKAY || !outPath) {
        return false;
    }

    // NFD returns a narrow (UTF-8/ANSI) path; convert to wide for WIC.
    const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, outPath, -1, nullptr, 0);
    std::wstring widePath(static_cast<size_t>(wideLen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, outPath, -1, widePath.data(), wideLen);
    std::free(outPath);

    // Append .png if the user didn't type an extension.
    if (widePath.size() >= 5) {
        auto ext = widePath.substr(widePath.size() - 5);
        while (!ext.empty() && ext.back() == L'\0') {
            ext.pop_back();
        }
        if (ext.size() < 4 ||
            (ext[ext.size()-4] != L'.' ||
             (ext[ext.size()-3] != L'p' && ext[ext.size()-3] != L'P') ||
             (ext[ext.size()-2] != L'n' && ext[ext.size()-2] != L'N') ||
             (ext[ext.size()-1] != L'g' && ext[ext.size()-1] != L'G'))) {
            while (!widePath.empty() && widePath.back() == L'\0') {
                widePath.pop_back();
            }
            widePath += L".png";
        }
    } else {
        while (!widePath.empty() && widePath.back() == L'\0') {
            widePath.pop_back();
        }
        widePath += L".png";
    }

    return WritePng(frame, widePath.c_str());
}

bool CopyImageToClipboard(const FrameData& frame)
{
    if (frame.width == 0 || frame.height == 0) {
        return false;
    }

    // Convert to BGRA8 if needed.
    std::vector<uint8_t> bgra8;
    const auto fmt = static_cast<DXGI_FORMAT>(frame.format);
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && frame.bytesPerPixel == 8) {
        if (!ScRgb16fToBgra8(frame, bgra8)) {
            return false;
        }
    } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM && frame.bytesPerPixel == 4) {
        bgra8 = frame.pixels;
    } else {
        return false;
    }

    // Build a CF_DIB: BITMAPINFOHEADER + bottom-up pixel rows.
    const DWORD stride = frame.width * 4;
    const DWORD imageSize = stride * frame.height;
    const DWORD totalSize = sizeof(BITMAPINFOHEADER) + imageSize;

    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (!hMem) {
        return false;
    }

    auto* ptr = static_cast<uint8_t*>(::GlobalLock(hMem));
    if (!ptr) {
        ::GlobalFree(hMem);
        return false;
    }

    // Fill BITMAPINFOHEADER.
    auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(ptr);
    std::memset(bih, 0, sizeof(BITMAPINFOHEADER));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = static_cast<LONG>(frame.width);
    bih->biHeight = static_cast<LONG>(frame.height); // positive = bottom-up
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = imageSize;

    // Copy rows in reverse order (top-down source -> bottom-up DIB).
    auto* dst = ptr + sizeof(BITMAPINFOHEADER);
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint32_t srcRow = frame.height - 1 - y;
        std::memcpy(
            dst + static_cast<size_t>(y) * stride,
            bgra8.data() + static_cast<size_t>(srcRow) * stride,
            stride);
    }

    ::GlobalUnlock(hMem);

    if (!::OpenClipboard(nullptr)) {
        ::GlobalFree(hMem);
        return false;
    }

    ::EmptyClipboard();
    const HANDLE result = ::SetClipboardData(CF_DIB, hMem);
    ::CloseClipboard();

    if (!result) {
        // SetClipboardData failed; the system did not take ownership.
        ::GlobalFree(hMem);
        return false;
    }

    // On success the system owns hMem; do NOT free it.
    return true;
}

// ── Toast thumbnail ─────────────────────────────────────────────────

std::wstring GetThumbnailTempPath()
{
    wchar_t tempDir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"ScreenCap_thumb.png";
}

bool WriteThumbnailPng(const FrameData& frame)
{
    // Delete any stale thumbnail from a previous capture.
    const auto path = GetThumbnailTempPath();
    (void)::DeleteFileW(path.c_str());

    if (frame.width == 0 || frame.height == 0 || frame.pixels.empty()) {
        return false;
    }

    // Get BGRA8 pixels.
    std::vector<uint8_t> bgra8;
    const auto fmt = static_cast<DXGI_FORMAT>(frame.format);
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && frame.bytesPerPixel == 8) {
        if (!ScRgb16fToBgra8(frame, bgra8)) return false;
    } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM && frame.bytesPerPixel == 4) {
        bgra8 = frame.pixels;
    } else {
        return false;
    }

    // Calculate scaled dimensions (max 360px on longest edge).
    constexpr uint32_t kMaxDim = 360;
    uint32_t thumbW = frame.width;
    uint32_t thumbH = frame.height;
    if (thumbW > kMaxDim || thumbH > kMaxDim) {
        if (thumbW >= thumbH) {
            thumbH = thumbH * kMaxDim / thumbW;
            thumbW = kMaxDim;
        } else {
            thumbW = thumbW * kMaxDim / thumbH;
            thumbH = kMaxDim;
        }
    }
    if (thumbW == 0) thumbW = 1;
    if (thumbH == 0) thumbH = 1;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    // Create WIC bitmap from the full-size BGRA8 data.
    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(
        frame.width, frame.height,
        GUID_WICPixelFormat32bppBGRA,
        frame.width * 4,
        static_cast<UINT>(bgra8.size()),
        bgra8.data(),
        &bitmap);
    if (FAILED(hr)) return false;

    // Scale down.
    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr)) return false;

    hr = scaler->Initialize(bitmap.Get(), thumbW, thumbH, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return false;

    // Encode to PNG.
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    hr = encoder->CreateNewFrame(&frameEncode, nullptr);
    if (FAILED(hr)) return false;

    hr = frameEncode->Initialize(nullptr);
    if (FAILED(hr)) return false;

    hr = frameEncode->SetSize(thumbW, thumbH);
    if (FAILED(hr)) return false;

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    hr = frameEncode->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) return false;

    hr = frameEncode->WriteSource(scaler.Get(), nullptr);
    if (FAILED(hr)) return false;

    hr = frameEncode->Commit();
    if (FAILED(hr)) return false;

    hr = encoder->Commit();
    if (FAILED(hr)) return false;

    return true;
}

} // namespace screencap::capture
