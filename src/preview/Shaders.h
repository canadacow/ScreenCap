#pragma once

namespace screencap::preview {

// Fullscreen triangle via SV_VertexID -- no vertex buffer needed.
// Produces a single triangle that covers [-1,1] clip space.
inline constexpr const char kVertexShaderHLSL[] = R"(
void VSMain(uint id : SV_VertexID,
            out float4 pos : SV_Position,
            out float2 uv  : TEXCOORD)
{
    uv  = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

// Simple texture sampler.
inline constexpr const char kPixelShaderHLSL[] = R"(
Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Passthrough. Desktop capture produces pixels already in the intended space:
    // - SDR preview: BGRA8 in display-referred space.
    // - HDR preview: RGBA16F linear scRGB.
    return tex.Sample(samp, uv);
}
)";

} // namespace screencap::preview
