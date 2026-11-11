// Persona 1 native port: sound.
//
//   sceAudio     -> per-channel FIFOs mixed by a device thread (Windows waveOut).
//   sceSasCore   -> software mixer, 32 voices, PSP-ADPCM / PCM / noise, ADSR envelopes.
//   sceAtrac3plus-> RIFF/AT3 parsing here, frame decoding by FFmpeg (PERSONA1_HAVE_FFMPEG).
//
// The game talks to the SAS mixer in "grains" (256 samples): it asks __sceSasCore for one
// grain of stereo s16 into a buffer and then hands that buffer to sceAudioOutputBlocking.
// Blocking output returns when the channel queue drops to one buffer, so the guest is paced
// by the sound device itself and cannot drift away from it.

#include "persona1_audio.hpp"
#include "persona1_kernel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#if defined(PERSONA1_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}
#endif

using psprecomp::AllegrexContext;
using psprecomp::Runtime;

namespace persona1 {
namespace {

constexpr int kRate = 44100;

// ------------------------------------------------------------------ guest memory access
Runtime *g_rt = nullptr;
bool g_log = false;

std::uint32_t rd32(std::uint32_t a) { return g_rt->memory().aot_fast_view().aot_load32(a); }
std::uint32_t rd16(std::uint32_t a) { return g_rt->memory().aot_fast_view().aot_load16(a); }
void wr32(std::uint32_t a, std::uint32_t v) {
    if (a != 0u) g_rt->memory().aot_fast_view().aot_store32(a, v);
}
std::uint8_t *host_ptr(std::uint32_t a, std::uint32_t len) {
    if (a == 0u || len == 0u) return nullptr;
    return g_rt->memory().raw_pointer(a, len);
}
void ret(AllegrexContext &ctx, std::uint32_t v) { ctx.set_gpr(2, v); }

// Logs the first few calls of each named function when PERSONA1_AUDIOLOG=1.
void log_call(const char *name, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0, std::uint32_t d = 0,
              std::uint32_t e = 0, std::uint32_t f = 0) {
    if (!g_log) return;
    static std::map<std::string, int> seen;
    if (++seen[name] > 6) return;
    std::fprintf(stderr, "[audio] %s(%08X, %08X, %08X, %08X, %08X, %08X)\n", name, a, b, c, d, e, f);
}

// ------------------------------------------------------------------ sceAudio channels + device
struct Channel {
    bool reserved{};
    std::uint32_t samples{256};
    bool mono{};
    int vol_l{0x8000}, vol_r{0x8000};
    std::deque<std::int16_t> q;  // interleaved stereo
    bool was_active{};           // has received data since it was reserved
};

std::mutex g_mu;
Channel g_ch[8];
bool g_device_ok = false;
float g_master = 1.0f;
std::atomic<std::uint64_t> g_pulled_frames{0};
std::uint64_t g_underruns = 0;
int g_peak = 0;  // loudest output sample since the last status line

// Mixes `frames` stereo frames from every channel into `out` (interleaved s16).
void pull(std::int16_t *out, int frames) {
    std::lock_guard<std::mutex> lock(g_mu);
    {
        // An underrun: a channel that was playing ran dry in the middle of this chunk.
        std::size_t most = 0;
        bool active = false;
        for (auto &c : g_ch) { most = std::max(most, c.q.size() / 2u); active = active || c.was_active; }
        if (active && most < static_cast<std::size_t>(frames)) ++g_underruns;
    }
    for (int i = 0; i < frames; ++i) {
        int l = 0, r = 0;
        for (auto &c : g_ch) {
            if (c.q.size() >= 2u) {
                l += c.q[0];
                r += c.q[1];
                c.q.pop_front();
                c.q.pop_front();
            }
        }
        l = static_cast<int>(static_cast<float>(l) * g_master);
        r = static_cast<int>(static_cast<float>(r) * g_master);
        out[i * 2] = static_cast<std::int16_t>(std::clamp(l, -32768, 32767));
        out[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(r, -32768, 32767));
        g_peak = std::max(g_peak, std::max(std::abs(out[i * 2]), std::abs(out[i * 2 + 1])));
    }
    g_pulled_frames += static_cast<std::uint64_t>(frames);
    if (g_log && g_pulled_frames % (static_cast<std::uint64_t>(kRate) * 5u) < static_cast<std::uint64_t>(frames))
    {
        std::fprintf(stderr, "[audio] underruns so far: %llu, output peak=%d (of 32767), master=%.2f\n",
                     static_cast<unsigned long long>(g_underruns), g_peak, static_cast<double>(g_master));
        g_peak = 0;
    }
}

#if defined(_WIN32)
class Device {
public:
    bool open() {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = kRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = kRate * 4;
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (waveOutOpen(&h_, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(event_), 0, CALLBACK_EVENT) !=
            MMSYSERR_NOERROR)
            return false;
        for (int i = 0; i < kBufs; ++i) {
            data_[i].assign(static_cast<std::size_t>(kFrames) * 2u, 0);
            hdr_[i] = WAVEHDR{};
            hdr_[i].lpData = reinterpret_cast<LPSTR>(data_[i].data());
            hdr_[i].dwBufferLength = static_cast<DWORD>(data_[i].size() * 2u);
            waveOutPrepareHeader(h_, &hdr_[i], sizeof(WAVEHDR));
            fill_and_write(i);
        }
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void close() {
        if (!run_.exchange(false)) return;
        if (event_) SetEvent(event_);
        if (th_.joinable()) th_.join();
        waveOutReset(h_);
        for (int i = 0; i < kBufs; ++i) waveOutUnprepareHeader(h_, &hdr_[i], sizeof(WAVEHDR));
        waveOutClose(h_);
        if (event_) CloseHandle(event_);
        event_ = nullptr;
    }
private:
    static constexpr int kBufs = 4;
    static constexpr int kFrames = 512;
    void fill_and_write(int i) {
        pull(data_[i].data(), kFrames);
        hdr_[i].dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
        waveOutWrite(h_, &hdr_[i], sizeof(WAVEHDR));
    }
    void loop() {
        while (run_) {
            for (int i = 0; i < kBufs; ++i)
                if (hdr_[i].dwFlags & WHDR_DONE) fill_and_write(i);
            WaitForSingleObject(event_, 20);
        }
    }
    HWAVEOUT h_{};
    HANDLE event_{};
    WAVEHDR hdr_[kBufs]{};
    std::vector<std::int16_t> data_[kBufs];
    std::thread th_;
    std::atomic<bool> run_{false};
};
Device g_device;
#endif

// How long the calling guest thread should wait after queueing data on `c`:
// until no more than one buffer of it is still waiting for the device.
std::uint32_t pacing_us(const Channel &c, std::uint32_t buffer_samples) {
    if (!g_device_ok) return static_cast<std::uint32_t>(buffer_samples * 1000000ull / kRate);
    // The device takes data in chunks, so keep about 1024 frames queued beyond the buffer being played.
    const std::size_t queued = c.q.size() / 2u;
    const std::size_t target = buffer_samples + 1024u;
    if (queued <= target) return 0u;
    const std::uint64_t us = (queued - target) * 1000000ull / kRate;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(us, 250000ull));
}

void push_pcm(Channel &c, const std::int16_t *src, std::uint32_t frames, int vl, int vr) {
    if (!g_device_ok) return;  // nobody would drain it
    c.was_active = true;
    for (std::uint32_t i = 0; i < frames; ++i) {
        int l, r;
        if (c.mono) l = r = src[i];
        else { l = src[i * 2]; r = src[i * 2 + 1]; }
        l = l * vl / 0x8000;
        r = r * vr / 0x8000;
        c.q.push_back(static_cast<std::int16_t>(std::clamp(l, -32768, 32767)));
        c.q.push_back(static_cast<std::int16_t>(std::clamp(r, -32768, 32767)));
    }
    // Never let a stalled device build up more than ~0.75 s of delay.
    const std::size_t cap = static_cast<std::size_t>(kRate) * 3u / 4u * 2u;
    while (c.q.size() > cap) c.q.pop_front();
}

// ---- sceAudio handlers
void h_audio_ch_reserve(Runtime &, AllegrexContext &ctx) {
    std::int32_t ch = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t samples = ctx.gpr[5];
    const std::uint32_t format = ctx.gpr[6];
    std::lock_guard<std::mutex> lock(g_mu);
    if (ch < 0) {
        for (int i = 0; i < 8; ++i)
            if (!g_ch[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch > 7) { ret(ctx, 0x80260002u); return; }
    Channel &c = g_ch[ch];
    c.reserved = true;
    c.samples = std::max<std::uint32_t>(samples, 1u);
    c.mono = (format & 0x10u) != 0u;
    c.vol_l = c.vol_r = 0x8000;
    c.q.clear();
    log_call("sceAudioChReserve", static_cast<std::uint32_t>(ch), samples, format);
    ret(ctx, static_cast<std::uint32_t>(ch));
}
void h_audio_ch_release(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t ch = ctx.gpr[4];
    if (ch < 8u) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_ch[ch].reserved = false;
        g_ch[ch].q.clear();
    }
    ret(ctx, 0u);
}
void h_audio_set_len(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t ch = ctx.gpr[4];
    if (ch < 8u) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_ch[ch].samples = std::max<std::uint32_t>(ctx.gpr[5], 1u);
    }
    ret(ctx, 0u);
}
void h_audio_change_config(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t ch = ctx.gpr[4];
    if (ch < 8u) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_ch[ch].mono = (ctx.gpr[5] & 0x10u) != 0u;
    }
    ret(ctx, 0u);
}
void h_audio_change_volume(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t ch = ctx.gpr[4];
    if (ch < 8u) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_ch[ch].vol_l = static_cast<int>(std::min<std::uint32_t>(ctx.gpr[5], 0x8000u));
        g_ch[ch].vol_r = static_cast<int>(std::min<std::uint32_t>(ctx.gpr[6], 0x8000u));
    }
    ret(ctx, 0u);
}
void h_audio_rest_length(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t ch = ctx.gpr[4];
    std::uint32_t n = 0;
    if (ch < 8u) {
        std::lock_guard<std::mutex> lock(g_mu);
        n = static_cast<std::uint32_t>(g_ch[ch].q.size() / 2u);
    }
    ret(ctx, n);
}

