// Persona 1 kernel HLE for PSPRecomp.
//
// Threads are cooperative. Generated code always returns to the runtime's outer
// loop at an import call, so a thread switch is done by swapping the contents of
// the live AllegrexContext inside an import handler (or inside the starvation hook,
// which fires at an outer-dispatch boundary).
//
// Time is real time (steady clock). When every thread is blocked the scheduler
// sleeps until the earliest timeout or vblank instead of spinning.
//
// NOTE: several SCE error code values below are from memory and are only compared
// for "negative = error" by most games. Verify them if a game starts checking
// specific values.

#include "persona1_kernel.hpp"
#include "persona1_ge.hpp"
#include "persona1_display.hpp"
#include "persona1_io.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace persona1 {

double g_idle_seconds = 0.0;
unsigned long long g_prof_vblank = 0, g_prof_setfb = 0;

namespace {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kErrGeneric = 0x80020001u;
constexpr std::uint32_t kErrNoMemory = 0x80020190u;
constexpr std::uint32_t kErrNotDormant = 0x80020193u;
constexpr std::uint32_t kErrUnknownThid = 0x80020197u;
constexpr std::uint32_t kErrUnknownSemaId = 0x80020198u;
constexpr std::uint32_t kErrUnknownEvfId = 0x80020199u;
constexpr std::uint32_t kErrWaitTimeout = 0x800201A8u;
constexpr std::uint32_t kErrWaitDelete = 0x800201AAu;
constexpr std::uint32_t kErrSemaOverflow = 0x800201AEu;
constexpr std::uint32_t kErrEvfCond = 0x800201AFu;

constexpr std::uint64_t kVblankUs = 16683;  // 59.94 Hz

enum class State { Dormant, Ready, Running, Waiting };
enum class Wait { None, Delay, Vblank, Sema, EventFlag, ThreadEnd, LwMutex };

const char *wait_name(Wait w) {
    switch (w) {
    case Wait::None: return "none";
    case Wait::Delay: return "delay";
    case Wait::Vblank: return "vblank";
    case Wait::Sema: return "semaphore";
    case Wait::EventFlag: return "event flag";
    case Wait::ThreadEnd: return "thread end";
    case Wait::LwMutex: return "lw mutex";
    }
    return "?";
}

const char *state_name(State s) {
    switch (s) {
    case State::Dormant: return "dormant";
    case State::Ready: return "ready";
    case State::Running: return "running";
    case State::Waiting: return "waiting";
    }
    return "?";
}

struct Thread {
    std::int32_t uid{};
    std::string name;
    std::uint32_t entry{};
    std::uint32_t attr{};
    std::uint32_t stack_size{};
    std::uint32_t stack_addr{};
    std::int32_t priority{32};
    AllegrexContext ctx{};
    State state{State::Dormant};
    Wait wait{Wait::None};
    std::int32_t wait_obj{};
    std::uint32_t wait_a{};
    std::uint32_t wait_b{};
    std::uint32_t wait_out{};
    std::uint32_t wait_timeout_ptr{};
    bool has_deadline{};
    std::uint64_t deadline{};
    std::uint64_t wait_seq{};
    std::uint64_t ready_seq{};
    bool has_pending{};
    std::uint32_t pending{};
    std::int32_t exit_status{};
    bool auto_delete{};  // host-created callback threads free themselves on exit
    std::function<std::uint32_t(std::uint32_t)> on_exit;  // host hook: maps the exit status seen by waiters
};

struct GeCallback {
    std::uint32_t signal_func{};
    std::uint32_t signal_arg{};
    std::uint32_t finish_func{};
    std::uint32_t finish_arg{};
};

struct Sema {
    std::string name;
    std::uint32_t attr{};
    std::int32_t count{};
    std::int32_t max{};
};

struct EventFlag {
    std::string name;
    std::uint32_t attr{};
    std::uint32_t init{};
    std::uint32_t pattern{};
};

struct LwMutex {
    std::int32_t owner{-1};
    std::uint32_t count{};
};

struct MemBlock {
    std::uint32_t addr{};
    std::uint32_t size{};
    std::string name;
};

// First-fit allocator over a range of guest RAM.
class Heap {
public:
    static std::uint32_t round(std::uint32_t size) { return (size + 0xFFu) & ~0xFFu; }

    void init(std::uint32_t start, std::uint32_t end) {
        free_.clear();
        if (end > start) free_.push_back({start, end - start});
    }

    std::uint32_t alloc_low(std::uint32_t size, std::uint32_t align) {
        size = round(size);
        align = std::max<std::uint32_t>(align, 0x100u);
        for (std::size_t i = 0; i < free_.size(); ++i) {
            const std::uint32_t a = (free_[i].first + align - 1u) & ~(align - 1u);
            if (a >= free_[i].first && a + size <= free_[i].first + free_[i].second && a + size > a) {
                carve(i, a, size);
                return a;
            }
        }
        return 0;
    }

    std::uint32_t alloc_high(std::uint32_t size, std::uint32_t align) {
        size = round(size);
        align = std::max<std::uint32_t>(align, 0x100u);
        for (std::size_t n = free_.size(); n > 0; --n) {
            const std::size_t i = n - 1u;
            const std::uint32_t end = free_[i].first + free_[i].second;
            if (end < size) continue;
            const std::uint32_t a = (end - size) & ~(align - 1u);
            if (a >= free_[i].first) {
                carve(i, a, size);
                return a;
            }
        }
        return 0;
    }

    std::uint32_t alloc_at(std::uint32_t addr, std::uint32_t size) {
        size = round(size);
        for (std::size_t i = 0; i < free_.size(); ++i) {
            if (addr >= free_[i].first && addr + size <= free_[i].first + free_[i].second) {
                carve(i, addr, size);
                return addr;
            }
        }
        return 0;
    }

    void release(std::uint32_t addr, std::uint32_t size) {
        size = round(size);
        free_.push_back({addr, size});
        std::sort(free_.begin(), free_.end());
        std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
        for (const auto &r : free_) {
            if (!merged.empty() && merged.back().first + merged.back().second >= r.first) {
                const std::uint32_t end = std::max(merged.back().first + merged.back().second,
                                                   r.first + r.second);
                merged.back().second = end - merged.back().first;
            } else {
                merged.push_back(r);
            }
        }
        free_ = std::move(merged);
    }

    std::uint32_t total_free() const {
        std::uint32_t total = 0;
        for (const auto &r : free_) total += r.second;
        return total;
    }

private:
    void carve(std::size_t index, std::uint32_t addr, std::uint32_t size) {
        const std::uint32_t start = free_[index].first;
        const std::uint32_t end = start + free_[index].second;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> repl;
        if (addr > start) repl.push_back({start, addr - start});
        if (addr + size < end) repl.push_back({addr + size, end - (addr + size)});
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(index));
        free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(index), repl.begin(), repl.end());
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> free_;
};

struct Kernel {
    Runtime *rt{};
    KernelConfig cfg{};
    Heap heap;
    std::map<std::int32_t, Thread> threads;
    std::map<std::int32_t, Sema> semas;
    std::map<std::int32_t, EventFlag> evfs;
    std::map<std::uint32_t, LwMutex> lwmutexes;
    std::map<std::int32_t, MemBlock> blocks;
    std::set<std::int32_t> callbacks;
    std::int32_t current{-1};
    std::int32_t next_uid{0x1000};
    std::uint64_t seq{0};
    Clock::time_point t0{Clock::now()};
    std::uint32_t fb_addr{};
    std::uint32_t fb_width{};
    std::uint32_t fb_pixfmt{};
    std::map<std::int32_t, std::uint32_t> audio_samples;  // reserved audio channels
    std::map<std::int32_t, GeCallback> ge_callbacks;
    std::int32_t next_ge_cb{0};
    std::int32_t next_ge_list{1};

    std::int32_t new_uid() { return next_uid++; }

    std::uint64_t now_us() const {
        return 5'000'000ull + static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
    }
    std::uint64_t next_vblank() const { return (now_us() / kVblankUs + 1u) * kVblankUs; }

