#pragma once

#include <dxgiformat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace screencap::capture {

// ── Bytes-per-pixel for common capture formats ──────────────────────

[[nodiscard]] inline uint32_t BytesPerPixel(DXGI_FORMAT fmt) noexcept
{
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    default:
        return 0;
    }
}

// ── Half-float (IEEE 754 binary16) → float ──────────────────────────

[[nodiscard]] inline float HalfToFloat(uint16_t h) noexcept
{
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
    const uint32_t mant = static_cast<uint32_t>(h) & 0x03FFu;

    uint32_t fBits{};
    if (exp == 0) {
        if (mant == 0) {
            fBits = sign;
        } else {
            uint32_t m = mant;
            uint32_t e = 113;
            while ((m & 0x0400u) == 0) {
                m <<= 1;
                --e;
            }
            m &= 0x03FFu;
            fBits = sign | (e << 23) | (m << 13);
        }
    } else if (exp == 31) {
        fBits = sign | 0x7F800000u | (mant << 13);
    } else {
        fBits = sign | ((exp + 112u) << 23) | (mant << 13);
    }

    float out{};
    std::memcpy(&out, &fBits, sizeof(out));
    return out;
}

// ── Quantisation / gamma ────────────────────────────────────────────

[[nodiscard]] inline uint8_t FloatToUNorm8(float v) noexcept
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    const float scaled = v * 255.0f + 0.5f;
    const int i = static_cast<int>(scaled);
    return static_cast<uint8_t>((std::min)(255, (std::max)(0, i)));
}

[[nodiscard]] inline float LinearToSrgb(float c) noexcept
{
    if (c <= 0.0031308f) {
        return c * 12.92f;
    }
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace screencap::capture
