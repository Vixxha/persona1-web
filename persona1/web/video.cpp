#include "../persona1_display.hpp"

#include <atomic>
#include <emscripten.h>

namespace persona1 {
void mods_refresh_web_prompts(int revision);
namespace {
std::atomic<unsigned long long> frames{0};
}

void display_init() {}

void display_present(const std::uint32_t *rgba, std::shared_ptr<const Layer3D>) {
    const int revision = MAIN_THREAD_EM_ASM_INT({
        Module['presentFrame']($0);
        return Module['getPromptRevision'] ? Module['getPromptRevision']() : 0;
    }, rgba);
    mods_refresh_web_prompts(revision);
    ++frames;
}

unsigned long long display_frames() { return frames.load(); }
bool display_hd2d_on() { return false; }
void save_image_png(const char *, const std::uint32_t *, int, int) {}
bool debug_key_pressed() { return false; }
bool debug_key_f10() { return false; }

std::uint32_t input_poll(std::uint8_t *lx, std::uint8_t *ly) {
    const std::uint32_t input = static_cast<std::uint32_t>(MAIN_THREAD_EM_ASM_INT({
        if (Module['readInput']) return Module['readInput']();
        return (Module['readButtons'] ? Module['readButtons']() : 0) | 0x80800000;
    }));
    if (lx) *lx = static_cast<std::uint8_t>(input >> 16);
    if (ly) *ly = static_cast<std::uint8_t>(input >> 24);
    return input & 0xFFFFu;
}
}