    // ---- guest memory helpers
    std::uint32_t rd32(std::uint32_t addr) {
        const auto m = rt->memory().aot_fast_view();
        return m.aot_load32(addr);
    }
    void wr32(std::uint32_t addr, std::uint32_t value) {
        const auto m = rt->memory().aot_fast_view();
        m.aot_store32(addr, value);
    }
    std::string read_cstr(std::uint32_t addr) {
        std::string s;
        if (addr == 0u) return s;
        const auto m = rt->memory().aot_fast_view();
        for (std::uint32_t i = 0; i < 64u; ++i) {
            const char c = static_cast<char>(m.aot_load8(addr + i));
            if (c == 0) break;
            s.push_back(c);
        }
        return s;
    }

    // PSP threads start with $k0 pointing at a zeroed 0x100-byte block at the top of
    // their stack. Sony's libraries use it as per-thread storage (the game stores
    // pointers at k0+4). Returns the k0 value; the thread's sp must start below it.
    std::uint32_t make_k0_block(const Thread &t) {
        const std::uint32_t k0 = t.stack_addr + t.stack_size - 0x100u;
        const auto m = rt->memory().aot_fast_view();
        for (std::uint32_t i = 0; i < 0x100u; i += 4u) m.aot_store32(k0 + i, 0u);
        m.aot_store32(k0 + 0xC0u, static_cast<std::uint32_t>(t.uid));
        m.aot_store32(k0 + 0xC8u, t.stack_addr);
        m.aot_store32(k0 + 0xF8u, 0xFFFFFFFFu);
        m.aot_store32(k0 + 0xFCu, 0xFFFFFFFFu);
        return k0;
    }

    // Runs a guest callback (for example a GE finish handler) as a short-lived,
    // highest-priority thread: it preempts the caller at the next scheduling point
    // and frees itself when the function returns.
    bool spawn_callback_thread(const char *name, std::uint32_t entry, std::uint32_t a0,
                               std::uint32_t a1) {
        Thread t;
        t.uid = new_uid();
        t.name = name;
        t.entry = entry;
        t.priority = 0;
        t.stack_size = 0x4000u;
        t.stack_addr = heap.alloc_high(t.stack_size, 0x100u);
        if (t.stack_addr == 0u) return false;
        t.auto_delete = true;
        const std::uint32_t k0 = make_k0_block(t);
        AllegrexContext c{};
        c.eat_vfpu_prefixes();
        c.gpr[26] = k0;
        c.gpr[28] = cfg.gp;
        c.gpr[29] = k0 - 0x40u;
        c.gpr[31] = cfg.exit_stub;
        c.gpr[4] = a0;
        c.gpr[5] = a1;
        c.pc = entry;
        t.ctx = c;
        t.state = State::Ready;
        t.ready_seq = ++seq;
        const std::int32_t uid = t.uid;
        threads.emplace(uid, std::move(t));
        return true;
    }

    // Like spawn_callback_thread, for up to three arguments; returns the new thread's uid or -1.
    std::int32_t spawn_call_thread(const char *name, std::uint32_t entry, std::uint32_t a0, std::uint32_t a1,
                                   std::uint32_t a2, std::uint32_t src_k0,
                                   std::function<std::uint32_t(std::uint32_t)> on_exit) {
        Thread t;
        t.uid = new_uid();
        t.name = name;
        t.entry = entry;
        t.priority = 0;
        t.stack_size = 0x4000u;
        t.stack_addr = heap.alloc_high(t.stack_size, 0x100u);
        if (t.stack_addr == 0u) return -1;
        t.auto_delete = true;
        t.on_exit = std::move(on_exit);
        const std::uint32_t k0 = make_k0_block(t);
        // The game keeps per-thread pointers in the k0 block (e.g. at k0+4); the helper thread works on
        // the caller's behalf, so it starts with a copy of the caller's block (except the thread ids).
        if (src_k0 != 0u) {
            const auto m = rt->memory().aot_fast_view();
            for (std::uint32_t i = 0; i < 0xC0u; i += 4u) m.aot_store32(k0 + i, m.aot_load32(src_k0 + i));
        }
        AllegrexContext c{};
        c.eat_vfpu_prefixes();
        c.gpr[26] = k0;
        c.gpr[28] = cfg.gp;
        c.gpr[29] = k0 - 0x40u;
        c.gpr[31] = cfg.exit_stub;
        c.gpr[4] = a0;
        c.gpr[5] = a1;
        c.gpr[6] = a2;
        c.pc = entry;
        t.ctx = c;
        t.state = State::Ready;
        t.ready_seq = ++seq;
        const std::int32_t uid = t.uid;
        threads.emplace(uid, std::move(t));
        return uid;
    }

    Thread *find_thread(std::uint32_t uid) {
        if (uid == 0u) uid = static_cast<std::uint32_t>(current);
        const auto it = threads.find(static_cast<std::int32_t>(uid));
        return it == threads.end() ? nullptr : &it->second;
    }
    Thread &cur() { return threads.at(current); }

    // ---- waking and timers
    void wake(Thread &t, std::uint32_t result) {
        t.state = State::Ready;
        t.wait = Wait::None;
        t.has_deadline = false;
        t.pending = result;
        t.has_pending = true;
        t.ready_seq = ++seq;
    }

    // Vblank sub-interrupt handlers (sceKernelRegisterSubIntrHandler on interrupt 30), run once per
    // vblank as a short highest-priority thread, like the interrupt handler on hardware.
    struct SubIntr { std::uint32_t handler{0}; std::uint32_t arg{0}; bool enabled{false}; };
    std::map<std::uint32_t, SubIntr> vblank_subintr;  // keyed by sub-interrupt number
    std::uint64_t last_intr_tick{0};
    std::int32_t intr_busy{-1};

    void fire_vblank_handlers() {
        if (vblank_subintr.empty()) return;
        if (intr_busy >= 0) {
            const auto it = threads.find(intr_busy);
            if (it != threads.end() && it->second.state != State::Dormant) return;
            intr_busy = -1;
        }
        const std::uint64_t tick = now_us() / kVblankUs;
        if (tick == last_intr_tick) return;
        last_intr_tick = tick;
        for (auto &e : vblank_subintr) {
            if (!e.second.enabled || e.second.handler == 0u) continue;
            intr_busy = spawn_call_thread("vblank_intr", e.second.handler, e.first, e.second.arg, 0u, 0u, nullptr);
            break;  // one handler thread at a time
        }
    }

    void process_timers() {
        fire_vblank_handlers();
        const std::uint64_t now = now_us();
        for (auto &entry : threads) {
            Thread &t = entry.second;
            if (t.state != State::Waiting || !t.has_deadline || t.deadline > now) continue;
            std::uint32_t result = 0;
            if (t.wait != Wait::Delay && t.wait != Wait::Vblank) {
                result = kErrWaitTimeout;
                if (t.wait_timeout_ptr != 0u) wr32(t.wait_timeout_ptr, 0u);
            }
            wake(t, result);
        }
    }

    std::vector<Thread *> waiters(Wait w, std::int32_t obj) {
        std::vector<Thread *> out;
        for (auto &entry : threads) {
            Thread &t = entry.second;
            if (t.state == State::Waiting && t.wait == w && t.wait_obj == obj) out.push_back(&t);
        }
        std::sort(out.begin(), out.end(),
                  [](const Thread *a, const Thread *b) { return a->wait_seq < b->wait_seq; });
        return out;
    }

    void wake_end_waiters(std::int32_t uid, std::int32_t status) {
        for (Thread *t : waiters(Wait::ThreadEnd, uid)) wake(*t, static_cast<std::uint32_t>(status));
    }

    void service_sema(std::int32_t uid) {
        const auto it = semas.find(uid);
        if (it == semas.end()) return;
        for (Thread *t : waiters(Wait::Sema, uid)) {
            if (it->second.count >= static_cast<std::int32_t>(t->wait_a)) {
                it->second.count -= static_cast<std::int32_t>(t->wait_a);
                wake(*t, 0u);
            }
        }
    }

