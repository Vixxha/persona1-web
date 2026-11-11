// Win32 window that shows the emulated 480x272 screen. The window lives on its own
// thread so the game loop never waits for the message pump.
#include "persona1_display.hpp"
#include "persona1_mods.hpp"
#include "persona1_io.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace persona1 { int io_save_slot_step(int delta); void cheat_run(); }
namespace persona1 {
namespace {

constexpr int kW = 480, kH = 272;
std::mutex g_mutex;
std::vector<std::uint32_t> g_frame(kW * kH, 0xFF000000u);  // BGRA for GDI
std::vector<std::uint32_t> g_base;                         // BGRA copy of the base image of a full high-resolution layer
std::atomic<HWND> g_hwnd{nullptr};
std::once_flag g_once;
std::mutex g_ready_mutex;
std::condition_variable g_ready_cv;
bool g_ready = false;

// ---- D3D11 presenter: letterboxed, sharp-bilinear scaling, borderless fullscreen --------
// Also redraws the captured 3D scene at a higher internal resolution (F7 cycles 1x/2x/3x/4x/6x).
std::shared_ptr<const Layer3D> g_layer;          // guarded by g_mutex
std::atomic<bool> g_smooth2d{true};               // F6 mode 0: bilinear filtering for the high-resolution 2D layer
std::atomic<int> g_hd2d_mode{0};                  // F6: 0 HD 2D smooth, 1 HD 2D sharp, 2 HD 2D off
std::atomic<int> g_scale{3};                      // 1 = off

struct D3D {
    ID3D11Device *dev{};
    ID3D11DeviceContext *ctx{};
    IDXGISwapChain *sc{};
    ID3D11RenderTargetView *rtv{};
    ID3D11Texture2D *tex{};
    ID3D11ShaderResourceView *srv{};
    ID3D11VertexShader *vs{};
    ID3D11PixelShader *ps{}, *ps_comp{};
    ID3D11SamplerState *samp{}, *samp_point{};
    ID3D11Buffer *cb{};
    bool ok{};
    int bw{}, bh{};
    int mode{1};  // 0 nearest, 1 sharp bilinear, 2 bilinear, 3 bicubic sharp, 4 bicubic smooth
    // 3D layer
    bool ok3d{};
    ID3D11VertexShader *vs3{};
    ID3D11PixelShader *ps3{};
    ID3D11InputLayout *il3{};
    ID3D11Buffer *vb3{}, *cb3{};
    UINT vb3_cap{};
    ID3D11RasterizerState *rs3{};
    ID3D11Texture2D *rt_tex{}, *ds_tex{}, *mask_tex{};
    ID3D11RenderTargetView *rt_rtv{};
    ID3D11ShaderResourceView *rt_srv{}, *mask_srv{};
    ID3D11DepthStencilView *ds_dsv{};
    int rt_scale{};
    ID3D11SamplerState *samp3[8]{};
    std::map<std::uint32_t, ID3D11BlendState *> blends;
    std::map<std::uint32_t, ID3D11DepthStencilState *> depths;
    std::map<std::uint64_t, ID3D11ShaderResourceView *> texs;
} g_d3d;

const char *kShader = R"(
cbuffer C : register(b0) { float4 p; };  // x = scale, y = mode, z = hires composite
Texture2D T : register(t0);
Texture2D H : register(t1);
Texture2D M : register(t2);
SamplerState S : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs_main(uint id : SV_VertexID) {
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float kern(float x, float B, float C) {
    x = abs(x);
    if (x < 1.0) return ((12.0 - 9.0 * B - 6.0 * C) * x * x * x + (-18.0 + 12.0 * B + 6.0 * C) * x * x + (6.0 - 2.0 * B)) / 6.0;
    if (x < 2.0) return ((-B - 6.0 * C) * x * x * x + (6.0 * B + 30.0 * C) * x * x + (-12.0 * B - 48.0 * C) * x + (8.0 * B + 24.0 * C)) / 6.0;
    return 0.0;
}
float3 bicubic_px(float2 uv, float B, float C) {
    float2 sz = float2(480, 272);
    float2 q = uv * sz - 0.5;
    float2 b = floor(q), t = q - b;
    float3 acc = 0; float wsum = 0;
    [unroll] for (int j = -1; j <= 2; ++j) {
        [unroll] for (int k = -1; k <= 2; ++k) {
            float w = kern(k - t.x, B, C) * kern(j - t.y, B, C);
            int2 pi = clamp(int2(b) + int2(k, j), int2(0, 0), int2(479, 271));
            acc += T.Load(int3(pi, 0)).rgb * w;
            wsum += w;
        }
    }
    return saturate(acc / wsum);
}
float3 native_px(float2 uv) {
    float2 sz = float2(480, 272);
    float2 q = uv * sz;
    if (p.y < 0.5) return T.Load(int3(floor(q), 0)).rgb;
    if (p.y > 3.5) return bicubic_px(uv, 1.0 / 3.0, 1.0 / 3.0);  // Mitchell: smooth
    if (p.y > 2.5) return bicubic_px(uv, 0.0, 0.5);              // Catmull-Rom: sharp
    if (p.y > 1.5) return T.Sample(S, uv).rgb;
    float2 f = frac(q) - 0.5;
    float sc = max(p.x, 1.0);
    float2 r = 0.5 - 0.5 / sc;
    float2 g = (f - clamp(f, -r, r)) * sc + 0.5;
    return T.Sample(S, (floor(q) + g) / sz).rgb;
}
float4 ps_main(VO i) : SV_Target { return float4(native_px(i.uv), 1); }
float4 ps_comp(VO i) : SV_Target {
    float2 sz = float2(480, 272);
    float m = M.Load(int3(min(floor(i.uv * sz), sz - 1), 0)).r;
    if (m > 0.001) return float4(native_px(i.uv), 1);
    return float4(H.Sample(S, i.uv).rgb, 1);
}
)";

const char *kShader3D = R"(
cbuffer C1 : register(b1) { float4 a; float4 b; };  // a: textured,tfunc,texalpha,double  b: atest,func,ref,depthGreater
Texture2D Tex : register(t3);
SamplerState S3 : register(s3);
struct VI { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float4 ur : TEXCOORD1; };
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; float4 ur : TEXCOORD1; };
VO vs3d(VI i) {
    float w = 1.0 / max(i.pos.w, 1e-6);
    float zd = b.w > 0.5 ? 1.0 - i.pos.z / 65535.0 : i.pos.z / 65535.0;
    VO o;
    o.pos = float4((i.pos.x / 480.0 * 2.0 - 1.0) * w, (1.0 - i.pos.y / 272.0 * 2.0) * w, saturate(zd) * w, w);
    o.uv = i.uv;
    o.col = i.col;
    o.ur = i.ur;
    return o;
}
float4 ps3d(VO i) : SV_Target {
    float4 c = i.col;
    if (a.x > 0.5) {
        float2 uv = i.uv;
        if (i.ur.z > i.ur.x && i.ur.w > i.ur.y) {  // keep filtering inside this sprite's own texels
            float2 sz; Tex.GetDimensions(sz.x, sz.y);
            float2 h = 0.5 / sz;
            float2 lo = i.ur.xy + h, hi = i.ur.zw - h;
            lo = min(lo, (i.ur.xy + i.ur.zw) * 0.5); hi = max(hi, (i.ur.xy + i.ur.zw) * 0.5);
            uv = clamp(uv, lo, hi);
        }
        float4 t = Tex.Sample(S3, uv);
        int f = (int)(a.y + 0.5);
        float4 r;
        if (f == 1) { r.rgb = a.z > 0.5 ? lerp(c.rgb, t.rgb, t.a) : t.rgb; r.a = c.a; }
        else if (f == 3) { r.rgb = t.rgb; r.a = a.z > 0.5 ? t.a : c.a; }
        else if (f == 4) { r.rgb = saturate(t.rgb + c.rgb); r.a = a.z > 0.5 ? t.a * c.a : c.a; }
        else { r.rgb = t.rgb * c.rgb; r.a = a.z > 0.5 ? t.a * c.a : c.a; }
        c = r;
    }
    if (a.w > 0.5) c.rgb = saturate(c.rgb * 2.0);
    if (b.x > 0.5) {
        int fn = (int)(b.y + 0.5);
        float av = round(c.a * 255.0), rf = b.z;
        bool keep = true;
        if (fn == 0) keep = false;
        else if (fn == 2) keep = av == rf;
        else if (fn == 3) keep = av != rf;
        else if (fn == 4) keep = av < rf;
        else if (fn == 5) keep = av <= rf;
        else if (fn == 6) keep = av > rf;
        else if (fn == 7) keep = av >= rf;
        if (!keep) discard;
    }
    return c;
}
)";

