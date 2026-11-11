#include "persona1_ge.hpp"

#include "persona1_display.hpp"
#include "persona1_mods.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace persona1 {

extern double g_idle_seconds;  // defined in persona1_kernel.cpp
extern unsigned long long g_prof_vblank, g_prof_setfb;

namespace {

using psprecomp::Runtime;

struct Vtx {
    float x{}, y{}, u{}, v{};
    std::uint32_t color{};  // 0xAABBGGRR
};

struct ListState {
    std::uint32_t pc{}, stall{};
    std::uint32_t stack[16]{};
    unsigned sp{};
    bool ended{};
};

// ---- hi-res 3D layer capture (consumed by the D3D11 presenter) ----------------------
std::vector<std::uint8_t> g_mask(512u * 272u, 0);  // 1 = pixel last written by 2D drawing
bool g_in3d = false;
Layer3D g_lay;
std::unordered_map<std::uint64_t, std::shared_ptr<const Layer3DTex>> g_tex_cache;
std::uint64_t g_next_tex_id = 1;
inline void mark_px(int x, int y) {
    if (static_cast<unsigned>(x) < 512u && static_cast<unsigned>(y) < 272u) g_mask[static_cast<std::size_t>(y) * 512u + x] = g_in3d ? 0 : 1;
}

struct Gpu {
    Runtime *rt{};
    std::uint32_t reg[256]{};
    std::uint32_t base{}, offset{}, vaddr{}, iaddr{};
    std::uint32_t clut[1024]{};
    std::uint8_t clut_raw[8192]{};  // the palette buffer exactly as the game loaded it, stale bytes included (for texture-pack hashes)
    unsigned clut_raw_len{}, clut_max{};  // bytes in the last load, and the most ever loaded
    std::map<int, ListState> lists;
    unsigned skipped_3d{}, draws{};
    float mat_world[12]{}, mat_view[12]{}, mat_proj[16]{};
    unsigned mat_idx[3]{};
    bool trace{};
    // Resolved once per primitive (null = fall back to checked guest memory accesses).
    const std::uint8_t *tex_ptr{};
    std::size_t tex_len{};
    std::uint32_t tex_addr{};
    std::uint8_t *fb_ptr{};
    std::size_t fb_len{};
} G;

// ---- guest memory --------------------------------------------------------------------
inline std::uint32_t rd8(std::uint32_t a) { return G.rt->memory().aot_fast_view().aot_load8(a); }
inline std::uint32_t rd16(std::uint32_t a) { return G.rt->memory().aot_fast_view().aot_load16(a); }
inline std::uint32_t rd32(std::uint32_t a) { return G.rt->memory().aot_fast_view().aot_load32(a); }
inline void wr16(std::uint32_t a, std::uint32_t v) { G.rt->memory().aot_fast_view().aot_store16(a, static_cast<std::uint16_t>(v)); }
inline void wr32(std::uint32_t a, std::uint32_t v) { G.rt->memory().aot_fast_view().aot_store32(a, v); }

// ---- pixel formats -------------------------------------------------------------------
// 0 = 565, 1 = 5551, 2 = 4444, 3 = 8888. Packed value <-> 0xAABBGGRR.
inline std::uint32_t expand5(std::uint32_t v) { return (v << 3) | (v >> 2); }
inline std::uint32_t expand6(std::uint32_t v) { return (v << 2) | (v >> 4); }
inline std::uint32_t expand4(std::uint32_t v) { return (v << 4) | v; }

std::uint32_t unpack(std::uint32_t p, unsigned fmt) {
    switch (fmt) {
    case 0: return 0xFF000000u | (expand5((p >> 11) & 31) << 16) | (expand6((p >> 5) & 63) << 8) | expand5(p & 31);
    case 1: return ((p & 0x8000u) ? 0xFF000000u : 0u) | (expand5((p >> 10) & 31) << 16) | (expand5((p >> 5) & 31) << 8) | expand5(p & 31);
    case 2: return (expand4((p >> 12) & 15) << 24) | (expand4((p >> 8) & 15) << 16) | (expand4((p >> 4) & 15) << 8) | expand4(p & 15);
    default: return p;
    }
}
std::uint32_t pack(std::uint32_t c, unsigned fmt) {
    const std::uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    switch (fmt) {
    case 0: return ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
    case 1: return ((a >> 7) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3);
    case 2: return ((a >> 4) << 12) | ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4);
    default: return c;
    }
}
// Colour formats in vertex types: 4 = 565, 5 = 5551, 6 = 4444, 7 = 8888.

// ---- framebuffer ---------------------------------------------------------------------
struct Target {
    std::uint32_t addr{}, stride{};
    unsigned fmt{};
};
Target target() {
    Target t;
    t.addr = (G.reg[0x9C] & 0xFFFFFFu) | ((G.reg[0x9D] & 0xFF0000u) << 8);
    if ((t.addr >> 24) == 0u) t.addr |= 0x04000000u;
    t.stride = G.reg[0x9D] & 0xFFFFu;
    t.fmt = G.reg[0xD2] & 3u;
    return t;
}
inline std::uint32_t get_px(const Target &t, int x, int y) {
    const std::uint32_t bpp = t.fmt == 3 ? 4u : 2u;
    const std::size_t off = (static_cast<std::size_t>(y) * t.stride + x) * bpp;
    if (G.fb_ptr) {
        if (off + bpp > G.fb_len) return 0;
        if (bpp == 4) { std::uint32_t v; std::memcpy(&v, G.fb_ptr + off, 4); return v; }
        std::uint16_t v; std::memcpy(&v, G.fb_ptr + off, 2); return unpack(v, t.fmt);
    }
    if (t.fmt == 3) return rd32(t.addr + static_cast<std::uint32_t>(off));
    return unpack(rd16(t.addr + static_cast<std::uint32_t>(off)), t.fmt);
}
inline void put_px(const Target &t, int x, int y, std::uint32_t c) {
    mark_px(x, y);
    const std::uint32_t bpp = t.fmt == 3 ? 4u : 2u;
    const std::size_t off = (static_cast<std::size_t>(y) * t.stride + x) * bpp;
    if (G.fb_ptr) {
        if (off + bpp > G.fb_len) return;
        if (bpp == 4) std::memcpy(G.fb_ptr + off, &c, 4);
        else { const std::uint16_t v = static_cast<std::uint16_t>(pack(c, t.fmt)); std::memcpy(G.fb_ptr + off, &v, 2); }
        return;
    }
    if (t.fmt == 3) wr32(t.addr + static_cast<std::uint32_t>(off), c);
    else wr16(t.addr + static_cast<std::uint32_t>(off), pack(c, t.fmt));
}

// ---- textures ------------------------------------------------------------------------
void load_clut(std::uint32_t arg) {
    const unsigned blocks = arg & 0x3F;
    const unsigned fmt = G.reg[0xC5] & 3u;
    const std::uint32_t addr = (G.reg[0xB0] & 0xFFFFFFu) | ((G.reg[0xB1] & 0x0F0000u) << 8);
    // CLOAD counts 32-byte blocks: 16 entries for the 16-bit palette formats, 8 for 32-bit.
    const unsigned entries = std::min(blocks * (fmt == 3 ? 8u : 16u), 1024u);
    if (blocks) {
        const unsigned raw_len = std::min(blocks * 32u, 2048u);
        G.clut_raw_len = raw_len;
        G.clut_max = std::max(G.clut_max, raw_len);
        if (const std::uint8_t *rp = G.rt->memory().raw_pointer(addr, raw_len)) std::memcpy(G.clut_raw, rp, raw_len);
        else for (unsigned i = 0; i < raw_len; ++i) G.clut_raw[i] = static_cast<std::uint8_t>(rd8(addr + i));
    }
    for (unsigned i = 0; i < entries; ++i)
        G.clut[i] = fmt == 3 ? rd32(addr + i * 4u) : unpack(rd16(addr + i * 2u), fmt);
}

std::uint32_t clut_lookup(std::uint32_t index) {
    const std::uint32_t mode = G.reg[0xC5];
    const unsigned shift = (mode >> 2) & 0x1F, mask = (mode >> 8) & 0xFF, start = (mode >> 16) & 0x1F;
    return G.clut[(((index >> shift) & mask) | (start << 4)) & 1023u];
}

