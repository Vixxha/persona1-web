#include "state-codec.hpp"
#include "psprecomp/runtime.hpp"
#include <atomic>
#include <emscripten.h>

namespace persona1::states {
namespace {
std::atomic<int> request{0};
std::atomic<int> token{0};
std::vector<std::uint8_t> incoming;

void report_error(int id, const char *message) {
    MAIN_THREAD_EM_ASM({
        if (Module['onStateError']) Module['onStateError']($0, UTF8ToString($1));
    }, id, message);
}

void checkpoint(psprecomp::Runtime &rt, psprecomp::AllegrexContext &, std::uint32_t, std::int32_t) {
    const int operation = request.load(std::memory_order_acquire);
    if (operation <= 0 || !kernel_safe()) return;
    const int id = token.load();
    const std::uint64_t previousTime = kernel_time();
    try {
        if (operation == 1) {
            Writer output;
            output.bytes.reserve(40u * 1024u * 1024u);
            const std::uint32_t magic = 0x31545350u, version = 1;
            output(magic, version, rt.cpu(), rt.memory().bytes(), rt.memory().vram_bytes());
            save_kernel(output); save_io(output); save_audio(output); save_movie(output); save_graphics(output);
            MAIN_THREAD_EM_ASM({
                if (Module['onStateCaptured']) Module['onStateCaptured']($0, HEAPU8.slice($1, $1 + $2), $3, $4, $5);
            }, id, output.bytes.data(), output.bytes.size(), rt.memory().raw_pointer(0x08000000u,rt.memory().size()),rt.memory().size(),rt.cpu().pc);
            kernel_rebase(previousTime);
        } else {
            Reader input{incoming};
            std::uint32_t magic{}, version{};
            psprecomp::AllegrexContext cpu;
            std::vector<std::uint8_t> ram, vram;
            input(magic, version, cpu, ram, vram);
            if (magic != 0x31545350u || version != 1 || ram.size() != rt.memory().size() ||
                vram.size() != rt.memory().vram_size() || !rt.has_function(cpu.pc)) {
                throw std::runtime_error("El estado no corresponde a esta versi\u00f3n del juego.");
            }
            auto kernel = load_kernel(input);
            auto io = load_io(input);
            auto audio = load_audio(input);
            auto movie = load_movie(input);
            auto graphics = load_graphics(input);
            input.finish();
            std::memcpy(rt.memory().raw_pointer(0x08000000u, ram.size()), ram.data(), ram.size());
            std::memcpy(rt.memory().raw_pointer(0x04000000u, vram.size()), vram.data(), vram.size());
            rt.cpu() = cpu;
            io(); audio(); movie(); graphics(); kernel();
            MAIN_THREAD_EM_ASM({
                if (Module['onStateRestored']) Module['onStateRestored']($0,$1,$2,$3);
            }, id, rt.memory().raw_pointer(0x08000000u,rt.memory().size()),rt.memory().size(),rt.cpu().pc);
        }
    } catch (const std::exception &error) {
        kernel_rebase(previousTime);
        report_error(id, error.what());
    }
    incoming.clear();
    request.store(0, std::memory_order_release);
}
}

extern "C" EMSCRIPTEN_KEEPALIVE int persona_state_save(int id) {
    int expected = 0;
    if (id <= 0 || !request.compare_exchange_strong(expected, -1)) return 0;
    token.store(id);
    request.store(1, std::memory_order_release);
    return 1;
}

extern "C" EMSCRIPTEN_KEEPALIVE std::uint8_t *persona_state_upload(std::uint32_t size, int id) {
    int expected = 0;
    if (!size || size > maxBytes || id <= 0 || !request.compare_exchange_strong(expected, -1)) return nullptr;
    try {
        incoming.resize(size);
        token.store(id);
        return incoming.data();
    } catch (...) {
        request.store(0);
        return nullptr;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void persona_state_restore() {
    if (request.load() == -1 && !incoming.empty()) request.store(2, std::memory_order_release);
}

extern "C" EMSCRIPTEN_KEEPALIVE void persona_state_cancel_upload() {
    if (request.load() == -1) { incoming.clear(); request.store(0, std::memory_order_release); }
}

void install(psprecomp::Runtime &) { psprecomp::set_runtime_post_dispatch_hook(checkpoint); }
}
