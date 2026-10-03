// =============================================================================
// Acrylic background blur for Direct3D 11.
//
// Pipeline (all at the moment blaze::RenderBlur() is called, i.e. after the game
// has drawn its frame and before ImGui draws):
//   1. Copy (or MSAA-resolve) the render target into `copy_`.
//   2. Dual-Kawase blur: N downsample passes (1/2, 1/4, ...) then N-1 upsample
//      passes back to 1/2 resolution. Cheap and very smooth: ~0.1 ms at 1080p.
//   3. Composite fullscreen onto the target with acrylic treatment
//      (saturation boost, luminosity dim, tint, grain) and alpha = visibility,
//      so it fades in/out with the menu.
//
// Every piece of pipeline state touched is backed up and restored, which makes
// this safe to run inside a hooked IDXGISwapChain::Present.
// =============================================================================
#include "internal.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace blaze::detail::blur {
namespace {

constexpr int kMaxLevels = 6;

const char* kShaderSrc = R"HLSL(
cbuffer Params : register(b0) {
    float2 texel;      // 1 / source size
    float  offset;
    float  pad0;
    float4 tint;
    float4 acrylic;    // x tintAmount, y noise, z saturation, w dim
    float4 misc;       // x visibility, y isDark
};
Texture2D    src : register(t0);
SamplerState smp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSDown(VSOut i) : SV_Target {
    float2 h = texel * 0.5 * offset;
    float4 s = src.Sample(smp, i.uv) * 4.0;
    s += src.Sample(smp, i.uv - h);
    s += src.Sample(smp, i.uv + h);
    s += src.Sample(smp, i.uv + float2(h.x, -h.y));
    s += src.Sample(smp, i.uv - float2(h.x, -h.y));
    return s * 0.125;
}

float4 PSUp(VSOut i) : SV_Target {
    float2 h = texel * 0.5 * offset;
    float4 s = src.Sample(smp, i.uv + float2(-h.x * 2, 0));
    s += src.Sample(smp, i.uv + float2(-h.x,  h.y)) * 2.0;
    s += src.Sample(smp, i.uv + float2(0,  h.y * 2));
    s += src.Sample(smp, i.uv + float2( h.x,  h.y)) * 2.0;
    s += src.Sample(smp, i.uv + float2( h.x * 2, 0));
    s += src.Sample(smp, i.uv + float2( h.x, -h.y)) * 2.0;
    s += src.Sample(smp, i.uv + float2(0, -h.y * 2));
    s += src.Sample(smp, i.uv + float2(-h.x, -h.y)) * 2.0;
    return s / 12.0;
}

float4 PSComposite(VSOut i) : SV_Target {
    float3 c = src.Sample(smp, i.uv).rgb;
    float  l = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(l.xxx, c, acrylic.z);                                 // saturation
    c = misc.y > 0.5 ? c * (1.0 - acrylic.w) : lerp(c, 1.0.xxx, acrylic.w); // luminosity
    c = lerp(c, tint.rgb, acrylic.x);                              // tint
    float n = frac(sin(dot(floor(i.pos.xy), float2(12.9898, 78.233))) * 43758.5453) - 0.5;
    c += n * acrylic.y;                                            // grain
    return float4(saturate(c), misc.x);
}
)HLSL";

struct alignas(16) Params {
    float texel[2];
    float offset;
    float pad0;
    float tint[4];
    float acrylic[4];
    float misc[4];
};

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Level {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    void Release() { SafeRelease(srv); SafeRelease(rtv); SafeRelease(tex); }
};

ID3D11Device*        g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11VertexShader*  g_vs = nullptr;
ID3D11PixelShader*   g_psDown = nullptr;
ID3D11PixelShader*   g_psUp = nullptr;
ID3D11PixelShader*   g_psComposite = nullptr;
ID3D11Buffer*        g_cb = nullptr;
ID3D11SamplerState*  g_sampler = nullptr;
ID3D11BlendState*    g_blendOpaque = nullptr;
ID3D11BlendState*    g_blendAlpha = nullptr;
ID3D11RasterizerState*   g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;

ID3D11Texture2D*          g_copy = nullptr;
ID3D11ShaderResourceView* g_copySrv = nullptr;
UINT        g_width = 0, g_height = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
Level       g_levels[kMaxLevels];
bool        g_failed = false;

DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}