std::uint32_t fetch_texel_slow(int x, int y) {
    const unsigned wlog = G.reg[0xB8] & 0xF, hlog = (G.reg[0xB8] >> 8) & 0xF;
    const int w = 1 << std::min(wlog, 9u), h = 1 << std::min(hlog, 9u);
    if (G.reg[0xC7] & 1u) x = std::clamp(x, 0, w - 1); else x &= (w - 1);
    if (G.reg[0xC7] & 0x100u) y = std::clamp(y, 0, h - 1); else y &= (h - 1);

    const unsigned fmt = G.reg[0xC3] & 0xF;
    const std::uint32_t stride = std::max<std::uint32_t>(G.reg[0xA8] & 0xFFFFu, 1u);
    const std::uint32_t addr = (G.reg[0xA0] & 0xFFFFFFu) | ((G.reg[0xA8] & 0xFF0000u) << 8);
    const bool swizzled = (G.reg[0xC2] & 1u) != 0u;
    static const unsigned bpp_of[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    if (fmt > 7) return 0xFFFF00FFu;  // compressed formats not implemented yet
    const unsigned bpp = bpp_of[fmt];
    const std::uint32_t row_bytes = stride * bpp / 8u;
    const std::uint32_t xb = static_cast<std::uint32_t>(x) * bpp / 8u;
    std::uint32_t off;
    if (!swizzled) off = static_cast<std::uint32_t>(y) * row_bytes + xb;
    else off = (static_cast<std::uint32_t>(y) / 8u) * (row_bytes / 16u) * 128u + (xb / 16u) * 128u +
               (static_cast<std::uint32_t>(y) % 8u) * 16u + (xb % 16u);
    // Fast path: the texture's memory was resolved once for the whole primitive.
    if (G.tex_ptr && addr == G.tex_addr) {
        const std::size_t need = bpp >= 8 ? bpp / 8u : 1u;
        if (off + need <= G.tex_len) {
            const std::uint8_t *p = G.tex_ptr + off;
            auto r16 = [&] { std::uint16_t v; std::memcpy(&v, p, 2); return static_cast<std::uint32_t>(v); };
            auto r32 = [&] { std::uint32_t v; std::memcpy(&v, p, 4); return v; };
            switch (fmt) {
            case 0: case 1: case 2: return unpack(r16(), fmt);
            case 3: return r32();
            case 4: return clut_lookup((x & 1) ? (p[0] >> 4) : (p[0] & 15));
            case 5: return clut_lookup(p[0]);
            case 6: return clut_lookup(r16());
            default: return clut_lookup(r32());
            }
        }
    }
    switch (fmt) {
    case 0: case 1: case 2: return unpack(rd16(addr + off), fmt);
    case 3: return rd32(addr + off);
    case 4: { const std::uint32_t b = rd8(addr + off); return clut_lookup((x & 1) ? (b >> 4) : (b & 15)); }
    case 5: return clut_lookup(rd8(addr + off));
    case 6: return clut_lookup(rd16(addr + off));
    default: return clut_lookup(rd32(addr + off));
    }
}


// Per-primitive texture description so the per-pixel path does no register decoding.
struct TexInfo {
    bool fast{};
    int wmask{}, hmask{}, w{}, h{};
    bool clamp_u{}, clamp_v{}, swz{};
    unsigned fmt{}, bpp{};
    std::uint32_t row_bytes{};
    unsigned shift{}, cmask{}, start{};
    bool nearest_only{};  // set by the sprite path when linear filtering would equal nearest
    bool use_linear{};    // chosen per primitive: magnify filter when enlarging, minify filter otherwise
} TI;

void setup_texinfo() {
    TI.fast = false;
    if (!G.tex_ptr) return;
    static const unsigned bpp_of[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    TI.fmt = G.reg[0xC3] & 0xF;
    if (TI.fmt > 7) return;
    TI.bpp = bpp_of[TI.fmt];
    TI.w = 1 << std::min(G.reg[0xB8] & 0xFu, 9u);
    TI.h = 1 << std::min((G.reg[0xB8] >> 8) & 0xFu, 9u);
    TI.wmask = TI.w - 1; TI.hmask = TI.h - 1;
    TI.clamp_u = (G.reg[0xC7] & 1u) != 0u;
    TI.clamp_v = (G.reg[0xC7] & 0x100u) != 0u;
    TI.swz = (G.reg[0xC2] & 1u) != 0u;
    const std::uint32_t stride = std::max<std::uint32_t>(G.reg[0xA8] & 0xFFFFu, 1u);
    TI.row_bytes = stride * TI.bpp / 8u;
    const std::uint32_t mode = G.reg[0xC5];
    TI.shift = (mode >> 2) & 0x1F; TI.cmask = (mode >> 8) & 0xFF; TI.start = (mode >> 16) & 0x1F;
    TI.fast = true;
}

inline std::uint32_t clut_fast(std::uint32_t index) {
    return G.clut[(((index >> TI.shift) & TI.cmask) | (TI.start << 4)) & 1023u];
}

std::uint32_t fetch_texel(int x, int y) {
    if (!TI.fast) return fetch_texel_slow(x, y);
    if (TI.clamp_u) x = x < 0 ? 0 : (x > TI.wmask ? TI.wmask : x); else x &= TI.wmask;
    if (TI.clamp_v) y = y < 0 ? 0 : (y > TI.hmask ? TI.hmask : y); else y &= TI.hmask;
    const std::uint32_t xb = static_cast<std::uint32_t>(x) * TI.bpp / 8u;
    std::uint32_t off;
    if (!TI.swz) off = static_cast<std::uint32_t>(y) * TI.row_bytes + xb;
    else off = (static_cast<std::uint32_t>(y) >> 3) * (TI.row_bytes >> 4) * 128u + (xb >> 4) * 128u +
               (static_cast<std::uint32_t>(y) & 7u) * 16u + (xb & 15u);
    const std::size_t need = TI.bpp >= 8 ? TI.bpp / 8u : 1u;
    if (off + need > G.tex_len) return fetch_texel_slow(x, y);
    const std::uint8_t *p = G.tex_ptr + off;
    switch (TI.fmt) {
    case 0: case 1: case 2: { std::uint16_t v; std::memcpy(&v, p, 2); return unpack(v, TI.fmt); }
    case 3: { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
    case 4: return clut_fast((x & 1) ? (p[0] >> 4) : (p[0] & 15));
    case 5: return clut_fast(p[0]);
    case 6: { std::uint16_t v; std::memcpy(&v, p, 2); return clut_fast(v); }
    default: { std::uint32_t v; std::memcpy(&v, p, 4); return clut_fast(v); }
    }
}

// TEXFILTER (0xC6): bits 0-2 minify, bits 8-10 magnify; odd values are linear.
void choose_filter(float texels_per_pixel) {
    const std::uint32_t filter = G.reg[0xC6];
    TI.use_linear = texels_per_pixel <= 1.0001f ? (((filter >> 8) & 1u) != 0u) : ((filter & 1u) != 0u);
}

std::uint32_t sample(float fu, float fv) {
    const bool linear = !TI.nearest_only && TI.use_linear;
    if (!linear) return fetch_texel(static_cast<int>(std::floor(fu)), static_cast<int>(std::floor(fv)));
    const float gu = fu - 0.5f, gv = fv - 0.5f;
    const int x0 = static_cast<int>(std::floor(gu)), y0 = static_cast<int>(std::floor(gv));
    const float fx = gu - x0, fy = gv - y0;
    const std::uint32_t c00 = fetch_texel(x0, y0), c10 = fetch_texel(x0 + 1, y0);
    const std::uint32_t c01 = fetch_texel(x0, y0 + 1), c11 = fetch_texel(x0 + 1, y0 + 1);
    std::uint32_t out = 0;
    for (int ch = 0; ch < 4; ++ch) {
        const int sh = ch * 8;
        const float top = ((c00 >> sh) & 0xFF) * (1.f - fx) + ((c10 >> sh) & 0xFF) * fx;
        const float bot = ((c01 >> sh) & 0xFF) * (1.f - fx) + ((c11 >> sh) & 0xFF) * fx;
        out |= static_cast<std::uint32_t>(std::clamp(static_cast<int>(top * (1.f - fy) + bot * fy + 0.5f), 0, 255)) << sh;
    }
    return out;
}

// ---- fragment pipeline ---------------------------------------------------------------
unsigned long long g_px_count = 0;
unsigned long long g_c_rect = 0, g_c_rect_fast = 0, g_c_rect_px = 0, g_c_rect_lin = 0, g_c_tri = 0, g_c_tri_px = 0;
unsigned long long g_lists_started = 0;
bool g_log_frame = false;  // F10: log every primitive of the next frame that draws anything
unsigned g_logged_prims = 0;

struct Frag {
    int x, y;
    std::uint32_t color;  // vertex colour
    float u, v;
};

int blend_factor(unsigned f, int s, int d, int sa, int da, std::uint32_t fix, bool is_dst) {
    // Returns a 0..255 factor for one colour channel (s/d are that channel's values).
    switch (f) {
    case 0: return is_dst ? s : d;
    case 1: return 255 - (is_dst ? s : d);
    case 2: return sa;
    case 3: return 255 - sa;
    case 4: return da;
    case 5: return 255 - da;
    case 6: return std::min(255, sa * 2);
    case 7: return 255 - std::min(255, sa * 2);
    case 8: return std::min(255, da * 2);
    case 9: return 255 - std::min(255, da * 2);
    default: (void)s; return static_cast<int>(fix);
    }
}

void shade(const Target &t, const Frag &f, bool clear_mode, bool textured, const std::uint32_t *pre_tex = nullptr) {
    std::uint32_t c = f.color;
    if (clear_mode) { put_px(t, f.x, f.y, c); return; }

    if (textured) {
        const std::uint32_t tx = pre_tex ? *pre_tex : sample(f.u, f.v);
        const std::uint32_t func = G.reg[0xC9];
        const bool tex_alpha = (func & 0x100u) != 0u;
        int tr = tx & 0xFF, tg = (tx >> 8) & 0xFF, tb = (tx >> 16) & 0xFF, ta = tx >> 24;
        int fr = c & 0xFF, fg = (c >> 8) & 0xFF, fb = (c >> 16) & 0xFF, fa = c >> 24;
        int r, g, b, a;
        switch (func & 7u) {
        case 0:  // modulate
            if (c == 0xFFFFFFFFu) { r = tr; g = tg; b = tb; a = ta; break; }
            r = tr * fr / 255; g = tg * fg / 255; b = tb * fb / 255; a = tex_alpha ? ta * fa / 255 : fa; break;
        case 1:  // decal
            if (tex_alpha) { r = (tr * ta + fr * (255 - ta)) / 255; g = (tg * ta + fg * (255 - ta)) / 255; b = (tb * ta + fb * (255 - ta)) / 255; }
            else { r = tr; g = tg; b = tb; }
            a = fa; break;
        case 3:  // replace
            r = tr; g = tg; b = tb; a = tex_alpha ? ta : fa; break;
        case 4:  // add
            r = std::min(255, tr + fr); g = std::min(255, tg + fg); b = std::min(255, tb + fb);
            a = tex_alpha ? ta * fa / 255 : fa; break;
        case 2: {  // blend: mix the fragment colour and the texture-environment colour by the texture
            const int er = G.reg[0xCA] & 0xFF, eg = (G.reg[0xCA] >> 8) & 0xFF, eb = (G.reg[0xCA] >> 16) & 0xFF;
            r = (fr * (255 - tr) + er * tr) / 255;
            g = (fg * (255 - tg) + eg * tg) / 255;
            b = (fb * (255 - tb) + eb * tb) / 255;
            a = tex_alpha ? ta * fa / 255 : fa;
            break;
        }
        default: // reserved function numbers: treat as modulate
            r = tr * fr / 255; g = tg * fg / 255; b = tb * fb / 255; a = tex_alpha ? ta * fa / 255 : fa; break;
        }
        if (func & 0x10000u) { r = std::min(255, r * 2); g = std::min(255, g * 2); b = std::min(255, b * 2); }
        c = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) | (static_cast<std::uint32_t>(b) << 16) |
            (static_cast<std::uint32_t>(a) << 24);
    }

    if (G.reg[0x22] & 1u) {  // alpha test
        const std::uint32_t at = G.reg[0xDB];
        const unsigned fn = at & 7u;
        const int ref = (at >> 8) & 0xFF, mask = (at >> 16) & 0xFF;
        const int a = (c >> 24) & mask, rf = ref & mask;
        bool pass = true;
        switch (fn) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = a == rf; break;
        case 3: pass = a != rf; break;
        case 4: pass = a < rf; break;
        case 5: pass = a <= rf; break;
        case 6: pass = a > rf; break;
        default: pass = a >= rf; break;
        }
        if (!pass) return;
    }

    if (G.reg[0x27] & 1u) {  // colour test
        const std::uint32_t ct = G.reg[0xD8], ref = G.reg[0xD9] & 0xFFFFFFu, mask = G.reg[0xDA] & 0xFFFFFFu;
        const bool eq = ((c & mask) & 0xFFFFFFu) == (ref & mask);
        bool pass = true;
        switch (ct & 3u) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = eq; break;
        default: pass = !eq; break;
        }
        if (!pass) return;
    }

    const bool stencil_on = (G.reg[0x24] & 1u) != 0u;
    std::uint32_t dst_px = 0;
    bool have_dst = false;
    std::uint32_t new_alpha_from_stencil = 0;
    if (stencil_on) {
        dst_px = get_px(t, f.x, f.y);
        have_dst = true;
        const std::uint32_t st = G.reg[0xDC], so = G.reg[0xDD];
        const unsigned fn = st & 7u;
        const int ref = (st >> 8) & 0xFF, mask = (st >> 16) & 0xFF;
        const int sv = (dst_px >> 24) & mask, rf = ref & mask;
        bool pass = true;
        switch (fn) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = sv == rf; break;
        case 3: pass = sv != rf; break;
        case 4: pass = sv < rf; break;
        case 5: pass = sv <= rf; break;
        case 6: pass = sv > rf; break;
        default: pass = sv >= rf; break;
        }
        const unsigned op = pass ? ((so >> 16) & 7u) : (so & 7u);
        int cur = static_cast<int>(dst_px >> 24);
        switch (op) {
        case 1: cur = 0; break;
        case 2: cur = ref; break;
        case 3: cur = ~cur & 0xFF; break;
        case 4: cur = std::min(255, cur + 1); break;
        case 5: cur = std::max(0, cur - 1); break;
        default: break;  // keep
        }
        new_alpha_from_stencil = static_cast<std::uint32_t>(cur) << 24;
        if (!pass) {
            put_px(t, f.x, f.y, (dst_px & 0x00FFFFFFu) | new_alpha_from_stencil);
            return;
        }
    }

    if ((G.reg[0x21] & 1u) && !stencil_on && (G.reg[0xDF] & 0x7FFu) == 0x032u && ((G.reg[0xE8] & 0xFFFFFFu) == 0u)) {
        // Ordinary alpha blending: fully transparent pixels change nothing, opaque ones overwrite.
        const int sa0 = static_cast<int>(c >> 24);
        if (sa0 == 0) return;
        if (sa0 == 255) {
            if (t.fmt == 3 && G.fb_ptr) {  // keep the framebuffer alpha: write the three colour bytes only
                const std::size_t off = (static_cast<std::size_t>(f.y) * t.stride + f.x) * 4u;
                if (off + 4 <= G.fb_len) { mark_px(f.x, f.y); std::memcpy(G.fb_ptr + off, &c, 3); return; }
            }
            put_px(t, f.x, f.y, (c & 0x00FFFFFFu) | (get_px(t, f.x, f.y) & 0xFF000000u));
            return;
        }
        // Partial transparency: result = src * a + dst * (1 - a), framebuffer alpha untouched.
        const std::uint32_t d = get_px(t, f.x, f.y);
        const unsigned ia = 255u - static_cast<unsigned>(sa0), sa = static_cast<unsigned>(sa0);
        const unsigned r = ((c & 0xFFu) * sa + (d & 0xFFu) * ia + 127u) / 255u;
        const unsigned gg = (((c >> 8) & 0xFFu) * sa + ((d >> 8) & 0xFFu) * ia + 127u) / 255u;
        const unsigned bb = (((c >> 16) & 0xFFu) * sa + ((d >> 16) & 0xFFu) * ia + 127u) / 255u;
        put_px(t, f.x, f.y, (d & 0xFF000000u) | (bb << 16) | (gg << 8) | r);
        return;
    }
    if (G.reg[0x21] & 1u) {  // alpha blend
        const std::uint32_t d = have_dst ? dst_px : get_px(t, f.x, f.y);
        const std::uint32_t bm = G.reg[0xDF];
        const unsigned sf = bm & 15u, df = (bm >> 4) & 15u, eq = (bm >> 8) & 7u;
        const int sa = c >> 24, da = d >> 24;
        const std::uint32_t fixa = G.reg[0xE0], fixb = G.reg[0xE1];
        std::uint32_t out = d & 0xFF000000u;
        for (int ch = 0; ch < 3; ++ch) {
            const int sh = ch * 8;
            const int s = (c >> sh) & 0xFF, dd = (d >> sh) & 0xFF;
            const int sfac = blend_factor(sf, s, dd, sa, da, (fixa >> sh) & 0xFF, false);
            const int dfac = blend_factor(df, s, dd, sa, da, (fixb >> sh) & 0xFF, true);
            const int sv = s * sfac / 255, dv = dd * dfac / 255;
            int r;
            switch (eq) {
            case 0: r = sv + dv; break;
            case 1: r = sv - dv; break;
            case 2: r = dv - sv; break;
            case 3: r = std::min(s, dd); break;
            case 4: r = std::max(s, dd); break;
            default: r = std::abs(sv - dv); break;
            }
            out |= static_cast<std::uint32_t>(std::clamp(r, 0, 255)) << sh;
        }
        c = out | (c & 0xFF000000u);
    }
    if (stencil_on) c = (c & 0x00FFFFFFu) | new_alpha_from_stencil;

    // Colour write masks: a set bit means "do not write this bit".
    // With the stencil test off the PSP leaves the framebuffer alpha alone (the game relies on that:
    // its glow/effect blends use "1 - destination alpha").
    const std::uint32_t wmask = (G.reg[0xE8] & 0xFFFFFFu) | ((G.reg[0xE9] & 0xFFu) << 24) |
                                (stencil_on ? 0u : 0xFF000000u);
    if (wmask != 0u) {
        const std::uint32_t d = have_dst ? dst_px : get_px(t, f.x, f.y);
        c = (c & ~wmask) | (d & wmask);
    }
    put_px(t, f.x, f.y, c);
}

