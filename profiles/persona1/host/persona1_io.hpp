#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace persona1 {

struct IoConfig {
    // Folder that contains PSP_GAME (the extracted UMD). "disc0:/..." and "umd0:/..."
    // paths are resolved relative to it.
    std::filesystem::path game_root;
    // Where "ms0:/..." (memory stick, savedata) paths go.
    std::filesystem::path save_root;
    // The ISO the game was extracted from. Needed for raw-sector paths such as
    // "disc0:/sce_lbn0xe020_size0x126b0", which Persona 1 uses for its data files.
    std::filesystem::path iso_path;
    bool trace{};
};

// Registers the IoFileMgrForUser handlers that are implemented (open, read, write, seek,
// close, getstat and the async variants). Calls that are not implemented stay unregistered.
void install_io(psprecomp::Runtime &rt, const IoConfig &cfg);

// F3/F4 save-slot selection (slot index 0-127). Returns the new index.
int io_save_slot_step(int delta);

// F5: run persona1\cheat.txt (memory search / poke tool) and write cheat_out.txt.
void cheat_run();

// R3 reward multiplier: cycle off/2x/4x, per-poll watcher, resync after loads.
int rewards_cycle();
int rewards_mult();
void rewards_resync();
void rewards_tick();

bool io_handles(const std::string &library, std::uint32_t nid);

} // namespace persona1