bool Compile(const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "blaze_blur", nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(hr)) {
        if (errors) {
            blaze::Log::Write(LogLevel::Error, "blur", "Shader %s failed: %s", entry, (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        return false;
    }
    SafeRelease(errors);
    return true;
}

bool CreatePS(const char* entry, ID3D11PixelShader** out) {
    ID3DBlob* blob = nullptr;
    if (!Compile(entry, "ps_5_0", &blob)) return false;
    HRESULT hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
    blob->Release();
    return SUCCEEDED(hr);
}

bool CreateTargets(UINT w, UINT h, DXGI_FORMAT srcFormat) {
    ReleaseTargets();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = srcFormat; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_copy))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = TypedFormat(srcFormat);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(g_copy, &sd, &g_copySrv))) return false;

    // Blur chain in R11G11B10_FLOAT: no banding, half the bandwidth of RGBA16F.
    for (int i = 0; i < kMaxLevels; ++i) {
        Level& L = g_levels[i];
        L.w = std::max(1u, w >> (i + 1));
        L.h = std::max(1u, h >> (i + 1));
        D3D11_TEXTURE2D_DESC ld{};
        ld.Width = L.w; ld.Height = L.h; ld.MipLevels = 1; ld.ArraySize = 1;
        ld.Format = DXGI_FORMAT_R11G11B10_FLOAT; ld.SampleDesc.Count = 1;
        ld.Usage = D3D11_USAGE_DEFAULT;
        ld.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&ld, nullptr, &L.tex))) return false;
        if (FAILED(g_device->CreateRenderTargetView(L.tex, nullptr, &L.rtv))) return false;
        if (FAILED(g_device->CreateShaderResourceView(L.tex, nullptr, &L.srv))) return false;
    }
    g_width = w; g_height = h; g_format = srcFormat;
    return true;
}

// Full backup of the state this module modifies.
struct StateBackup {
    UINT                     numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT           viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT                     numScissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT               scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RenderTargetView*  rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView*  dsv = nullptr;
    ID3D11RasterizerState*   rs = nullptr;
    ID3D11BlendState*        blend = nullptr;
    FLOAT                    blendFactor[4]{};
    UINT                     sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT                     stencilRef = 0;
    ID3D11ShaderResourceView* psSrv = nullptr;
    ID3D11SamplerState*      psSampler = nullptr;
    ID3D11Buffer*            psCb = nullptr;
    ID3D11PixelShader*       ps = nullptr;
    ID3D11VertexShader*      vs = nullptr;
    ID3D11GeometryShader*    gs = nullptr;
    ID3D11HullShader*        hs = nullptr;
    ID3D11DomainShader*      ds = nullptr;
    ID3D11ClassInstance*     psInst[256]{};  UINT psInstCount = 256;
    ID3D11ClassInstance*     vsInst[256]{};  UINT vsInstCount = 256;
    ID3D11ClassInstance*     gsInst[256]{};  UINT gsInstCount = 256;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11InputLayout*       layout = nullptr;

    void Save(ID3D11DeviceContext* c) {
        c->RSGetViewports(&numViewports, viewports);
        c->RSGetScissorRects(&numScissors, scissors);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        c->RSGetState(&rs);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->PSGetShaderResources(0, 1, &psSrv);
        c->PSGetSamplers(0, 1, &psSampler);
        c->PSGetConstantBuffers(0, 1, &psCb);
        c->PSGetShader(&ps, psInst, &psInstCount);
        c->VSGetShader(&vs, vsInst, &vsInstCount);
        c->GSGetShader(&gs, gsInst, &gsInstCount);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->IAGetPrimitiveTopology(&topology);
        c->IAGetInputLayout(&layout);
    }

    void Restore(ID3D11DeviceContext* c) {
        c->RSSetViewports(numViewports, viewports);
        c->RSSetScissorRects(numScissors, scissors);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        c->RSSetState(rs);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->PSSetShaderResources(0, 1, &psSrv);
        c->PSSetSamplers(0, 1, &psSampler);
        c->PSSetConstantBuffers(0, 1, &psCb);
        c->PSSetShader(ps, psInst, psInstCount);
        c->VSSetShader(vs, vsInst, vsInstCount);
        c->GSSetShader(gs, gsInst, gsInstCount);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->IASetPrimitiveTopology(topology);
        c->IASetInputLayout(layout);

        for (auto*& r : rtvs) SafeRelease(r);
        SafeRelease(dsv); SafeRelease(rs); SafeRelease(blend); SafeRelease(depth);
        SafeRelease(psSrv); SafeRelease(psSampler); SafeRelease(psCb);
        SafeRelease(ps); SafeRelease(vs); SafeRelease(gs); SafeRelease(hs); SafeRelease(ds);
        for (UINT i = 0; i < psInstCount; ++i) SafeRelease(psInst[i]);
        for (UINT i = 0; i < vsInstCount; ++i) SafeRelease(vsInst[i]);
        for (UINT i = 0; i < gsInstCount; ++i) SafeRelease(gsInst[i]);
        SafeRelease(layout);
    }
};

void Pass(ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, UINT srcW, UINT srcH,
          ID3D11RenderTargetView* dst, UINT dstW, UINT dstH, Params& p) {
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);       // avoid read/write hazard warnings
    g_ctx->OMSetRenderTargets(1, &dst, nullptr);

    D3D11_VIEWPORT vp{ 0, 0, float(dstW), float(dstH), 0, 1 };
    g_ctx->RSSetViewports(1, &vp);

    p.texel[0] = 1.0f / float(srcW);
    p.texel[1] = 1.0f / float(srcH);
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, &p, 0, 0);

    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &src);
    g_ctx->Draw(3, 0);
}

} // namespace