// ---- vertices ------------------------------------------------------------------------
struct VertexLayout {
    bool through{};
    unsigned tex{}, col{}, norm{}, pos{}, weight{}, idx{}, wcount{};
    std::uint32_t size{};
    std::uint32_t off_w{}, off_t{}, off_c{}, off_n{}, off_p{};
};

inline std::uint32_t comp_size(unsigned fmt) { return fmt == 3 ? 4u : fmt; }  // 1=8bit 2=16bit 3=float

VertexLayout layout_from_vtype(std::uint32_t vt) {
    VertexLayout L;
    L.through = (vt >> 23) & 1u;
    L.tex = vt & 3u;
    L.col = (vt >> 2) & 7u;
    L.norm = (vt >> 5) & 3u;
    L.pos = (vt >> 7) & 3u;
    L.weight = (vt >> 9) & 3u;
    L.idx = (vt >> 11) & 3u;
    L.wcount = ((vt >> 14) & 7u) + 1u;
    std::uint32_t off = 0, maxal = 1;
    auto align = [&](std::uint32_t a) { off = (off + a - 1) & ~(a - 1); maxal = std::max(maxal, a); };
    if (L.weight) { const auto s = comp_size(L.weight); align(s); L.off_w = off; off += s * L.wcount; }
    if (L.tex) { const auto s = comp_size(L.tex); align(s); L.off_t = off; off += s * 2; }
    if (L.col) { const std::uint32_t s = L.col == 7 ? 4u : 2u; align(s); L.off_c = off; off += s; }
    if (L.norm) { const auto s = comp_size(L.norm); align(s); L.off_n = off; off += s * 3; }
    if (L.pos) { const auto s = comp_size(L.pos); align(s); L.off_p = off; off += s * 3; }
    align(maxal);
    L.size = off;
    return L;
}

float read_comp(std::uint32_t a, unsigned fmt, bool is_signed) {
    switch (fmt) {
    case 1: { const std::uint32_t v = rd8(a); return is_signed ? static_cast<float>(static_cast<std::int8_t>(v)) : static_cast<float>(v); }
    case 2: { const std::uint32_t v = rd16(a); return is_signed ? static_cast<float>(static_cast<std::int16_t>(v)) : static_cast<float>(v); }
    default: { const std::uint32_t v = rd32(a); float f; std::memcpy(&f, &v, 4); return f; }
    }
}

Vtx fetch(const VertexLayout &L, std::uint32_t a, bool *has_color) {
    Vtx v;
    if (L.tex) { v.u = read_comp(a + L.off_t, L.tex, false); v.v = read_comp(a + L.off_t + comp_size(L.tex), L.tex, false); }
    *has_color = L.col != 0;
    if (L.col == 7) v.color = rd32(a + L.off_c);
    else if (L.col) v.color = unpack(rd16(a + L.off_c), L.col - 4);
    if (L.pos) {
        v.x = read_comp(a + L.off_p, L.pos, true);
        v.y = read_comp(a + L.off_p + comp_size(L.pos), L.pos, true);
    }
    return v;
}

// ---- rasterizer ----------------------------------------------------------------------
struct Scissor { int x1, y1, x2, y2; };

inline float edge(const Vtx &a, const Vtx &b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}
inline bool top_left(const Vtx &a, const Vtx &b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    return (dy == 0.f && dx > 0.f) || dy < 0.f;
}

void draw_triangle(const Target &t, const Scissor &sc, Vtx a, Vtx b, Vtx c, bool clear_mode, bool textured) {
    float area = edge(a, b, c.x, c.y);
    if (area == 0.f) return;
    ++g_c_tri; g_c_tri_px += static_cast<unsigned long long>(std::fabs(area) * 0.5f);
    if (area < 0.f) { std::swap(b, c); area = -area; }
    const int minx = std::max(sc.x1, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int maxx = std::min(sc.x2, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int miny = std::max(sc.y1, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int maxy = std::min(sc.y2, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    const bool tl0 = top_left(b, c), tl1 = top_left(c, a), tl2 = top_left(a, b);
    if (textured) {
        const float uv_area = std::fabs((b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v));
        choose_filter(std::sqrt(uv_area / area));
    }
    // Gouraud: interpolate the four colour channels.
    const float inv_area = 1.f / area;
    const bool flat = a.color == b.color && b.color == c.color;
    const Vtx *ep[3][2] = {{&b, &c}, {&c, &a}, {&a, &b}};  // edges opposite a, b, c
    for (int y = miny; y <= maxy; ++y) {
        const float py = y + 0.5f;
        // Narrow the x range for this row to where all three edge functions can be >= 0
        // (with a one-pixel margin); the exact test below decides coverage.
        int xa = minx, xb = maxx;
        bool empty = false;
        for (int e = 0; e < 3; ++e) {
            const Vtx &p = *ep[e][0], &q = *ep[e][1];
            const float s = -(q.y - p.y);                                   // d(w)/d(px)
            const float a0 = (q.x - p.x) * (py - p.y) + (q.y - p.y) * p.x;  // w = a0 + s * px
            if (s > 0.f) xa = std::max(xa, static_cast<int>(std::floor(-a0 / s - 0.5f)) - 1);
            else if (s < 0.f) xb = std::min(xb, static_cast<int>(std::ceil(-a0 / s - 0.5f)) + 1);
            else if (a0 < 0.f) { empty = true; break; }
        }
        if (empty) continue;
        for (int x = xa; x <= xb; ++x) {
            const float px = x + 0.5f;
            const float w0 = edge(b, c, px, py), w1 = edge(c, a, px, py), w2 = edge(a, b, px, py);
            if (!((w0 > 0.f || (w0 == 0.f && tl0)) && (w1 > 0.f || (w1 == 0.f && tl1)) && (w2 > 0.f || (w2 == 0.f && tl2))))
                continue;
            const float l0 = w0 * inv_area, l1 = w1 * inv_area, l2 = w2 * inv_area;
            std::uint32_t col = a.color;
            if (!flat) {
                col = 0;
                for (int ch = 0; ch < 4; ++ch) {
                    const int sh = ch * 8;
                    const float v = l0 * ((a.color >> sh) & 0xFF) + l1 * ((b.color >> sh) & 0xFF) + l2 * ((c.color >> sh) & 0xFF);
                    col |= static_cast<std::uint32_t>(std::clamp(static_cast<int>(v + 0.5f), 0, 255)) << sh;
                }
            }
            Frag f{x, y, col, l0 * a.u + l1 * b.u + l2 * c.u, l0 * a.v + l1 * b.v + l2 * c.v};
            shade(t, f, clear_mode, textured);
        }
    }
}

void draw_sprite(const Target &t, const Scissor &sc, const Vtx &a, const Vtx &b, bool clear_mode, bool textured) {
    const int x0 = static_cast<int>(std::min(a.x, b.x)), x1 = static_cast<int>(std::max(a.x, b.x));
    const int y0 = static_cast<int>(std::min(a.y, b.y)), y1 = static_cast<int>(std::max(a.y, b.y));
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float du = dx != 0.f ? (b.u - a.u) / dx : 0.f;
    const float dv = dy != 0.f ? (b.v - a.v) / dy : 0.f;
    const int xs = std::max(x0, sc.x1), xe = std::min(x1, sc.x2 + 1);
    const int ys = std::max(y0, sc.y1), ye = std::min(y1, sc.y2 + 1);
    if (xs >= xe || ys >= ye) return;
    const float u_start = a.u + (xs + 0.5f - a.x) * du;
    const float v_start = a.v + (ys + 0.5f - a.y) * dv;
    // A 1:1 (or mirrored) copy landing on texel centres makes bilinear filtering equal to nearest.
    TI.nearest_only = false;
    if (textured) choose_filter(std::sqrt(std::fabs(du * dv)));
    if (textured) {
        auto near_int = [](float v) { return std::fabs(v - std::floor(v + 0.5f)) < 0.002f; };
        const bool su = std::fabs(std::fabs(du) - 1.f) < 0.0005f, sv = std::fabs(std::fabs(dv) - 1.f) < 0.0005f;
        if (su && sv && near_int(u_start - 0.5f) && near_int(v_start - 0.5f)) TI.nearest_only = true;
    }
    for (int y = ys; y < ye; ++y) {
        const float v = v_start + (y - ys) * dv;
        float u = u_start;
        for (int x = xs; x < xe; ++x, u += du) {
            Frag f{x, y, b.color, u, v};
            shade(t, f, clear_mode, textured);
        }
    }
    TI.nearest_only = false;
}




// ---- small worker pool: big rectangles are split into bands of rows drawn in parallel -----
class RowPool {
public:
    RowPool() {
        unsigned n = std::thread::hardware_concurrency();
        n = n > 1 ? std::min(n - 1, 7u) : 0u;
        if (const char *e = std::getenv("PERSONA1_THREADS")) n = static_cast<unsigned>(std::atoi(e));
        for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~RowPool() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        for (auto &t : workers_) t.join();
    }
    bool usable() const { return !workers_.empty(); }
    // Runs fn(0..jobs-1) on the pool (and the calling thread) and returns when all are done.
    void run(int jobs, const std::function<void(int)> &fn) {
        {
            std::lock_guard<std::mutex> lk(m_);
            fn_ = &fn; jobs_ = jobs; next_ = 0; remaining_ = jobs; ++gen_;
        }
        cv_.notify_all();
        work();
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [this] { return remaining_ == 0; });
        fn_ = nullptr;
    }
private:
    void work() {
        for (;;) {
            int i;
            const std::function<void(int)> *fn;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (!fn_ || next_ >= jobs_) return;
                i = next_++; fn = fn_;
            }
            try { (*fn)(i); } catch (...) {}
            std::lock_guard<std::mutex> lk(m_);
            if (--remaining_ == 0) done_.notify_all();
        }
    }
    void loop() {
        unsigned seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
                if (stop_) return;
                seen = gen_;
            }
            work();
        }
    }
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)> *fn_{};
    int jobs_{}, next_{}, remaining_{};
    unsigned gen_{};
    bool stop_{};
};
RowPool &row_pool() { static RowPool p; return p; }

