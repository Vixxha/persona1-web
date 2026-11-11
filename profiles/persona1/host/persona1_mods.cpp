// Texture-replacement packs in the PPSSPP format (textures.ini + PNG files).
//
// A pack key is 24 hex digits: 16 digits of "cache key" (the high half is the texture address, normally zero/ignored;
// the low half is a hash of the palette for palette textures) followed by 8 digits of texture-content hash.
// A zero content hash is a wildcard (any texture content with that palette).
// The hash is xxh64 with seed 0xBACD7814, truncated to 32 bits, over the texture memory. PPSSPP has changed small details
// between versions, so several variants are tried; the first one that matches is remembered (see PERSONA1_MODSLOG=1).
#include "persona1_mods.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#endif

namespace persona1 {
namespace {

// ---- xxHash (32 and 64 bit, one-shot) ---------------------------------------------------
inline std::uint32_t rotl32(std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
inline std::uint64_t rotl64(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline std::uint32_t rd32(const std::uint8_t *p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
inline std::uint64_t rd64(const std::uint8_t *p) { std::uint64_t v; std::memcpy(&v, p, 8); return v; }

std::uint32_t xxh32(const std::uint8_t *p, std::size_t len, std::uint32_t seed) {
    const std::uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    const std::uint8_t *end = p + len;
    std::uint32_t h;
    auto round = [&](std::uint32_t acc, std::uint32_t in) { acc += in * P2; acc = rotl32(acc, 13); return acc * P1; };
    if (len >= 16) {
        std::uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const std::uint8_t *lim = end - 16;
        do {
            v1 = round(v1, rd32(p)); p += 4;
            v2 = round(v2, rd32(p)); p += 4;
            v3 = round(v3, rd32(p)); p += 4;
            v4 = round(v4, rd32(p)); p += 4;
        } while (p <= lim);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P5;
    }
    h += static_cast<std::uint32_t>(len);
    while (p + 4 <= end) { h += rd32(p) * P3; h = rotl32(h, 17) * P4; p += 4; }
    while (p < end) { h += (*p) * P5; h = rotl32(h, 11) * P1; ++p; }
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

std::uint64_t xxh64(const std::uint8_t *p, std::size_t len, std::uint64_t seed) {
    const std::uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                        P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
    const std::uint8_t *end = p + len;
    std::uint64_t h;
    auto round = [&](std::uint64_t acc, std::uint64_t in) { acc += in * P2; acc = rotl64(acc, 31); return acc * P1; };
    auto merge = [&](std::uint64_t acc, std::uint64_t v) { v = round(0, v); acc ^= v; return acc * P1 + P4; };
    if (len >= 32) {
        std::uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const std::uint8_t *lim = end - 32;
        do {
            v1 = round(v1, rd64(p)); p += 8;
            v2 = round(v2, rd64(p)); p += 8;
            v3 = round(v3, rd64(p)); p += 8;
            v4 = round(v4, rd64(p)); p += 8;
        } while (p <= lim);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += len;
    while (p + 8 <= end) { h ^= round(0, rd64(p)); h = rotl64(h, 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { h ^= static_cast<std::uint64_t>(rd32(p)) * P1; h = rotl64(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (*p) * P5; h = rotl64(h, 11) * P1; ++p; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}


// XXH3 64-bit, seed 0, default secret (used for the palette hash by newer PPSSPP versions)
const std::uint8_t kSecret[192] = {
    0xb8,0xfe,0x6c,0x39,0x23,0xa4,0x4b,0xbe,0x7c,0x01,0x81,0x2c,0xf7,0x21,0xad,0x1c,0xde,0xd4,0x6d,0xe9,0x83,0x90,0x97,0xdb,
    0x72,0x40,0xa4,0xa4,0xb7,0xb3,0x67,0x1f,0xcb,0x79,0xe6,0x4e,0xcc,0xc0,0xe5,0x78,0x82,0x5a,0xd0,0x7d,0xcc,0xff,0x72,0x21,
    0xb8,0x08,0x46,0x74,0xf7,0x43,0x24,0x8e,0xe0,0x35,0x90,0xe6,0x81,0x3a,0x26,0x4c,0x3c,0x28,0x52,0xbb,0x91,0xc3,0x00,0xcb,
    0x88,0xd0,0x65,0x8b,0x1b,0x53,0x2e,0xa3,0x71,0x64,0x48,0x97,0xa2,0x0d,0xf9,0x4e,0x38,0x19,0xef,0x46,0xa9,0xde,0xac,0xd8,
    0xa8,0xfa,0x76,0x3f,0xe3,0x9c,0x34,0x3f,0xf9,0xdc,0xbb,0xc7,0xc7,0x0b,0x4f,0x1d,0x8a,0x51,0xe0,0x4b,0xcd,0xb4,0x59,0x31,
    0xc8,0x9f,0x7e,0xc9,0xd9,0x78,0x73,0x64,0xea,0xc5,0xac,0x83,0x34,0xd3,0xeb,0xc3,0xc5,0x81,0xa0,0xff,0xfa,0x13,0x63,0xeb,
    0x17,0x0d,0xdd,0x51,0xb7,0xf0,0xda,0x49,0xd3,0x16,0x55,0x26,0x29,0xd4,0x68,0x9e,0x2b,0x16,0xbe,0x58,0x7d,0x47,0xa1,0xfc,
    0x8f,0xf8,0xb8,0xd1,0x7a,0xd0,0x31,0xce,0x45,0xcb,0x3a,0x8f,0x95,0x16,0x04,0x28,0xaf,0xd7,0xfb,0xca,0xbb,0x4b,0x40,0x7e};

inline std::uint64_t mulfold(std::uint64_t a, std::uint64_t b) {
#if defined(_MSC_VER) && defined(_M_X64)
    std::uint64_t hi; const std::uint64_t lo = _umul128(a, b, &hi); return lo ^ hi;
#else
    const unsigned __int128 m = static_cast<unsigned __int128>(a) * b; return static_cast<std::uint64_t>(m) ^ static_cast<std::uint64_t>(m >> 64);
#endif
}
inline std::uint64_t bswap64(std::uint64_t x) {
    x = ((x & 0x00000000FFFFFFFFull) << 32) | (x >> 32);
    x = ((x & 0x0000FFFF0000FFFFull) << 16) | ((x >> 16) & 0x0000FFFF0000FFFFull);
    return ((x & 0x00FF00FF00FF00FFull) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFull);
}
std::uint64_t xxh3_64(const std::uint8_t *in, std::size_t len) {
    const std::uint64_t P32_1 = 0x9E3779B1ull, P32_2 = 0x85EBCA77ull, P32_3 = 0xC2B2AE3Dull;
    const std::uint64_t P64_1 = 0x9E3779B185EBCA87ull, P64_2 = 0xC2B2AE3D27D4EB4Full, P64_3 = 0x165667B19E3779F9ull, P64_4 = 0x85EBCA77C2B2AE63ull, P64_5 = 0x27D4EB2F165667C5ull;
    const std::uint64_t MX1 = 0x165667919E3779F9ull, MX2 = 0x9FB21C651E98DF25ull;
    const std::uint8_t *sec = kSecret;
    auto av64 = [&](std::uint64_t h) { h ^= h >> 33; h *= P64_2; h ^= h >> 29; h *= P64_3; h ^= h >> 32; return h; };
    auto av3 = [&](std::uint64_t h) { h ^= h >> 37; h *= MX1; h ^= h >> 32; return h; };
    auto mix16 = [&](const std::uint8_t *p, const std::uint8_t *k) { return mulfold(rd64(p) ^ rd64(k), rd64(p + 8) ^ rd64(k + 8)); };
    if (len == 0) return av64(rd64(sec + 56) ^ rd64(sec + 64));
    if (len <= 3) {
        const std::uint32_t c1 = in[0], c2 = in[len >> 1], c3 = in[len - 1];
        const std::uint32_t comb = (c1 << 16) | (c2 << 24) | c3 | (static_cast<std::uint32_t>(len) << 8);
        const std::uint64_t flip = static_cast<std::uint64_t>(rd32(sec) ^ rd32(sec + 4));
        return av64(static_cast<std::uint64_t>(comb) ^ flip);
    }
    if (len <= 8) {
        const std::uint64_t in1 = rd32(in), in2 = rd32(in + len - 4);
        const std::uint64_t flip = rd64(sec + 8) ^ rd64(sec + 16);
        std::uint64_t h = (in2 + (in1 << 32)) ^ flip;
        h ^= rotl64(h, 49) ^ rotl64(h, 24); h *= MX2; h ^= (h >> 35) + len; h *= MX2; return h ^ (h >> 28);
    }
    if (len <= 16) {
        const std::uint64_t f1 = rd64(sec + 24) ^ rd64(sec + 32), f2 = rd64(sec + 40) ^ rd64(sec + 48);
        const std::uint64_t lo = rd64(in) ^ f1, hi = rd64(in + len - 8) ^ f2;
        return av3(len + bswap64(lo) + hi + mulfold(lo, hi));
    }
    if (len <= 128) {
        std::uint64_t acc = len * P64_1;
        if (len > 32) {
            if (len > 64) {
                if (len > 96) { acc += mix16(in + 48, sec + 96); acc += mix16(in + len - 64, sec + 112); }
                acc += mix16(in + 32, sec + 64); acc += mix16(in + len - 48, sec + 80);
            }
            acc += mix16(in + 16, sec + 32); acc += mix16(in + len - 32, sec + 48);
        }
        acc += mix16(in, sec); acc += mix16(in + len - 16, sec + 16);
        return av3(acc);
    }
    if (len <= 240) {
        std::uint64_t acc = len * P64_1;
        const std::size_t rounds = len / 16;
        for (std::size_t i = 0; i < 8; ++i) acc += mix16(in + 16 * i, sec + 16 * i);
        acc = av3(acc);
        for (std::size_t i = 8; i < rounds; ++i) acc += mix16(in + 16 * i, sec + 16 * (i - 8) + 3);
        acc += mix16(in + len - 16, sec + 136 - 17);
        return av3(acc);
    }
    std::uint64_t acc[8] = {P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1};
    auto stripe = [&](const std::uint8_t *p, const std::uint8_t *k) {
        for (int i = 0; i < 8; ++i) {
            const std::uint64_t d = rd64(p + 8 * i), dk = d ^ rd64(k + 8 * i);
            acc[i ^ 1] += d;
            acc[i] += (dk & 0xFFFFFFFFull) * (dk >> 32);
        }
    };
    const std::size_t stripes_per_block = (192 - 64) / 8, block_len = 64 * stripes_per_block;
    const std::size_t nb_blocks = (len - 1) / block_len;
    for (std::size_t n = 0; n < nb_blocks; ++n) {
        for (std::size_t s2 = 0; s2 < stripes_per_block; ++s2) stripe(in + n * block_len + s2 * 64, sec + s2 * 8);
        for (int i = 0; i < 8; ++i) { std::uint64_t a = acc[i]; a ^= a >> 47; a ^= rd64(sec + 192 - 64 + 8 * i); a *= P32_1; acc[i] = a; }
    }
    const std::size_t nb_stripes = ((len - 1) - block_len * nb_blocks) / 64;
    for (std::size_t s2 = 0; s2 < nb_stripes; ++s2) stripe(in + nb_blocks * block_len + s2 * 64, sec + s2 * 8);
    stripe(in + len - 64, sec + 192 - 64 - 7);
    std::uint64_t res = len * P64_1;
    for (int i = 0; i < 4; ++i) res += mulfold(acc[2 * i] ^ rd64(sec + 11 + 16 * i), acc[2 * i + 1] ^ rd64(sec + 11 + 16 * i + 8));
    return av3(res);
}

constexpr std::uint64_t kTexSeed = 0xBACD7814ull;
constexpr std::uint32_t kClutSeed = 0xC0108888u;

// ---- PNG decoding (Windows Imaging Component) ---------------------------------------------
bool decode_png(const std::filesystem::path &path, ModTexture &out) {
#ifdef _WIN32
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)ci;  // RPC_E_CHANGED_MODE just means this thread already has COM
    IWICImagingFactory *factory = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void **>(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            out.w = static_cast<int>(w); out.h = static_cast<int>(h);
            out.px.resize(static_cast<std::size_t>(w) * h);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size() * 4), reinterpret_cast<BYTE *>(out.px.data())));
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (factory) factory->Release();
    return ok;
#else
    (void)path; (void)out;
    return false;
#endif
}

// ---- pack index ------------------------------------------------------------------------
struct Entry {
    std::filesystem::path file;
    std::string name;  // for logging
    std::uint32_t clut{};
    std::shared_ptr<const ModTexture> tex;
    bool failed{false};
};

struct State {
    bool active{false};
    bool log{false};
    std::mutex mu;
    std::unordered_map<std::uint64_t, int> index;                 // (clut << 32 | texhash) -> entry
    std::unordered_map<std::uint32_t, std::vector<int>> by_tex;   // texhash -> entries (for the fallback)
    std::vector<Entry> entries;
    int win_clut{-1};
    bool tex_variant_seen[64]{};
    std::unordered_set<std::uint32_t> clut_keys;  // palette halves present in the pack
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> by_clut;  // palette half -> content hashes in the pack
    unsigned raw_dumped{0};
    bool loose_warned{false};
    unsigned loose_logged{0};
    unsigned logged_hits{0}, logged_miss{0}, hits{0}, misses{0};
    std::size_t decoded_bytes{0};
    std::map<std::string, int> seen;  // textures already logged (by keys)
} S;

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool is_hex(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

void add_key(std::uint64_t cachekey, std::uint32_t hash, const std::filesystem::path &file, const std::string &name) {
    const std::uint32_t clut = static_cast<std::uint32_t>(cachekey & 0xFFFFFFFFull);
    const std::uint64_t key = (static_cast<std::uint64_t>(clut) << 32) | hash;
    if (S.index.count(key)) return;
    Entry e; e.file = file; e.name = name; e.clut = clut;
    S.entries.push_back(std::move(e));
    const int id = static_cast<int>(S.entries.size() - 1);
    S.index[key] = id;
    S.clut_keys.insert(clut);
    S.by_clut[clut].push_back(hash);
    if (hash) S.by_tex[hash].push_back(id);
}

void load_pack(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path ini = root / "textures.ini";
    std::size_t before = S.index.size();
    if (fs::exists(ini, ec)) {
        std::ifstream f(ini);
        std::string line, section;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line[0] == '[') { section = line; continue; }
            if (section != "[hashes]") continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (k.size() != 24 || !is_hex(k) || v.empty() || v == "ignore") continue;
            const std::uint64_t cachekey = std::strtoull(k.substr(0, 16).c_str(), nullptr, 16);
            const std::uint32_t hash = static_cast<std::uint32_t>(std::strtoul(k.substr(16).c_str(), nullptr, 16));
            add_key(cachekey, hash, root / fs::path(v), v);
        }
    }
    // Files named by hash: "<24 hex>.png" or "tex1_<w>x<h>_<cachekey>_<hash>_<fmt>.png", anywhere in the pack.
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".png") continue;
        const std::string stem = p.stem().string();
        const std::string rel = fs::relative(p, root, ec).generic_string();
        if (stem.size() == 24 && is_hex(stem)) {
            add_key(std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16), static_cast<std::uint32_t>(std::strtoul(stem.substr(16).c_str(), nullptr, 16)), p, rel);
        } else if (stem.rfind("tex1_", 0) == 0) {
            unsigned w = 0, h = 0, fmt = 0; unsigned long long ck = 0; unsigned hash = 0;
            if (std::sscanf(stem.c_str(), "tex1_%ux%u_%llx_%x_%u", &w, &h, &ck, &hash, &fmt) == 5) add_key(ck, hash, p, rel);
        }
    }
    std::fprintf(stderr, "[mods] %s: %zu texture keys\n", root.string().c_str(), S.index.size() - before);
}

// ---- hash variants ---------------------------------------------------------------------

// Content hash variants. PPSSPP hashes half of the texture's bytes ("reduce hash"), and for a texture 512 rows tall it only covers the
// rows the game has actually drawn from (maxSeenV), which this code cannot know, so a list of common heights is tried.
const int kRows[] = {-1 /* full height */, 272, 256, 240, 224, 192, 176, 160, 144, 128, 112, 96, 80, 64, 48, 32, 16};
constexpr int kNumRows = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
struct Variants { std::uint32_t v[64]{}; int rows[64]{}; int n{0}; };

Variants tex_hashes(const ModQuery &q) {
    Variants r;
    const std::size_t bits = q.bits;
    std::vector<std::uint8_t> buf;
    auto span = [&](int H, std::size_t &size) -> bool {
        // contiguous when the stride does not exceed the width, else one row at a time
        if (q.bufw <= static_cast<unsigned>(q.w)) {
            const std::size_t total = static_cast<std::size_t>(q.bufw) * H + (q.w - q.bufw);
            size = bits * total / 8;
            if (size > q.tex_len) return false;
            buf.assign(q.tex, q.tex + size);
            return true;
        }
        const std::size_t line = bits * q.w / 8, stride = bits * q.bufw / 8;
        if (stride * (H - 1) + line > q.tex_len) return false;
        buf.clear();
        for (int y = 0; y < H; ++y) buf.insert(buf.end(), q.tex + y * stride, q.tex + y * stride + line);
        size = buf.size();
        return true;
    };
    for (int k = 0; k < kNumRows; ++k) {
        const int H = kRows[k] < 0 ? q.h : kRows[k];
        if (H > q.h || H < 1) continue;
        if (k > 0 && q.h < 256) continue;  // partial heights only matter for tall textures
        std::size_t size = 0;
        if (!span(H, size)) continue;
        if (r.n < 64) { r.v[r.n] = static_cast<std::uint32_t>(xxh64(buf.data(), size / 2, kTexSeed)); r.rows[r.n++] = H; }                       // half the bytes
        if (k == 0 && r.n < 64) { r.v[r.n] = static_cast<std::uint32_t>(xxh64(buf.data(), size, kTexSeed)); r.rows[r.n++] = -H; }              // all bytes
        std::size_t hsize = 0;
        if (H >= 2 && span(H / 2, hsize) && r.n < 64) { r.v[r.n] = static_cast<std::uint32_t>(xxh64(buf.data(), hsize, kTexSeed)); r.rows[r.n++] = -(H / 2) - 100000; }  // half the rows
    }
    return r;
}

// PPSSPP's cache key (low 32 bits) is  dim ^ (palette hash ^ palette format word)  for palette textures, and just  dim  otherwise,
// where dim is the texture size register (& 0xF0F) and the palette hash is XXH32 (seed 0xC0108888) of the palette buffer.
Variants clut_hashes(const ModQuery &q) {
    Variants r;
    auto push = [&](std::uint32_t h) { if (r.n < 12) r.v[r.n++] = h; };
    const std::uint8_t *c = q.clut;
    const unsigned total = q.clut_len;
    if (!c || total == 0) { push(0); push(0); push(0); return r; }
    const unsigned base_bytes = q.clut_offset;
    const unsigned maxb = std::max(q.clut_max, total);
    const unsigned ext = std::min(total + base_bytes, maxb);
    const std::uint32_t k = q.clutformat ^ q.dim;
    push(xxh32(c, ext, kClutSeed) ^ k);     // 0: what PPSSPP does
    push(xxh32(c, total, kClutSeed) ^ k);   // 1: ignoring the start offset
    push(xxh32(c, total, kClutSeed));       // 2: without the format word / size
    return r;
}

std::shared_ptr<const ModTexture> load_entry(int id) {
    Entry &e = S.entries[static_cast<std::size_t>(id)];
    if (e.tex) return e.tex;
    if (e.failed) return nullptr;
    auto t = std::make_shared<ModTexture>();
    if (!decode_png(e.file, *t)) {
        e.failed = true;
        std::fprintf(stderr, "[mods] cannot read %s\n", e.file.string().c_str());
        return nullptr;
    }
    const std::size_t bytes = t->px.size() * 4;
    if (S.decoded_bytes + bytes > (1200ull << 20)) {  // keep memory bounded: drop everything decoded so far
        for (auto &x : S.entries) x.tex.reset();
        S.decoded_bytes = 0;
    }
    S.decoded_bytes += bytes;
    e.tex = t;
    return e.tex;
}

}  // namespace

void mods_init(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (const char *l = std::getenv("PERSONA1_MODSLOG")) S.log = l[0] && l[0] != '0';
    fs::path root = dir;
    if (const char *e = std::getenv("PERSONA1_MODS")) root = e;
    if (std::getenv("PERSONA1_NOMODS") || !fs::is_directory(root, ec)) {
        std::fprintf(stderr, "[mods] no mods folder at %s (texture packs go there)\n", root.string().c_str());
        return;
    }
    // The pack may sit directly in the folder, or in sub-folders (like PPSSPP's TEXTURES/<game id>/).
    if (fs::exists(root / "textures.ini", ec)) load_pack(root);
    std::vector<fs::path> subs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && fs::exists(it->path() / "textures.ini", ec)) subs.push_back(it->path());
    std::sort(subs.begin(), subs.end());
    for (const auto &s : subs) load_pack(s);
    if (S.index.empty()) {
        // no ini anywhere: still pick up hash-named files
        load_pack(root);
    }
    S.active = !S.index.empty();
    std::fprintf(stderr, "[mods] %s (%zu keys)\n", S.active ? "texture pack loaded" : "no usable texture pack found", S.index.size());
}

bool mods_active() { return S.active; }

std::shared_ptr<const ModTexture> mods_find(const ModQuery &q) {
    if (!S.active || !q.tex) return nullptr;
    std::lock_guard<std::mutex> lk(S.mu);
    const Variants tv = tex_hashes(q);
    Variants cv;
    if (q.palette) cv = clut_hashes(q); else { cv.v[0] = q.dim; cv.n = 1; }

    int found = -1, wc = -1, wt = -1;
    bool wildcard = false;
    auto lookup = [&](std::uint32_t c, std::uint32_t t) -> int {
        auto it = S.index.find((static_cast<std::uint64_t>(c) << 32) | t);
        return it == S.index.end() ? -1 : it->second;
    };
    // exact: (palette hash, content hash); then the wildcard: (palette hash, any content)
    for (int ci = 0; ci < cv.n && found < 0; ++ci) {
        if (S.win_clut >= 0 && ci != S.win_clut) continue;
        for (int ti = 0; ti < tv.n && found < 0; ++ti) {
            if (tv.v[ti] == 0) continue;
            const int id = lookup(cv.v[ci], tv.v[ti]);
            if (id >= 0) { found = id; wc = ci; wt = ti; }
        }
    }
    if (found < 0) {
        for (int ci = 0; ci < cv.n && found < 0; ++ci) {
            if (S.win_clut >= 0 && ci != S.win_clut) continue;
            if (cv.v[ci] == 0) continue;
            const int id = lookup(cv.v[ci], 0);
            if (id >= 0) { found = id; wc = ci; wildcard = true; }
        }
    }
    bool palette_only = false;
    if (found < 0 && q.palette && S.win_clut >= 0) {
        // Last resort for textures whose content hash cannot be reproduced (small tiles with padded rows, for one): if this palette appears
        // exactly once in the pack and the replacement has this texture's proportions, use it. A 32-bit palette hash makes a false match very unlikely.
        const auto bc = S.by_clut.find(cv.v[0]);
        if (cv.v[0] != 0 && bc != S.by_clut.end() && bc->second.size() == 1) {
            const int id = lookup(cv.v[0], bc->second[0]);
            if (id >= 0) {
                const auto cand = load_entry(id);
                if (cand && static_cast<long long>(cand->w) * q.h == static_cast<long long>(cand->h) * q.w) { found = id; palette_only = true; wc = S.win_clut; }
            }
        }
    }
    if (found >= 0) {
        if (S.win_clut < 0 && q.palette) { S.win_clut = wc; std::fprintf(stderr, "[mods] palette hash variant %d matches the pack\n", wc); }
        if (wt >= 0 && wt < 64 && !S.tex_variant_seen[wt]) { S.tex_variant_seen[wt] = true; const int rr = tv.rows[wt]; std::fprintf(stderr, "[mods] texture hash matched using %s of %d rows\n", rr <= -100000 ? "all bytes" : (rr < 0 ? "all bytes" : "half the bytes"), rr <= -100000 ? -(rr + 100000) * 2 : std::abs(rr)); }
    } else if (S.win_clut < 0 && q.palette) {
        // No palette hash variant has matched anything yet: try the content hash alone, so a pack still works if the
        // palette hash is computed differently here. Only used until a palette hash has matched.
        for (int ti = 0; ti < tv.n && found < 0; ++ti) {
            if (tv.v[ti] == 0) continue;
            auto it = S.by_tex.find(tv.v[ti]);
            if (it != S.by_tex.end() && !it->second.empty()) {
                found = it->second.front();
                if (!S.loose_warned) { S.loose_warned = true; std::fprintf(stderr, "[mods] WARNING: matching by content hash only (palette hash does not match yet)\n"); }
                if (S.loose_logged < 10) {
                    ++S.loose_logged;
                    const Entry &en = S.entries[static_cast<std::size_t>(found)];
                    std::fprintf(stderr, "[mods] loose %s: texture hash variant %d; pack palette hash %08x; ours:", en.name.c_str(), ti, en.clut);
                    for (int k = 0; k < cv.n; ++k) std::fprintf(stderr, " %08x", cv.v[k]);
                    std::fprintf(stderr, "\n[mods] loose palette: bits=%u entries=%u entry_bytes=%u offset=%u loaded=%u bytes:", q.bits, q.entries, q.entry_bytes, q.clut_offset, q.clut_len);
                    for (unsigned k = 0; k < q.clut_len && k < 1100; ++k) std::fprintf(stderr, "%02x", q.clut[k]);
                    std::fprintf(stderr, "\n");
                }
            }
        }
    }
    std::shared_ptr<const ModTexture> res;
    if (found >= 0) res = load_entry(found);
    if (found >= 0) ++S.hits; else ++S.misses;
    if (S.log) {
        char key[160];
        std::snprintf(key, sizeof key, "%u:%u:%08x:%08x:%08x", q.bits, q.w * 1000 + q.h, cv.v[0], tv.v[0], cv.v[1]);
        if (S.seen.emplace(key, 1).second) {
            if (found >= 0 && S.logged_hits < 400) {
                ++S.logged_hits;
                std::fprintf(stderr, "[mods] hit  %s (%dx%d -> %dx%d)%s\n", S.entries[static_cast<std::size_t>(found)].name.c_str(), q.w, q.h, res ? res->w : 0, res ? res->h : 0,
                             palette_only ? " [palette only]" : (wildcard ? " [palette wildcard]" : ""));
            } else if (found < 0 && S.logged_miss < 600) {
                ++S.logged_miss;
                std::fprintf(stderr, "[mods] miss %ubpp %dx%d bufw=%u clut:%08x,%08x%s tex(rows %d..):%08x,%08x,%08x,%08x\n", q.bits, q.w, q.h, q.bufw, cv.v[0], cv.n > 1 ? cv.v[1] : 0u,
                             S.clut_keys.count(cv.v[0]) ? " [palette IS in pack]" : "", tv.n ? std::abs(tv.rows[0]) : 0, tv.v[0], tv.v[1], tv.v[2], tv.v[3]);
                if (S.clut_keys.count(cv.v[0])) {
                    std::fprintf(stderr, "[mods]   pack wants content hash:");
                    const auto &lst = S.by_clut[cv.v[0]];
                    for (std::size_t k = 0; k < lst.size() && k < 4; ++k) std::fprintf(stderr, " %08x", lst[k]);
                    std::fprintf(stderr, "\n");
                    if (q.tex_len <= 2048 && S.raw_dumped < 8) {
                        ++S.raw_dumped;
                        std::fprintf(stderr, "[mods]   rawtex bits=%u w=%d h=%d bufw=%u len=%zu:", q.bits, q.w, q.h, q.bufw, q.tex_len);
                        for (std::size_t k = 0; k < q.tex_len; ++k) std::fprintf(stderr, "%02x", q.tex[k]);
                        std::fprintf(stderr, "\n");
                    }
                }
            }
        }
    }
    return res;
}

}  // namespace persona1
