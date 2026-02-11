#pragma once

namespace screencap::capture {

// Compute shader: BGRA8 (sRGB) → RGBA16F (linear scRGB).
//
// t0 = source BGRA8 texture (SRV)
// u0 = destination FP16 texture (UAV)
// b0 = { srcOffset, dstOffset, blitSize }
inline constexpr const char kBgra8ToFp16CS[] = R"(
Texture2D<float4>   srcTex : register(t0);
RWTexture2D<float4> dstTex : register(u0);

cbuffer BlitParams : register(b0) {
    int2 srcOffset;
    int2 dstOffset;
    int2 blitSize;
};

float SrgbToLinear(float c) {
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if ((int)dtid.x >= blitSize.x || (int)dtid.y >= blitSize.y)
        return;

    // SRV reads BGRA8 in RGBA channel order (hardware swizzle).
    float4 c = srcTex[srcOffset + int2(dtid.xy)];

    dstTex[dstOffset + int2(dtid.xy)] = float4(
        SrgbToLinear(c.r),
        SrgbToLinear(c.g),
        SrgbToLinear(c.b),
        1.0);
}
)";

} // namespace screencap::capture