    static bool evf_cond(std::uint32_t pattern, std::uint32_t bits, std::uint32_t mode) {
        return (mode & 1u) != 0u ? (pattern & bits) != 0u : (pattern & bits) == bits;
    }

    static void evf_apply(EventFlag &e, std::uint32_t bits, std::uint32_t mode) {
        if ((mode & 0x10u) != 0u) e.pattern = 0u;
        else if ((mode & 0x20u) != 0u) e.pattern &= ~bits;
    }

    void service_evf(std::int32_t uid) {
        const auto it = evfs.find(uid);
        if (it == evfs.end()) return;
        for (Thread *t : waiters(Wait::EventFlag, uid)) {
            EventFlag &e = it->second;
            if (evf_cond(e.pattern, t->wait_a, t->wait_b)) {
                if (t->wait_out != 0u) wr32(t->wait_out, e.pattern);
                evf_apply(e, t->wait_a, t->wait_b);
                wake(*t, 0u);
            }
        }
    }

    // ---- scheduling
    void report_deadlock() {
        bool any_waiting = false;
        for (const auto &entry : threads)
            if (entry.second.state == State::Waiting) any_waiting = true;
        if (!any_waiting) {
            rt->stop("all threads have exited");
            return;
        }
        std::cerr << "[kernel] deadlock: every thread is blocked with no timeout\n";
        kernel_report();
        rt->stop("deadlock: all threads blocked");
    }

    // Picks the thread that should own the CPU. Sleeps when nobody can run.
    // Returns -1 if the runtime was stopped (deadlock or all threads gone).
    // While a host-initiated guest call (see kernel_call_guest) runs, no other thread may be scheduled:
    // on hardware the callback runs inline in the caller. When it ends the caller continues first.
    std::int32_t exclusive{-1};
    std::int32_t exclusive_caller{-1};
    std::int32_t handoff{-1};

    std::int32_t choose_next() {
        for (;;) {
            process_timers();
            if (handoff >= 0) {
                const auto hit = threads.find(handoff);
                if (hit != threads.end() && hit->second.state == State::Ready) return handoff;
                handoff = -1;
            }
            if (exclusive >= 0) {
                const auto eit = threads.find(exclusive);
                if (eit == threads.end() || eit->second.state == State::Dormant) exclusive = -1;
                else if (eit->second.state == State::Ready || eit->second.state == State::Running) return exclusive;
            }
            Thread *best = nullptr;
            for (auto &entry : threads) {
                Thread &t = entry.second;
                if (t.state != State::Ready) continue;
                if (best == nullptr || t.priority < best->priority ||
                    (t.priority == best->priority && t.ready_seq < best->ready_seq))
                    best = &t;
            }
            const auto cit = threads.find(current);
            if (cit != threads.end() && cit->second.state == State::Running) {
                if (best != nullptr && best->priority < cit->second.priority) return best->uid;
                return cit->second.uid;
            }
            if (best != nullptr) return best->uid;

            bool have_deadline = false;
            std::uint64_t earliest = ~0ull;
            for (const auto &entry : threads) {
                const Thread &t = entry.second;
                if (t.state == State::Waiting && t.has_deadline) {
                    have_deadline = true;
                    earliest = std::min(earliest, t.deadline);
                }
            }
            if (!have_deadline) {
                report_deadlock();
                return -1;
            }
            const std::uint64_t now = now_us();
            if (earliest > now) {
                const auto s0 = Clock::now();
                std::this_thread::sleep_for(std::chrono::microseconds(earliest - now));
                g_idle_seconds += std::chrono::duration<double>(Clock::now() - s0).count();
            }
        }
    }

    void switch_to(std::int32_t uid, AllegrexContext &ctx, std::uint32_t resume_pc, bool save_current) {
        if (uid == current) {
            Thread &t = threads.at(uid);
            t.state = State::Running;
            if (t.has_pending) {
                ctx.set_gpr(2, t.pending);
                t.has_pending = false;
            }
            return;
        }
        if (save_current && current >= 0) {
            const auto it = threads.find(current);
            if (it != threads.end()) {
                Thread &c = it->second;
                c.ctx = ctx;
                c.ctx.pc = resume_pc;
                if (c.state == State::Running) {
                    c.state = State::Ready;
                    c.ready_seq = ++seq;
                }
            }
        }
        Thread &n = threads.at(uid);
        ctx = n.ctx;
        n.state = State::Running;
        if (n.has_pending) {
            ctx.set_gpr(2, n.pending);
            n.has_pending = false;
        }
        current = uid;
        psprecomp::set_runtime_thread_identity(uid, n.name);
        if (cfg.trace) std::cerr << "[kernel] switch to thread '" << n.name << "' pc=0x" << std::hex
                                 << ctx.pc << std::dec << "\n";
    }

    // The current thread has just been put into the Waiting state.
    void block_and_switch(AllegrexContext &ctx) {
        const std::uint32_t ra = ctx.gpr[31];
        const std::int32_t next = choose_next();
        if (next < 0) return;
        switch_to(next, ctx, ra, true);
    }

    // After a non-blocking call that may have woken a better thread.
    void maybe_preempt(AllegrexContext &ctx) {
        const std::uint32_t ra = ctx.gpr[31];
        const std::int32_t next = choose_next();
        if (next < 0 || next == current) return;
        switch_to(next, ctx, ra, true);
    }

    void block_until(AllegrexContext &ctx, Wait w, std::uint64_t deadline) {
        Thread &c = cur();
        c.state = State::Waiting;
        c.wait = w;
        c.wait_obj = 0;
        c.wait_timeout_ptr = 0;
        c.has_deadline = true;
        c.deadline = deadline;
        c.wait_seq = ++seq;
        c.has_pending = false;
        block_and_switch(ctx);
    }

    void block_current(AllegrexContext &ctx, Wait w, std::int32_t obj, std::uint32_t a,
                       std::uint32_t b, std::uint32_t out, std::uint32_t timeout_ptr) {
        Thread &c = cur();
        c.state = State::Waiting;
        c.wait = w;
        c.wait_obj = obj;
        c.wait_a = a;
        c.wait_b = b;
        c.wait_out = out;
        c.wait_timeout_ptr = timeout_ptr;
        c.wait_seq = ++seq;
        c.has_pending = false;
        c.has_deadline = timeout_ptr != 0u;
        if (c.has_deadline) c.deadline = now_us() + rd32(timeout_ptr);
        block_and_switch(ctx);
    }

    // End the running thread. It becomes dormant (or is deleted) and another runs.
    void exit_current(AllegrexContext &ctx, std::int32_t status, bool del) {
        if (current < 0 || threads.find(current) == threads.end()) {
            rt->stop("thread exit with no current thread");
            return;
        }
        const std::int32_t uid = current;
        Thread &t = threads.at(uid);
        if (cfg.trace) std::cerr << "[kernel] thread '" << t.name << "' exits status=" << status << "\n";
        if (t.on_exit) {
            auto hook = std::move(t.on_exit);
            t.on_exit = nullptr;
            status = static_cast<std::int32_t>(hook(static_cast<std::uint32_t>(status)));
        }
        t.exit_status = status;
        t.state = State::Dormant;
        wake_end_waiters(uid, status);
        if (uid == exclusive) {
            handoff = exclusive_caller;
            exclusive = -1;
        }
        if (del) {
            if (t.stack_addr != 0u) heap.release(t.stack_addr, t.stack_size);
            threads.erase(uid);
        }
        current = -1;
        const std::int32_t next = choose_next();
        if (next < 0) return;
        switch_to(next, ctx, 0u, false);
    }
};

Kernel *g_kernel = nullptr;
Kernel &K() { return *g_kernel; }

void ret(AllegrexContext &ctx, std::uint32_t value) { ctx.set_gpr(2, value); }