void d3d_release_rt() {
    if (g_d3d.rtv) { g_d3d.rtv->Release(); g_d3d.rtv = nullptr; }
}
bool d3d_make_rt() {
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(g_d3d.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb)))) return false;
    const HRESULT hr = g_d3d.dev->CreateRenderTargetView(bb, nullptr, &g_d3d.rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

bool compile(const char *src, const char *entry, const char *prof, ID3DBlob **out) {
    ID3DBlob *err = nullptr;
    if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, prof, 0, 0, out, &err))) {
        if (err) { std::fprintf(stderr, "[display] shader error: %s\n", static_cast<const char *>(err->GetBufferPointer())); err->Release(); }
        return false;
    }
    return true;
}

bool d3d3_init() {
    ID3DBlob *vb = nullptr, *pb = nullptr;
    if (!compile(kShader3D, "vs3d", "vs_4_0", &vb) || !compile(kShader3D, "ps3d", "ps_4_0", &pb)) return false;
    if (FAILED(g_d3d.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_d3d.vs3))) return false;
    if (FAILED(g_d3d.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_d3d.ps3))) return false;
    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g_d3d.dev->CreateInputLayout(ied, 4, vb->GetBufferPointer(), vb->GetBufferSize(), &g_d3d.il3))) return false;
    vb->Release(); pb->Release();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_d3d.dev->CreateBuffer(&bd, nullptr, &g_d3d.cb3))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE; rd.ScissorEnable = TRUE;
    if (FAILED(g_d3d.dev->CreateRasterizerState(&rd, &g_d3d.rs3))) return false;
    for (int i = 0; i < 8; ++i) {
        D3D11_SAMPLER_DESC sm{};
        sm.Filter = (i & 4) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sm.AddressU = (i & 1) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        sm.AddressV = (i & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(g_d3d.dev->CreateSamplerState(&sm, &g_d3d.samp3[i]))) return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.mask_tex))) return false;
    if (FAILED(g_d3d.dev->CreateShaderResourceView(g_d3d.mask_tex, nullptr, &g_d3d.mask_srv))) return false;
    g_d3d.ok3d = true;
    return true;
}