// Axis-aligned rectangle with affine texture coordinates at any (fractional) position and scale.
// Pixels whose centre lies in [x0,x1) x [y0,y1) are covered (same result as two triangles).
// Texels are decoded once per row (and shared between rows) instead of once per pixel.
void draw_rect(const Target &t, const Scissor &sc, const Vtx &a, const Vtx &b, bool clear_mode, bool textured) {
    const float xl = std::min(a.x, b.x), xr = std::max(a.x, b.x), yt = std::min(a.y, b.y), yb = std::max(a.y, b.y);
    const int xs = std::max(static_cast<int>(std::ceil(xl - 0.5f)), sc.x1), xe = std::min(static_cast<int>(std::ceil(xr - 0.5f)), sc.x2 + 1);
    const int ys = std::max(static_cast<int>(std::ceil(yt - 0.5f)), sc.y1), ye = std::min(static_cast<int>(std::ceil(yb - 0.5f)), sc.y2 + 1);
    if (xs >= xe || ys >= ye) return;
    const int ncols = xe - xs;
    if (!textured) {
        for (int y = ys; y < ye; ++y)
            for (int x = xs; x < xe; ++x) { Frag f{x, y, a.color, 0.f, 0.f}; shade(t, f, clear_mode, false); }
        return;
    }
    const float du = (b.u - a.u) / (b.x - a.x), dv = (b.v - a.v) / (b.y - a.y);
    choose_filter(std::sqrt(std::fabs(du * dv)));
    const float u_first = a.u + (xs + 0.5f - a.x) * du, v_first = a.v + (ys + 0.5f - a.y) * dv;
    auto near_int = [](float v) { return std::fabs(v - std::floor(v + 0.5f)) < 0.002f; };
    bool linear = TI.use_linear;
    if (linear && std::fabs(std::fabs(du) - 1.f) < 0.0005f && std::fabs(std::fabs(dv) - 1.f) < 0.0005f &&
        near_int(u_first - 0.5f) && near_int(v_first - 0.5f))
        linear = false;  // 1:1 on texel centres: linear == nearest

    // Per-column texel index and weight.
    std::vector<int> cix(ncols), cfx(ncols);
    int xmin = 1 << 30, xmax = -(1 << 30);
    for (int i = 0; i < ncols; ++i) {
        const float u = a.u + (xs + i + 0.5f - a.x) * du;
        const float gu = linear ? u - 0.5f : u;
        const int ix = static_cast<int>(std::floor(gu));
        cix[i] = ix;
        cfx[i] = linear ? std::clamp(static_cast<int>((gu - ix) * 256.f + 0.5f), 0, 256) : 0;
        xmin = std::min(xmin, ix); xmax = std::max(xmax, ix + (linear ? 1 : 0));
    }
    const int span = xmax - xmin + 1;
    // Fast path: plain white-vertex-colour texturing with ordinary alpha blending. Fully transparent texels
    // change nothing and fully opaque ones overwrite, exactly as shade() would do, without the per-pixel overhead.
    const std::uint32_t fn_reg = G.reg[0xC9];
    const bool fast_ok = textured && !clear_mode && a.color == 0xFFFFFFFFu && (fn_reg & 0x100u) != 0u && !(fn_reg & 0x10000u) &&
                         ((fn_reg & 7u) == 0u || (fn_reg & 7u) == 3u) && !(G.reg[0x22] & 1u) && !(G.reg[0x27] & 1u) &&
                         !(G.reg[0x24] & 1u) && (G.reg[0x21] & 1u) && (G.reg[0xDF] & 0x7FFu) == 0x032u &&
                         (G.reg[0xE8] & 0xFFFFFFu) == 0u && t.fmt == 3 && G.fb_ptr != nullptr;
    ++g_c_rect; if (fast_ok) ++g_c_rect_fast; if (linear) ++g_c_rect_lin;
    g_c_rect_px += static_cast<unsigned long long>(std::max(0, ye - ys)) * static_cast<unsigned long long>(std::max(0, ncols));
    if (std::getenv("PERSONA1_PROF") && !fast_ok && textured) {
        static unsigned shown = 0;
        if (shown++ < 12)
            std::cerr << "[rectdbg] fast_ok=0 color=" << std::hex << a.color << " func=" << fn_reg << " atest=" << (G.reg[0x22] & 1) << " ctest=" << (G.reg[0x27] & 1)
                      << " stencil=" << (G.reg[0x24] & 1) << " blendEn=" << (G.reg[0x21] & 1) << " blend=" << G.reg[0xDF] << " e8=" << G.reg[0xE8]
                      << " tfmt=" << t.fmt << " fb=" << (G.fb_ptr != nullptr) << std::dec << " size=" << ncols << "x" << (ye - ys) << "\n";
    }
    auto rows = [&](int ya, int yb) {
        std::vector<std::uint32_t> r0(span), r1(linear ? span : 0);
        int row0 = 1 << 30;  // texel row currently held in r0 (and r0+1 in r1 when linear)
        auto fill = [&](std::vector<std::uint32_t> &dst, int ty) { for (int i = 0; i < span; ++i) dst[i] = fetch_texel(xmin + i, ty); };
        for (int y = ya; y < yb; ++y) {
            const float v = v_first + (y - ys) * dv;
            const float gv = linear ? v - 0.5f : v;
            const int iy = static_cast<int>(std::floor(gv));
            const int fy = linear ? std::clamp(static_cast<int>((gv - iy) * 256.f + 0.5f), 0, 256) : 0;
            if (iy != row0) {
                if (linear && iy == row0 + 1) { r0.swap(r1); fill(r1, iy + 1); }
                else { fill(r0, iy); if (linear) fill(r1, iy + 1); }
                row0 = iy;
            }
            for (int i = 0; i < ncols; ++i) {
                const int k = cix[i] - xmin;
                std::uint32_t tx;
                if (!linear) tx = r0[k];
                else if (cfx[i] == 0 && fy == 0) tx = r0[k];
                else {
                    const int wx = cfx[i];
                    const std::uint32_t c00 = r0[k], c10 = r0[k + 1], c01 = r1[k], c11 = r1[k + 1];
                    tx = 0;
                    if (c00 == c10 && c00 == c01 && c00 == c11) tx = c00;
                    else for (int sh = 0; sh < 32; sh += 8) {
                        const int top = (static_cast<int>((c00 >> sh) & 0xFF) * (256 - wx) + static_cast<int>((c10 >> sh) & 0xFF) * wx + 128) >> 8;
                        const int bot = (static_cast<int>((c01 >> sh) & 0xFF) * (256 - wx) + static_cast<int>((c11 >> sh) & 0xFF) * wx + 128) >> 8;
                        tx |= static_cast<std::uint32_t>(std::min(255, (top * (256 - fy) + bot * fy + 128) >> 8)) << sh;
                    }
                }
                if (fast_ok) {
                    const std::uint32_t a8 = tx >> 24;
                    if (a8 == 0u) continue;
                    if (a8 == 255u) {
                        const int px = xs + i;
                        const std::size_t off = (static_cast<std::size_t>(y) * t.stride + px) * 4u;
                        if (off + 4 <= G.fb_len) {
                            mark_px(px, y);
                            std::memcpy(G.fb_ptr + off, &tx, 3);
                            continue;
                        }
                    }
                }
                Frag f{xs + i, y, a.color, 0.f, 0.f};
                shade(t, f, clear_mode, true, &tx);
            }
        }
    };
    const int nrows = ye - ys;
    if (static_cast<long long>(nrows) * ncols >= 20000 && nrows >= 32 && row_pool().usable()) {
        const int band = std::max(8, nrows / 16);
        const int jobs = (nrows + band - 1) / band;
        row_pool().run(jobs, [&](int j) { rows(ys + j * band, std::min(ye, ys + (j + 1) * band)); });
    } else {
        rows(ys, ye);
    }
}

// Axis-aligned rectangles drawn as a 4-vertex triangle strip (how the game draws most text and
// sprites) are rendered with the much faster sprite path when the result is identical.
bool quad_as_sprite(const Target &t, const Scissor &sc, const Vtx *v, bool clear_mode, bool textured) {
    static const bool disabled = std::getenv("PERSONA1_NOQUAD") != nullptr;
    if (disabled) return false;
    const Vtx &a = v[0], &d = v[3];
    if (a.x == d.x || a.y == d.y) return false;
    if (v[1].color != a.color || v[2].color != a.color || d.color != a.color) return false;
    const bool c1 = v[1].x == a.x && v[1].y == d.y && v[2].x == d.x && v[2].y == a.y;
    const bool c2 = v[1].x == d.x && v[1].y == a.y && v[2].x == a.x && v[2].y == d.y;
    if (!c1 && !c2) return false;
    if (textured) {
        const Vtx &p = c1 ? v[1] : v[2];  // shares x with a, y with d
        const Vtx &q = c1 ? v[2] : v[1];  // shares x with d, y with a
        if (p.u != a.u || p.v != d.v || q.u != d.u || q.v != a.v) return false;
    }
    Vtx b = d;
    b.color = a.color;
    draw_rect(t, sc, a, b, clear_mode, textured);
    return true;
}

// Debug: PERSONA1_TEXDUMP=1 saves every distinct texture the game draws with, decoded as
// currently configured and with the swizzle flag flipped, so wrong decoding is visible.
void dump_texture_once() {
    static const bool enabled = std::getenv("PERSONA1_TEXDUMP") != nullptr;
    if (!enabled) return;
    static std::vector<std::uint64_t> seen;
    const std::uint64_t key = (static_cast<std::uint64_t>(G.reg[0xA0]) << 40) ^ (static_cast<std::uint64_t>(G.reg[0xA8]) << 16) ^
                              (static_cast<std::uint64_t>(G.reg[0xB8]) << 4) ^ G.reg[0xC3] ^ (static_cast<std::uint64_t>(G.reg[0xC2] & 1u) << 63);
    if (std::find(seen.begin(), seen.end(), key) != seen.end() || seen.size() >= 12) return;
    seen.push_back(key);
    const int w = 1 << std::min(G.reg[0xB8] & 0xFu, 9u), h = 1 << std::min((G.reg[0xB8] >> 8) & 0xFu, 9u);
    std::vector<std::uint32_t> img(static_cast<std::size_t>(w) * h);
    for (int pass = 0; pass < 2; ++pass) {
        const std::uint32_t saved = G.reg[0xC2];
        if (pass == 1) G.reg[0xC2] ^= 1u;
        try {
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) img[static_cast<std::size_t>(y) * w + x] = fetch_texel(x, y) | 0xFF000000u;
        } catch (const std::exception &) {}
        G.reg[0xC2] = saved;
        char name[128];
        std::snprintf(name, sizeof(name), "tex_%02zu_addr%06X_fmt%u_%ux%u_swz%u_%s.png", seen.size(), G.reg[0xA0] & 0xFFFFFFu,
                      G.reg[0xC3] & 0xFu, w, h, (G.reg[0xC2] & 1u) ^ pass, pass ? "alt" : "asis");
        save_image_png(name, img.data(), w, h);
    }
}

// Debug: PERSONA1_GESTATES=1 prints each distinct combination of pipeline settings once.
void log_state_once(const VertexLayout &L, bool textured, bool clear_mode) {
    static const bool enabled = std::getenv("PERSONA1_GESTATES") != nullptr;
    if (!enabled) return;
    static std::vector<std::string> seen;
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "tex=%d clear=%d vtype=%06X en[blend=%u atest=%u ztest=%u stencil=%u ctest=%u logic=%u] blend=%06X fixa=%06X fixb=%06X "
                  "atest=%06X ctest=%06X/%06X/%06X stencil=%06X/%06X zfunc=%06X zmask=%06X cmask=%06X/%02X func=%06X filter=%03X "
                  "tfmt=%X cfmt=%04X psm=%X",
                  textured, clear_mode, G.reg[0x12], G.reg[0x21] & 1, G.reg[0x22] & 1, G.reg[0x23] & 1, G.reg[0x24] & 1,
                  G.reg[0x27] & 1, G.reg[0x28] & 1, G.reg[0xDF], G.reg[0xE0], G.reg[0xE1], G.reg[0xDB], G.reg[0xD8],
                  G.reg[0xD9], G.reg[0xDA], G.reg[0xDC], G.reg[0xDD], G.reg[0xDE], G.reg[0xE7], G.reg[0xE8], G.reg[0xE9] & 0xFF,
                  G.reg[0xC9], G.reg[0xC6], textured ? (G.reg[0xC3] & 0xF) : 0u, textured ? (G.reg[0xC5] & 0xFFFF) : 0u,
                  G.reg[0xD2] & 3);
    const std::string key(buf);
    if (std::find(seen.begin(), seen.end(), key) != seen.end() || seen.size() >= 80) return;
    seen.push_back(key);
    std::cerr << "[gestate] " << key << "\n";
    (void)L;
}


// Debug: PERSONA1_PROF=2 also reports which kinds of draws cost the most time.
struct DrawStat { double sec{}; unsigned draws{}; unsigned long long px{}; };
std::map<std::string, DrawStat> g_draw_stats;


// ---- 3D pipeline (software): transform, near clip, perspective-correct raster, depth test ----
struct V3 { float x, y, z, w, u, v; std::uint32_t color; };
std::vector<float> g_depth(512u * 272u, 0.f);

float read_norm(std::uint32_t a, unsigned fmt, bool sgn) {
    switch (fmt) {
    case 1: { const std::uint32_t v = rd8(a); return sgn ? static_cast<std::int8_t>(v) / 128.f : v / 128.f; }
    case 2: { const std::uint32_t v = rd16(a); return sgn ? static_cast<std::int16_t>(v) / 32768.f : v / 32768.f; }
    default: { const std::uint32_t v = rd32(a); float f; std::memcpy(&f, &v, 4); return f; }
    }
}
inline float regf(std::uint32_t r) { const std::uint32_t b = r << 8; float f; std::memcpy(&f, &b, 4); return f; }

V3 transform_vertex(const VertexLayout &L, std::uint32_t a, std::uint32_t color) {
    float p[3] = {0, 0, 0};
    if (L.pos) {
        const unsigned cs = comp_size(L.pos);
        for (int i = 0; i < 3; ++i) p[i] = read_norm(a + L.off_p + i * cs, L.pos, true);
    }
    const float *W = G.mat_world, *Vm = G.mat_view, *P = G.mat_proj;
    float w[3], e[3];
    for (int r = 0; r < 3; ++r) w[r] = W[r] * p[0] + W[3 + r] * p[1] + W[6 + r] * p[2] + W[9 + r];
    for (int r = 0; r < 3; ++r) e[r] = Vm[r] * w[0] + Vm[3 + r] * w[1] + Vm[6 + r] * w[2] + Vm[9 + r];
    V3 o;
    o.x = P[0] * e[0] + P[4] * e[1] + P[8] * e[2] + P[12];
    o.y = P[1] * e[0] + P[5] * e[1] + P[9] * e[2] + P[13];
    o.z = P[2] * e[0] + P[6] * e[1] + P[10] * e[2] + P[14];
    o.w = P[3] * e[0] + P[7] * e[1] + P[11] * e[2] + P[15];
    o.u = o.v = 0.f;
    if (L.tex) {
        const unsigned cs = comp_size(L.tex);
        float u = read_norm(a + L.off_t, L.tex, false), v = read_norm(a + L.off_t + cs, L.tex, false);
        u = u * regf(G.reg[0x48]) + regf(G.reg[0x4A]);
        v = v * regf(G.reg[0x49]) + regf(G.reg[0x4B]);
        o.u = u * static_cast<float>(1u << std::min(G.reg[0xB8] & 0xFu, 9u));
        o.v = v * static_cast<float>(1u << std::min((G.reg[0xB8] >> 8) & 0xFu, 9u));
    }
    o.color = color;
    return o;
}

struct SV { float x, y, z, iw, u, v; std::uint32_t color; };