std::set<std::string> &handled_set() {
    static std::set<std::string> s;
    return s;
}
std::string hle_key(const std::string &library, std::uint32_t nid) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "::%08X", static_cast<unsigned>(nid));
    return library + buf;
}

// ======================================================= ThreadManForUser: threads
void h_create_thread(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    Thread t;
    t.name = k.read_cstr(ctx.gpr[4]);
    t.entry = ctx.gpr[5];
    t.priority = static_cast<std::int32_t>(ctx.gpr[6]);
    std::uint32_t stack = ctx.gpr[7];
    t.attr = ctx.gpr[8];
    stack = std::max<std::uint32_t>(Heap::round(stack), 0x200u);
    const std::uint32_t addr = k.heap.alloc_high(stack, 0x100u);
    if (addr == 0u) {
        ret(ctx, kErrNoMemory);
        return;
    }
    t.stack_addr = addr;
    t.stack_size = stack;
    t.uid = k.new_uid();
    if (k.cfg.trace)
        std::cerr << "[kernel] create thread '" << t.name << "' uid=" << t.uid << " entry=0x" << std::hex
                  << t.entry << std::dec << " prio=" << t.priority << " stack=0x" << std::hex << stack
                  << std::dec << "\n";
    const std::int32_t uid = t.uid;
    k.threads.emplace(uid, std::move(t));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void h_start_thread(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    Thread *t = k.find_thread(ctx.gpr[4]);
    if (t == nullptr) {
        ret(ctx, kErrUnknownThid);
        return;
    }
    if (t->state != State::Dormant) {
        ret(ctx, kErrNotDormant);
        return;
    }
    const std::uint32_t arglen = ctx.gpr[5];
    const std::uint32_t argp = ctx.gpr[6];
    AllegrexContext c{};
    c.eat_vfpu_prefixes();
    const std::uint32_t k0 = k.make_k0_block(*t);
    std::uint32_t sp = k0;
    std::uint32_t arg_copy = 0u;
    if (arglen != 0u && argp != 0u) {
        sp -= (arglen + 0xFu) & ~0xFu;
        arg_copy = sp;
        const auto m = k.rt->memory().aot_fast_view();
        for (std::uint32_t i = 0; i < arglen; ++i) m.aot_store8(arg_copy + i, m.aot_load8(argp + i));
    }
    sp -= 0x40u;
    c.gpr[29] = sp;
    c.gpr[26] = k0;
    c.gpr[28] = k.cfg.gp;
    c.gpr[31] = k.cfg.exit_stub;
    c.gpr[4] = arglen;
    c.gpr[5] = arg_copy;
    c.pc = t->entry;
    t->ctx = c;
    t->state = State::Ready;
    t->ready_seq = ++k.seq;
    t->has_pending = false;
    if (k.cfg.trace) std::cerr << "[kernel] start thread '" << t->name << "'\n";
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_exit_thread(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    // A thread whose entry function returns lands here through the exit stub with
    // the status in v0; an explicit call passes it in a0.
    const std::uint32_t status = (ctx.gpr[31] == k.cfg.exit_stub) ? ctx.gpr[2] : ctx.gpr[4];
    const auto cur_it = k.threads.find(k.current);
    const bool del = cur_it != k.threads.end() && cur_it->second.auto_delete;
    k.exit_current(ctx, static_cast<std::int32_t>(status), del);
}

void h_exit_delete_thread(Runtime &, AllegrexContext &ctx) {
    K().exit_current(ctx, static_cast<std::int32_t>(ctx.gpr[4]), true);
}

void h_terminate_delete_thread(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    Thread *t = k.find_thread(ctx.gpr[4]);
    if (t == nullptr) {
        ret(ctx, kErrUnknownThid);
        return;
    }
    if (t->uid == k.current) {
        k.exit_current(ctx, 0, true);
        return;
    }
    const std::int32_t uid = t->uid;
    k.wake_end_waiters(uid, 0);
    if (t->stack_addr != 0u) k.heap.release(t->stack_addr, t->stack_size);
    k.threads.erase(uid);
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_delete_thread(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    Thread *t = k.find_thread(ctx.gpr[4]);
    if (t == nullptr) {
        ret(ctx, kErrUnknownThid);
        return;
    }
    if (t->state != State::Dormant) {
        ret(ctx, kErrNotDormant);
        return;
    }
    if (t->stack_addr != 0u) k.heap.release(t->stack_addr, t->stack_size);
    k.threads.erase(t->uid);
    ret(ctx, 0u);
}

void h_wait_thread_end(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    Thread *t = k.find_thread(ctx.gpr[4]);
    if (t == nullptr) {
        ret(ctx, kErrUnknownThid);
        return;
    }
    if (t->state == State::Dormant) {
        ret(ctx, static_cast<std::uint32_t>(t->exit_status));
        return;
    }
    k.block_current(ctx, Wait::ThreadEnd, t->uid, 0u, 0u, 0u, ctx.gpr[5]);
}

void h_get_thread_id(Runtime &, AllegrexContext &ctx) {
    ret(ctx, static_cast<std::uint32_t>(K().current));
}

void h_change_thread_priority(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    Thread *t = k.find_thread(ctx.gpr[4]);
    if (t == nullptr) {
        ret(ctx, kErrUnknownThid);
        return;
    }
    t->priority = static_cast<std::int32_t>(ctx.gpr[5]);
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_delay_thread(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    ret(ctx, 0u);
    k.block_until(ctx, Wait::Delay, k.now_us() + ctx.gpr[4]);
}

void h_get_system_time_low(Runtime &, AllegrexContext &ctx) {
    ret(ctx, static_cast<std::uint32_t>(K().now_us()));
}

// ======================================================= semaphores
void h_create_sema(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    Sema s;
    s.name = k.read_cstr(ctx.gpr[4]);
    s.attr = ctx.gpr[5];
    s.count = static_cast<std::int32_t>(ctx.gpr[6]);
    s.max = static_cast<std::int32_t>(ctx.gpr[7]);
    const std::int32_t uid = k.new_uid();
    if (k.cfg.trace)
        std::cerr << "[kernel] " << k.cur().name << " creates sema '" << s.name << "' uid=0x" << std::hex << uid
                  << " count=" << std::dec << s.count << "\n";
    k.semas.emplace(uid, std::move(s));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void h_delete_sema(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (k.semas.erase(uid) == 0u) {
        ret(ctx, kErrUnknownSemaId);
        return;
    }
    for (Thread *t : k.waiters(Wait::Sema, uid)) k.wake(*t, kErrWaitDelete);
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_signal_sema(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = k.semas.find(uid);
    if (it == k.semas.end()) {
        ret(ctx, kErrUnknownSemaId);
        return;
    }
    const std::int32_t signal = static_cast<std::int32_t>(ctx.gpr[5]);
    if (it->second.max > 0 && it->second.count + signal > it->second.max) {
        ret(ctx, kErrSemaOverflow);
        return;
    }
    if (k.cfg.trace)
        std::cerr << "[kernel] " << k.cur().name << " signals sema 0x" << std::hex << uid << std::dec << " by " << signal << "\n";
    it->second.count += signal;
    k.service_sema(uid);
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_wait_sema(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = k.semas.find(uid);
    if (it == k.semas.end()) {
        ret(ctx, kErrUnknownSemaId);
        return;
    }
    const std::int32_t need = std::max<std::int32_t>(static_cast<std::int32_t>(ctx.gpr[5]), 1);
    if (k.cfg.trace)
        std::cerr << "[kernel] " << k.cur().name << " waits sema 0x" << std::hex << uid << std::dec << " (count "
                  << it->second.count << ", need " << need << ")\n";
    if (it->second.count >= need) {
        it->second.count -= need;
        ret(ctx, 0u);
        return;
    }
    k.block_current(ctx, Wait::Sema, uid, static_cast<std::uint32_t>(need), 0u, 0u, ctx.gpr[6]);
}

// ======================================================= event flags
void h_create_evf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    EventFlag e;
    e.name = k.read_cstr(ctx.gpr[4]);
    e.attr = ctx.gpr[5];
    e.init = ctx.gpr[6];
    e.pattern = ctx.gpr[6];
    const std::int32_t uid = k.new_uid();
    if (k.cfg.trace)
        std::cerr << "[kernel] " << k.cur().name << " creates evf '" << e.name << "' uid=0x" << std::hex << uid
                  << " init=0x" << e.init << std::dec << "\n";
    k.evfs.emplace(uid, std::move(e));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void h_delete_evf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    if (k.evfs.erase(uid) == 0u) {
        ret(ctx, kErrUnknownEvfId);
        return;
    }
    for (Thread *t : k.waiters(Wait::EventFlag, uid)) k.wake(*t, kErrWaitDelete);
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_set_evf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = k.evfs.find(uid);
    if (it == k.evfs.end()) {
        ret(ctx, kErrUnknownEvfId);
        return;
    }
    if (k.cfg.trace)
        std::cerr << "[kernel] " << k.cur().name << " sets evf 0x" << std::hex << uid << " |= 0x" << ctx.gpr[5]
                  << std::dec << "\n";
    it->second.pattern |= ctx.gpr[5];
    k.service_evf(uid);
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_clear_evf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const auto it = k.evfs.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == k.evfs.end()) {
        ret(ctx, kErrUnknownEvfId);
        return;
    }
    if (k.cfg.trace)
        std::cerr << "[kernel] " << k.cur().name << " clears evf 0x" << std::hex << ctx.gpr[4] << " &= 0x" << ctx.gpr[5]
                  << std::dec << "\n";
    it->second.pattern &= ctx.gpr[5];
    ret(ctx, 0u);
}

void h_wait_evf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = k.evfs.find(uid);
    if (it == k.evfs.end()) {
        ret(ctx, kErrUnknownEvfId);
        return;
    }
    const std::uint32_t bits = ctx.gpr[5];
    const std::uint32_t mode = ctx.gpr[6];
    const std::uint32_t out = ctx.gpr[7];
    EventFlag &e = it->second;
    if (k.cfg.trace)
        std::cerr << "[kernel] " << k.cur().name << " waits evf 0x" << std::hex << uid << " bits=0x" << bits
                  << " mode=0x" << mode << " pattern=0x" << e.pattern << std::dec
                  << (Kernel::evf_cond(e.pattern, bits, mode) ? " (ready)" : " (blocks)") << "\n";
    if (Kernel::evf_cond(e.pattern, bits, mode)) {
        if (out != 0u) k.wr32(out, e.pattern);
        Kernel::evf_apply(e, bits, mode);
        ret(ctx, 0u);
        return;
    }
    k.block_current(ctx, Wait::EventFlag, uid, bits, mode, out, ctx.gpr[8]);
}

void h_poll_evf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const auto it = k.evfs.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == k.evfs.end()) {
        ret(ctx, kErrUnknownEvfId);
        return;
    }
    const std::uint32_t bits = ctx.gpr[5];
    const std::uint32_t mode = ctx.gpr[6];
    const std::uint32_t out = ctx.gpr[7];
    EventFlag &e = it->second;
    if (out != 0u) k.wr32(out, e.pattern);
    if (Kernel::evf_cond(e.pattern, bits, mode)) {
        Kernel::evf_apply(e, bits, mode);
        ret(ctx, 0u);
    } else {
        ret(ctx, kErrEvfCond);
    }
}

void h_refer_evf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = static_cast<std::int32_t>(ctx.gpr[4]);
    const auto it = k.evfs.find(uid);
    if (it == k.evfs.end()) {
        ret(ctx, kErrUnknownEvfId);
        return;
    }
    const std::uint32_t info = ctx.gpr[5];
    if (info != 0u) {
        const auto m = k.rt->memory().aot_fast_view();
        m.aot_store32(info + 0u, 52u);
        for (std::uint32_t i = 0; i < 32u; ++i) {
            const char c = i < it->second.name.size() ? it->second.name[i] : '\0';
            m.aot_store8(info + 4u + i, static_cast<std::uint8_t>(c));
        }
        m.aot_store32(info + 36u, it->second.attr);
        m.aot_store32(info + 40u, it->second.init);
        m.aot_store32(info + 44u, it->second.pattern);
        m.aot_store32(info + 48u, static_cast<std::uint32_t>(k.waiters(Wait::EventFlag, uid).size()));
    }
    ret(ctx, 0u);
}

// ======================================================= light-weight mutexes
void h_create_lwmutex(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    LwMutex m;
    const std::uint32_t init = ctx.gpr[7];
    m.count = init;
    m.owner = init != 0u ? k.current : -1;
    k.lwmutexes[ctx.gpr[4]] = m;
    ret(ctx, 0u);
}

void h_delete_lwmutex(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t key = ctx.gpr[4];
    k.lwmutexes.erase(key);
    for (Thread *t : k.waiters(Wait::LwMutex, static_cast<std::int32_t>(key))) k.wake(*t, kErrWaitDelete);
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

void h_lock_lwmutex(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t key = ctx.gpr[4];
    const std::uint32_t n = std::max<std::uint32_t>(ctx.gpr[5], 1u);
    LwMutex &m = k.lwmutexes[key];  // an unknown workarea is treated as a fresh free mutex
    if (m.owner < 0 || m.owner == k.current) {
        m.owner = k.current;
        m.count += n;
        ret(ctx, 0u);
        return;
    }
    k.block_current(ctx, Wait::LwMutex, static_cast<std::int32_t>(key), n, 0u, 0u, ctx.gpr[6]);
}

void h_unlock_lwmutex(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t key = ctx.gpr[4];
    const std::uint32_t n = std::max<std::uint32_t>(ctx.gpr[5], 1u);
    const auto it = k.lwmutexes.find(key);
    if (it == k.lwmutexes.end() || it->second.owner != k.current || it->second.count < n) {
        ret(ctx, kErrGeneric);
        return;
    }
    LwMutex &m = it->second;
    m.count -= n;
    if (m.count == 0u) {
        m.owner = -1;
        const auto w = k.waiters(Wait::LwMutex, static_cast<std::int32_t>(key));
        if (!w.empty()) {
            m.owner = w.front()->uid;
            m.count = w.front()->wait_a;
            k.wake(*w.front(), 0u);
        }
    }
    ret(ctx, 0u);
    k.maybe_preempt(ctx);
}

// ======================================================= callbacks and interrupts
void h_create_callback(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t uid = k.new_uid();
    k.callbacks.insert(uid);
    ret(ctx, static_cast<std::uint32_t>(uid));
}
void h_delete_callback(Runtime &, AllegrexContext &ctx) {
    K().callbacks.erase(static_cast<std::int32_t>(ctx.gpr[4]));
    ret(ctx, 0u);
}
void h_ret_zero(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }

// sceKernelGetModuleId / sceKernelGetModuleIdByAddress: the game has one module, so
// give it a fixed non-zero UID (0 would look like a failure to some callers).
void h_get_module_id(Runtime &, AllegrexContext &ctx) { ret(ctx, 0x0F00u); }
void h_cpu_suspend_intr(Runtime &, AllegrexContext &ctx) { ret(ctx, 1u); }

// ======================================================= memory partitions
void h_alloc_partition(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t type = ctx.gpr[6];
    const std::uint32_t size = ctx.gpr[7];
    const std::uint32_t arg = ctx.gpr[8];
    std::uint32_t addr = 0u;
    switch (type) {
    case 0u: addr = k.heap.alloc_low(size, 0x100u); break;
    case 1u: addr = k.heap.alloc_high(size, 0x100u); break;
    case 2u: addr = k.heap.alloc_at(arg, size); break;
    case 3u: addr = k.heap.alloc_low(size, arg); break;
    case 4u: addr = k.heap.alloc_high(size, arg); break;
    default: break;
    }
    if (addr == 0u) {
        if (k.cfg.trace)
            std::cerr << "[kernel] AllocPartitionMemory failed: size=0x" << std::hex << size << " type=" << std::dec
                      << type << " free=0x" << std::hex << k.heap.total_free() << std::dec << "\n";
        ret(ctx, kErrNoMemory);
        return;
    }
    MemBlock b;
    b.addr = addr;
    b.size = Heap::round(size);
    b.name = k.read_cstr(ctx.gpr[5]);
    if (k.cfg.trace)
        std::cerr << "[kernel] alloc '" << b.name << "' addr=0x" << std::hex << addr << " size=0x" << b.size
                  << std::dec << " type=" << type << "\n";
    const std::int32_t uid = k.new_uid();
    k.blocks.emplace(uid, std::move(b));
    ret(ctx, static_cast<std::uint32_t>(uid));
}

void h_free_partition(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const auto it = k.blocks.find(static_cast<std::int32_t>(ctx.gpr[4]));
    if (it == k.blocks.end()) {
        ret(ctx, kErrGeneric);
        return;
    }
    k.heap.release(it->second.addr, it->second.size);
    k.blocks.erase(it);
    ret(ctx, 0u);
}

void h_get_block_head_addr(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const auto it = k.blocks.find(static_cast<std::int32_t>(ctx.gpr[4]));
    ret(ctx, it == k.blocks.end() ? 0u : it->second.addr);
}

// ======================================================= time (UtilsForUser, sceRtc basics)
void h_libc_clock(Runtime &, AllegrexContext &ctx) {
    ret(ctx, static_cast<std::uint32_t>(K().now_us()));
}
void h_libc_time(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t secs = static_cast<std::uint32_t>(std::time(nullptr));
    if (ctx.gpr[4] != 0u) K().wr32(ctx.gpr[4], secs);
    ret(ctx, secs);
}
void h_libc_gettimeofday(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    if (ctx.gpr[4] != 0u) {
        k.wr32(ctx.gpr[4], static_cast<std::uint32_t>(us / 1'000'000));
        k.wr32(ctx.gpr[4] + 4u, static_cast<std::uint32_t>(us % 1'000'000));
    }
    ret(ctx, 0u);
}

// ======================================================= display (timing parts only)
void h_register_subintr(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    if (ctx.gpr[4] == 30u) {
        auto &e = k.vblank_subintr[ctx.gpr[5]];
        e.handler = ctx.gpr[6];
        e.arg = ctx.gpr[7];
        e.enabled = false;
    }
    ret(ctx, 0u);
}
void h_release_subintr(Runtime &, AllegrexContext &ctx) {
    if (ctx.gpr[4] == 30u) K().vblank_subintr.erase(ctx.gpr[5]);
    ret(ctx, 0u);
}
void h_enable_subintr(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    if (ctx.gpr[4] == 30u) {
        const auto it = k.vblank_subintr.find(ctx.gpr[5]);
        if (it != k.vblank_subintr.end()) it->second.enabled = true;
    }
    ret(ctx, 0u);
}
void h_disable_subintr(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    if (ctx.gpr[4] == 30u) {
        const auto it = k.vblank_subintr.find(ctx.gpr[5]);
        if (it != k.vblank_subintr.end()) it->second.enabled = false;
    }
    ret(ctx, 0u);
}

void h_wait_vblank(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    ++g_prof_vblank;
    ret(ctx, 0u);
    k.block_until(ctx, Wait::Vblank, k.next_vblank());
}
void h_display_is_vblank(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }
void h_display_get_vcount(Runtime &, AllegrexContext &ctx) {
    ret(ctx, static_cast<std::uint32_t>(K().now_us() / kVblankUs));
}
void h_display_get_fps(Runtime &, AllegrexContext &ctx) {
    ctx.set_fpr_bits(0u, std::bit_cast<std::uint32_t>(59.940060f));
}
void h_display_set_mode(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }
void h_display_set_framebuf(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    ++g_prof_setfb;
    k.fb_addr = ctx.gpr[4];
    k.fb_width = ctx.gpr[5];
    k.fb_pixfmt = ctx.gpr[6];
    gpu_present(*k.rt, k.fb_addr, k.fb_width, k.fb_pixfmt);
    ret(ctx, 0u);
}

// ======================================================= controller, UMD, audio pacing, misc
void h_ctrl_read(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t buf = ctx.gpr[4];
    if (buf != 0u) {
        const auto m = k.rt->memory().aot_fast_view();
        m.aot_store32(buf + 0u, static_cast<std::uint32_t>(k.now_us()));
        std::uint8_t lx = 128, ly = 128;
        const std::uint32_t buttons = input_poll(&lx, &ly);
        rewards_tick();
        m.aot_store32(buf + 4u, buttons);
        m.aot_store8(buf + 8u, lx);
        m.aot_store8(buf + 9u, ly);
        for (std::uint32_t i = 10u; i < 16u; ++i) m.aot_store8(buf + i, 0u);
    }
    ret(ctx, 1u);
}

void h_umd_check_medium(Runtime &, AllegrexContext &ctx) { ret(ctx, 1u); }
// PSP_UMD_PRESENT (0x02) | PSP_UMD_INITED (0x10) | PSP_UMD_READY (0x20)
void h_umd_drive_stat(Runtime &, AllegrexContext &ctx) { ret(ctx, 0x32u); }

void h_exit_game(Runtime &rt, AllegrexContext &) { rt.stop("sceKernelExitGame called"); }

void h_rtc_local_time(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t out = ctx.gpr[4];
    if (out != 0u) {
        const std::time_t now = std::time(nullptr);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        const auto m = k.rt->memory().aot_fast_view();
        m.aot_store16(out + 0u, static_cast<std::uint16_t>(lt.tm_year + 1900));
        m.aot_store16(out + 2u, static_cast<std::uint16_t>(lt.tm_mon + 1));
        m.aot_store16(out + 4u, static_cast<std::uint16_t>(lt.tm_mday));
        m.aot_store16(out + 6u, static_cast<std::uint16_t>(lt.tm_hour));
        m.aot_store16(out + 8u, static_cast<std::uint16_t>(lt.tm_min));
        m.aot_store16(out + 10u, static_cast<std::uint16_t>(lt.tm_sec));
        m.aot_store32(out + 12u, 0u);
    }
    ret(ctx, 0u);
}

std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2u ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// sceRtcGetTick(const pspTime *time, u64 *tick): ticks are microseconds since 0001-01-01.
void h_rtc_get_tick(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t in = ctx.gpr[4];
    const std::uint32_t out = ctx.gpr[5];
    if (in == 0u || out == 0u) {
        ret(ctx, 0x80010016u);
        return;
    }
    const auto m = k.rt->memory().aot_fast_view();
    const std::int64_t year = m.aot_load16(in + 0u);
    const unsigned month = m.aot_load16(in + 2u);
    const unsigned day = m.aot_load16(in + 4u);
    const std::int64_t hour = m.aot_load16(in + 6u);
    const std::int64_t minute = m.aot_load16(in + 8u);
    const std::int64_t second = m.aot_load16(in + 10u);
    const std::int64_t micro = m.aot_load32(in + 12u);
    const std::int64_t days = days_from_civil(year, month == 0u ? 1u : month, day == 0u ? 1u : day);
    const std::int64_t secs = days * 86400 + hour * 3600 + minute * 60 + second;
    const std::int64_t tick = secs * 1'000'000 + micro + 62135596800000000ll;
    k.wr32(out, static_cast<std::uint32_t>(static_cast<std::uint64_t>(tick) & 0xFFFFFFFFu));
    k.wr32(out + 4u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(tick) >> 32u));
    ret(ctx, 0u);
}

// ======================================================= sceGe_user (bring-up only)
// Display lists are accepted and reported as already finished. Nothing is drawn yet;
// the command interpreter and renderer come next. The finish callback is still
// delivered so the game's own synchronization keeps working.
void h_ge_edram_addr(Runtime &, AllegrexContext &ctx) { ret(ctx, 0x04000000u); }

void h_ge_set_callback(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::uint32_t p = ctx.gpr[4];
    GeCallback cb;
    if (p != 0u) {
        cb.signal_func = k.rd32(p + 0u);
        cb.signal_arg = k.rd32(p + 4u);
        cb.finish_func = k.rd32(p + 8u);
        cb.finish_arg = k.rd32(p + 12u);
    }
    const std::int32_t id = k.next_ge_cb++;
    k.ge_callbacks[id] = cb;
    if (k.cfg.trace)
        std::cerr << "[ge] callback " << id << " signal=0x" << std::hex << cb.signal_func << " finish=0x"
                  << cb.finish_func << std::dec << "\n";
    ret(ctx, static_cast<std::uint32_t>(id));
}

void h_ge_unset_callback(Runtime &, AllegrexContext &ctx) {
    K().ge_callbacks.erase(static_cast<std::int32_t>(ctx.gpr[4]));
    ret(ctx, 0u);
}

void h_ge_enqueue(Runtime &, AllegrexContext &ctx) {
    Kernel &k = K();
    const std::int32_t list = k.next_ge_list++;
    if (k.cfg.trace && list <= 20)
        std::cerr << "[ge] enqueue list=0x" << std::hex << ctx.gpr[4] << " stall=0x" << ctx.gpr[5] << std::dec
                  << " cb=" << static_cast<std::int32_t>(ctx.gpr[6]) << " id=" << list << "\n";
    ret(ctx, static_cast<std::uint32_t>(list));
    ge_list_start(*k.rt, ctx.gpr[4], ctx.gpr[5], list);
    const auto it = k.ge_callbacks.find(static_cast<std::int32_t>(ctx.gpr[6]));
    if (it != k.ge_callbacks.end() && it->second.finish_func != 0u)
        k.spawn_callback_thread("ge_finish", it->second.finish_func, static_cast<std::uint32_t>(list),
                                it->second.finish_arg);
    k.maybe_preempt(ctx);
}

void h_ge_update_stall(Runtime &, AllegrexContext &ctx) {
    ge_list_update(*K().rt, static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5]);
    ret(ctx, 0u);
}

