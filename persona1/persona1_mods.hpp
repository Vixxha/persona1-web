#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace persona1 {

// A replacement image from a PPSSPP-style texture pack. px: 0xAABBGGRR, straight alpha.
struct ModTexture {
    int w{}, h{};
    std::vector<std::uint32_t> px;
};

// What the GE knows about the texture being captured; used to compute PPSSPP-compatible hashes.
struct ModQuery {
    const std::uint8_t *tex{};   // texture memory (stride * height rows)
    std::size_t tex_len{};
    unsigned bits{};             // bits per texel (4, 8, 16, 32)
    int w{}, h{};
    unsigned bufw{};             // row stride in texels
    bool palette{};
    const std::uint8_t *clut{};  // raw palette bytes as loaded by the game
    unsigned clut_len{};
    unsigned entry_bytes{};      // 2 or 4
    unsigned entries{};          // 16 or 256
    unsigned clut_offset{};      // byte offset of the first palette entry in use
    std::uint32_t dim{};         // texture size register & 0xF0F
    std::uint32_t clutformat{};  // palette format command word (with its 0xC5 opcode byte)
    unsigned clut_max{};         // most palette bytes ever loaded
};

// Looks for textures.ini (in `dir` or in a sub-folder of it) and PNG files named by hash. Safe to call with a missing folder.
void mods_init(const std::filesystem::path &dir);
bool mods_active();
// Returns the replacement for this texture, or null. The result is cached; the pointer stays valid as long as it is held.
std::shared_ptr<const ModTexture> mods_find(const ModQuery &q);

}  // namespace persona1
