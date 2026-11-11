// Persona 1 (ULUS10432) bring-up host for PSPRecomp.
//
//   Persona1Native <decrypted ELF> <game root> [max_dispatches] [iso path]
//
// Kernel services (threads, semaphores, event flags, memory, time, vblank timing)
// are implemented in persona1_kernel.cpp. Every other import gets a logging stub:
// by default the first unimplemented import stops the run so we learn what to
// implement next.
//
// Environment variables:
//   PERSONA1_STUB_CONTINUE=1  unimplemented imports log once and return 0
//   PERSONA1_TRACE=1          log thread, switch and memory events
//   PERSONA1_TICK=<n>         scheduler check interval in dispatches (0 = off)
//   PERSONA1_NIDS=<path>      import names file (default: next to the ELF)

#include <chrono>
#include <thread>
#include "persona1_audio.hpp"
#include "persona1_io.hpp"
#include "persona1_movie.hpp"
#include "persona1_kernel.hpp"
#include "persona1_display.hpp"
#include "persona1_mods.hpp"

#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace psprecomp {
// Defined in generated/imports.cpp
void register_generated_functions(Runtime &runtime);
} // namespace psprecomp

namespace {

constexpr std::uint32_t kExitThreadNid = 0xAA73C935u;  // ThreadManForUser::sceKernelExitThread

// Optional names for imports: one "library,0xNID,name" per line (see make_nid_csv.py).
void load_nid_names(psprecomp::Runtime &rt, const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto c1 = line.find(',');
        if (c1 == std::string::npos) continue;
        const auto c2 = line.find(',', c1 + 1);
        if (c2 == std::string::npos) continue;
        std::string name = line.substr(c2 + 1);
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
        if (name.empty()) continue;
        try {
            rt.nids().add(line.substr(0, c1),
                          static_cast<std::uint32_t>(
                              std::stoul(line.substr(c1 + 1, c2 - c1 - 1), nullptr, 0)),
                          name);
        } catch (const std::exception &) {
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: Persona1Native <decrypted ELF> <game root> [max_dispatches] [iso path]\n";
        return 2;
    }
    try {
        const std::filesystem::path elf_path = argv[1];
        const std::filesystem::path game_root = argv[2];
        const std::uint64_t max_dispatches =
            argc >= 4 ? std::stoull(argv[3], nullptr, 0) : 5'000'000ull;
        const bool keep_going = std::getenv("PERSONA1_STUB_CONTINUE") != nullptr;
        const bool strict = std::getenv("PERSONA1_STRICT") != nullptr;
        const std::uint32_t base = psprecomp::kDefaultPspUserLoadBase;

        psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(elf_path);
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        runtime.set_game_root(game_root);

        const auto relocations = elf.load_and_relocate(runtime.memory(), base);
        psprecomp::register_generated_functions(runtime);

        {
            const char *nid_env = std::getenv("PERSONA1_NIDS");
            load_nid_names(runtime, nid_env != nullptr
                                        ? std::filesystem::path(nid_env)
                                        : elf_path.parent_path() / "nids_persona1.csv");
        }

        const auto module = elf.find_module_info(runtime.memory(), base);
        if (!module) throw std::runtime_error("PSP module info not found after relocation");
        const auto imports = elf.scan_imports(runtime.memory(), *module);

        // Guest heap starts after the loaded image (including bss).
        std::uint32_t image_end = 0u;
        for (std::size_t i = 0; i < elf.segments().size(); ++i) {
            const auto &segment = elf.segments()[i];
            if (segment.type != 1u) continue;  // PT_LOAD
            const std::uint32_t start = elf.segment_runtime_address(i, base);
            image_end = std::max(image_end, start + segment.memory_size);
        }

        persona1::KernelConfig kernel_config;
        kernel_config.heap_start = (image_end + 0xFFFFu) & ~0xFFFFu;
        kernel_config.gp = module->gp;
        kernel_config.trace = std::getenv("PERSONA1_TRACE") != nullptr;
        if (const char *tick = std::getenv("PERSONA1_TICK"))
            kernel_config.tick_interval = static_cast<std::uint32_t>(std::stoul(tick, nullptr, 0));
        for (const auto &imp : imports) {
            if (imp.library == "ThreadManForUser" && imp.nid == kExitThreadNid)
                kernel_config.exit_stub = imp.stub_address;
        }
        if (kernel_config.exit_stub == 0u)
            throw std::runtime_error("sceKernelExitThread import not found; cannot set thread exit address");

        persona1::install_kernel(runtime, kernel_config);

        persona1::IoConfig io_config;
        io_config.game_root = game_root;
        io_config.save_root = elf_path.parent_path() / "ms0";
        io_config.trace = kernel_config.trace;
        if (argc >= 5) io_config.iso_path = argv[4];
        else if (const char *iso = std::getenv("PERSONA1_ISO")) io_config.iso_path = iso;
        persona1::install_io(runtime, io_config);
        persona1::mods_init(elf_path.parent_path() / "mods");
        persona1::install_audio(runtime);
        persona1::install_movie(runtime);

        // Everything the kernel and I/O modules do not implement gets a logging stub.
        std::size_t stubbed = 0;
        for (const auto &imp : imports) {
            if (persona1::kernel_handles(imp.library, imp.nid) || persona1::io_handles(imp.library, imp.nid) ||
                persona1::audio_handles(imp.library, imp.nid) ||
                persona1::movie_handles(imp.library, imp.nid))
                continue;
            ++stubbed;
            // Sound, interrupt and dialog calls are not needed to get the game running,
            // so they log once and return 0 instead of stopping the run. Set
            // PERSONA1_STRICT=1 to make every unimplemented import stop the run.
            const bool soft = !strict && (imp.library == "sceSasCore" || imp.library == "sceAtrac3plus" ||
                                          imp.library == "InterruptManager" || imp.library == "sceUtility" ||
                                          imp.library == "sceMpeg");
            runtime.register_hle(
                imp.library, imp.nid,
                [lib = imp.library, nid = imp.nid, keep_going, soft, calls = 0](
                    psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) mutable {
                    if (calls++ < (lib == "sceMpeg" ? 6 : 1)) {
                        char head[160];
                        std::snprintf(head, sizeof(head), "%s::0x%08X", lib.c_str(),
                                      static_cast<unsigned>(nid));
                        const auto name = rt.nids().resolve(lib, nid);
                        char args[160];
                        std::snprintf(args, sizeof(args),
                                      " a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X",
                                      static_cast<unsigned>(ctx.gpr[4]),
                                      static_cast<unsigned>(ctx.gpr[5]),
                                      static_cast<unsigned>(ctx.gpr[6]),
                                      static_cast<unsigned>(ctx.gpr[7]),
                                      static_cast<unsigned>(ctx.gpr[31]));
                        std::cerr << "[import] " << head
                                  << (name ? " (" + *name + ")" : std::string()) << args << "\n";
                    }
                    if (!keep_going && !soft) {
                        rt.stop("unimplemented import " + lib + "::" + std::to_string(nid));
                        return;
                    }
                    ctx.set_gpr(2, 0u);
                    // The music decoder is not implemented yet. Its caller (the BGM thread) polls it
                    // in a loop at higher priority than the main thread, so give up the CPU briefly.
                    if (lib == "sceAtrac3plus") persona1::kernel_sleep(ctx, 1000u);
                });
        }

        std::cout << "Entry:      0x" << std::hex << elf.runtime_entry() << std::dec << "\n"
                  << "Relocs:     " << relocations.total << " (invalid " << relocations.invalid
                  << ", unsupported " << relocations.unsupported << ")\n"
                  << "Functions:  " << runtime.function_count() << "\n"
                  << "Imports:    " << imports.size() << " (" << (imports.size() - stubbed)
                  << " implemented, " << stubbed << " stubbed)\n"
                  << "Guest heap: 0x" << std::hex << kernel_config.heap_start << " - 0x"
                  << kernel_config.heap_end << std::dec << "\n"
                  << "Game root:  " << game_root.string() << "\n"
                  << "Max steps:  " << max_dispatches << "\n"
                  << "Stub mode:  " << (keep_going ? "log once and continue" : "stop at first import")
                  << std::endl;

        if (!std::getenv("PERSONA1_NOWINDOW")) persona1::display_init();

        {
            // Hang watchdog: if no frame is presented for 5 seconds, print what every game thread is waiting on.
            std::thread([] {
                unsigned long long last = persona1::display_frames();
                int still = 0, reports = 0;
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    const unsigned long long now = persona1::display_frames();
                    if (now != last) { last = now; still = 0; continue; }
                    ++still;
                    if ((still == 5 || still == 15 || still == 45) && reports < 6) {
                        ++reports;
                        std::cerr << "[watchdog] no frame presented for " << still << " s - thread dump follows\n";
                        persona1::kernel_report();
                    }
                }
            }).detach();
        }        runtime.run(elf.runtime_entry(), max_dispatches == 0 ? ~0ull : max_dispatches);

        std::cout << "Runtime stopped: "
                  << (runtime.stop_reason().empty() ? "(no reason given)" : runtime.stop_reason())
                  << "\n"
                  << "Last dispatch pc: 0x" << std::hex << psprecomp::runtime_dispatch_pc()
                  << std::dec << "\n";
        persona1::kernel_report();
        persona1::audio_shutdown();
        return runtime.stop_reason().empty() ? 0 : 4;
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << "\n";
        persona1::kernel_report();
        return 1;
    }
}