struct HleDef {
    const char *library;
    std::uint32_t nid;
    void (*fn)(Runtime &, AllegrexContext &);
};

const HleDef kDefs[] = {
    // ThreadManForUser
    {"ThreadManForUser", 0x446D8DE6u, h_create_thread},
    {"InterruptManager", 0xCA04A2B9u, h_register_subintr},
    {"InterruptManager", 0xD61E6961u, h_release_subintr},
    {"InterruptManager", 0xFB8E22ECu, h_enable_subintr},
    {"InterruptManager", 0x8A389411u, h_disable_subintr},
    {"ThreadManForUser", 0xF475845Du, h_start_thread},
    {"ThreadManForUser", 0xAA73C935u, h_exit_thread},
    {"ThreadManForUser", 0x809CE29Bu, h_exit_delete_thread},
    {"ThreadManForUser", 0x383F7BCCu, h_terminate_delete_thread},
    {"ThreadManForUser", 0x9FA03CD3u, h_delete_thread},
    {"ThreadManForUser", 0x278C0DF5u, h_wait_thread_end},
    {"ThreadManForUser", 0x840E8133u, h_wait_thread_end},
    {"ThreadManForUser", 0x293B45B8u, h_get_thread_id},
    {"ThreadManForUser", 0x71BC9871u, h_change_thread_priority},
    {"ThreadManForUser", 0xCEADEB47u, h_delay_thread},
    {"ThreadManForUser", 0x68DA9E36u, h_delay_thread},
    {"ThreadManForUser", 0x369ED59Du, h_get_system_time_low},
    {"ThreadManForUser", 0xD6DA4BA1u, h_create_sema},
    {"ThreadManForUser", 0x28B6489Cu, h_delete_sema},
    {"ThreadManForUser", 0x3F53E640u, h_signal_sema},
    {"ThreadManForUser", 0x4E3A1105u, h_wait_sema},
    {"ThreadManForUser", 0x6D212BACu, h_wait_sema},
    {"ThreadManForUser", 0x55C20A00u, h_create_evf},
    {"ThreadManForUser", 0xEF9E4C70u, h_delete_evf},
    {"ThreadManForUser", 0x1FB15A32u, h_set_evf},
    {"ThreadManForUser", 0x812346E4u, h_clear_evf},
    {"ThreadManForUser", 0x402FCF22u, h_wait_evf},
    {"ThreadManForUser", 0x30FD48F0u, h_poll_evf},
    {"ThreadManForUser", 0xA66B0120u, h_refer_evf},
    {"ThreadManForUser", 0x19CFF145u, h_create_lwmutex},
    {"ThreadManForUser", 0x60107536u, h_delete_lwmutex},
    {"ThreadManForUser", 0xE81CAF8Fu, h_create_callback},
    {"ThreadManForUser", 0xEDBA5844u, h_delete_callback},
    {"ThreadManForUser", 0x349D6D6Cu, h_ret_zero},  // sceKernelCheckCallback: no callbacks pending
    // Kernel_Library
    {"Kernel_Library", 0xBEA46419u, h_lock_lwmutex},
    {"Kernel_Library", 0x15B6446Bu, h_unlock_lwmutex},
    {"Kernel_Library", 0x092968F4u, h_cpu_suspend_intr},  // believed to be sceKernelCpuSuspendIntr
    {"Kernel_Library", 0x5F10D406u, h_ret_zero},          // sceKernelCpuResumeIntr
    // ModuleMgrForUser
    {"ModuleMgrForUser", 0xD8B73127u, h_get_module_id},  // sceKernelGetModuleIdByAddress
    {"ModuleMgrForUser", 0xF0A26395u, h_get_module_id},  // sceKernelGetModuleId
    // SysMemUserForUser
    {"SysMemUserForUser", 0x91DE343Cu, h_ret_zero},  // SetCompiledSdkVersion500_505
    {"SysMemUserForUser", 0xF77D77CBu, h_ret_zero},  // SetCompilerVersion
    {"SysMemUserForUser", 0x237DBD4Fu, h_alloc_partition},
    {"SysMemUserForUser", 0xB6D61D02u, h_free_partition},
    {"SysMemUserForUser", 0x9D9A5BA1u, h_get_block_head_addr},
    // UtilsForUser
    {"UtilsForUser", 0x79D1C3FAu, h_ret_zero},  // DcacheWritebackAll
    {"UtilsForUser", 0x3EE30821u, h_ret_zero},  // DcacheWritebackRange
    {"UtilsForUser", 0x91E4F6A7u, h_libc_clock},
    {"UtilsForUser", 0x27CC57F0u, h_libc_time},
    {"UtilsForUser", 0x71EC4271u, h_libc_gettimeofday},
    // sceSuspendForUser
    {"sceSuspendForUser", 0x090CCB3Fu, h_ret_zero},  // sceKernelPowerTick
    // sceImpose, scePower, LoadExecForUser
    {"sceImpose", 0x36AA6E91u, h_ret_zero},         // SetLanguageMode
    {"scePower", 0x04B7766Eu, h_ret_zero},          // RegisterCallback
    {"LoadExecForUser", 0x4AC57943u, h_ret_zero},   // RegisterExitCallback
    {"LoadExecForUser", 0x05572A5Fu, h_exit_game},  // sceKernelExitGame
    // sceCtrl (no input yet: no buttons, stick centered)
    {"sceCtrl", 0x6A2774F3u, h_ret_zero},  // SetSamplingCycle
    {"sceCtrl", 0x1F4011E6u, h_ret_zero},  // SetSamplingMode
    {"sceCtrl", 0x1F803938u, h_ctrl_read},  // ReadBufferPositive
    {"sceCtrl", 0x3A622550u, h_ctrl_read},  // PeekBufferPositive
    // sceUmdUser (the disc is always present and ready)
    {"sceUmdUser", 0x46EBB729u, h_umd_check_medium},
    {"sceUmdUser", 0xC6183D47u, h_ret_zero},  // Activate
    {"sceUmdUser", 0xE83742BAu, h_ret_zero},  // Deactivate
    {"sceUmdUser", 0x6B4A146Cu, h_umd_drive_stat},
    {"sceUmdUser", 0x8EF08FCEu, h_ret_zero},  // WaitDriveStat
    {"sceUmdUser", 0x4A9E5E29u, h_ret_zero},  // WaitDriveStatCB
    {"sceUmdUser", 0x6AF9B50Au, h_ret_zero},  // CancelWaitDriveStat
    // sceRtc
    {"sceRtc", 0xE7C27D1Bu, h_rtc_local_time},  // GetCurrentClockLocalTime
    {"sceRtc", 0x6FF40ACCu, h_rtc_get_tick},    // GetTick
    // sceGe_user (lists finish instantly; nothing is drawn yet)
    {"sceGe_user", 0xE47E40E4u, h_ge_edram_addr},
    {"sceGe_user", 0xA4FC06A4u, h_ge_set_callback},
    {"sceGe_user", 0x05DB22CEu, h_ge_unset_callback},
    {"sceGe_user", 0xAB49E76Au, h_ge_enqueue},       // sceGeListEnQueue
    {"sceGe_user", 0x1C0D95A6u, h_ge_enqueue},       // sceGeListEnQueueHead
    {"sceGe_user", 0x03444EB4u, h_ret_zero},         // sceGeListSync: list is done
    {"sceGe_user", 0xB287BD61u, h_ret_zero},         // sceGeDrawSync: nothing pending
    {"sceGe_user", 0xE0D68148u, h_ge_update_stall},         // sceGeListUpdateStallAddr
    {"sceGe_user", 0xB448EC0Du, h_ret_zero},         // sceGeBreak
    {"sceGe_user", 0x4C06E472u, h_ret_zero},         // sceGeContinue
    // sceDisplay (timing only; presenting frames comes with the renderer)
    {"sceDisplay", 0x46F186C3u, h_wait_vblank},  // WaitVblankStartCB
    {"sceDisplay", 0x8EB9EC49u, h_wait_vblank},  // WaitVblankCB
    {"sceDisplay", 0x4D4E10ECu, h_display_is_vblank},
    {"sceDisplay", 0x9C6EAAD7u, h_display_get_vcount},
    {"sceDisplay", 0xDBA6C4C4u, h_display_get_fps},
    {"sceDisplay", 0x0E20F177u, h_display_set_mode},
    {"sceDisplay", 0x289D82FEu, h_display_set_framebuf},
};