void raster3d(const Target &t, const Scissor &sc, SV a, SV b, SV c, bool textured) {
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.f) return;
    const int minx = std::max(sc.x1, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int maxx = std::min(std::min(sc.x2, 511), static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int miny = std::max(sc.y1, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int maxy = std::min(std::min(sc.y2, 271), static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    if (textured) {
        const float uva = std::fabs((b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v));
        choose_filter(std::sqrt(uva / std::fabs(area)));
    }
    const bool ztest = (G.reg[0x23] & 1u) != 0u;
    const unsigned zfunc = G.reg[0xDE] & 7u;
    const bool zwrite = (G.reg[0xE7] & 0xFFFFu) == 0u;
    const float inv = 1.f / area;
    const bool flat = a.color == b.color && b.color == c.color;
    const SV *vv[3] = {&a, &b, &c};
    for (int y = std::max(miny, 0); y <= maxy; ++y) {
        const float py = y + 0.5f;
        // x extent of the triangle on this scanline
        float xlo = 1e30f, xhi = -1e30f;
        for (int e = 0; e < 3; ++e) {
            const SV &p = *vv[e], &q = *vv[(e + 1) % 3];
            if ((p.y <= py && q.y >= py) || (q.y <= py && p.y >= py)) {
                if (p.y == q.y) { xlo = std::min({xlo, p.x, q.x}); xhi = std::max({xhi, p.x, q.x}); }
                else { const float xi = p.x + (q.x - p.x) * (py - p.y) / (q.y - p.y); xlo = std::min(xlo, xi); xhi = std::max(xhi, xi); }
            }
        }
        if (xlo > xhi) continue;
        const int xs = std::max(minx, static_cast<int>(std::floor(xlo - 0.5f)) - 1), xe = std::min(maxx, static_cast<int>(std::ceil(xhi + 0.5f)) + 1);
        for (int x = std::max(xs, 0); x <= xe; ++x) {
            const float px = x + 0.5f;
            const float w0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inv;
            const float w1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inv;
            const float w2 = 1.f - w0 - w1;
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
            const float z = w0 * a.z + w1 * b.z + w2 * c.z;
            float &d = g_depth[static_cast<std::size_t>(y) * 512u + x];
            if (ztest) {
                bool pass;
                switch (zfunc) {
                case 0: pass = false; break; case 1: pass = true; break; case 2: pass = z == d; break;
                case 3: pass = z != d; break; case 4: pass = z < d; break; case 5: pass = z <= d; break;
                case 6: pass = z > d; break; default: pass = z >= d; break;
                }
                if (!pass) continue;
            }
            const float iw = w0 * a.iw + w1 * b.iw + w2 * c.iw;
            const float k = iw != 0.f ? 1.f / iw : 0.f;
            std::uint32_t col = a.color;
            if (!flat) {
                col = 0;
                for (int ch = 0; ch < 4; ++ch) {
                    const int sh = ch * 8;
                    const float v = w0 * ((a.color >> sh) & 0xFF) + w1 * ((b.color >> sh) & 0xFF) + w2 * ((c.color >> sh) & 0xFF);
                    col |= static_cast<std::uint32_t>(std::clamp(static_cast<int>(v + 0.5f), 0, 255)) << sh;
                }
            }
            const float u = (w0 * a.u * a.iw + w1 * b.u * b.iw + w2 * c.u * c.iw) * k;
            const float v = (w0 * a.v * a.iw + w1 * b.v * b.iw + w2 * c.v * c.iw) * k;
            if (zwrite && ztest) d = z;
            shade(t, Frag{x, y, col, u, v}, false, textured);
        }
    }
}


double g_cap_seconds = 0.0;
bool g_lay_presented = false;
bool g_3d_since_present = false;
unsigned long long g_lists_at_present = 0;
float g_lay_tw = 1.f, g_lay_th = 1.f;

int g_texlog = 0;
std::shared_ptr<const Layer3DTex> capture_texture() {
    if (!G.tex_ptr || !TI.fast) {
        if (g_texlog < 40) {
            ++g_texlog;
            std::fprintf(stderr, "[tex3d] unusable texture: addr=0x%08X fmt=%u %ux%u ptr=%d fast=%d reg_c3=0x%X c5=0x%X\n", static_cast<unsigned>(G.tex_addr),
                         static_cast<unsigned>(TI.fmt), static_cast<unsigned>(TI.w), static_cast<unsigned>(TI.h), G.tex_ptr ? 1 : 0, TI.fast ? 1 : 0,
                         static_cast<unsigned>(G.reg[0xC3]), static_cast<unsigned>(G.reg[0xC5]));
        }
        return nullptr;
    }
    const auto cap_t0 = std::chrono::steady_clock::now();
    struct CapTimer { std::chrono::steady_clock::time_point t0; ~CapTimer() { g_cap_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); } } cap_timer{cap_t0};
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(G.tex_addr); mix(G.reg[0xC3] & 0xF); mix(G.reg[0xB8]); mix(G.reg[0xA8] & 0xFFFFu); mix(G.reg[0xC2] & 1u);
    if (TI.fmt >= 4) {  // only the palette entries this texture can reach
        mix(G.reg[0xC5]);
        const unsigned n = TI.fmt == 4 ? 16u : 256u;
        for (unsigned i = 0; i < n; ++i) mix(G.clut[(((i >> TI.shift) & TI.cmask) | (TI.start << 4)) & 1023u]);
    }
    const std::size_t words = G.tex_len / 8u;
    const std::size_t step = std::max<std::size_t>(1, words / 384u);
    for (std::size_t i = 0; i < words; i += step) { std::uint64_t v; std::memcpy(&v, G.tex_ptr + i * 8u, 8); mix(v); }
    auto it = g_tex_cache.find(h);
    if (it != g_tex_cache.end()) return it->second;
    if (g_tex_cache.size() > 600) g_tex_cache.clear();
    auto t = std::make_shared<Layer3DTex>();
    t->w = static_cast<int>(TI.w); t->h = static_cast<int>(TI.h); t->id = g_next_tex_id++;
    t->px.resize(static_cast<std::size_t>(t->w) * t->h);
    for (int y = 0; y < t->h; ++y) for (int x = 0; x < t->w; ++x) t->px[static_cast<std::size_t>(y) * t->w + x] = fetch_texel(x, y);
    if (mods_active()) {
        ModQuery q;
        q.tex = G.tex_ptr; q.tex_len = G.tex_len; q.bits = TI.bpp; q.w = t->w; q.h = t->h;
        q.bufw = std::max<std::uint32_t>(G.reg[0xA8] & 0xFFFFu, 1u);
        q.palette = TI.fmt >= 4;
        q.clut = G.clut_raw; q.clut_len = G.clut_raw_len;
        q.entry_bytes = (G.reg[0xC5] & 3u) == 3u ? 4u : 2u;
        q.entries = TI.fmt == 4 ? 16u : 256u;
        q.clut_offset = TI.start * 16u * q.entry_bytes;
        q.dim = G.reg[0xB8] & 0xF0Fu;
        q.clutformat = 0xC5000000u | (G.reg[0xC5] & 0xFFFFFFu);
        q.clut_max = G.clut_max;
        t->hi = mods_find(q);
    }
    {
        // Bilinear filtering blends in the colour of fully transparent texels (usually black, giving dark fringes):
        // give those texels the colour of an opaque neighbour. Only the high-resolution copy is touched.
        bool has0 = false, has1 = false;
        for (std::uint32_t px : t->px) { if ((px >> 24) == 0u) has0 = true; else has1 = true; if (has0 && has1) break; }
        if (has0 && has1) {
            const std::vector<std::uint32_t> src = t->px;
            const int w = t->w, h = t->h;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                if ((src[static_cast<std::size_t>(y) * w + x] >> 24) != 0u) continue;
                unsigned r = 0, g2 = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    const std::uint32_t p = src[static_cast<std::size_t>(yy) * w + xx];
                    if ((p >> 24) == 0u) continue;
                    r += p & 0xFFu; g2 += (p >> 8) & 0xFFu; b += (p >> 16) & 0xFFu; ++n;
                }
                if (n) t->px[static_cast<std::size_t>(y) * w + x] = (r / n) | ((g2 / n) << 8) | ((b / n) << 16);
            }
        }
    }
    {
        bool black = true;
        for (std::uint32_t px : t->px) if ((px & 0xFFFFFFu) != 0u) { black = false; break; }
        if (black && g_texlog < 40) {
            ++g_texlog;
            std::fprintf(stderr, "[tex3d] all-black texture: addr=0x%08X fmt=%u %dx%d len=0x%zX clutmode=0x%X c5=0x%X first_clut=0x%08X\n", static_cast<unsigned>(G.tex_addr),
                         static_cast<unsigned>(TI.fmt), t->w, t->h, static_cast<std::size_t>(G.tex_len), static_cast<unsigned>(G.reg[0xC5]),
                         static_cast<unsigned>(G.reg[0xC5]), static_cast<unsigned>(G.clut[0]));
        }
    }
    g_tex_cache[h] = t;
    return t;
}

// ---- full-frame recording: every draw into a frame buffer is kept so the whole frame (2D and 3D) can be replayed at high resolution
struct FrameRec {
    bool active{false}, valid{true}, presented{false};
    const char *why{nullptr};
    unsigned long long born{0};
    std::vector<std::uint32_t> base;
    std::vector<Layer3DVertex> verts;
    std::vector<Layer3DDraw> draws;
    bool depth_greater{true};
    std::shared_ptr<const Layer3D> last;  // what was shown for this recording (the game often presents the same buffer twice)
};
std::map<std::uint32_t, FrameRec> g_recs;
struct TRange { std::uint32_t lo, hi; };
std::vector<TRange> g_target_ranges;
unsigned long long g_frame_no = 0;
unsigned g_rec_replayed = 0, g_rec_fallback = 0;
int g_trace_left = 0;
std::map<std::string, unsigned> g_why_counts;
bool hd2d_enabled() { static const bool e = std::getenv("PERSONA1_NOHD2D") == nullptr; return e; }

void note_target(const Target &t) {
    const std::uint32_t len = t.stride * (t.fmt == 3 ? 4u : 2u) * 272u;
    for (auto &r : g_target_ranges) if (r.lo == t.addr) { r.hi = std::max(r.hi, t.addr + len); return; }
    g_target_ranges.push_back({t.addr, t.addr + len});
}
bool in_target_range(std::uint32_t a, std::size_t len) {
    for (const auto &r : g_target_ranges) if (a < r.hi && a + len > r.lo) return true;
    return false;
}
FrameRec &rec_begin(const Target &t) {
    FrameRec &r = g_recs[t.addr];
    if (r.presented || (r.active && g_frame_no - r.born >= 3)) r = FrameRec{};
    if (!r.active) {
        r.active = true;
        r.born = g_frame_no;
        r.base.assign(480u * 272u, 0xFF000000u);
        const std::uint32_t bpp = t.fmt == 3 ? 4u : 2u;
        if (const std::uint8_t *p = G.rt->memory().raw_pointer(t.addr, static_cast<std::size_t>(t.stride) * bpp * 272u)) {
            for (std::uint32_t y = 0; y < 272; ++y) {
                const std::uint8_t *row = p + static_cast<std::size_t>(y) * t.stride * bpp;
                for (std::uint32_t x = 0; x < 480; ++x) {
                    std::uint32_t c;
                    if (t.fmt == 3) std::memcpy(&c, row + x * 4u, 4);
                    else { std::uint16_t v; std::memcpy(&v, row + x * 2u, 2); c = unpack(v, t.fmt); }
                    r.base[y * 480u + x] = c | 0xFF000000u;
                }
            }
        } else { r.valid = false; r.why = "frame buffer not in RAM"; }
    }
    return r;
}

// The recorded state shared by 3D and 2D draws.
Layer3DDraw make_ld(bool textured, const Scissor &sc) {
    Layer3DDraw ld;
    ld.tex = textured ? capture_texture() : nullptr;
    ld.textured = textured && ld.tex != nullptr;
    g_lay_tw = ld.tex ? static_cast<float>(ld.tex->w) : 1.f; g_lay_th = ld.tex ? static_cast<float>(ld.tex->h) : 1.f;
    ld.ztest = (G.reg[0x23] & 1u) != 0u; ld.zwrite = (G.reg[0xE7] & 0xFFFFu) == 0u; ld.zfunc = static_cast<int>(G.reg[0xDE] & 7u);
    ld.blend = (G.reg[0x21] & 1u) != 0u; ld.bsrc = static_cast<int>(G.reg[0xDF] & 15u);
    ld.bdst = static_cast<int>((G.reg[0xDF] >> 4) & 15u); ld.beq = static_cast<int>((G.reg[0xDF] >> 8) & 7u);
    ld.fix_a = G.reg[0xE0] & 0xFFFFFFu; ld.fix_b = G.reg[0xE1] & 0xFFFFFFu;
    ld.atest = (G.reg[0x22] & 1u) != 0u; ld.atest_func = static_cast<int>(G.reg[0xDB] & 7u); ld.atest_ref = static_cast<int>((G.reg[0xDB] >> 8) & 0xFFu);
    ld.tfunc = static_cast<int>(G.reg[0xC9] & 7u); ld.tex_alpha = (G.reg[0xC9] & 0x100u) != 0u; ld.dbl = (G.reg[0xC9] & 0x10000u) != 0u;
    ld.clamp_u = TI.fast && TI.clamp_u; ld.clamp_v = TI.fast && TI.clamp_v;
    ld.linear = (G.reg[0xC6] & 1u) != 0u || ((G.reg[0xC6] >> 8) & 1u) != 0u;
    ld.sx1 = sc.x1; ld.sy1 = sc.y1; ld.sx2 = sc.x2; ld.sy2 = sc.y2;
    return ld;
}