bool d3d_init(HWND hwnd) {
    if (const char *m = std::getenv("PERSONA1_FILTER")) {
        const std::string v(m);
        g_d3d.mode = v == "nearest" ? 0 : (v == "linear" ? 2 : (v == "bicubic" ? 3 : (v == "smooth" ? 4 : 1)));
    }
    if (const char *m = std::getenv("PERSONA1_SCALE")) g_scale = std::clamp(std::atoi(m), 1, 8);
    if (std::getenv("PERSONA1_GDI")) return false;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &g_d3d.sc, &g_d3d.dev, &fl, &g_d3d.ctx);
    if (FAILED(hr)) return false;
    {
        IDXGIDevice *dd = nullptr; IDXGIAdapter *ad = nullptr; IDXGIFactory *fa = nullptr;
        if (SUCCEEDED(g_d3d.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dd)))) {
            if (SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&fa))))
                fa->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            if (fa) fa->Release();
            if (ad) ad->Release();
            dd->Release();
        }
    }
    if (!d3d_make_rt()) return false;
    ID3DBlob *vb = nullptr, *pb = nullptr, *pc = nullptr;
    if (!compile(kShader, "vs_main", "vs_4_0", &vb) || !compile(kShader, "ps_main", "ps_4_0", &pb) ||
        !compile(kShader, "ps_comp", "ps_4_0", &pc)) return false;
    if (FAILED(g_d3d.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_d3d.vs))) return false;
    if (FAILED(g_d3d.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_d3d.ps))) return false;
    if (FAILED(g_d3d.dev->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &g_d3d.ps_comp))) return false;
    vb->Release(); pb->Release(); pc->Release();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.tex))) return false;
    if (FAILED(g_d3d.dev->CreateShaderResourceView(g_d3d.tex, nullptr, &g_d3d.srv))) return false;
    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(g_d3d.dev->CreateSamplerState(&sm, &g_d3d.samp))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_d3d.dev->CreateBuffer(&bd, nullptr, &g_d3d.cb))) return false;
    g_d3d.ok = true;
    if (!d3d3_init()) std::fprintf(stderr, "[display] 3D layer unavailable (internal resolution scaling disabled)\n");
    return true;
}

template <class T> void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

bool ensure_hires(int scale) {
    if (g_d3d.rt_scale == scale && g_d3d.rt_tex) return true;
    rel(g_d3d.rt_rtv); rel(g_d3d.rt_srv); rel(g_d3d.rt_tex); rel(g_d3d.ds_dsv); rel(g_d3d.ds_tex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(kW * scale); td.Height = static_cast<UINT>(kH * scale); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.rt_tex))) return false;
    if (FAILED(g_d3d.dev->CreateRenderTargetView(g_d3d.rt_tex, nullptr, &g_d3d.rt_rtv))) return false;
    if (FAILED(g_d3d.dev->CreateShaderResourceView(g_d3d.rt_tex, nullptr, &g_d3d.rt_srv))) return false;
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.ds_tex))) return false;
    if (FAILED(g_d3d.dev->CreateDepthStencilView(g_d3d.ds_tex, nullptr, &g_d3d.ds_dsv))) return false;
    g_d3d.rt_scale = scale;
    return true;
}

D3D11_BLEND map_blend(int f, bool src) {
    switch (f) {
    case 0: return src ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR;
    case 1: return src ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
    case 2: case 6: return D3D11_BLEND_SRC_ALPHA;
    case 3: case 7: return D3D11_BLEND_INV_SRC_ALPHA;
    case 4: case 8: return D3D11_BLEND_DEST_ALPHA;
    case 5: case 9: return D3D11_BLEND_INV_DEST_ALPHA;
    case 10: return D3D11_BLEND_BLEND_FACTOR;
    default: return D3D11_BLEND_SRC_ALPHA;
    }
}
D3D11_COMPARISON_FUNC map_cmp(int f, bool greater_is_near) {
    // PSP depth: larger = nearer when 'greater_is_near'; our depth is inverted in that case.
    switch (f & 7) {
    case 0: return D3D11_COMPARISON_NEVER;
    case 1: return D3D11_COMPARISON_ALWAYS;
    case 2: return D3D11_COMPARISON_EQUAL;
    case 3: return D3D11_COMPARISON_NOT_EQUAL;
    case 4: return greater_is_near ? D3D11_COMPARISON_GREATER : D3D11_COMPARISON_LESS;
    case 5: return greater_is_near ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
    case 6: return greater_is_near ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_GREATER;
    default: return greater_is_near ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_GREATER_EQUAL;
    }
}

ID3D11ShaderResourceView *get_tex(const Layer3DTex &t) {
    auto it = g_d3d.texs.find(t.id);
    if (it != g_d3d.texs.end()) return it->second;
    if (g_d3d.texs.size() > 400) { for (auto &kv : g_d3d.texs) kv.second->Release(); g_d3d.texs.clear(); }
    const int tw = t.hi ? t.hi->w : t.w, th = t.hi ? t.hi->h : t.h;
    const std::uint32_t *tpx = t.hi ? t.hi->px.data() : t.px.data();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(tw); td.Height = static_cast<UINT>(th); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{tpx, static_cast<UINT>(tw * 4), 0};
    ID3D11Texture2D *tx = nullptr;
    ID3D11ShaderResourceView *sv = nullptr;
    if (SUCCEEDED(g_d3d.dev->CreateTexture2D(&td, &sd, &tx))) {
        g_d3d.dev->CreateShaderResourceView(tx, nullptr, &sv);
        tx->Release();
    }
    g_d3d.texs[t.id] = sv;
    return sv;
}

