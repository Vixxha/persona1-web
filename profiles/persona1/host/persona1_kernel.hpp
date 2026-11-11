#pragma once

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace persona1 {

struct KernelConfig {
    std::uint32_t heap_start{};            // first free guest address after the ELF image
    std::uint32_t heap_end{0x0A000000u};   // end of 32 MB guest RAM
    std::uint32_t gp{};                    // module global pointer, given to every thread
    std::uint32_t exit_stub{};             // address of the sceKernelExitThread import stub
    std::uint32_t tick_interval{4096};     // dispatches between scheduler checks (0 = off)
    bool trace{};                          // log thread/memory events to stderr
};

// Registers the kernel HLE handlers, creates the initial "module_start" thread and
// points the CPU at it. Call after the ELF is loaded and generated code is registered.
void install_kernel(psprecomp::Runtime &rt, const KernelConfig &cfg);

// True if the kernel module provides a real implementation for this import, so the
// host should not install a logging stub over it.
bool kernel_handles(const std::string &library, std::uint32_t nid);

// Puts the calling guest thread to sleep for `us` microseconds (lets lower-priority threads run).
// For use inside an import handler, after its return value has been set.
void kernel_sleep(psprecomp::AllegrexContext &ctx, std::uint32_t us);
// Runs a guest function (entry(a0,a1,a2)) on a helper thread and blocks the calling guest thread until
// it returns. `on_done` receives the function's return value; what it returns becomes the calling
// import's return value (v0). For use as the last action of an import handler. False if no thread could be made.
bool kernel_call_guest(psprecomp::AllegrexContext &ctx, std::uint32_t entry, std::uint32_t a0, std::uint32_t a1,
                       std::uint32_t a2, std::function<std::uint32_t(std::uint32_t)> on_done);
// Calls a guest function directly on the current thread (no scheduling) and returns its v0.
// Use for callbacks the real library would call inline, such as the movie ring-buffer refill.
bool kernel_call_guest_inline(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t entry,
                              std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t &result);
// Seconds the host spent sleeping because every guest thread was waiting (profiling).
extern double g_idle_seconds;

// Prints every thread and what it is waiting for. Useful after a stop or hang.
void kernel_report();

} // namespace persona1