int cull_override() {  // PERSONA1_CULL: 0 = off, 1 = default, 2 = inverted
    static const int v = [] { const char *e = std::getenv("PERSONA1_CULL"); return e ? std::atoi(e) : 1; }();
    return v;
}

void draw_tri3d(const Target &t, const Scissor &sc, const V3 &p0, const V3 &p1, const V3 &p2, bool textured, bool odd = false) {
    // Face culling (GE CULLFACEENABLE 0x1D, CULL 0x9B). Triangle-strip parity flips the winding of every second triangle.
    if ((G.reg[0x1D] & 1u) && cull_override() != 0 && p0.w > 0.f && p1.w > 0.f && p2.w > 0.f) {
        const float sx = regf(G.reg[0x42]), sy = regf(G.reg[0x43]);
        const float ax = p0.x / p0.w * sx, ay = p0.y / p0.w * sy, bx = p1.x / p1.w * sx, by = p1.y / p1.w * sy, cx = p2.x / p2.w * sx, cy = p2.y / p2.w * sy;
        float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);  // > 0: clockwise on screen (y down)
        if (odd) area = -area;
        const bool cull_cw = ((G.reg[0x9B] & 1u) == 0u) != (cull_override() == 2);
        if (area != 0.f && (cull_cw ? area > 0.f : area < 0.f)) return;
    }
    // Clip against the near plane (z >= -w) and require w > 0.
    V3 in[3] = {p0, p1, p2};
    V3 poly[4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const V3 &c = in[i], &d = in[(i + 1) % 3];
        const float dc = c.z + c.w, dd = d.z + d.w;
        const bool cin = dc >= 0.f && c.w > 0.f, din = dd >= 0.f && d.w > 0.f;
        if (cin && n < 4) poly[n++] = c;
        if (cin != din && n < 4) {
            const float f = dc / (dc - dd);
            V3 m;
            m.x = c.x + (d.x - c.x) * f; m.y = c.y + (d.y - c.y) * f; m.z = c.z + (d.z - c.z) * f; m.w = c.w + (d.w - c.w) * f;
            m.u = c.u + (d.u - c.u) * f; m.v = c.v + (d.v - c.v) * f; m.color = c.color;
            poly[n++] = m;
        }
    }
    if (n < 3) return;
    const float vsx = regf(G.reg[0x42]), vsy = regf(G.reg[0x43]), vsz = regf(G.reg[0x44]);
    const float vpx = regf(G.reg[0x45]), vpy = regf(G.reg[0x46]), vpz = regf(G.reg[0x47]);
    const float ox = (G.reg[0x4C] & 0xFFFFu) / 16.f, oy = (G.reg[0x4D] & 0xFFFFu) / 16.f;
    SV sv[4];
    for (int i = 0; i < n; ++i) {
        const float iw = 1.f / poly[i].w;
        sv[i].x = poly[i].x * iw * vsx + vpx - ox;
        sv[i].y = poly[i].y * iw * vsy + vpy - oy;
        sv[i].z = poly[i].z * iw * vsz + vpz;
        sv[i].iw = iw; sv[i].u = poly[i].u; sv[i].v = poly[i].v; sv[i].color = poly[i].color;
    }
    for (int i = 1; i + 1 < n; ++i) {
        raster3d(t, sc, sv[0], sv[i], sv[i + 1], textured);
        const SV *tri[3] = {&sv[0], &sv[i], &sv[i + 1]};
        for (const SV *q : tri) g_lay.verts.push_back(Layer3DVertex{q->x, q->y, q->z, q->iw, q->u / g_lay_tw, q->v / g_lay_th, q->color});
    }
}

inline float fl(std::uint32_t a) { const std::uint32_t b = a << 8; float f; std::memcpy(&f, &b, 4); return f; }