void draw_layer(const Layer3D &L, int scale) {
    if (!g_d3d.ok3d || L.draws.empty() || L.verts.empty()) return;
    const UINT need = static_cast<UINT>(L.verts.size() * sizeof(Layer3DVertex));
    if (need > g_d3d.vb3_cap) {
        rel(g_d3d.vb3);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = need + need / 2 + 4096; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_d3d.dev->CreateBuffer(&bd, nullptr, &g_d3d.vb3))) return;
        g_d3d.vb3_cap = bd.ByteWidth;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_d3d.ctx->Map(g_d3d.vb3, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    std::memcpy(ms.pData, L.verts.data(), need);
    g_d3d.ctx->Unmap(g_d3d.vb3, 0);
    g_d3d.ctx->ClearDepthStencilView(g_d3d.ds_dsv, D3D11_CLEAR_DEPTH, L.depth_greater ? 1.f : 0.f, 0);
    g_d3d.ctx->OMSetRenderTargets(1, &g_d3d.rt_rtv, g_d3d.ds_dsv);
    D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(kW * scale), static_cast<float>(kH * scale), 0.f, 1.f};
    g_d3d.ctx->RSSetViewports(1, &vp);
    g_d3d.ctx->RSSetState(g_d3d.rs3);
    const UINT stride = sizeof(Layer3DVertex), off = 0;
    g_d3d.ctx->IASetVertexBuffers(0, 1, &g_d3d.vb3, &stride, &off);
    g_d3d.ctx->IASetInputLayout(g_d3d.il3);
    g_d3d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_d3d.ctx->VSSetShader(g_d3d.vs3, nullptr, 0);
    g_d3d.ctx->PSSetShader(g_d3d.ps3, nullptr, 0);
    g_d3d.ctx->VSSetConstantBuffers(1, 1, &g_d3d.cb3);
    g_d3d.ctx->PSSetConstantBuffers(1, 1, &g_d3d.cb3);
    for (const Layer3DDraw &d : L.draws) {
        if (d.clear_depth) g_d3d.ctx->ClearDepthStencilView(g_d3d.ds_dsv, D3D11_CLEAR_DEPTH, L.depth_greater ? 1.f : 0.f, 0);
        if (d.count == 0) continue;
        {
            const int x1 = std::clamp(d.sx1, 0, kW - 1), y1 = std::clamp(d.sy1, 0, kH - 1);
            const int x2 = std::clamp(d.sx2, x1, kW - 1), y2 = std::clamp(d.sy2, y1, kH - 1);
            const D3D11_RECT sr{static_cast<LONG>(x1 * scale), static_cast<LONG>(y1 * scale), static_cast<LONG>((x2 + 1) * scale), static_cast<LONG>((y2 + 1) * scale)};
            g_d3d.ctx->RSSetScissorRects(1, &sr);
        }
        // depth state
        const std::uint32_t dk = (d.ztest ? 1u : 0u) | (d.zwrite ? 2u : 0u) | (static_cast<std::uint32_t>(d.zfunc & 7) << 2);
        ID3D11DepthStencilState *&ds = g_d3d.depths[dk];
        if (!ds) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = d.ztest ? TRUE : FALSE;
            dd.DepthWriteMask = (d.ztest && d.zwrite) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = d.ztest ? map_cmp(d.zfunc, L.depth_greater) : D3D11_COMPARISON_ALWAYS;
            g_d3d.dev->CreateDepthStencilState(&dd, &ds);
        }
        g_d3d.ctx->OMSetDepthStencilState(ds, 0);
        // blend state
        const std::uint32_t bk = d.blend ? (1u | (static_cast<std::uint32_t>(d.bsrc & 15) << 1) | (static_cast<std::uint32_t>(d.bdst & 15) << 5) |
                                            (static_cast<std::uint32_t>(d.beq & 7) << 9)) : 0u;
        ID3D11BlendState *&bs = g_d3d.blends[bk];
        if (!bs) {
            D3D11_BLEND_DESC bd{};
            auto &rt = bd.RenderTarget[0];
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (d.blend) {
                rt.BlendEnable = TRUE;
                rt.SrcBlend = map_blend(d.bsrc, true);
                rt.DestBlend = map_blend(d.bdst, false);
                static const D3D11_BLEND_OP ops[8] = {D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT,
                                                      D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX, D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_ADD};
                rt.BlendOp = ops[d.beq & 7];
                rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            }
            g_d3d.dev->CreateBlendState(&bd, &bs);
        }
        const std::uint32_t fc = d.bsrc == 10 ? d.fix_a : d.fix_b;
        const float bf[4] = {(fc & 0xFF) / 255.f, ((fc >> 8) & 0xFF) / 255.f, ((fc >> 16) & 0xFF) / 255.f, 1.f};
        g_d3d.ctx->OMSetBlendState(bs, bf, 0xFFFFFFFFu);
        // constants + texture
        ID3D11ShaderResourceView *sv = (d.textured && d.tex) ? get_tex(*d.tex) : nullptr;
        if (D3D11_MAPPED_SUBRESOURCE cm; SUCCEEDED(g_d3d.ctx->Map(g_d3d.cb3, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
            float *p = static_cast<float *>(cm.pData);
            p[0] = sv ? 1.f : 0.f; p[1] = static_cast<float>(d.tfunc); p[2] = d.tex_alpha ? 1.f : 0.f; p[3] = d.dbl ? 1.f : 0.f;
            p[4] = d.atest ? 1.f : 0.f; p[5] = static_cast<float>(d.atest_func); p[6] = static_cast<float>(d.atest_ref);
            p[7] = L.depth_greater ? 1.f : 0.f;
            g_d3d.ctx->Unmap(g_d3d.cb3, 0);
        }
        g_d3d.ctx->PSSetShaderResources(3, 1, &sv);
        const int si = (d.clamp_u ? 1 : 0) | (d.clamp_v ? 2 : 0) | ((d.linear || (d.tex && d.tex->hi) || (d.is2d && g_smooth2d.load())) ? 4 : 0);
        g_d3d.ctx->PSSetSamplers(3, 1, &g_d3d.samp3[si]);
        g_d3d.ctx->Draw(d.count, d.first);
    }
    ID3D11ShaderResourceView *none = nullptr;
    g_d3d.ctx->PSSetShaderResources(3, 1, &none);
    g_d3d.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g_d3d.ctx->OMSetDepthStencilState(nullptr, 0);
}

