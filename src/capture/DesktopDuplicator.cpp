#include "capture/DesktopDuplicator.h"
#include "capture/ConvertShader.h"
#include "capture/PixelFormats.h"

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace screencap::capture {
namespace {

// ── Per-output frame acquisition ─────────────────────────────────────

using Bounds = DesktopDuplicator::Bounds;

// Constant buffer layout matching the compute shader's BlitParams.
struct BlitParams {
    int srcOffsetX, srcOffsetY;
    int dstOffsetX, dstOffsetY;
    int blitW, blitH;
    int pad0, pad1;           // Align to 16-byte boundary.
};

// Compile and cache the BGRA8→FP16 compute shader (once per device).
ComPtr<ID3D11ComputeShader> CompileConvertCS(ID3D11Device* device)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        kBgra8ToFp16CS,
        sizeof(kBgra8ToFp16CS) - 1,
        "ConvertBgra8ToFp16",
        nullptr, nullptr,
        "CSMain", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &blob, &errors);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID3D11ComputeShader> cs;
    hr = device->CreateComputeShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
    if (FAILED(hr)) return nullptr;

    return cs;
}

// GPU compute blit: BGRA8 (sRGB) → FP16 (linear scRGB).
// Copies the DD-acquired texture to a temp SRV-capable texture, then
// dispatches the conversion shader writing directly into the composite UAV.
bool BlitConvertedGPU(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11ComputeShader* cs,
    ID3D11Texture2D* src,
    const D3D11_TEXTURE2D_DESC& srcDesc,
    ID3D11Texture2D* composite,
    int srcX, int srcY,
    int dstX, int dstY,
    int blitW, int blitH)
{
    HRESULT hr{};

    // 1. Copy the DD texture to a temp texture with SRV binding
    //    (DD textures are DWM-owned and don't support SRV).
    D3D11_TEXTURE2D_DESC tempDesc{};
    tempDesc.Width            = srcDesc.Width;
    tempDesc.Height           = srcDesc.Height;
    tempDesc.MipLevels        = 1;
    tempDesc.ArraySize        = 1;
    tempDesc.Format           = srcDesc.Format;
    tempDesc.SampleDesc.Count = 1;
    tempDesc.Usage            = D3D11_USAGE_DEFAULT;
    tempDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> temp;
    hr = device->CreateTexture2D(&tempDesc, nullptr, &temp);
    if (FAILED(hr)) return false;

    ctx->CopyResource(temp.Get(), src);

    // 2. Create SRV for the source (BGRA8).
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                    = srcDesc.Format;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(temp.Get(), &srvDesc, &srv);
    if (FAILED(hr)) return false;

    // 3. Create UAV for the composite (FP16).
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavDesc.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11UnorderedAccessView> uav;
    hr = device->CreateUnorderedAccessView(composite, &uavDesc, &uav);
    if (FAILED(hr)) return false;

    // 4. Fill constant buffer with blit parameters.
    BlitParams params{};
    params.srcOffsetX = srcX;
    params.srcOffsetY = srcY;
    params.dstOffsetX = dstX;
    params.dstOffsetY = dstY;
    params.blitW      = blitW;
    params.blitH      = blitH;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth      = sizeof(BlitParams);
    cbDesc.Usage           = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA cbInit{};
    cbInit.pSysMem = &params;

    ComPtr<ID3D11Buffer> cb;
    hr = device->CreateBuffer(&cbDesc, &cbInit, &cb);
    if (FAILED(hr)) return false;

    // 5. Dispatch the compute shader.
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, srv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());

    // 16×16 thread groups.
    const UINT groupsX = (static_cast<UINT>(blitW) + 15u) / 16u;
    const UINT groupsY = (static_cast<UINT>(blitH) + 15u) / 16u;
    ctx->Dispatch(groupsX, groupsY, 1);

    // 6. Unbind resources.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11Buffer* nullCB = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSRV);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &nullCB);
    ctx->CSSetShader(nullptr, nullptr, 0);

    return true;
}

// Acquire one monitor's frame and blit it into the composite texture.
bool BlitOutputToComposite(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11ComputeShader* cs,
    IDXGIOutputDuplication* dupl,
    const Bounds& bounds,
    const DXGI_OUTPUT_DESC& outputDesc,
    ID3D11Texture2D* composite,
    DXGI_FORMAT compositeFormat)
{
    // Acquire the current desktop frame.
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl->AcquireNextFrame(1000, &info, &resource);
    if (FAILED(hr) || !resource) {
        return false;
    }

    ComPtr<ID3D11Texture2D> tex;
    hr = resource.As(&tex);
    if (FAILED(hr)) {
        dupl->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc{};
    tex->GetDesc(&texDesc);

    // Calculate clamped blit region.
    const auto compW = static_cast<int>(bounds.Width());
    const auto compH = static_cast<int>(bounds.Height());

    int dstX = outputDesc.DesktopCoordinates.left - bounds.left;
    int dstY = outputDesc.DesktopCoordinates.top  - bounds.top;
    int srcX = 0, srcY = 0;
    int blitW = static_cast<int>(texDesc.Width);
    int blitH = static_cast<int>(texDesc.Height);

    if (dstX < 0) { srcX = -dstX; blitW -= srcX; dstX = 0; }
    if (dstY < 0) { srcY = -dstY; blitH -= srcY; dstY = 0; }
    if (dstX + blitW > compW) { blitW = compW - dstX; }
    if (dstY + blitH > compH) { blitH = compH - dstY; }

    if (blitW <= 0 || blitH <= 0) {
        dupl->ReleaseFrame();
        return false;
    }

    // Check format compatibility for a direct GPU copy.
    const bool formatMatch = (texDesc.Format == compositeFormat);

    if (formatMatch) {
        // Fast path — direct GPU blit (same format, no conversion).
        D3D11_BOX box{};
        box.left   = static_cast<UINT>(srcX);
        box.top    = static_cast<UINT>(srcY);
        box.front  = 0;
        box.right  = static_cast<UINT>(srcX + blitW);
        box.bottom = static_cast<UINT>(srcY + blitH);
        box.back   = 1;

        ctx->CopySubresourceRegion(
            composite, 0,
            static_cast<UINT>(dstX), static_cast<UINT>(dstY), 0,
            tex.Get(), 0, &box);
    } else {
        // Compute-shader path — GPU format conversion (BGRA8 → FP16).
        BlitConvertedGPU(device, ctx, cs, tex.Get(), texDesc,
                         composite,
                         srcX, srcY, dstX, dstY, blitW, blitH);
    }

    dupl->ReleaseFrame();
    return true;
}

} // namespace