bool Init(ID3D11Device* device, ID3D11DeviceContext* context) {
    g_device = device; g_ctx = context;
    g_device->AddRef(); g_ctx->AddRef();
    g_failed = true;

    ID3DBlob* vsBlob = nullptr;
    if (!Compile("VSMain", "vs_5_0", &vsBlob)) return false;
    HRESULT hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    vsBlob->Release();
    if (FAILED(hr)) return false;
    if (!CreatePS("PSDown", &g_psDown) || !CreatePS("PSUp", &g_psUp) || !CreatePS("PSComposite", &g_psComposite))
        return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, &g_cb))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sd, &g_sampler))) return false;

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_device->CreateBlendState(&bl, &g_blendOpaque))) return false;
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(g_device->CreateBlendState(&bl, &g_blendAlpha))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(g_device->CreateRasterizerState(&rd, &g_raster))) return false;

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    if (FAILED(g_device->CreateDepthStencilState(&dd, &g_depth))) return false;

    g_failed = false;
    return true;
}

bool Available() { return g_device && !g_failed; }

void ReleaseTargets() {
    SafeRelease(g_copySrv);
    SafeRelease(g_copy);
    for (auto& L : g_levels) L.Release();
    g_width = g_height = 0;
    g_format = DXGI_FORMAT_UNKNOWN;
}

void Shutdown() {
    ReleaseTargets();
    SafeRelease(g_depth); SafeRelease(g_raster);
    SafeRelease(g_blendAlpha); SafeRelease(g_blendOpaque);
    SafeRelease(g_sampler); SafeRelease(g_cb);
    SafeRelease(g_psComposite); SafeRelease(g_psUp); SafeRelease(g_psDown); SafeRelease(g_vs);
    SafeRelease(g_ctx); SafeRelease(g_device);
}

bool Apply(ID3D11RenderTargetView* target, const BlurSettings& s, const ImVec4& tint, float visibility) {
    if (g_failed || !g_device || !target || !s.enabled || visibility <= 0.001f) return false;

    ID3D11Resource* res = nullptr;
    target->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
    target->GetDesc(&rtvDesc);
    DXGI_FORMAT fmt = desc.Format;
    if (fmt != g_format || desc.Width != g_width || desc.Height != g_height) {
        if (!CreateTargets(desc.Width, desc.Height, fmt)) {
            Log::Write(LogLevel::Error, "blur", "Failed to create blur targets (%ux%u, format %d) - blur disabled",
                       desc.Width, desc.Height, int(fmt));
            ReleaseTargets();
            tex->Release();
            g_failed = true;
            return false;
        }
    }

    if (desc.SampleDesc.Count > 1)
        g_ctx->ResolveSubresource(g_copy, 0, tex, rtvDesc.Texture2D.MipSlice, TypedFormat(rtvDesc.Format));
    else
        g_ctx->CopyResource(g_copy, tex);
    tex->Release();

    StateBackup backup;
    backup.Save(g_ctx);

    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->HSSetShader(nullptr, nullptr, 0);
    g_ctx->DSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->RSSetState(g_raster);
    g_ctx->OMSetDepthStencilState(g_depth, 0);
    const float zero[4] = {};
    g_ctx->OMSetBlendState(g_blendOpaque, zero, 0xFFFFFFFF);

    const int levels = std::clamp(s.strength, 1, kMaxLevels);
    Params p{};
    p.offset = 1.0f;

    // Downsample chain
    Pass(g_psDown, g_copySrv, g_width, g_height, g_levels[0].rtv, g_levels[0].w, g_levels[0].h, p);
    for (int i = 1; i < levels; ++i)
        Pass(g_psDown, g_levels[i - 1].srv, g_levels[i - 1].w, g_levels[i - 1].h,
             g_levels[i].rtv, g_levels[i].w, g_levels[i].h, p);
    // Upsample chain back to level 0
    for (int i = levels - 1; i > 0; --i)
        Pass(g_psUp, g_levels[i].srv, g_levels[i].w, g_levels[i].h,
             g_levels[i - 1].rtv, g_levels[i - 1].w, g_levels[i - 1].h, p);

    // Acrylic composite onto the real target
    p.tint[0] = tint.x; p.tint[1] = tint.y; p.tint[2] = tint.z; p.tint[3] = 1.0f;
    p.acrylic[0] = std::clamp(s.tintAmount, 0.0f, 1.0f);
    p.acrylic[1] = std::clamp(s.noise, 0.0f, 0.5f);
    p.acrylic[2] = std::clamp(s.saturation, 0.0f, 3.0f);
    p.acrylic[3] = std::clamp(s.dim, 0.0f, 1.0f);
    p.misc[0] = std::clamp(visibility, 0.0f, 1.0f);
    p.misc[1] = theme::IsDark() ? 1.0f : 0.0f;
    g_ctx->OMSetBlendState(g_blendAlpha, zero, 0xFFFFFFFF);
    Pass(g_psComposite, g_levels[0].srv, g_levels[0].w, g_levels[0].h, target, g_width, g_height, p);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    backup.Restore(g_ctx);
    return true;
}

} // namespace blaze::detail::blur