void do_prim(std::uint32_t arg) {
    const unsigned type = (arg >> 16) & 7u;
    const unsigned count = arg & 0xFFFFu;
    const VertexLayout L = layout_from_vtype(G.reg[0x12]);
    if (!L.through) {
        if (G.skipped_3d++ < 0) std::cerr << "[gpu] 3D primitive (vtype=0x" << std::hex << G.reg[0x12] << std::dec << ")\n";
        static unsigned dumped = 0;
        if ((dumped < 40 || g_log_frame) && dumped < 400 && std::getenv("PERSONA1_GE3D")) {
            ++dumped;
            std::cerr << std::hex << "[ge3d] prim=" << type << " n=" << std::dec << count << std::hex << " vtype=0x" << G.reg[0x12]
                      << " size=" << std::dec << L.size << " offs t/c/n/p=" << L.off_t << "/" << L.off_c << "/" << L.off_n << "/" << L.off_p
                      << " idx=" << L.idx << " vaddr=0x" << std::hex << G.vaddr << " iaddr=0x" << G.iaddr << " tex=" << (G.reg[0x1E] & 1)
                      << " texaddr=0x" << G.reg[0xA0] << " tsize=0x" << G.reg[0xB8] << " tpf=0x" << G.reg[0xC3]
                      << " cull=" << (G.reg[0x1D] & 1) << "/" << G.reg[0x9B] << " ztest=" << (G.reg[0x23] & 1) << " zfunc=" << G.reg[0xDE]
                      << " blend=" << (G.reg[0x21] & 1) << ":" << G.reg[0xDF] << " fog=" << (G.reg[0x1F] & 1) << " light=" << (G.reg[0x17] & 1)
                      << " fbp=0x" << G.reg[0x9C] << " zbp=0x" << G.reg[0x9E] << std::dec << "\n";
            std::cerr << "    viewport scale=(" << fl(G.reg[0x42]) << "," << fl(G.reg[0x43]) << "," << fl(G.reg[0x44]) << ") pos=("
                      << fl(G.reg[0x45]) << "," << fl(G.reg[0x46]) << "," << fl(G.reg[0x47]) << ") offsetxy=(" << (G.reg[0x4C] & 0xFFFF) / 16.0 << ","
                      << (G.reg[0x4D] & 0xFFFF) / 16.0 << ") depthrange=" << std::hex << G.reg[0xD6] << "/" << G.reg[0xD7] << std::dec << "\n";
            std::cerr << "    world:"; for (float f : G.mat_world) std::cerr << " " << f; std::cerr << "\n";
            std::cerr << "    view:"; for (float f : G.mat_view) std::cerr << " " << f; std::cerr << "\n";
            std::cerr << "    proj:"; for (float f : G.mat_proj) std::cerr << " " << f; std::cerr << "\n";
            for (unsigned i = 0; i < std::min(count, 6u); ++i) {
                std::uint32_t index = i;
                if (L.idx == 1) index = rd8(G.iaddr + i); else if (L.idx == 2) index = rd16(G.iaddr + i * 2u);
                const std::uint32_t a = G.vaddr + index * L.size;
                std::cerr << "    v" << i << " idx=" << index << " raw:" << std::hex;
                for (unsigned b = 0; b < L.size && b < 40; ++b) { std::cerr << " " << std::setw(2) << std::setfill('0') << rd8(a + b); }
                std::cerr << std::setfill(' ') << std::dec << "\n";
            }
        }
    }
    if (count == 0 || L.size == 0) return;
    if (L.norm && !L.through) {
        static int lit_logged = 0;
        if (lit_logged < 24 && (L.tex == 0 || lit_logged < 4)) {
            ++lit_logged;
            std::fprintf(stderr, "[lit] vtype=0x%X light_en=%u lights=%u/%u/%u/%u mat_upd=0x%X emis=0x%X amb=0x%X diff=0x%X spec=0x%X mat_alpha=0x%X "
                         "model_amb=0x%X model_amb_alpha=0x%X lmode=0x%X blend=0x%X fixa=0x%X fixb=0x%X ztest=%u zfunc=%u zmask=0x%X cmask=0x%X/0x%X\n",
                         G.reg[0x12], G.reg[0x17] & 1, G.reg[0x18] & 1, G.reg[0x19] & 1, G.reg[0x1A] & 1, G.reg[0x1B] & 1,
                         G.reg[0x53] & 7, G.reg[0x54] & 0xFFFFFF, G.reg[0x55] & 0xFFFFFF, G.reg[0x56] & 0xFFFFFF, G.reg[0x57] & 0xFFFFFF,
                         G.reg[0x58] & 0xFF, G.reg[0x5C] & 0xFFFFFF, G.reg[0x5D] & 0xFF, G.reg[0x5E] & 0xFFFF, G.reg[0xDF] & 0xFFFFFF,
                         G.reg[0xE0] & 0xFFFFFF, G.reg[0xE1] & 0xFFFFFF, G.reg[0x23] & 1, G.reg[0xDE] & 7, G.reg[0xE7] & 0xFFFF,
                         G.reg[0xE8] & 0xFFFFFF, G.reg[0xE9] & 0xFF);
        }
    }
    ++G.draws;
    std::vector<Vtx> vs(count);
    std::vector<std::uint32_t> vaddrs(count);
    bool has_color = false;
    const std::uint32_t vaddr = G.vaddr;
    const std::uint32_t iaddr = G.iaddr;
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t index = i;
        if (L.idx == 1) index = rd8(iaddr + i);
        else if (L.idx == 2) index = rd16(iaddr + i * 2u);
        vaddrs[i] = vaddr + index * L.size;
        vs[i] = fetch(L, vaddrs[i], &has_color);
    }
    // The GPU advances its pointers after each draw.
    if (L.idx == 1) G.iaddr += count; else if (L.idx == 2) G.iaddr += count * 2u;
    else G.vaddr += count * L.size;

    if (!has_color) {
        const std::uint32_t m = G.reg[0x55], ma = G.reg[0x58] & 0xFF;
        const std::uint32_t col = (m & 0xFFFFFFu) | (ma << 24);
        for (auto &v : vs) v.color = col;
    }
    if (const char *lim = std::getenv("PERSONA1_GELOG")) {
        // Debug: log textured draws that use the 256x512 4-bit font sheet.
        static int logged = 0;
        const bool font = (G.reg[0xC3] & 0xF) == 4 && (G.reg[0xB8] & 0xF) == 8 && ((G.reg[0xB8] >> 8) & 0xF) == 9;
        if ((G.reg[0x1E] & 1u) && !(G.reg[0xD3] & 1u) && font && logged < std::atoi(lim)) {
            ++logged;
            std::cerr << std::hex << "[gelog] prim type=" << type << " count=" << std::dec << count << std::hex
                      << " vtype=0x" << G.reg[0x12] << " filter=0x" << G.reg[0xC6] << " func=0x" << G.reg[0xC9]
                      << " env=0x" << G.reg[0xCA] << " blendEn=" << (G.reg[0x21] & 1) << " blend=0x" << G.reg[0xDF]
                      << " atestEn=" << (G.reg[0x22] & 1) << " atest=0x" << G.reg[0xDB] << " cmode=0x" << G.reg[0xC5]
                      << " texmode=0x" << G.reg[0xC2] << " wrap=0x" << G.reg[0xC7] << std::dec << "\n";
            for (unsigned i = 0; i < std::min(count, 4u); ++i)
                std::cerr << "    v" << i << " pos=(" << vs[i].x << "," << vs[i].y << ") uv=(" << vs[i].u << ","
                          << vs[i].v << ") color=0x" << std::hex << vs[i].color << std::dec << "\n";
        }
    }
    if (g_log_frame) {
        ++g_logged_prims;
        std::cerr << "[drawlog] prim=" << type << " n=" << count << " vtype=0x" << std::hex << G.reg[0x12] << " blend=0x" << G.reg[0xDF]
                  << " en=" << (G.reg[0x21] & 1) << " tex=0x" << G.reg[0xA0] << " bw=0x" << G.reg[0xA8] << " size=0x" << G.reg[0xB8]
                  << " tpf=0x" << G.reg[0xC3] << " filt=0x" << G.reg[0xC6] << " wrap=0x" << G.reg[0xC7] << " func=0x" << G.reg[0xC9]
                  << " fbp=0x" << G.reg[0x9C] << std::dec << "\n";
        if ((G.reg[0xC3] & 0xF) >= 4 && (G.reg[0xC3] & 0xF) <= 7) {
            const unsigned start = ((G.reg[0xC5] >> 16) & 0x1F) << 4;
            std::cerr << "    cmode=0x" << std::hex << G.reg[0xC5] << " clut[" << start << "..+16]:";
            for (unsigned i = 0; i < 16; ++i) std::cerr << " " << G.clut[(start + i) & 1023u];
            std::cerr << std::dec << "\n";
        }
        for (unsigned i = 0; i < std::min(count, 8u); ++i)
            std::cerr << "    v" << i << " (" << vs[i].x << "," << vs[i].y << ") uv=(" << vs[i].u << "," << vs[i].v << ") col=0x"
                      << std::hex << vs[i].color << std::dec << "\n";
    }
    const Target t = target();
    Scissor sc{static_cast<int>(G.reg[0xD4] & 0x3FF), static_cast<int>((G.reg[0xD4] >> 10) & 0x3FF),
               static_cast<int>(G.reg[0xD5] & 0x3FF), static_cast<int>((G.reg[0xD5] >> 10) & 0x3FF)};
    const bool clear_mode = (G.reg[0xD3] & 1u) != 0u;
    const bool textured = !clear_mode && (G.reg[0x1E] & 1u) && L.tex;
    if (clear_mode && (G.reg[0xD3] & 0x400u)) std::fill(g_depth.begin(), g_depth.end(), (G.reg[0xDE] & 7u) == 4 || (G.reg[0xDE] & 7u) == 5 ? 65535.f : 0.f);
    log_state_once(L, textured, clear_mode);

    // Resolve raw pointers for this primitive.
    const std::uint32_t bpp_fb = t.fmt == 3 ? 4u : 2u;
    G.fb_ptr = nullptr;
    G.fb_len = 0;
    for (std::uint32_t rows = std::min<std::uint32_t>(static_cast<std::uint32_t>(sc.y2) + 1u, 1024u); rows >= 272u; rows -= 16u) {
        const std::size_t len = static_cast<std::size_t>(rows) * t.stride * bpp_fb;
        if (std::uint8_t *p = G.rt->memory().raw_pointer(t.addr, len)) { G.fb_ptr = p; G.fb_len = len; break; }
        if (rows < 288u) break;
    }
    G.tex_ptr = nullptr;
    G.tex_len = 0;
    if (textured) {
        static const unsigned bpp_of[8] = {16, 16, 16, 32, 4, 8, 16, 32};
        const unsigned tf = G.reg[0xC3] & 0xF;
        if (tf <= 7) {
            const std::uint32_t th = 1u << std::min((G.reg[0xB8] >> 8) & 0xFu, 9u);
            const std::uint32_t stride = std::max<std::uint32_t>(G.reg[0xA8] & 0xFFFFu, 1u);
            const std::uint32_t addr = (G.reg[0xA0] & 0xFFFFFFu) | ((G.reg[0xA8] & 0xFF0000u) << 8);
            const std::uint32_t tw = 1u << std::min(G.reg[0xB8] & 0xFu, 9u);
            // a texture wider than its stride (bufw) still reads (width - stride) texels past the last row; texture-pack hashes cover them
            const std::size_t len = (static_cast<std::size_t>(stride) * th + (tw > stride ? tw - stride : 0u)) * bpp_of[tf] / 8u;
            if (const std::uint8_t *p = G.rt->memory().raw_pointer(addr, len)) {
                G.tex_ptr = p; G.tex_len = len; G.tex_addr = addr;
            }
        }
    }
    setup_texinfo();
    TI.nearest_only = false;
    if (textured && G.tex_ptr) dump_texture_once();
    const bool prof2 = [] { const char *e = std::getenv("PERSONA1_PROF"); return e && e[0] == '2'; }();
    const auto prof_t0 = std::chrono::steady_clock::now();
    const unsigned long long px0 = g_px_count;
    FrameRec *R = nullptr;
    if (hd2d_enabled()) {
        note_target(t);
        R = &rec_begin(t);
        if (R->valid) {
            const char *bad = nullptr;
            // Points and lines are not drawn by the native renderer either, so they are simply left out of the replay.
            if (!clear_mode && ((G.reg[0x28] & 1u) || (G.reg[0x24] & 1u) || (G.reg[0x27] & 1u))) bad = "logic op / stencil / colour test";
            else if (!clear_mode && textured && G.tex_ptr && in_target_range(G.tex_addr, G.tex_len)) bad = "texture read from a frame buffer";
            if (bad) { R->valid = false; R->why = bad; }
        }
    }
    if (!L.through) {
        std::vector<V3> cv(count);
        for (unsigned i = 0; i < count; ++i) cv[i] = transform_vertex(L, vaddrs[i], vs[i].color);
        if (g_lay_presented) { g_lay.verts.clear(); g_lay.draws.clear(); std::fill(g_mask.begin(), g_mask.end(), std::uint8_t{0}); g_lay_presented = false; }
        Layer3DDraw ld = make_ld(textured, sc);
        if (R && R->valid && textured && !ld.textured) { R->valid = false; R->why = "3D texture not capturable"; }
        ld.first = static_cast<std::uint32_t>(g_lay.verts.size());
        if (ld.ztest) g_lay.depth_greater = !(ld.zfunc == 4 || ld.zfunc == 5);
        g_3d_since_present = true;
        g_in3d = true;
        const auto p3_t0 = std::chrono::steady_clock::now();
        const unsigned long long p3_px0 = g_px_count;
        switch (type) {
        case 3: for (unsigned i = 0; i + 2 < count; i += 3) draw_tri3d(t, sc, cv[i], cv[i + 1], cv[i + 2], textured); break;
        case 4: for (unsigned i = 0; i + 2 < count; ++i) draw_tri3d(t, sc, cv[i], cv[i + 1], cv[i + 2], textured, (i & 1u) != 0u); break;
        case 5: for (unsigned i = 1; i + 1 < count; ++i) draw_tri3d(t, sc, cv[0], cv[i], cv[i + 1], textured); break;
        default: break;
        }
        g_in3d = false;
        if (const char *pe = std::getenv("PERSONA1_PROF"); pe && pe[0] == '2') {
            char key[160];
            std::snprintf(key, sizeof key, "3D prim=%u n=%u tex=%d tfmt=%u %ux%u blend=%u ztest=%u atest=%u", type, count, textured ? 1 : 0,
                          G.reg[0xC3] & 15u, 1u << (G.reg[0xB8] & 15u), 1u << ((G.reg[0xB8] >> 8) & 15u), G.reg[0x21] & 1u, G.reg[0x23] & 1u, G.reg[0x22] & 1u);
            DrawStat &st = g_draw_stats[key];
            st.sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - p3_t0).count();
            st.draws++;
            st.px += g_px_count - p3_px0;
        }
        ld.count = static_cast<std::uint32_t>(g_lay.verts.size()) - ld.first;
        if (ld.count && R && R->valid) {
            Layer3DDraw rd = ld;
            rd.first = static_cast<std::uint32_t>(R->verts.size());
            R->verts.insert(R->verts.end(), g_lay.verts.begin() + ld.first, g_lay.verts.begin() + ld.first + ld.count);
            R->draws.push_back(rd);
            if (ld.ztest) R->depth_greater = g_lay.depth_greater;
        }
        if (ld.count) g_lay.draws.push_back(std::move(ld));
        G.fb_ptr = nullptr; G.tex_ptr = nullptr;
        return;
    }
    if (R && R->valid) {
        Layer3DDraw ld = make_ld(textured, sc);
        if (!clear_mode && textured && !ld.textured) { R->valid = false; R->why = "2D texture not capturable"; }
        ld.is2d = true; ld.ztest = false; ld.zwrite = false;
        bool emit = true;
        if (clear_mode) {
            ld.textured = false; ld.tex = nullptr; ld.blend = false; ld.atest = false;
            ld.clear_depth = (G.reg[0xD3] & 0x400u) != 0u;
            emit = (G.reg[0xD3] & 0x100u) != 0u;  // only a colour clear paints anything
        }
        ld.first = static_cast<std::uint32_t>(R->verts.size());
        if (emit) {
            auto V = [&](float x, float y, float u, float v, std::uint32_t col) {
                R->verts.push_back(Layer3DVertex{x, y, 0.f, 1.f, u / g_lay_tw, v / g_lay_th, col});
            };
            // Keep bilinear filtering inside the sprite's own texels (no bleeding from neighbouring atlas entries).
            auto set_rect = [&](std::size_t from, float u0, float v0, float u1, float v1) {
                if (!ld.textured) return;
                const float a0 = std::min(u0, u1) / g_lay_tw, b0 = std::min(v0, v1) / g_lay_th, a1 = std::max(u0, u1) / g_lay_tw, b1 = std::max(v0, v1) / g_lay_th;
                if (a0 < 0.f || b0 < 0.f || a1 > 1.0001f || b1 > 1.0001f) return;  // repeating / scrolling textures
                for (std::size_t k = from; k < R->verts.size(); ++k) { auto &q = R->verts[k]; q.ur0 = a0; q.vr0 = b0; q.ur1 = a1; q.vr1 = b1; }
            };
            auto tri = [&](const Vtx &a, const Vtx &b, const Vtx &c) {
                const std::size_t from = R->verts.size();
                V(a.x, a.y, a.u, a.v, a.color); V(b.x, b.y, b.u, b.v, b.color); V(c.x, c.y, c.u, c.v, c.color);
                set_rect(from, std::min({a.u, b.u, c.u}), std::min({a.v, b.v, c.v}), std::max({a.u, b.u, c.u}), std::max({a.v, b.v, c.v}));
            };
            switch (type) {
            case 3: for (unsigned i = 0; i + 2 < count; i += 3) tri(vs[i], vs[i + 1], vs[i + 2]); break;
            case 4: for (unsigned i = 0; i + 2 < count; ++i) tri(vs[i], vs[i + 1], vs[i + 2]); break;
            case 5: for (unsigned i = 1; i + 1 < count; ++i) tri(vs[0], vs[i], vs[i + 1]); break;
            case 6:
                for (unsigned i = 0; i + 1 < count; i += 2) {
                    const Vtx &a = vs[i], &b = vs[i + 1];
                    const std::uint32_t col = b.color;
                    const std::size_t from = R->verts.size();
                    V(a.x, a.y, a.u, a.v, col); V(b.x, a.y, b.u, a.v, col); V(a.x, b.y, a.u, b.v, col);
                    V(b.x, a.y, b.u, a.v, col); V(b.x, b.y, b.u, b.v, col); V(a.x, b.y, a.u, b.v, col);
                    set_rect(from, a.u, a.v, b.u, b.v);
                }
                break;
            default: break;
            }
        }
        ld.count = static_cast<std::uint32_t>(R->verts.size()) - ld.first;
        if (ld.count || ld.clear_depth) R->draws.push_back(ld);
    }
    switch (type) {
    case 3: for (unsigned i = 0; i + 2 < count; i += 3) draw_triangle(t, sc, vs[i], vs[i + 1], vs[i + 2], clear_mode, textured); break;
    case 4:
        if (count == 4 && quad_as_sprite(t, sc, vs.data(), clear_mode, textured)) break;
        for (unsigned i = 0; i + 2 < count; ++i) draw_triangle(t, sc, vs[i], vs[i + 1], vs[i + 2], clear_mode, textured); break;
    case 5: for (unsigned i = 1; i + 1 < count; ++i) draw_triangle(t, sc, vs[0], vs[i], vs[i + 1], clear_mode, textured); break;
    case 6: for (unsigned i = 0; i + 1 < count; i += 2) draw_sprite(t, sc, vs[i], vs[i + 1], clear_mode, textured); break;
    default: break;  // points and lines are not used by the 2D screens seen so far
    }
    if (prof2) {
        char key[160];
        std::snprintf(key, sizeof key, "prim=%u n=%u vtype=%06X tex=%d blend=%03X en=%u filt=%03X tfmt=%u func=%X",
                      type, count, G.reg[0x12], textured ? 1 : 0, G.reg[0xDF] & 0xFFF, G.reg[0x21] & 1u,
                      G.reg[0xC6] & 0xFFF, G.reg[0xC3] & 15u, G.reg[0xC9] & 0x1FFFFu);
        DrawStat &st = g_draw_stats[key];
        st.sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - prof_t0).count();
        st.draws++;
        st.px += g_px_count - px0;
    }
    G.fb_ptr = nullptr;
    G.tex_ptr = nullptr;
}

// ---- block transfer (TRXKICK): copies a rectangle of pixels, e.g. RAM -> VRAM -------
void do_transfer(std::uint32_t kick) {
    auto addr_of = [](std::uint32_t bp, std::uint32_t bw) {
        std::uint32_t a = (bp & 0xFFFFFFu) | ((bw & 0xFF0000u) << 8);
        if ((a >> 24) == 0u) a |= 0x04000000u;
        return a;
    };
    const std::uint32_t src = addr_of(G.reg[0xB2], G.reg[0xB3]), dst = addr_of(G.reg[0xB4], G.reg[0xB5]);
    const std::uint32_t sstride = G.reg[0xB3] & 0x7FFu, dstride = G.reg[0xB5] & 0x7FFu;
    const std::uint32_t sx = G.reg[0xEB] & 0x3FFu, sy = (G.reg[0xEB] >> 10) & 0x3FFu;
    const std::uint32_t dx = G.reg[0xEC] & 0x3FFu, dy = (G.reg[0xEC] >> 10) & 0x3FFu;
    const std::uint32_t w = (G.reg[0xEE] & 0x3FFu) + 1u, h = ((G.reg[0xEE] >> 10) & 0x3FFu) + 1u;
    const std::uint32_t bpp = (kick & 1u) ? 4u : 2u;
    for (auto &kv : g_recs) {
        const std::uint32_t lo = kv.first, hi = lo + 512u * 272u * 4u;
        if (dst < hi && dst + dstride * h * bpp > lo && kv.second.valid) { kv.second.valid = false; kv.second.why = "block transfer into a frame buffer"; }
    }
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t sa = src + ((sy + y) * sstride + sx) * bpp;
        const std::uint32_t da = dst + ((dy + y) * dstride + dx) * bpp;
        if (bpp == 4) for (std::uint32_t x = 0; x < w; ++x) wr32(da + x * 4u, rd32(sa + x * 4u));
        else for (std::uint32_t x = 0; x < w; ++x) wr16(da + x * 2u, rd16(sa + x * 2u));
    }
}