// ── Init: enumerate outputs, set up duplications ─────────────────────

bool DesktopDuplicator::Init(ID3D11Device* device)
{
    ready_ = false;
    dupls_.clear();
    device_.Reset();
    ctx_.Reset();
    convertCS_.Reset();

    if (!device) return false;
    device_ = device;
    device_->GetImmediateContext(&ctx_);

    // Pre-compile the format-conversion compute shader.
    convertCS_ = CompileConvertCS(device_.Get());
    // Not fatal if it fails — we just won't handle mixed HDR/SDR setups.

    HRESULT hr{};
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    // Enumerate attached outputs.
    struct OutputInfo {
        ComPtr<IDXGIOutput1> output;
        DXGI_OUTPUT_DESC     desc{};
    };
    std::vector<OutputInfo> outputs;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(i, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        OutputInfo oi;
        output->GetDesc(&oi.desc);
        if (!oi.desc.AttachedToDesktop) continue;
        if (SUCCEEDED(output.As(&oi.output))) {
            outputs.push_back(std::move(oi));
        }
    }
    if (outputs.empty()) return false;

    // Compute virtual-desktop bounding rect.
    bool first = true;
    for (const auto& o : outputs) {
        const auto& r = o.desc.DesktopCoordinates;
        if (first) {
            bounds_ = {r.left, r.top, r.right, r.bottom};
            first = false;
        } else {
            bounds_.left   = (std::min)(bounds_.left,   static_cast<int>(r.left));
            bounds_.top    = (std::min)(bounds_.top,    static_cast<int>(r.top));
            bounds_.right  = (std::max)(bounds_.right,  static_cast<int>(r.right));
            bounds_.bottom = (std::max)(bounds_.bottom, static_cast<int>(r.bottom));
        }
    }

    // Create output duplications.
    dupls_.reserve(outputs.size());
    for (auto& oi : outputs) {
        ComPtr<IDXGIOutputDuplication> dupl;
        ComPtr<IDXGIOutput5> out5;
        if (SUCCEEDED(oi.output.As(&out5)) && out5) {
            const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
            hr = out5->DuplicateOutput1(device_.Get(), 0, static_cast<UINT>(std::size(formats)), formats, &dupl);
        } else {
            hr = oi.output->DuplicateOutput(device_.Get(), &dupl);
        }
        if (SUCCEEDED(hr) && dupl) {
            dupls_.push_back({std::move(dupl), oi.desc});
        }
    }
    if (dupls_.empty()) return false;

    ready_ = true;
    return true;
}

// ── CaptureFullDesktop: GPU-composited frame ─────────────────────────

std::optional<FrameData> DesktopDuplicator::CaptureFullDesktop()
{
    if (!ready_) return std::nullopt;

    const uint32_t totalW = bounds_.Width();
    const uint32_t totalH = bounds_.Height();

    // Always composite into FP16 (linear scRGB).
    constexpr DXGI_FORMAT compositeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Create the composite GPU texture.
    // BIND_UNORDERED_ACCESS is needed for the compute-shader conversion path.
    D3D11_TEXTURE2D_DESC compDesc{};
    compDesc.Width            = totalW;
    compDesc.Height           = totalH;
    compDesc.MipLevels        = 1;
    compDesc.ArraySize        = 1;
    compDesc.Format           = compositeFormat;
    compDesc.SampleDesc.Count = 1;
    compDesc.Usage            = D3D11_USAGE_DEFAULT;
    compDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    ComPtr<ID3D11Texture2D> composite;
    HRESULT hr = device_->CreateTexture2D(&compDesc, nullptr, &composite);
    if (FAILED(hr)) return std::nullopt;

    // Blit each monitor into the composite.
    bool anyCaptured = false;
    for (auto& di : dupls_) {
        if (BlitOutputToComposite(device_.Get(), ctx_.Get(), convertCS_.Get(),
                                  di.dupl.Get(), bounds_, di.desc,
                                  composite.Get(), compositeFormat)) {
            anyCaptured = true;
        }
    }

    if (!anyCaptured) {
        return std::nullopt;
    }

    FrameData frame;
    frame.gpuTexture    = std::move(composite);
    frame.width         = totalW;
    frame.height        = totalH;
    frame.format        = static_cast<uint32_t>(compositeFormat);
    frame.bytesPerPixel = BytesPerPixel(compositeFormat);
    // pixels left empty — read back lazily via ReadbackPixels() when needed.
    return frame;
}

} // namespace screencap::capture
