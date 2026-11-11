#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace persona1 {
// Opens the game window (480x272 scaled 2x). Safe to call more than once.
void display_init();
// Shows one 480x272 frame; pixels are 0xAABBGGRR (PSP 8888 order).
// layer: optional captured 3D scene for the GPU to redraw at a higher internal resolution.
struct ModTexture;
struct Layer3DTex { int w{}, h{}; std::uint64_t id{}; std::vector<std::uint32_t> px; std::shared_ptr<const ModTexture> hi; };  // px: 0xAABBGGRR; hi: replacement from a texture pack (any size, same layout)
struct Layer3DVertex { float x, y, z, iw, u, v; std::uint32_t color; float ur0{}, vr0{}, ur1{}, vr1{}; };  // ur/vr: optional uv rectangle (normalised) the sample is kept inside  // screen space (480x272), depth 0..65535, u/v normalised
struct Layer3DDraw {
    std::uint32_t first{}, count{};
    std::shared_ptr<const Layer3DTex> tex;
    bool textured{}, ztest{}, zwrite{}, blend{}, atest{}, tex_alpha{}, dbl{}, clamp_u{}, clamp_v{}, linear{};
    int zfunc{}, tfunc{}, atest_func{}, atest_ref{}, bsrc{}, bdst{}, beq{};
    std::uint32_t fix_a{}, fix_b{};  // 0x00BBGGRR blend constants
    int sx1{0}, sy1{0}, sx2{479}, sy2{271};  // scissor rectangle in native pixels (inclusive)
    bool clear_depth{};                      // clear the depth buffer before this draw
    bool is2d{};                             // a 2D (through-mode) draw
};
struct Layer3D {
    std::vector<Layer3DVertex> verts;
    std::vector<Layer3DDraw> draws;
    std::vector<std::uint8_t> mask;  // 512x272, 1 = the native pixel was drawn by 2D after the 3D scene
    bool depth_greater{true};
    bool full{};                        // every draw of the frame was recorded: replay all of it at high resolution
    std::vector<std::uint32_t> base;    // full: the native frame as it was before the first recorded draw (480x272)
};
void display_present(const std::uint32_t *rgba, std::shared_ptr<const Layer3D> layer = nullptr);

// Writes any RGBA image (0xAABBGGRR pixels) as a PNG, for debugging.
void save_image_png(const char *name, const std::uint32_t *rgba, int w, int h);

// True once each time F11 is pressed (debug: dump thread states).
// Number of frames presented so far (used by the hang watchdog).
unsigned long long display_frames();
// False when the user switched high-resolution 2D off with F6 (3D stays high resolution).
bool display_hd2d_on();
bool debug_key_pressed();
bool debug_key_f10();  // F10 edge: log every draw of the next frame

// Reads the keyboard (while the game window is focused) and the first Xbox controller.
// Returns PSP button bits (SceCtrlButtons); lx/ly are the analog stick, 128 = centered.
std::uint32_t input_poll(std::uint8_t *lx, std::uint8_t *ly);
}