// mode: 0 = OutputBlocking(ch, vol, buf), 1 = OutputPannedBlocking(ch, l, r, buf), 2 = OutputPanned
template <int Mode>
void h_audio_output(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t ch = ctx.gpr[4];
    if (ch >= 8u) { ret(ctx, 0x80260002u); return; }
    int vl, vr;
    std::uint32_t buf;
    if (Mode == 0) {
        vl = vr = static_cast<int>(std::min<std::uint32_t>(ctx.gpr[5], 0x8000u));
        buf = ctx.gpr[6];
    } else {
        vl = static_cast<int>(std::min<std::uint32_t>(ctx.gpr[5], 0x8000u));
        vr = static_cast<int>(std::min<std::uint32_t>(ctx.gpr[6], 0x8000u));
        buf = ctx.gpr[7];
    }
    std::uint32_t wait_us = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Channel &c = g_ch[ch];
        if (!c.reserved) { ret(ctx, 0x80260001u); return; }
        const std::uint32_t frames = c.samples;
        const std::uint32_t bytes = frames * (c.mono ? 2u : 4u);
        if (const std::uint8_t *p = host_ptr(buf, bytes)) {
            std::vector<std::int16_t> tmp(frames * (c.mono ? 1u : 2u));
            std::memcpy(tmp.data(), p, bytes);
            push_pcm(c, tmp.data(), frames, vl * c.vol_l / 0x8000, vr * c.vol_r / 0x8000);
        }
        if (Mode != 2) wait_us = pacing_us(c, frames);
    }
    ret(ctx, 0u);
    if (Mode != 2) kernel_sleep(ctx, std::max<std::uint32_t>(wait_us, 1u));
}

// ------------------------------------------------------------------ SAS
constexpr int kVoices = 32;

struct Voice {
    enum Kind { None, Vag, Pcm, Noise } kind{None};
    // source
    std::vector<std::int16_t> pcm;  // decoded source
    std::size_t loop_start{0};
    bool loop{};
    bool vag_repeat{true};  // VAG end block carries the repeat flag (bit 1)
    double pos{};
    double step{1.0};
    int pitch{0x1000};
    int vol_l{0x1000}, vol_r{0x1000};
    // state
    bool on{};
    bool ended{};
    bool paused{};
    bool keyed_off{};
    std::uint32_t noise_lfsr{0x1u};
    int noise_clock{0};
    float noise_cur{};
    double noise_acc{};
    // envelope: either PS-style (simple ADSR) or time based
    enum Phase { Attack, Decay, Sustain, Release, Off } phase{Off};
    bool simple{false};
    std::uint16_t adsr1{0x80ff}, adsr2{0x1fee};
    float time_a{0.f}, time_d{0.f}, time_r{0.010f};
    float sustain{1.0f};
    int level{0};         // PS-style level 0..0x7FFF
    int env_counter{0};
    float env{0.f};       // current envelope 0..1
};

struct Sas {
    bool inited{};
    std::uint32_t core{};
    std::uint32_t grain{256};
    std::uint32_t outmode{0};
    Voice v[kVoices];
};
Sas g_sas;