// ---- command interpreter -------------------------------------------------------------
inline std::uint32_t relative(std::uint32_t arg) { return ((G.base | (arg & 0xFFFFFFu)) + G.offset) & 0x0FFFFFFFu; }

void run(int id, ListState &l) {
    unsigned guard = 0;
    while (!l.ended && (l.pc & 0x0FFFFFFFu) != (l.stall & 0x0FFFFFFFu) && guard++ < 2000000u) {
        const std::uint32_t w = rd32(l.pc);
        const unsigned cmd = w >> 24;
        const std::uint32_t arg = w & 0xFFFFFFu;
        l.pc += 4;
        try {
        switch (cmd) {
        case 0x01: G.vaddr = relative(arg); break;
        case 0x02: G.iaddr = relative(arg); break;
        case 0x04: G.reg[0x04] = arg; do_prim(arg); break;
        case 0x08: l.pc = relative(arg) & ~3u; break;
        case 0x0A: if (l.sp < 16) l.stack[l.sp++] = l.pc; l.pc = relative(arg) & ~3u; break;
        case 0x0B: if (l.sp) l.pc = l.stack[--l.sp]; else l.ended = true; break;
        case 0x0C: l.ended = true; break;
        case 0x10: G.base = (arg << 8) & 0xFF000000u; break;
        case 0x13: G.offset = arg << 8; break;
        case 0x3A: G.mat_idx[0] = 0; break;
        case 0x3C: G.mat_idx[1] = 0; break;
        case 0x3E: G.mat_idx[2] = 0; break;
        case 0x3B: case 0x3D: case 0x3F: {
            const unsigned k = (cmd - 0x3B) / 2u;
            const std::uint32_t bits = arg << 8;
            float f; std::memcpy(&f, &bits, 4);
            if (k == 0 && G.mat_idx[0] < 12) G.mat_world[G.mat_idx[0]++] = f;
            else if (k == 1 && G.mat_idx[1] < 12) G.mat_view[G.mat_idx[1]++] = f;
            else if (k == 2 && G.mat_idx[2] < 16) G.mat_proj[G.mat_idx[2]++] = f;
            break;
        }
        case 0xC4: G.reg[0xC4] = arg; load_clut(arg); break;
        case 0xEA: G.reg[0xEA] = arg; do_transfer(arg); break;
        default: G.reg[cmd] = arg; break;
        }
        } catch (const std::exception &e) {
            static unsigned reported = 0;
            if (reported++ < 8) {
                std::cerr << "[gpu] error in list " << id << " cmd=0x" << std::hex << cmd << " arg=0x" << arg
                          << " list_pc=0x" << (l.pc - 4) << "\n  vaddr=0x" << G.vaddr << " iaddr=0x" << G.iaddr
                          << " base=0x" << G.base << " vtype=0x" << G.reg[0x12] << " fbp=0x" << G.reg[0x9C]
                          << " fbw=0x" << G.reg[0x9D] << "\n  texaddr=0x" << G.reg[0xA0] << " texbufw=0x"
                          << G.reg[0xA8] << " tpf=0x" << G.reg[0xC3] << " tsize=0x" << G.reg[0xB8]
                          << " clutaddr=0x" << G.reg[0xB0] << " clutupper=0x" << G.reg[0xB1] << " cmode=0x"
                          << G.reg[0xC5] << std::dec << "\n  " << e.what() << "\n";
            }
        }
    }
    (void)id;
}

void ensure_init(Runtime &rt) {
    if (G.rt == nullptr) {
        G.rt = &rt;
        G.trace = std::getenv("PERSONA1_TRACE") != nullptr;
        G.reg[0xD4] = 0; G.reg[0xD5] = (271u << 10) | 479u;  // default scissor: whole screen
    }
}

} // namespace

namespace prof {
using clk = std::chrono::steady_clock;
double ge_seconds = 0.0;
unsigned frames = 0;
clk::time_point last = clk::now();
std::uint64_t last_draws = 0;
double last_idle = 0.0;
unsigned long long last_vb = 0, last_sf = 0, last_ls = 0;
bool enabled() { static const bool e = std::getenv("PERSONA1_PROF") != nullptr; return e; }
struct Timer {
    clk::time_point t0 = clk::now();
    ~Timer() { ge_seconds += std::chrono::duration<double>(clk::now() - t0).count(); }
};
} // namespace prof

void ge_list_start(Runtime &rt, std::uint32_t list_addr, std::uint32_t stall_addr, int list_id) {
    ensure_init(rt);
    ++g_lists_started;
    prof::Timer pt;
    ListState &l = G.lists[list_id];
    l = ListState{};
    l.pc = list_addr & ~3u;
    l.stall = stall_addr == 0u ? 0xFFFFFFFFu : (stall_addr & ~3u);
    run(list_id, l);
}

void ge_list_update(Runtime &rt, int list_id, std::uint32_t stall_addr) {
    ensure_init(rt);
    prof::Timer pt;
    const auto it = G.lists.find(list_id);
    if (it == G.lists.end() || it->second.ended) return;
    it->second.stall = stall_addr == 0u ? 0xFFFFFFFFu : (stall_addr & ~3u);
    run(list_id, it->second);
}

void gpu_present(Runtime &rt, std::uint32_t addr, std::uint32_t stride, std::uint32_t pixel_format) {
    ensure_init(rt);
    if (addr == 0u || stride == 0u) return;
    if (g_log_frame && g_logged_prims > 0) { std::cerr << "[drawlog] ---- end of frame ----\n"; g_log_frame = false; }
    if (debug_key_f10()) { g_log_frame = true; g_logged_prims = 0; g_trace_left = 16; std::cerr << "[drawlog] ---- logging next frame ----\n"; }
    if ((addr >> 24) == 0u) addr |= 0x04000000u;
    static std::vector<std::uint32_t> frame(480 * 272);
    const unsigned fmt = pixel_format & 3u;
    const std::uint32_t bpp = fmt == 3 ? 4u : 2u;
    const std::uint8_t *base = G.rt->memory().raw_pointer(addr, stride * bpp * 272u);
    if (base != nullptr) {
        for (std::uint32_t y = 0; y < 272; ++y) {
            const std::uint8_t *row = base + static_cast<std::size_t>(y) * stride * bpp;
            for (std::uint32_t x = 0; x < 480; ++x) {
                std::uint32_t c;
                if (fmt == 3) { std::uint32_t v; std::memcpy(&v, row + x * 4u, 4); c = v; }
                else { std::uint16_t v; std::memcpy(&v, row + x * 2u, 2); c = unpack(v, fmt); }
                frame[y * 480 + x] = c | 0xFF000000u;
            }
        }
    }
    if (prof::enabled() && ++prof::frames >= 60) {
        const auto now = prof::clk::now();
        const double dt = std::chrono::duration<double>(now - prof::last).count();
        std::cerr << "[prof] " << prof::frames / dt << " fps, GE " << prof::ge_seconds * 1000.0 / prof::frames
                  << " ms/frame, idle " << (g_idle_seconds - prof::last_idle) * 1000.0 / prof::frames
                  << " ms/frame, draws/frame " << (G.draws - prof::last_draws) / prof::frames
                  << ", 3D capture " << g_cap_seconds * 1000.0 / prof::frames << " ms/frame\n";
        g_cap_seconds = 0.0;
        if (std::getenv("PERSONA1_PROF")[0] == '2') {
            std::cerr << "[prof]   rects=" << g_c_rect << " (fast " << g_c_rect_fast << ", linear " << g_c_rect_lin << ") rect px=" << g_c_rect_px
                      << " | triangles=" << g_c_tri << " tri px=" << g_c_tri_px << "\n";
            g_c_rect = g_c_rect_fast = g_c_rect_px = g_c_rect_lin = g_c_tri = g_c_tri_px = 0;
        }
        prof::last_idle = g_idle_seconds;
        std::cerr << "[prof]   per second: vblank waits " << (g_prof_vblank - prof::last_vb) / dt << ", SetFrameBuf calls "
                  << (g_prof_setfb - prof::last_sf) / dt << ", GE lists " << (g_lists_started - prof::last_ls) / dt << "\n";
        prof::last_vb = g_prof_vblank; prof::last_sf = g_prof_setfb; prof::last_ls = g_lists_started;
        if (std::getenv("PERSONA1_PROF")[0] == '2') {
            std::vector<std::pair<double, std::string>> top;
            for (auto &kv : g_draw_stats)
                top.push_back({kv.second.sec, kv.first + " | draws=" + std::to_string(kv.second.draws) +
                               " px=" + std::to_string(kv.second.px) + " ms=" + std::to_string(kv.second.sec * 1000.0)});
            std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.first > b.first; });
            for (size_t i = 0; i < std::min<size_t>(top.size(), 4); ++i) std::cerr << "   [top] " << top[i].second << "\n";
            g_draw_stats.clear();
        }
        prof::frames = 0; prof::ge_seconds = 0.0; prof::last = now; prof::last_draws = G.draws;
    }
    // New GE work since the last present that drew no 3D means the scene changed (menu, 2D room): drop the old 3D layer.
    static unsigned no3d_streak = 0;
    if (g_3d_since_present) no3d_streak = 0;
    else if (g_lists_started != g_lists_at_present) ++no3d_streak;
    if (no3d_streak >= 3) {  // a few frames of new 2D-only work in a row
        g_lay.verts.clear(); g_lay.draws.clear(); g_lay_presented = false;
    }
    g_lists_at_present = g_lists_started;
    g_3d_since_present = false;
    if (g_trace_left > 0) {
        --g_trace_left;
        std::uint64_t fh = 1469598103934665603ull;
        for (std::size_t i = 0; i < frame.size(); i += 7) { fh ^= frame[i]; fh *= 1099511628211ull; }
        const auto it = g_recs.find(addr);
        std::cerr << "[hd2d-trace] present buffer=0x" << std::hex << addr << " native-frame-hash=" << fh << std::dec;
        if (it == g_recs.end()) std::cerr << " (no recording)";
        else std::cerr << " active=" << it->second.active << " presented=" << it->second.presented << " valid=" << it->second.valid
                       << " draws=" << it->second.draws.size() << " verts=" << it->second.verts.size()
                       << " why=" << (it->second.why ? it->second.why : "-") << " born=" << it->second.born << " now=" << g_frame_no;
        std::cerr << " legacy3d_draws=" << g_lay.draws.size() << "\n";
    }
    std::shared_ptr<const Layer3D> snap;
    if (const auto it = g_recs.find(addr); it != g_recs.end() && it->second.active && !it->second.presented) {
        FrameRec &r = it->second;
        r.presented = true;
        if (r.valid && !r.draws.empty() && display_hd2d_on()) {
            auto L = std::make_shared<Layer3D>();
            L->verts = r.verts; L->draws = r.draws; L->depth_greater = r.depth_greater;
            L->full = true; L->base = r.base;
            L->mask.assign(512u * 272u, 0);
            snap = L;
            r.last = L;
            ++g_rec_replayed;
            if (!g_lay.draws.empty()) g_lay_presented = true;
        } else {
            ++g_rec_fallback;
            static int shown = 0;
            if (!r.valid) ++g_why_counts[r.why ? r.why : "?"];
            if (!r.valid && shown < 30) { ++shown; std::cerr << "[hd2d] frame not replayed at high resolution: " << (r.why ? r.why : "?") << " (buffer 0x" << std::hex << addr << std::dec << ")\n"; }
        }
        static unsigned cnt = 0;
        if (++cnt % 600 == 0 && cnt < 12000) {
            std::cerr << "[hd2d] frames replayed=" << g_rec_replayed << " fallback=" << g_rec_fallback;
            for (const auto &kv : g_why_counts) std::cerr << " | " << kv.first << ": " << kv.second;
            std::cerr << "\n";
        }
    }
    if (!snap) {  // the same buffer presented again without new drawing: show the same high-resolution frame again
        const auto it = g_recs.find(addr);
        if (it != g_recs.end() && it->second.active && it->second.presented && it->second.last && display_hd2d_on()) snap = it->second.last;
    }
    if (!snap && !g_lay.draws.empty()) {
        auto L = std::make_shared<Layer3D>(g_lay);
        L->mask = g_mask;
        snap = L;
        g_lay_presented = true;
        static unsigned n = 0, logged = 0;
        if (++n % 60 == 0 && logged < 25) {
            ++logged;
            std::size_t zeros = 0;
            for (unsigned y = 0; y < 272; ++y) for (unsigned x = 0; x < 480; ++x) zeros += g_mask[y * 512u + x] == 0;
            std::cerr << "[layer3d] draws=" << g_lay.draws.size() << " verts=" << g_lay.verts.size() << " 3d-visible px=" << zeros
                      << "/130560 tex=" << g_tex_cache.size() << " depth_greater=" << g_lay.depth_greater << "\n";
        }
    } else {
        static unsigned m = 0;
        if (++m % 300 == 0 && m < 3000) std::cerr << "[layer3d] no 3D layer this frame\n";
    }
    ++g_frame_no;
    display_present(frame.data(), snap);
}

} // namespace persona1