void set_cb0(float scale, float hires) {
    if (D3D11_MAPPED_SUBRESOURCE cm; SUCCEEDED(g_d3d.ctx->Map(g_d3d.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        float *p = static_cast<float *>(cm.pData);
        p[0] = scale; p[1] = static_cast<float>(g_d3d.mode); p[2] = hires; p[3] = 0.f;
        g_d3d.ctx->Unmap(g_d3d.cb, 0);
    }
}

// ---- On-screen toast (top centre of the picture) so settings can be seen without leaving full screen. ----
std::mutex g_toast_mu;
std::wstring g_toast_text;
bool g_toast_dirty = false;
std::atomic<ULONGLONG> g_toast_until{0};
struct Toast {
    ID3D11Texture2D *tex{};
    ID3D11ShaderResourceView *srv{};
    ID3D11PixelShader *ps{};
    ID3D11BlendState *bs{};
    bool tried{}, ok{};
} g_toast;
constexpr int kToastW = 640, kToastH = 80;

void toast_show(const wchar_t *text, unsigned hold_ms = 1800) {
    {
        std::lock_guard<std::mutex> lk(g_toast_mu);
        if (g_toast_text != text) { g_toast_text = text; g_toast_dirty = true; }
    }
    g_toast_until = GetTickCount64() + hold_ms;
    if (HWND h = g_hwnd.load()) InvalidateRect(h, nullptr, FALSE);
}

bool toast_init() {
    g_toast.tried = true;
    static const char *src = R"(
Texture2D X : register(t4);
SamplerState S : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps_toast(VO i) : SV_Target { return X.Sample(S, i.uv); }
)";
    ID3DBlob *pb = nullptr;
    if (!compile(src, "ps_toast", "ps_4_0", &pb)) return false;
    const HRESULT h1 = g_d3d.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_toast.ps);
    pb->Release();
    if (FAILED(h1)) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kToastW; td.Height = kToastH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_toast.tex))) return false;
    if (FAILED(g_d3d.dev->CreateShaderResourceView(g_toast.tex, nullptr, &g_toast.srv))) return false;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_d3d.dev->CreateBlendState(&bd, &g_toast.bs))) return false;
    return true;
}

