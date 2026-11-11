#pragma once
#include "psprecomp/runtime.hpp"
#include <cstdint>

namespace persona1 {
// Software GE: executes display lists into guest memory (VRAM). The game streams lists:
// it enqueues with stall == start and later calls sceGeListUpdateStallAddr to release more
// commands, so every list keeps its position and continues on each update.
// Supports 2D "through mode" drawing (sprites, triangles, strips, fans), palette and
// direct-colour textures, blending and alpha test. 3D (transformed) primitives are counted
// and skipped for now.
void ge_list_start(psprecomp::Runtime &rt, std::uint32_t list_addr, std::uint32_t stall_addr, int list_id);
void ge_list_update(psprecomp::Runtime &rt, int list_id, std::uint32_t stall_addr);

// Copies the frame the game selected with sceDisplaySetFrameBuf to the window.
void gpu_present(psprecomp::Runtime &rt, std::uint32_t addr, std::uint32_t stride, std::uint32_t pixel_format);
}