// Runs at an outer-dispatch boundary. Handles timers and priority preemption for
// guest code that never calls into the kernel.
int g_inline_depth = 0;  // >0 while a host handler runs guest code in the middle of an import

void starvation_hook(Runtime &, AllegrexContext &ctx) {
    if (g_inline_depth > 0) return;  // never switch threads in the middle of a nested guest call
    Kernel &k = K();
    static unsigned tick = 0;
    if ((++tick & 0x3FFu) == 0u && debug_key_pressed()) {
        std::cerr << "[debug] F11: running thread " << k.current << " pc=0x" << std::hex << ctx.pc << " ra=0x"
                  << ctx.gpr[31] << std::dec << "\n";
        kernel_report();
    }
    if (k.current < 0) return;
    const std::int32_t next = k.choose_next();
    if (next < 0 || next == k.current) return;
    k.switch_to(next, ctx, ctx.pc, true);
}

} // namespace

void install_kernel(Runtime &rt, const KernelConfig &cfg) {
    static Kernel kernel;
    g_kernel = &kernel;
    kernel.rt = &rt;
    kernel.cfg = cfg;
    kernel.heap.init(cfg.heap_start, cfg.heap_end);
    kernel.t0 = Clock::now();

    for (const HleDef &d : kDefs) {
        rt.register_hle(d.library, d.nid, d.fn);
        handled_set().insert(hle_key(d.library, d.nid));
    }

    // The thread that runs the module's start function.
    Thread t;
    t.uid = kernel.new_uid();
    t.name = "module_start";
    t.priority = 32;
    t.stack_size = 0x10000u;
    t.stack_addr = kernel.heap.alloc_high(t.stack_size, 0x100u);
    t.state = State::Running;
    t.ready_seq = ++kernel.seq;
    AllegrexContext &cpu = rt.cpu();
    cpu.eat_vfpu_prefixes();
    cpu.set_gpr(28, cfg.gp);
    const std::uint32_t k0 = kernel.make_k0_block(t);
    cpu.set_gpr(26, k0);
    cpu.set_gpr(29, k0 - 0x40u);
    cpu.set_gpr(31, cfg.exit_stub);
    cpu.set_gpr(4, 0u);
    cpu.set_gpr(5, 0u);
    kernel.current = t.uid;
    psprecomp::set_runtime_thread_identity(t.uid, t.name);
    kernel.threads.emplace(t.uid, std::move(t));

    if (cfg.tick_interval != 0u) psprecomp::set_runtime_starvation_hook(&starvation_hook, cfg.tick_interval);
}