// Renders the text with GDI into a 640x80 BGRA image and uploads it.
void toast_upload(const std::wstring &text) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kToastW; bi.bmiHeader.biHeight = -kToastH; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return; }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<std::size_t>(kToastW) * kToastH * 4);
    HBRUSH br = CreateSolidBrush(RGB(20, 20, 26));
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(220, 220, 235));
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, 2, 2, kToastW - 2, kToastH - 2, 26, 26);
    SelectObject(dc, ob); SelectObject(dc, op);
    HFONT font = CreateFontW(-38, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r{0, 0, kToastW, kToastH};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();
    auto *px = static_cast<std::uint32_t *>(bits);
    for (int i = 0; i < kToastW * kToastH; ++i) px[i] = (px[i] & 0x00FFFFFFu) ? (px[i] | 0xEC000000u) : 0u;
    if (D3D11_MAPPED_SUBRESOURCE ms; SUCCEEDED(g_d3d.ctx->Map(g_toast.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        for (int y = 0; y < kToastH; ++y)
            std::memcpy(static_cast<std::uint8_t *>(ms.pData) + static_cast<std::size_t>(y) * ms.RowPitch, px + static_cast<std::size_t>(y) * kToastW, kToastW * 4);
        g_d3d.ctx->Unmap(g_toast.tex, 0);
    }
    SelectObject(dc, of); DeleteObject(font);
    DeleteObject(br); DeleteObject(pen);
    SelectObject(dc, old_bmp); DeleteObject(bmp); DeleteDC(dc);
}

// Called after the picture is drawn, before Present. vx/vy/vw/vh = the letterboxed picture rectangle.
void toast_draw(float vx, float vy, float vw, float vh) {
    if (GetTickCount64() >= g_toast_until.load()) return;
    if (!g_toast.tried && !toast_init()) {}
    if (!g_toast.tex || !g_toast.ps || !g_toast.bs) return;
    std::wstring text;
    bool dirty;
    {
        std::lock_guard<std::mutex> lk(g_toast_mu);
        text = g_toast_text; dirty = g_toast_dirty; g_toast_dirty = false;
    }
    if (dirty) toast_upload(text);
    const float w = std::min(vw * 0.62f, 960.f), h = w * kToastH / kToastW;
    D3D11_VIEWPORT tvp{vx + (vw - w) * 0.5f, vy + vh * 0.04f, w, h, 0.f, 1.f};
    g_d3d.ctx->RSSetViewports(1, &tvp);
    g_d3d.ctx->PSSetShader(g_toast.ps, nullptr, 0);
    g_d3d.ctx->PSSetShaderResources(4, 1, &g_toast.srv);
    const float bf[4] = {0, 0, 0, 0};
    g_d3d.ctx->OMSetBlendState(g_toast.bs, bf, 0xFFFFFFFFu);
    g_d3d.ctx->Draw(3, 0);
    g_d3d.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ID3D11ShaderResourceView *none = nullptr;
    g_d3d.ctx->PSSetShaderResources(4, 1, &none);
}

void d3d_render(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = std::max<int>(rc.right - rc.left, 1), ch = std::max<int>(rc.bottom - rc.top, 1);
    if (cw != g_d3d.bw || ch != g_d3d.bh) {
        d3d_release_rt();
        g_d3d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g_d3d.sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        d3d_make_rt();
        g_d3d.bw = cw; g_d3d.bh = ch;
    }
    std::shared_ptr<const Layer3D> layer;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        D3D11_MAPPED_SUBRESOURCE ms;
        const std::uint32_t *src = g_frame.data();
        if (g_layer && g_layer->full && g_scale.load() > 1 && g_d3d.ok3d && g_base.size() >= static_cast<std::size_t>(kW) * kH) src = g_base.data();
        if (SUCCEEDED(g_d3d.ctx->Map(g_d3d.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            for (int y = 0; y < kH; ++y)
                std::memcpy(static_cast<std::uint8_t *>(ms.pData) + static_cast<std::size_t>(y) * ms.RowPitch, &src[static_cast<std::size_t>(y) * kW], kW * 4);
            g_d3d.ctx->Unmap(g_d3d.tex, 0);
        }
        layer = g_layer;
    }
    const int scale = g_scale.load();
    {
        static unsigned n = 0, logged = 0;
        if (++n % 120 == 0 && logged < 25) {
            ++logged;
            std::fprintf(stderr, "[display] scale=%d ok3d=%d layer=%d draws=%zu mask=%zu\n", scale, g_d3d.ok3d ? 1 : 0, layer ? 1 : 0,
                         layer ? layer->draws.size() : 0u, layer ? layer->mask.size() : 0u);
        }
    }
    const bool hires = scale > 1 && g_d3d.ok3d && layer && !layer->draws.empty() && layer->mask.size() >= 512u * kH && ensure_hires(scale);
    // Letterbox to 480:272.
    float vw = static_cast<float>(cw), vh = vw * kH / kW;
    if (vh > ch) { vh = static_cast<float>(ch); vw = vh * kW / kH; }
    g_d3d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_d3d.ctx->VSSetShader(g_d3d.vs, nullptr, 0);
    g_d3d.ctx->PSSetConstantBuffers(0, 1, &g_d3d.cb);
    g_d3d.ctx->PSSetSamplers(0, 1, &g_d3d.samp);
    g_d3d.ctx->PSSetShaderResources(0, 1, &g_d3d.srv);
    if (hires) {
        // Pass 1: native frame, sharp-scaled, into the high-resolution target.
        g_d3d.ctx->OMSetRenderTargets(1, &g_d3d.rt_rtv, nullptr);
        D3D11_VIEWPORT rvp{0.f, 0.f, static_cast<float>(kW * scale), static_cast<float>(kH * scale), 0.f, 1.f};
        g_d3d.ctx->RSSetViewports(1, &rvp);
        g_d3d.ctx->PSSetShader(g_d3d.ps, nullptr, 0);
        set_cb0(static_cast<float>(scale), 0.f);
        g_d3d.ctx->Draw(3, 0);
        // Pass 2: the captured 3D scene at N x resolution.
        draw_layer(*layer, scale);
        // Upload the "2D drew over the 3D" mask.
        if (D3D11_MAPPED_SUBRESOURCE ms; SUCCEEDED(g_d3d.ctx->Map(g_d3d.mask_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            for (int y = 0; y < kH; ++y)
                std::memcpy(static_cast<std::uint8_t *>(ms.pData) + static_cast<std::size_t>(y) * ms.RowPitch, &layer->mask[static_cast<std::size_t>(y) * 512u], kW);
            g_d3d.ctx->Unmap(g_d3d.mask_tex, 0);
        }
        g_d3d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_d3d.ctx->VSSetShader(g_d3d.vs, nullptr, 0);
        g_d3d.ctx->RSSetState(nullptr);
        g_d3d.ctx->IASetInputLayout(nullptr);
        g_d3d.ctx->PSSetConstantBuffers(0, 1, &g_d3d.cb);
        g_d3d.ctx->PSSetSamplers(0, 1, &g_d3d.samp);
    }
    const float black[4] = {0, 0, 0, 1};
    g_d3d.ctx->OMSetRenderTargets(1, &g_d3d.rtv, nullptr);
    g_d3d.ctx->ClearRenderTargetView(g_d3d.rtv, black);
    D3D11_VIEWPORT vp{(cw - vw) * 0.5f, (ch - vh) * 0.5f, vw, vh, 0.f, 1.f};
    g_d3d.ctx->RSSetViewports(1, &vp);
    set_cb0(vh / kH, hires ? 1.f : 0.f);
    ID3D11ShaderResourceView *srvs[3] = {g_d3d.srv, hires ? g_d3d.rt_srv : nullptr, hires ? g_d3d.mask_srv : nullptr};
    g_d3d.ctx->PSSetShaderResources(0, 3, srvs);
    g_d3d.ctx->PSSetShader(hires ? g_d3d.ps_comp : g_d3d.ps, nullptr, 0);
    g_d3d.ctx->Draw(3, 0);
    ID3D11ShaderResourceView *clear3[3] = {nullptr, nullptr, nullptr};
    g_d3d.ctx->PSSetShaderResources(0, 3, clear3);
    toast_draw(vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    g_d3d.sc->Present(0, 0);
}

bool g_fullscreen = false;
WINDOWPLACEMENT g_prev{sizeof(WINDOWPLACEMENT)};
void toggle_fullscreen(HWND hwnd) {
    const DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        GetWindowPlacement(hwnd, &g_prev);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(hwnd, &g_prev);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

void cycle_scale() {
    static const int steps[] = {1, 2, 3, 4, 5, 6, 8};
    constexpr int kN = 7;
    int cur = g_scale.load(), nx = steps[0];
    for (int i = 0; i < kN; ++i) if (steps[i] == cur) nx = steps[(i + 1) % kN];
    g_scale = nx;
    const wchar_t *label = nx == 1 ? L"native 480x272" : nx == 2 ? L"960x544 (~540p)" : nx == 3 ? L"1440x816" : nx == 4 ? L"1920x1088 (~1080p)"
                          : nx == 5 ? L"2400x1360 (~1440p)" : nx == 6 ? L"2880x1632" : L"3840x2176 (~4K)";
    std::fprintf(stderr, "[display] 3D internal resolution x%d\n", nx);
    wchar_t title[128];
    swprintf(title, 128, L"Persona: Project Philemon - 3D internal resolution x%d: %ls", nx, label);
    if (HWND h = g_hwnd.load()) SetWindowTextW(h, title);
    wchar_t t[96];
    swprintf(t, 96, L"3D resolution: %ls", label);
    toast_show(t);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_d3d.ok) { d3d_render(hwnd); EndPaint(hwnd, &ps); return 0; }
        RECT rc;
        GetClientRect(hwnd, &rc);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = kW;
        bi.bmiHeader.biHeight = -kH;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, kW, kH, g_frame.data(), &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { toggle_fullscreen(hwnd); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F9 || (wp == VK_ESCAPE && g_fullscreen)) { toggle_fullscreen(hwnd); return 0; }
        if (wp == VK_F5) { persona1::cheat_run(); return 0; }
        if (wp == VK_F6) {
            const int m = (g_hd2d_mode.load() + 1) % 3;
            g_hd2d_mode = m;
            g_smooth2d = (m == 0);
            static const wchar_t *names[] = {L"HD 2D smooth", L"HD 2D sharp (nearest)", L"HD 2D off (3D only)"};
            wchar_t title[128];
            swprintf(title, 128, L"Persona: Project Philemon - %ls (F6)", names[m]);
            SetWindowTextW(hwnd, title);
            std::fprintf(stderr, "[display] F6: %ls\n", names[m]);
            return 0;
        }
        if (wp == VK_F3 || wp == VK_F4) {
            const int slot = persona1::io_save_slot_step(wp == VK_F4 ? 1 : -1);
            wchar_t title[96];
            swprintf(title, 96, L"Persona: Project Philemon - save slot %d (F3/F4)", slot + 1);
            SetWindowTextW(hwnd, title);
            return 0;
        }
        if (wp == VK_F8) {
            g_d3d.mode = (g_d3d.mode + 1) % 5;
            static const wchar_t *names[] = {L"nearest", L"sharp bilinear", L"bilinear", L"bicubic (sharp)", L"bicubic (smooth)"};
            wchar_t title[128];
            swprintf(title, 128, L"Persona: Project Philemon - upscale filter: %ls (F8)", names[g_d3d.mode]);
            SetWindowTextW(hwnd, title);
            return 0;
        }
        if (wp == VK_F7) { cycle_scale(); return 0; }
        break;
    case WM_SIZE: if (g_d3d.ok) InvalidateRect(hwnd, nullptr, FALSE); break;
    case WM_CLOSE:
    case WM_DESTROY: std::_Exit(0);
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void window_thread() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"Persona1NativeWindow";
    RegisterClassW(&wc);
    RECT rc{0, 0, kW * 2, kH * 2};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Persona: Project Philemon", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                              nullptr, wc.hInstance, nullptr);
    g_hwnd = hwnd;
    if (!d3d_init(hwnd)) std::fprintf(stderr, "[display] D3D11 unavailable, using GDI\n");
    else std::fprintf(stderr, "[display] D3D11 presenter active (Alt+Enter/F9 fullscreen, F8 filter)\n");
    {
        std::lock_guard<std::mutex> lock(g_ready_mutex);
        g_ready = true;
    }
    g_ready_cv.notify_all();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}


// ---- F12 screenshot (uncompressed PNG, no external library) ----------------------------
std::uint32_t crc32_of(const std::uint8_t *p, std::size_t n, std::uint32_t crc = 0xFFFFFFFFu) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}
void put_be32(std::vector<std::uint8_t> &v, std::uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void png_chunk(std::vector<std::uint8_t> &out, const char *type, const std::vector<std::uint8_t> &data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, ~crc32_of(body.data(), body.size()));
}
void write_png(const std::string &name, const std::uint32_t *rgba, int W, int H) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < H; ++y) {
        raw.push_back(0);
        for (int x = 0; x < W; ++x) {
            const std::uint32_t p = rgba[y * W + x];
            raw.push_back(p & 0xFF); raw.push_back((p >> 8) & 0xFF); raw.push_back((p >> 16) & 0xFF);
        }
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    for (std::size_t pos = 0; pos < raw.size();) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    put_be32(z, (b << 16) | a);
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, W); put_be32(ihdr, H);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", z);
    png_chunk(out, "IEND", {});
    if (FILE *f = std::fopen(name.c_str(), "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "[display] saved %s\n", name.c_str());
    }
}
void save_png(const std::uint32_t *rgba) {
    static int shot = 0;
    write_png("persona1_screenshot_" + std::to_string(++shot) + ".png", rgba, kW, kH);
}

} // namespace

void save_image_png(const char *name, const std::uint32_t *rgba, int w, int h) { write_png(name, rgba, w, h); }

void display_init() {
    std::call_once(g_once, [] {
        timeBeginPeriod(1);  // 1 ms timer resolution so frame-pacing sleeps are accurate
        std::thread(window_thread).detach();
        std::unique_lock<std::mutex> lock(g_ready_mutex);
        g_ready_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_ready; });
    });
}

std::atomic<unsigned long long> g_frames_presented{0};
unsigned long long display_frames() { return g_frames_presented.load(); }

void display_present(const std::uint32_t *rgba, std::shared_ptr<const Layer3D> layer) {
    ++g_frames_presented;
    static bool f12_was_down = false;
    const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12 && !f12_was_down) save_png(rgba);
    f12_was_down = f12;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int i = 0; i < kW * kH; ++i) {
            const std::uint32_t p = rgba[i];
            // 0xAABBGGRR -> 0x00RRGGBB (GDI BGRA, alpha ignored)
            g_frame[i] = ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
        g_layer = std::move(layer);
        if (g_layer && g_layer->full && g_layer->base.size() >= static_cast<std::size_t>(kW) * kH) {
            g_base.resize(static_cast<std::size_t>(kW) * kH);
            for (int i = 0; i < kW * kH; ++i) {
                const std::uint32_t p = g_layer->base[i];
                g_base[i] = ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
        }
    }
    if (HWND h = g_hwnd.load()) InvalidateRect(h, nullptr, FALSE);
}