// ---- PSP ADPCM ("VAG") to PCM. 16-byte blocks: [shift|filter<<4][flags][14 data bytes].
void decode_vag(const std::uint8_t *data, std::uint32_t size, Voice &v) {
    static const int f0[5] = {0, 60, 115, 98, 122};
    static const int f1[5] = {0, 0, -52, -55, -60};
    v.pcm.clear();
    v.loop_start = 0;
    v.vag_repeat = false;
    if (size >= 0x30u && std::memcmp(data, "VAGp", 4) == 0) { data += 0x30; size -= 0x30; }
    int h1 = 0, h2 = 0;
    bool have_loop_start = false;
    const std::uint32_t blocks = size / 16u;
    v.pcm.reserve(static_cast<std::size_t>(blocks) * 28u);
    for (std::uint32_t b = 0; b < blocks; ++b) {
        const std::uint8_t *p = data + b * 16u;
        const int shift = p[0] & 0xF;
        const int filter = std::min<int>(p[0] >> 4, 4);
        const int flags = p[1];
        if ((flags & 4) != 0 && !have_loop_start) {
            v.loop_start = v.pcm.size();
            have_loop_start = true;
        }
        if (flags == 7) break;  // end marker block without data
        for (int i = 0; i < 28; ++i) {
            int nib = (p[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
            int s = static_cast<std::int16_t>(nib << 12) >> shift;
            s += (h1 * f0[filter] + h2 * f1[filter]) >> 6;
            s = std::clamp(s, -32768, 32767);
            h2 = h1;
            h1 = s;
            v.pcm.push_back(static_cast<std::int16_t>(s));
        }
        if (flags & 1) {  // end of sample; only flag 3 means "jump back to the loop start"
            v.vag_repeat = (flags & 2) != 0;
            break;
        }
    }
}

void start_envelope(Voice &v) {
    v.phase = Voice::Attack;
    v.env_counter = 0;
    if (v.simple) {
        v.level = 0;
        v.env = 0.f;
    } else {
        v.env = v.time_a <= 0.f ? 1.f : 0.f;
        if (v.time_a <= 0.f) v.phase = Voice::Decay;
        if (v.phase == Voice::Decay && v.time_d <= 0.f) {
            v.env = v.sustain;
            v.phase = Voice::Sustain;
        }
    }
}

// One envelope step per output sample.
void step_envelope(Voice &v) {
    if (v.phase == Voice::Off) return;
    if (!v.simple) {
        const float dt = 1.f / kRate;
        switch (v.phase) {
        case Voice::Attack:
            v.env += dt / std::max(v.time_a, 0.0005f);
            if (v.env >= 1.f) { v.env = 1.f; v.phase = Voice::Decay; }
            break;
        case Voice::Decay:
            v.env -= (1.f - v.sustain) * dt / std::max(v.time_d, 0.0005f);
            if (v.env <= v.sustain) { v.env = v.sustain; v.phase = Voice::Sustain; }
            break;
        case Voice::Sustain: break;
        case Voice::Release:
            v.env -= dt / std::max(v.time_r, 0.0005f);
            if (v.env <= 0.f) { v.env = 0.f; v.phase = Voice::Off; v.on = false; v.ended = true; }
            break;
        default: break;
        }
        return;
    }
    // PS1-style envelope driven by the packed ADSR1/ADSR2 words.
    auto run = [&](bool exp, bool decrease, int shift, int stepv) {
        int cycles = 1 << std::max(0, shift - 11);
        int inc = (decrease ? (-8 + stepv) : (7 - stepv)) << std::max(0, 11 - shift);
        if (exp && !decrease && v.level > 0x6000) cycles *= 4;
        if (exp && decrease) inc = inc * v.level / 0x8000;
        if (++v.env_counter < cycles) return;
        v.env_counter = 0;
        v.level = std::clamp(v.level + inc, 0, 0x7FFF);
    };
    switch (v.phase) {
    case Voice::Attack:
        run((v.adsr1 & 0x8000) != 0, false, (v.adsr1 >> 10) & 0x1F, (v.adsr1 >> 8) & 3);
        if (v.level >= 0x7FFF) v.phase = Voice::Decay;
        break;
    case Voice::Decay: {
        run(true, true, (v.adsr1 >> 4) & 0xF, 0);
        const int sl = ((v.adsr1 & 0xF) + 1) * 0x800;
        if (v.level <= sl) v.phase = Voice::Sustain;
        break;
    }
    case Voice::Sustain:
        run((v.adsr2 & 0x8000) != 0, (v.adsr2 & 0x4000) != 0, (v.adsr2 >> 8) & 0x1F, (v.adsr2 >> 6) & 3);
        break;
    case Voice::Release:
        run((v.adsr2 & 0x20) != 0, true, v.adsr2 & 0x1F, 0);
        if (v.level <= 0) { v.phase = Voice::Off; v.on = false; v.ended = true; }
        break;
    default: break;
    }
    v.env = static_cast<float>(v.level) / 32767.f;
}

float next_sample(Voice &v) {
    if (v.kind == Voice::Noise) {
        v.noise_acc += 1.0;
        const double period = std::max(1.0, 44100.0 / (v.noise_clock * 2 + 100.0));
        if (v.noise_acc >= period) {
            v.noise_acc -= period;
            const std::uint32_t bit = ((v.noise_lfsr >> 0) ^ (v.noise_lfsr >> 1) ^ (v.noise_lfsr >> 21) ^
                                       (v.noise_lfsr >> 31)) & 1u;
            v.noise_lfsr = (v.noise_lfsr << 1) | bit;
            v.noise_cur = (static_cast<float>(v.noise_lfsr & 0xFFFFu) - 32768.f);
        }
        return v.noise_cur;
    }
    if (v.pcm.empty()) { v.on = false; v.ended = true; return 0.f; }
    const std::size_t n = v.pcm.size();
    std::size_t i = static_cast<std::size_t>(v.pos);
    if (i >= n) {
        if (v.loop && (v.kind != Voice::Vag || v.vag_repeat)) {
            const std::size_t ls = std::min(v.loop_start, n - 1u);
            v.pos = static_cast<double>(ls) + (v.pos - static_cast<double>(n));
            if (v.pos >= static_cast<double>(n) || v.pos < static_cast<double>(ls)) v.pos = static_cast<double>(ls);
            i = static_cast<std::size_t>(v.pos);
        } else {
            v.on = false;
            v.ended = true;
            v.phase = Voice::Off;
            return 0.f;
        }
    }
    const float frac = static_cast<float>(v.pos - static_cast<double>(i));
    const float a = v.pcm[i];
    std::size_t j = i + 1;
    if (j >= n) j = (v.loop && (v.kind != Voice::Vag || v.vag_repeat)) ? std::min(v.loop_start, n - 1u) : i;
    const float b = v.pcm[j];
    v.pos += v.step;
    return a + (b - a) * frac;
}

// Renders one grain into `out` (interleaved s16). If `mix` the existing buffer is kept.
void sas_render(std::int16_t *out, std::uint32_t grain, bool mix, int mix_l, int mix_r) {
    std::vector<float> acc(static_cast<std::size_t>(grain) * 2u, 0.f);
    if (mix) {
        for (std::uint32_t i = 0; i < grain; ++i) {
            acc[i * 2] = out[i * 2] * (mix_l / 4096.f);
            acc[i * 2 + 1] = out[i * 2 + 1] * (mix_r / 4096.f);
        }
    }
    for (auto &v : g_sas.v) {
        if (!v.on || v.paused) continue;
        for (std::uint32_t i = 0; i < grain && v.on; ++i) {
            const float s = next_sample(v);
            step_envelope(v);
            const float e = v.env;
            acc[i * 2] += s * e * (v.vol_l / 4096.f);
            acc[i * 2 + 1] += s * e * (v.vol_r / 4096.f);
        }
    }
    for (std::uint32_t i = 0; i < grain * 2u; ++i)
        out[i] = static_cast<std::int16_t>(std::clamp(acc[i], -32768.f, 32767.f));
}

Voice *voice_of(std::uint32_t idx) { return idx < static_cast<std::uint32_t>(kVoices) ? &g_sas.v[idx] : nullptr; }

void h_sas_init(Runtime &, AllegrexContext &ctx) {
    g_sas = Sas{};
    g_sas.inited = true;
    g_sas.core = ctx.gpr[4];
    g_sas.grain = ctx.gpr[5] ? ctx.gpr[5] : 256u;
    g_sas.outmode = ctx.gpr[7];
    log_call("sasInit", ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7], ctx.gpr[8]);
    ret(ctx, 0u);
}
void h_sas_core(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t out = ctx.gpr[5];
    const std::uint32_t grain = g_sas.grain;
    if (std::uint8_t *p = host_ptr(out, grain * 4u))
        sas_render(reinterpret_cast<std::int16_t *>(p), grain, false, 0, 0);
    ret(ctx, 0u);
}
void h_sas_core_with_mix(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t out = ctx.gpr[5];
    const std::uint32_t grain = g_sas.grain;
    if (std::uint8_t *p = host_ptr(out, grain * 4u))
        sas_render(reinterpret_cast<std::int16_t *>(p), grain, true, static_cast<int>(ctx.gpr[6]),
                   static_cast<int>(ctx.gpr[7]));
    ret(ctx, 0u);
}
void h_sas_set_voice(Runtime &, AllegrexContext &ctx) {  // (core, voice, vag, size, loop)
    log_call("sasSetVoice", ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7], ctx.gpr[8]);
    Voice *v = voice_of(ctx.gpr[5]);
    if (!v) { ret(ctx, 0x80420010u); return; }
    v->kind = Voice::Vag;
    v->loop = ctx.gpr[8] != 0u;
    if (const std::uint8_t *p = host_ptr(ctx.gpr[6], ctx.gpr[7])) decode_vag(p, ctx.gpr[7], *v);
    else v->pcm.clear();
    ret(ctx, 0u);
}
void h_sas_set_voice_pcm(Runtime &, AllegrexContext &ctx) {  // (core, voice, pcm, size, loop)
    log_call("sasSetVoicePCM", ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7], ctx.gpr[8]);
    Voice *v = voice_of(ctx.gpr[5]);
    if (!v) { ret(ctx, 0x80420010u); return; }
    v->kind = Voice::Pcm;
    v->loop = ctx.gpr[8] != 0u;
    v->loop_start = 0;
    v->pcm.clear();
    // size is in samples on the PSP; accept either unit by bounding to what is readable.
    const std::uint32_t n = ctx.gpr[7];
    if (const std::uint8_t *p = host_ptr(ctx.gpr[6], n * 2u)) {
        v->pcm.resize(n);
        std::memcpy(v->pcm.data(), p, static_cast<std::size_t>(n) * 2u);
    }
    ret(ctx, 0u);
}
void h_sas_set_pitch(Runtime &, AllegrexContext &ctx) {
    log_call("sasSetPitch", ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]);
    if (Voice *v = voice_of(ctx.gpr[5])) {
        v->pitch = static_cast<int>(ctx.gpr[6]);
        v->step = static_cast<double>(std::clamp(v->pitch, 1, 0x4000)) / 4096.0;
    }
    ret(ctx, 0u);
}
void h_sas_set_volume(Runtime &, AllegrexContext &ctx) {  // (core, voice, l, r, effl, effr)
    if (Voice *v = voice_of(ctx.gpr[5])) {
        v->vol_l = static_cast<std::int16_t>(ctx.gpr[6]);
        v->vol_r = static_cast<std::int16_t>(ctx.gpr[7]);
    }
    ret(ctx, 0u);
}
void h_sas_set_noise(Runtime &, AllegrexContext &ctx) {
    if (Voice *v = voice_of(ctx.gpr[5])) {
        v->kind = Voice::Noise;
        v->noise_clock = static_cast<int>(ctx.gpr[6] & 0x3Fu);
    }
    ret(ctx, 0u);
}
void h_sas_set_simple_adsr(Runtime &, AllegrexContext &ctx) {  // (core, voice, adsr1, adsr2)
    log_call("sasSetSimpleADSR", ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]);
    if (Voice *v = voice_of(ctx.gpr[5])) {
        v->simple = true;
        v->adsr1 = static_cast<std::uint16_t>(ctx.gpr[6]);
        v->adsr2 = static_cast<std::uint16_t>(ctx.gpr[7]);
    }
    ret(ctx, 0u);
}
// Rate arguments of the full ADSR call are treated as times in milliseconds; very large values
// (the "instant" encodings the SDK uses) become zero.
float rate_to_seconds(std::uint32_t r) {
    if (r == 0u || r >= 0x100000u) return 0.f;
    return std::min(static_cast<float>(r) / 1000.f, 20.f);
}
void h_sas_set_adsr(Runtime &, AllegrexContext &ctx) {  // (core, voice, flag, a, d, s, r)
    log_call("sasSetADSR", ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7], ctx.gpr[8], ctx.gpr[9]);
    if (Voice *v = voice_of(ctx.gpr[5])) {
        const std::uint32_t flag = ctx.gpr[6];
        v->simple = false;
        if (flag & 1u) v->time_a = rate_to_seconds(ctx.gpr[7]);
        if (flag & 2u) v->time_d = rate_to_seconds(ctx.gpr[8]);
        if (flag & 8u) v->time_r = std::max(rate_to_seconds(ctx.gpr[10]), 0.005f);
    }
    ret(ctx, 0u);
}
void h_sas_set_sl(Runtime &, AllegrexContext &ctx) {
    if (Voice *v = voice_of(ctx.gpr[5])) {
        const std::uint32_t l = ctx.gpr[6];
        v->sustain = l > 0x7FFFu ? std::min(1.f, static_cast<float>(l) / 1073741824.f)
                                 : std::min(1.f, static_cast<float>(l) / 32767.f);
    }
    ret(ctx, 0u);
}
void h_sas_key_on(Runtime &, AllegrexContext &ctx) {
    log_call("sasSetKeyOn", ctx.gpr[4], ctx.gpr[5]);
    if (Voice *v = voice_of(ctx.gpr[5])) {
        v->pos = 0.0;
        v->on = true;
        v->ended = false;
        v->keyed_off = false;
        v->noise_lfsr = 1u;
        start_envelope(*v);
    }
    ret(ctx, 0u);
}
void h_sas_key_off(Runtime &, AllegrexContext &ctx) {
    log_call("sasSetKeyOff", ctx.gpr[4], ctx.gpr[5]);
    if (Voice *v = voice_of(ctx.gpr[5])) {
        if (v->on) {
            v->keyed_off = true;
            v->phase = Voice::Release;
            v->env_counter = 0;
        }
    }
    ret(ctx, 0u);
}
void h_sas_set_pause(Runtime &, AllegrexContext &ctx) {  // (core, voiceBits, pause)
    for (int i = 0; i < kVoices; ++i)
        if (ctx.gpr[5] & (1u << i)) g_sas.v[i].paused = ctx.gpr[6] != 0u;
    ret(ctx, 0u);
}
void h_sas_get_pause_flag(Runtime &, AllegrexContext &ctx) {
    std::uint32_t m = 0;
    for (int i = 0; i < kVoices; ++i)
        if (g_sas.v[i].paused) m |= 1u << i;
    ret(ctx, m);
}
void h_sas_get_end_flag(Runtime &, AllegrexContext &ctx) {
    std::uint32_t m = 0;
    for (int i = 0; i < kVoices; ++i)
        if (!g_sas.v[i].on) m |= 1u << i;  // a voice that is not playing counts as ended
    ret(ctx, m);
}
void h_sas_get_all_env(Runtime &, AllegrexContext &ctx) {
    for (int i = 0; i < kVoices; ++i)
        wr32(ctx.gpr[5] + static_cast<std::uint32_t>(i) * 4u,
             static_cast<std::uint32_t>(g_sas.v[i].env * 1073741824.f));
    ret(ctx, 0u);
}
void h_sas_get_env(Runtime &, AllegrexContext &ctx) {
    const Voice *v = voice_of(ctx.gpr[5]);
    ret(ctx, v ? static_cast<std::uint32_t>(v->env * 1073741824.f) : 0u);
}
void h_sas_get_grain(Runtime &, AllegrexContext &ctx) { ret(ctx, g_sas.grain); }
void h_sas_set_grain(Runtime &, AllegrexContext &ctx) {
    g_sas.grain = ctx.gpr[5] ? ctx.gpr[5] : 256u;
    ret(ctx, 0u);
}
void h_sas_get_outmode(Runtime &, AllegrexContext &ctx) { ret(ctx, g_sas.outmode); }
void h_sas_set_outmode(Runtime &, AllegrexContext &ctx) {
    g_sas.outmode = ctx.gpr[5];
    ret(ctx, 0u);
}
void h_ret_zero(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }

// ------------------------------------------------------------------ ATRAC3 / ATRAC3+
constexpr std::uint32_t kAtracErrAllDecoded = 0x80630024u;
constexpr std::uint32_t kAtracErrNoData = 0x80630022u;
constexpr std::uint32_t kAtracErrBadId = 0x80630005u;
constexpr std::uint32_t kAtracErrUnknownFormat = 0x80630006u;

#if defined(PERSONA1_HAVE_FFMPEG)
class FFDecoder {
public:
    ~FFDecoder() { close(); }
    bool open(bool at3p, int channels, int rate, int block_align, const std::uint8_t *extra, int extra_size) {
        const AVCodec *codec = avcodec_find_decoder(at3p ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) return false;
        ctx_->sample_rate = rate;
        ctx_->block_align = block_align;
        av_channel_layout_default(&ctx_->ch_layout, channels);
        if (extra && extra_size > 0) {
            ctx_->extradata = static_cast<std::uint8_t *>(av_mallocz(static_cast<size_t>(extra_size) + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(ctx_->extradata, extra, static_cast<size_t>(extra_size));
            ctx_->extradata_size = extra_size;
        }
        if (avcodec_open2(ctx_, codec, nullptr) < 0) { close(); return false; }
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        return pkt_ && frame_;
    }
    void close() {
        if (frame_) av_frame_free(&frame_);
        if (pkt_) av_packet_free(&pkt_);
        if (ctx_) avcodec_free_context(&ctx_);
    }
    void flush() { if (ctx_) avcodec_flush_buffers(ctx_); }
    // Decodes one packet into interleaved stereo s16. Returns the number of sample frames, <0 on error.
    int decode(const std::uint8_t *data, int size, std::vector<std::int16_t> &out) {
        if (!ctx_) return -1;
        if (av_new_packet(pkt_, size) < 0) return -1;
        std::memcpy(pkt_->data, data, static_cast<size_t>(size));
        int r = avcodec_send_packet(ctx_, pkt_);
        av_packet_unref(pkt_);
        if (r < 0) return -1;
        r = avcodec_receive_frame(ctx_, frame_);
        if (r < 0) return -1;
        const int n = frame_->nb_samples;
        const int ch = frame_->ch_layout.nb_channels;
        out.assign(static_cast<size_t>(n) * 2u, 0);
        for (int i = 0; i < n; ++i) {
            float s[2] = {0.f, 0.f};
            for (int c = 0; c < 2; ++c) {
                const int sc = std::min(c, ch - 1);
                switch (frame_->format) {
                case AV_SAMPLE_FMT_FLTP: s[c] = reinterpret_cast<const float *>(frame_->extended_data[sc])[i] * 32768.f; break;
                case AV_SAMPLE_FMT_FLT: s[c] = reinterpret_cast<const float *>(frame_->data[0])[i * ch + sc] * 32768.f; break;
                case AV_SAMPLE_FMT_S16P: s[c] = reinterpret_cast<const std::int16_t *>(frame_->extended_data[sc])[i]; break;
                case AV_SAMPLE_FMT_S16: s[c] = reinterpret_cast<const std::int16_t *>(frame_->data[0])[i * ch + sc]; break;
                default: break;
                }
            }
            out[static_cast<size_t>(i) * 2u] = static_cast<std::int16_t>(std::clamp(s[0], -32768.f, 32767.f));
            out[static_cast<size_t>(i) * 2u + 1u] = static_cast<std::int16_t>(std::clamp(s[1], -32768.f, 32767.f));
        }
        av_frame_unref(frame_);
        return n;
    }
private:
    AVCodecContext *ctx_{};
    AVPacket *pkt_{};
    AVFrame *frame_{};
};
#else
class FFDecoder {
public:
    bool open(bool, int, int, int, const std::uint8_t *, int) { return false; }
    void flush() {}
    int decode(const std::uint8_t *, int, std::vector<std::int16_t> &) { return -1; }
};
#endif

struct Atrac {
    bool used{};
    // header
    bool at3p{true};
    int channels{2};
    int rate{44100};
    int block_align{0};
    std::vector<std::uint8_t> extra;
    std::uint32_t data_off{0};
    std::uint32_t file_size{0};
    std::uint32_t total_samples{0};
    std::uint32_t skip{0};
    std::uint32_t frame_samples{2048};
    bool has_loop{};
    std::uint32_t loop_start{0}, loop_end{0};
    std::uint32_t fact_first{0};
    // guest buffer
    std::uint32_t buf{}, buf_size{};
    // mirror of the file as far as the game has supplied it: [lo, hi)
    std::vector<std::uint8_t> mirror;
    std::uint32_t lo{0}, hi{0};
    // playback
    std::int32_t loops{0};
    std::uint32_t pos{0};
    bool finished{};
    // decoder
    std::unique_ptr<FFDecoder> dec;
    bool dec_ok{};
    std::int64_t cur_frame{-1};
    std::vector<std::int16_t> frame_pcm;
};
Atrac g_atrac[6];

bool atrac_complete(const Atrac &a) { return a.lo <= a.data_off && a.hi >= a.file_size; }

bool parse_riff(const std::uint8_t *p, std::uint32_t n, Atrac &a) {
    auto u16 = [&](std::uint32_t o) { return static_cast<std::uint32_t>(p[o] | (p[o + 1] << 8)); };
    auto u32 = [&](std::uint32_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (n < 24u || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return false;
    std::uint32_t off = 12;
    bool have_fmt = false, have_data = false;
    std::uint32_t fact_first = 0;
    while (off + 8u <= n) {
        const std::uint32_t sz = u32(off + 4);
        const std::uint32_t body = off + 8u;
        if (std::memcmp(p + off, "fmt ", 4) == 0 && body + 16u <= n) {
            const std::uint32_t tag = u16(body);
            a.channels = static_cast<int>(u16(body + 2));
            a.rate = static_cast<int>(u32(body + 4));
            a.block_align = static_cast<int>(u16(body + 12));
            a.at3p = (tag == 0xFFFEu);  // extensible = ATRAC3+, 0x270 = ATRAC3
            if (sz > 18u && body + sz <= n) {
                const std::uint32_t cb = u16(body + 16);
                a.extra.assign(p + body + 18, p + body + 18 + std::min<std::uint32_t>(cb, sz - 18u));
            }
            have_fmt = true;
        } else if (std::memcmp(p + off, "fact", 4) == 0 && body + 4u <= n) {
            a.total_samples = u32(body);
            if (sz >= 8u && body + 8u <= n) fact_first = u32(body + 4);
        } else if (std::memcmp(p + off, "smpl", 4) == 0 && body + 36u + 24u <= n) {
            if (u32(body + 28) > 0u) {
                a.has_loop = true;
                a.loop_start = u32(body + 36 + 8);
                a.loop_end = u32(body + 36 + 12);
            }
        } else if (std::memcmp(p + off, "data", 4) == 0) {
            a.data_off = body;
            a.file_size = body + sz;
            have_data = true;
            break;
        }
        off = body + sz + (sz & 1u);
    }
    if (!have_fmt || !have_data || a.block_align <= 0) return false;
    a.frame_samples = a.at3p ? 2048u : 1024u;
    a.fact_first = fact_first;
    // Loop points in the smpl chunk count from the start of the encoder's input (the first valid
    // sample sits at fact_first); playback positions count from the first valid sample.
    if (a.has_loop) {
        a.loop_start = a.loop_start > fact_first ? a.loop_start - fact_first : 0u;
        a.loop_end = a.loop_end > fact_first ? a.loop_end - fact_first : 0u;
    }
    a.skip = fact_first + (a.at3p ? 368u : 69u);
    if (a.total_samples == 0u)
        a.total_samples = static_cast<std::uint32_t>((a.file_size - a.data_off) / static_cast<std::uint32_t>(a.block_align)) *
                          a.frame_samples;
    return true;
}

// Makes frame `f` the current decoded frame. False if the data for it has not been supplied yet.
bool atrac_load_frame(Atrac &a, std::int64_t f) {
    if (a.cur_frame == f) return true;
    auto frame_ok = [&](std::int64_t k) {
        const std::uint64_t o = a.data_off + static_cast<std::uint64_t>(k) * static_cast<std::uint64_t>(a.block_align);
        return o >= a.lo && o + static_cast<std::uint64_t>(a.block_align) <= a.hi && o + a.block_align <= a.mirror.size();
    };
    if (!frame_ok(f)) return false;
    std::int64_t first = f;
    if (a.cur_frame != f - 1) {
        // Random access: restart the decoder one frame earlier so the overlap-add state is primed.
        if (a.dec) a.dec->flush();
        first = std::max<std::int64_t>(0, f - 2);
        while (first < f && !frame_ok(first)) ++first;
    }
    for (std::int64_t k = first; k <= f; ++k) {
        const std::uint32_t o = a.data_off + static_cast<std::uint32_t>(k) * static_cast<std::uint32_t>(a.block_align);
        int n = -1;
        if (a.dec && a.dec_ok) n = a.dec->decode(a.mirror.data() + o, a.block_align, a.frame_pcm);
        if (n < 0) a.frame_pcm.assign(static_cast<std::size_t>(a.frame_samples) * 2u, 0);  // silence on failure
    }
    a.cur_frame = f;
    return true;
}

Atrac *atrac_of(std::uint32_t id) { return id < 6u && g_atrac[id].used ? &g_atrac[id] : nullptr; }

void h_atrac_set_data(Runtime &, AllegrexContext &ctx) {  // (buf, bufSize) -> id
    const std::uint32_t buf = ctx.gpr[4], size = ctx.gpr[5];
    log_call("atracSetDataAndGetID", buf, size);
    const std::uint8_t *p = host_ptr(buf, size);
    int id = -1;
    for (int i = 0; i < 6; ++i) if (!g_atrac[i].used) { id = i; break; }
    if (!p || id < 0) { ret(ctx, 0x80630008u); return; }
    Atrac a;
    if (!parse_riff(p, size, a)) { ret(ctx, kAtracErrUnknownFormat); return; }
    a.used = true;
    a.buf = buf;
    a.buf_size = size;
    a.mirror.assign(a.file_size, 0);
    const std::uint32_t have = std::min<std::uint32_t>(size, a.file_size);
    std::memcpy(a.mirror.data(), p, have);
    a.lo = 0;
    a.hi = have;
    a.dec = std::make_unique<FFDecoder>();
    a.dec_ok = a.dec->open(a.at3p, a.channels, a.rate, a.block_align, a.extra.data(), static_cast<int>(a.extra.size()));
    if (g_log || !a.dec_ok)
        std::fprintf(stderr, "[audio] atrac id=%d %s ch=%d rate=%d block=%d samples=%u skip=%u loop=%d[%u..%u] file=%u/%u decoder=%s\n",
                     id, a.at3p ? "AT3+" : "AT3", a.channels, a.rate, a.block_align, a.total_samples, a.skip,
                     a.has_loop ? 1 : 0, a.loop_start, a.loop_end, have, a.file_size,
                     a.dec_ok ? "ffmpeg" : "NONE (silent)");
    g_atrac[id] = std::move(a);
    ret(ctx, static_cast<std::uint32_t>(id));
}
void h_atrac_release(Runtime &, AllegrexContext &ctx) {
    if (Atrac *a = atrac_of(ctx.gpr[4])) *a = Atrac{};
    ret(ctx, 0u);
}
void h_atrac_decode(Runtime &, AllegrexContext &ctx) {  // (id, outBuf, *samples, *finished, *remain)
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    const std::uint32_t out = ctx.gpr[5];
    if (a->finished) {
        wr32(ctx.gpr[6], 0u);
        wr32(ctx.gpr[7], 1u);
        wr32(ctx.gpr[8], 0xFFFFFFFFu);
        ret(ctx, kAtracErrAllDecoded);
        return;
    }
    const std::uint32_t want = a->frame_samples;
    std::uint32_t got = 0;
    std::vector<std::int16_t> res(static_cast<std::size_t>(want) * 2u, 0);
    std::uint32_t err = 0;
    while (got < want) {
        std::uint32_t boundary = a->total_samples;
        const bool looping = a->loops != 0;
        if (looping) boundary = a->has_loop ? std::min(a->loop_end + 1u, a->total_samples) : a->total_samples;
        if (a->pos >= boundary) {
            if (looping) {
                a->pos = a->has_loop ? a->loop_start : 0u;
                if (a->loops > 0) --a->loops;
                continue;
            }
            a->finished = true;
            break;
        }
        const std::int64_t f = (static_cast<std::int64_t>(a->pos) + a->skip) / a->frame_samples;
        if (!atrac_load_frame(*a, f)) { err = kAtracErrNoData; break; }
        const std::uint32_t in_frame = static_cast<std::uint32_t>((static_cast<std::int64_t>(a->pos) + a->skip) % a->frame_samples);
        const std::uint32_t avail = std::min<std::uint32_t>(static_cast<std::uint32_t>(a->frame_pcm.size() / 2u) > in_frame ? static_cast<std::uint32_t>(a->frame_pcm.size() / 2u) - in_frame : 0u,
                                                            boundary - a->pos);
        const std::uint32_t take = std::min(avail, want - got);
        if (take == 0u) {  // decoder gave a short frame; skip ahead rather than spin
            a->pos += 1;
            continue;
        }
        std::memcpy(res.data() + static_cast<std::size_t>(got) * 2u, a->frame_pcm.data() + static_cast<std::size_t>(in_frame) * 2u,
                    static_cast<std::size_t>(take) * 4u);
        got += take;
        a->pos += take;
    }
    if (err != 0u && got == 0u) {
        wr32(ctx.gpr[6], 0u);
        ret(ctx, err);
        return;
    }
    if (a->pos >= a->total_samples && a->loops == 0) a->finished = true;
    if (std::uint8_t *p = host_ptr(out, want * 4u)) std::memcpy(p, res.data(), static_cast<std::size_t>(want) * 4u);
    wr32(ctx.gpr[6], got);
    wr32(ctx.gpr[7], a->finished ? 1u : 0u);
    std::uint32_t remain = 0xFFFFFFFFu;
    if (!atrac_complete(*a)) {
        const std::int64_t cur = (static_cast<std::int64_t>(a->pos) + a->skip) / a->frame_samples;
        const std::int64_t last = (static_cast<std::int64_t>(a->hi) - a->data_off) / a->block_align;
        remain = static_cast<std::uint32_t>(std::max<std::int64_t>(0, last - cur));
    }
    wr32(ctx.gpr[8], remain);
    ret(ctx, 0u);
}
void h_atrac_remain(Runtime &, AllegrexContext &ctx) {  // (id, *remain)
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    std::uint32_t remain = 0xFFFFFFFFu;
    if (!atrac_complete(*a)) {
        const std::int64_t cur = (static_cast<std::int64_t>(a->pos) + a->skip) / a->frame_samples;
        const std::int64_t last = (static_cast<std::int64_t>(a->hi) - a->data_off) / a->block_align;
        remain = static_cast<std::uint32_t>(std::max<std::int64_t>(0, last - cur));
    }
    wr32(ctx.gpr[5], remain);
    ret(ctx, 0u);
}
void h_atrac_next_pos(Runtime &, AllegrexContext &ctx) {  // (id, *pos)
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    if (a->finished) { ret(ctx, kAtracErrAllDecoded); return; }
    wr32(ctx.gpr[5], a->pos);
    ret(ctx, 0u);
}
void h_atrac_sound_sample(Runtime &, AllegrexContext &ctx) {  // (id, *end, *loopStart, *loopEnd)
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    wr32(ctx.gpr[5], a->total_samples ? a->total_samples - 1u : 0u);
    wr32(ctx.gpr[6], a->has_loop ? a->loop_start : 0xFFFFFFFFu);
    wr32(ctx.gpr[7], a->has_loop ? a->loop_end : 0xFFFFFFFFu);
    ret(ctx, 0u);
}
void h_atrac_set_loop(Runtime &, AllegrexContext &ctx) {
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    a->loops = static_cast<std::int32_t>(ctx.gpr[5]);
    ret(ctx, 0u);
}
void h_atrac_stream_info(Runtime &, AllegrexContext &ctx) {  // (id, **writePtr, *writable, *readOffset)
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    wr32(ctx.gpr[5], a->buf);
    if (atrac_complete(*a)) {
        wr32(ctx.gpr[6], 0u);
        wr32(ctx.gpr[7], 0u);
    } else {
        wr32(ctx.gpr[6], std::min<std::uint32_t>(a->buf_size, a->file_size - a->hi));
        wr32(ctx.gpr[7], a->hi);
    }
    ret(ctx, 0u);
}
void h_atrac_add_stream(Runtime &, AllegrexContext &ctx) {  // (id, bytes)
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    const std::uint32_t n = std::min<std::uint32_t>(ctx.gpr[5], a->file_size - a->hi);
    if (n > 0u) {
        if (const std::uint8_t *p = host_ptr(a->buf, n)) {
            std::memcpy(a->mirror.data() + a->hi, p, n);
            a->hi += n;
        }
    }
    ret(ctx, 0u);
}
void h_atrac_reset_pos(Runtime &, AllegrexContext &ctx) {  // (id, sample, bytes1, bytes2)
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    a->pos = ctx.gpr[5];
    a->finished = false;
    a->cur_frame = -1;
    const std::uint32_t bytes = ctx.gpr[6];
    if (bytes > 0u) {
        const std::uint32_t frame = (a->pos + a->skip) / a->frame_samples;
        const std::uint32_t off = a->data_off + frame * static_cast<std::uint32_t>(a->block_align);
        if (const std::uint8_t *p = host_ptr(a->buf, bytes)) {
            const std::uint32_t n = std::min<std::uint32_t>(bytes, a->file_size > off ? a->file_size - off : 0u);
            std::memcpy(a->mirror.data() + off, p, n);
            a->lo = off;
            a->hi = off + n;
        }
    }
    ret(ctx, 0u);
}
void h_atrac_reset_info(Runtime &, AllegrexContext &ctx) {  // (id, sample, *info{writePtr, writable, minWrite, filePos})
    Atrac *a = atrac_of(ctx.gpr[4]);
    if (!a) { ret(ctx, kAtracErrBadId); return; }
    const std::uint32_t info = ctx.gpr[6];
    const std::uint32_t frame = (ctx.gpr[5] + a->skip) / a->frame_samples;
    const std::uint32_t off = a->data_off + frame * static_cast<std::uint32_t>(a->block_align);
    const bool have = off >= a->lo && off + static_cast<std::uint32_t>(a->block_align) <= a->hi;
    wr32(info + 0u, a->buf);
    wr32(info + 4u, have ? 0u : std::min<std::uint32_t>(a->buf_size, a->file_size > off ? a->file_size - off : 0u));
    wr32(info + 8u, have ? 0u : static_cast<std::uint32_t>(a->block_align));
    wr32(info + 12u, off);
    ret(ctx, 0u);
}
void h_atrac_second_info(Runtime &, AllegrexContext &ctx) {
    wr32(ctx.gpr[5], 0u);
    wr32(ctx.gpr[6], 0u);
    ret(ctx, 0u);
}
void h_atrac_err_info(Runtime &, AllegrexContext &ctx) {
    wr32(ctx.gpr[5], 0u);
    ret(ctx, 0u);
}

// ------------------------------------------------------------------ registration
struct Entry { const char *lib; std::uint32_t nid; void (*fn)(Runtime &, AllegrexContext &); };
const Entry kTable[] = {
    {"sceAudio", 0x5EC81C55u, h_audio_ch_reserve},
    {"sceAudio", 0x6FC46853u, h_audio_ch_release},
    {"sceAudio", 0xCB2E439Eu, h_audio_set_len},
    {"sceAudio", 0x95FD0C2Du, h_audio_change_config},
    {"sceAudio", 0xB7E1D8E7u, h_audio_change_volume},
    {"sceAudio", 0xB011922Fu, h_audio_rest_length},
    {"sceAudio", 0x136CAF51u, h_audio_output<0>},
    {"sceAudio", 0x13F592BCu, h_audio_output<1>},
    {"sceAudio", 0xE2D56B2Du, h_audio_output<2>},

    {"sceSasCore", 0x42778A9Fu, h_sas_init},
    {"sceSasCore", 0xA3589D81u, h_sas_core},
    {"sceSasCore", 0x50A14DFCu, h_sas_core_with_mix},
    {"sceSasCore", 0x99944089u, h_sas_set_voice},
    {"sceSasCore", 0xE1CD9561u, h_sas_set_voice_pcm},
    {"sceSasCore", 0xAD84D37Fu, h_sas_set_pitch},
    {"sceSasCore", 0x440CA7D8u, h_sas_set_volume},
    {"sceSasCore", 0xB7660A23u, h_sas_set_noise},
    {"sceSasCore", 0xCBCD4F79u, h_sas_set_simple_adsr},
    {"sceSasCore", 0x019B25EBu, h_sas_set_adsr},
    {"sceSasCore", 0x9EC3676Au, h_ret_zero},  // SetADSRmode
    {"sceSasCore", 0x5F9529F6u, h_sas_set_sl},
    {"sceSasCore", 0x76F01ACAu, h_sas_key_on},
    {"sceSasCore", 0xA0CF2FA4u, h_sas_key_off},
    {"sceSasCore", 0x787D04D5u, h_sas_set_pause},
    {"sceSasCore", 0x2C8E6AB3u, h_sas_get_pause_flag},
    {"sceSasCore", 0x68A46B95u, h_sas_get_end_flag},
    {"sceSasCore", 0x07F58C24u, h_sas_get_all_env},
    {"sceSasCore", 0x74AE582Au, h_sas_get_env},
    {"sceSasCore", 0xBD11B7C2u, h_sas_get_grain},
    {"sceSasCore", 0xD1E0A01Eu, h_sas_set_grain},
    {"sceSasCore", 0xE175EF66u, h_sas_get_outmode},
    {"sceSasCore", 0xE855BF76u, h_sas_set_outmode},
    {"sceSasCore", 0x33D4AB37u, h_ret_zero},  // RevType
    {"sceSasCore", 0x267A6DD2u, h_ret_zero},  // RevParam
    {"sceSasCore", 0xD5A229C9u, h_ret_zero},  // RevEVOL
    {"sceSasCore", 0xF983B186u, h_ret_zero},  // RevVON

    {"sceAtrac3plus", 0x7A20E7AFu, h_atrac_set_data},
    {"sceAtrac3plus", 0x61EB33F5u, h_atrac_release},
    {"sceAtrac3plus", 0x6A8C3CD5u, h_atrac_decode},
    {"sceAtrac3plus", 0x9AE849A7u, h_atrac_remain},
    {"sceAtrac3plus", 0xE23E3A35u, h_atrac_next_pos},
    {"sceAtrac3plus", 0xA2BBA8BEu, h_atrac_sound_sample},
    {"sceAtrac3plus", 0x868120B5u, h_atrac_set_loop},
    {"sceAtrac3plus", 0x5D268707u, h_atrac_stream_info},
    {"sceAtrac3plus", 0x7DB31251u, h_atrac_add_stream},
    {"sceAtrac3plus", 0x644E5607u, h_atrac_reset_pos},
    {"sceAtrac3plus", 0x2DD3E298u, h_atrac_reset_info},
    {"sceAtrac3plus", 0x83E85EA0u, h_atrac_second_info},
    {"sceAtrac3plus", 0x83BF7AFDu, h_ret_zero},  // SetSecondBuffer
    {"sceAtrac3plus", 0xE88F759Bu, h_atrac_err_info},
};

std::set<std::string> &handled() {
    static std::set<std::string> s;
    return s;
}
std::string key(const std::string &lib, std::uint32_t nid) { return lib + ":" + std::to_string(nid); }

struct ShutdownGuard {
    ~ShutdownGuard() { audio_shutdown(); }
} g_guard;

} // namespace

void install_audio(Runtime &rt) {
    g_rt = &rt;
    g_log = std::getenv("PERSONA1_AUDIOLOG") != nullptr;
    for (const Entry &e : kTable) {
        rt.register_hle(e.lib, e.nid, e.fn);
        handled().insert(key(e.lib, e.nid));
    }
    if (const char *v = std::getenv("PERSONA1_VOLUME")) g_master = std::clamp(static_cast<float>(std::atoi(v)) / 100.f, 0.f, 2.f);
#if defined(_WIN32)
    if (!std::getenv("PERSONA1_NOAUDIO")) {
        g_device_ok = g_device.open();
        if (!g_device_ok) std::fprintf(stderr, "[audio] could not open the audio device; running silent\n");
        else if (g_log) std::fprintf(stderr, "[audio] device opened (waveOut, %d Hz stereo)\n", static_cast<int>(kRate));
    }
#endif
}

bool audio_handles(const std::string &library, std::uint32_t nid) { return handled().count(key(library, nid)) != 0u; }

void audio_shutdown() {
#if defined(_WIN32)
    g_device.close();
#endif
    g_device_ok = false;
}

} // namespace persona1