bool kernel_handles(const std::string &library, std::uint32_t nid) {
    return handled_set().count(hle_key(library, nid)) != 0u;
}

bool kernel_call_guest_inline(psprecomp::Runtime &rt, AllegrexContext &ctx, std::uint32_t entry, std::uint32_t a0,
                              std::uint32_t a1, std::uint32_t a2, std::uint32_t &result) {
    if (g_kernel == nullptr) return false;
    AllegrexContext c = ctx;  // same thread, same k0 / gp
    c.gpr[4] = a0;
    c.gpr[5] = a1;
    c.gpr[6] = a2;
    c.gpr[29] = (ctx.gpr[29] - 0x100u) & ~0xFu;  // below the caller's live stack
    c.gpr[31] = g_kernel->cfg.exit_stub;          // returning there ends the nested call
    ++g_inline_depth;
    const bool ok = rt.invoke_isolated_aot(entry, c);
    --g_inline_depth;
    if (!ok) return false;
    result = c.gpr[2];
    return true;
}

bool kernel_call_guest(AllegrexContext &ctx, std::uint32_t entry, std::uint32_t a0, std::uint32_t a1,
                       std::uint32_t a2, std::function<std::uint32_t(std::uint32_t)> on_done) {
    if (g_kernel == nullptr) return false;
    Kernel &k = *g_kernel;
    const std::int32_t uid = k.spawn_call_thread("host_call", entry, a0, a1, a2, ctx.gpr[26], std::move(on_done));
    if (uid < 0) return false;
    k.exclusive = uid;
    k.exclusive_caller = k.current;
    k.block_current(ctx, Wait::ThreadEnd, uid, 0u, 0u, 0u, 0u);
    return true;
}

void kernel_sleep(AllegrexContext &ctx, std::uint32_t us) {
    if (g_kernel == nullptr) return;
    g_kernel->block_until(ctx, Wait::Delay, g_kernel->now_us() + us);
}

void kernel_report() {
    if (g_kernel == nullptr) return;
    Kernel &k = *g_kernel;
    std::cerr << "[kernel] threads:\n";
    for (const auto &entry : k.threads) {
        const Thread &t = entry.second;
        std::cerr << "  uid=0x" << std::hex << t.uid << std::dec << " '" << t.name << "' " << state_name(t.state)
                  << " prio=" << t.priority;
        if (t.state == State::Waiting) {
            std::cerr << " waiting on " << wait_name(t.wait) << " obj=0x" << std::hex << t.wait_obj << std::dec;
            if (t.has_deadline) std::cerr << " (has timeout)";
        }
        if (t.state == State::Ready || t.state == State::Waiting)
            std::cerr << " pc=0x" << std::hex << t.ctx.pc << std::dec;
        std::cerr << "\n";
    }
    std::cerr << "[kernel] guest heap free: 0x" << std::hex << k.heap.total_free() << std::dec << " bytes\n";
}

} // namespace persona1