bool display_hd2d_on() { return g_hd2d_mode.load() != 2; }

bool debug_key_pressed() {
    static bool was_down = false;
    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

bool debug_key_f10() {
    static bool was_down = false;
    const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

std::uint32_t input_poll(std::uint8_t *lx, std::uint8_t *ly) {
    // PSP button bits
    constexpr std::uint32_t SELECT = 0x1, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
                            LTRIG = 0x100, RTRIG = 0x200, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000,
                            SQUARE = 0x8000;
    std::uint32_t b = 0;
    int ax = 128, ay = 128;

    // Keyboard, only while our window has focus.
    if (GetForegroundWindow() == g_hwnd.load()) {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down(VK_UP)) b |= UP;
        if (down(VK_DOWN)) b |= DOWN;
        if (down(VK_LEFT)) b |= LEFT;
        if (down(VK_RIGHT)) b |= RIGHT;
        if (down('Z')) b |= CROSS;
        if (down('X')) b |= CIRCLE;
        if (down('A')) b |= SQUARE;
        if (down('S')) b |= TRIANGLE;
        if (down('Q')) b |= LTRIG;
        if (down('W')) b |= RTRIG;
        if (down(VK_SPACE)) b |= SELECT;
        if (down(VK_RETURN)) b |= START;
    }

    // Xbox controller through XInput, loaded at run time so nothing extra has to be linked.
    struct XGamepad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    struct XState { DWORD packet; XGamepad pad; };
    using GetStateFn = DWORD(WINAPI *)(DWORD, XState *);
    static GetStateFn get_state = [] {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    static ULONGLONG next_try = 0;
    if (get_state && GetTickCount64() >= next_try) {
        XState st{};
        if (get_state(0, &st) == 0) {
            const WORD x = st.pad.buttons;
            if (x & 0x0001) b |= UP;
            if (x & 0x0002) b |= DOWN;
            if (x & 0x0004) b |= LEFT;
            if (x & 0x0008) b |= RIGHT;
            if (x & 0x0010) b |= START;
            if (x & 0x0020) b |= SELECT;
            if (x & 0x0100) b |= LTRIG;
            if (x & 0x0200) b |= RTRIG;
            if (x & 0x1000) b |= CROSS;     // A
            if (x & 0x2000) b |= CIRCLE;    // B
            if (x & 0x4000) b |= SQUARE;    // X
            if (x & 0x8000) b |= TRIANGLE;  // Y
            static bool r3_was = false;
            const bool r3 = (x & 0x0080) != 0;  // right stick click
            auto reward_toast = [](int mu) {
                wchar_t t[96];
                if (mu == 1) swprintf(t, 96, L"EXP / Yen rewards: OFF");
                else swprintf(t, 96, L"EXP / Yen rewards: %dx", mu);
                toast_show(t);
            };
            if (r3 && !r3_was) reward_toast(rewards_cycle());
            else if (r3) g_toast_until = GetTickCount64() + 1800;  // keep the popup up while the stick is held
            r3_was = r3;
            static bool lt_was = false;
            const bool lt = st.pad.lt > 160;  // left trigger: 3D resolution (same as F7)
            if (lt && !lt_was) cycle_scale();
            else if (lt) g_toast_until = GetTickCount64() + 1800;
            lt_was = lt;
            static bool rt_was = false;
            const bool rtg = st.pad.rt > 160;  // right trigger: fullscreen (same as F9)
            if (rtg && !rt_was) { if (HWND hw = g_hwnd.load()) PostMessageW(hw, WM_KEYDOWN, VK_F9, 0); }
            rt_was = rtg;
            const int sx = st.pad.lx, sy = st.pad.ly;
            if (std::abs(sx) > 8000 || std::abs(sy) > 8000) {
                ax = std::clamp(128 + sx / 256, 0, 255);
                ay = std::clamp(128 - sy / 256, 0, 255);
                if (sx < -16000) b |= LEFT;
                if (sx > 16000) b |= RIGHT;
                if (sy > 16000) b |= UP;
                if (sy < -16000) b |= DOWN;
            }
        } else {
            next_try = GetTickCount64() + 1000;  // no controller: poll once a second
        }
    }
    if (lx) *lx = static_cast<std::uint8_t>(ax);
    if (ly) *ly = static_cast<std::uint8_t>(ay);
    return b;
}

} // namespace persona1

#else
namespace persona1 {
void display_init() {}
void display_present(const std::uint32_t *, std::shared_ptr<const Layer3D>) {}
void save_image_png(const char *, const std::uint32_t *, int, int) {}
unsigned long long display_frames() { return 0; }
bool debug_key_pressed() { return false; }
bool debug_key_f10() { return false; }
std::uint32_t input_poll(std::uint8_t *lx, std::uint8_t *ly) { if (lx) *lx = 128; if (ly) *ly = 128; return 0; }
}
#endif
