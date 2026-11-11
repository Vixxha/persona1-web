// Persona 1 native port: movies (sceMpeg).
//
// Data path:  game callback reads PSMF sectors -> sceMpegRingbufferPut (HLE) runs that callback on a
// helper guest thread -> bytes are demuxed here (MPEG-2 program stream: video PES 0xE0 carrying
// H.264, private stream 1 carrying ATRAC3+ frames) -> sceMpegGetAvcAu / GetAtracAu hand out access
// units -> sceMpegAvcDecodeYCbCr / AtracDecode decode them with FFmpeg -> sceMpegAvcCsc writes the
// picture to the game's RGBA8888 buffer.

#include "persona1_movie.hpp"
#include "persona1_kernel.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#if defined(PERSONA1_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}
#endif

using psprecomp::AllegrexContext;
using psprecomp::Runtime;

namespace persona1 {
namespace {

Runtime *g_rt = nullptr;
bool g_log = false;

constexpr std::uint32_t kErrNoData = 0x80618001u;
constexpr std::uint32_t kErrInvalid = 0x806101FEu;

std::uint32_t rd32(std::uint32_t a) { return g_rt->memory().aot_fast_view().aot_load32(a); }
void wr32(std::uint32_t a, std::uint32_t v) {
    if (a != 0u) g_rt->memory().aot_fast_view().aot_store32(a, v);
}
std::uint8_t *host_ptr(std::uint32_t a, std::uint32_t len) {
    if (a == 0u || len == 0u) return nullptr;
    return g_rt->memory().raw_pointer(a, len);
}
void ret(AllegrexContext &ctx, std::uint32_t v) { ctx.set_gpr(2, v); }
std::uint32_t be32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

long long now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

std::map<std::string, int> g_seen;
void log_call(const char *name, const AllegrexContext &c) {
    if (!g_log) return;
    auto &seen = g_seen;
    if (++seen[name] > 8) return;
    std::fprintf(stderr, "[movie +%lldms] %s(%08X, %08X, %08X, %08X, %08X, %08X)\n", now_ms(), name, c.gpr[4], c.gpr[5], c.gpr[6],
                 c.gpr[7], c.gpr[8], c.gpr[9]);
}

// ------------------------------------------------------------------ FFmpeg wrappers
#if defined(PERSONA1_HAVE_FFMPEG)
class H264 {
public:
    ~H264() { close(); }
    bool open() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return false;
        c_ = avcodec_alloc_context3(codec);
        if (!c_) return false;
        c_->thread_count = 1;
        c_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(c_, codec, nullptr) < 0) { close(); return false; }
        p_ = av_packet_alloc();
        f_ = av_frame_alloc();
        return p_ && f_;
    }
    void close() {
        if (f_) av_frame_free(&f_);
        if (p_) av_packet_free(&p_);
        if (c_) avcodec_free_context(&c_);
    }
    // Feeds one access unit. Returns true when a picture came out (converted into rgba).
    int last_send{0};
    bool decode(const std::uint8_t *d, int n, std::vector<std::uint8_t> &rgba, int &w, int &h) {
        if (!c_) return false;
        if (av_new_packet(p_, n) < 0) return false;
        std::memcpy(p_->data, d, static_cast<size_t>(n));
        const int sr = avcodec_send_packet(c_, p_);
        av_packet_unref(p_);
        last_send = sr;
        if (sr < 0) return false;
        bool got = false;
        while (avcodec_receive_frame(c_, f_) >= 0) {
            convert(*f_, rgba, w, h);
            av_frame_unref(f_);
            got = true;
        }
        return got;
    }
private:
    static void convert(const AVFrame &f, std::vector<std::uint8_t> &rgba, int &w, int &h) {
        w = f.width;
        h = f.height;
        rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 255);
        if (f.format != AV_PIX_FMT_YUV420P && f.format != AV_PIX_FMT_YUVJ420P) return;
        const bool full = f.format == AV_PIX_FMT_YUVJ420P || f.color_range == AVCOL_RANGE_JPEG;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t *yp = f.data[0] + static_cast<size_t>(y) * static_cast<size_t>(f.linesize[0]);
            const std::uint8_t *up = f.data[1] + static_cast<size_t>(y / 2) * static_cast<size_t>(f.linesize[1]);
            const std::uint8_t *vp = f.data[2] + static_cast<size_t>(y / 2) * static_cast<size_t>(f.linesize[2]);
            std::uint8_t *o = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
            for (int x = 0; x < w; ++x) {
                const int Y = yp[x], U = up[x / 2] - 128, V = vp[x / 2] - 128;
                int r, g, b;
                if (full) {  // BT.601 full range, 16.16 fixed point
                    r = Y + ((91881 * V) >> 16);
                    g = Y - ((22554 * U + 46802 * V) >> 16);
                    b = Y + ((116130 * U) >> 16);
                } else {     // BT.601 limited range
                    const int c = (Y - 16) * 76309;
                    r = (c + 104597 * V) >> 16;
                    g = (c - 25675 * U - 53279 * V) >> 16;
                    b = (c + 132201 * U) >> 16;
                }
                o[x * 4 + 0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
                o[x * 4 + 1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
                o[x * 4 + 2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
            }
        }
    }
    AVCodecContext *c_{};
    AVPacket *p_{};
    AVFrame *f_{};
};

class AtracDec {
public:
    ~AtracDec() { close(); }
    bool open(int channels, int rate, int block_align) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        if (!codec) return false;
        c_ = avcodec_alloc_context3(codec);
        if (!c_) return false;
        c_->sample_rate = rate;
        c_->block_align = block_align;
        av_channel_layout_default(&c_->ch_layout, channels);
        if (avcodec_open2(c_, codec, nullptr) < 0) { close(); return false; }
        p_ = av_packet_alloc();
        f_ = av_frame_alloc();
        return p_ && f_;
    }
    void close() {
        if (f_) av_frame_free(&f_);
        if (p_) av_packet_free(&p_);
        if (c_) avcodec_free_context(&c_);
    }
    // One frame in, 2048 stereo s16 samples out (empty on failure).
    bool decode(const std::uint8_t *d, int n, std::vector<std::int16_t> &out) {
        if (!c_) return false;
        if (av_new_packet(p_, n) < 0) return false;
        std::memcpy(p_->data, d, static_cast<size_t>(n));
        int r = avcodec_send_packet(c_, p_);
        av_packet_unref(p_);
        if (r < 0) return false;
        if (avcodec_receive_frame(c_, f_) < 0) return false;
        const int cnt = f_->nb_samples, ch = f_->ch_layout.nb_channels;
        out.assign(static_cast<size_t>(cnt) * 2u, 0);
        for (int i = 0; i < cnt; ++i)
            for (int c = 0; c < 2; ++c) {
                const int sc = std::min(c, ch - 1);
                float s = 0.f;
                if (f_->format == AV_SAMPLE_FMT_FLTP) s = reinterpret_cast<const float *>(f_->extended_data[sc])[i] * 32768.f;
                else if (f_->format == AV_SAMPLE_FMT_FLT) s = reinterpret_cast<const float *>(f_->data[0])[i * ch + sc] * 32768.f;
                else if (f_->format == AV_SAMPLE_FMT_S16P) s = reinterpret_cast<const std::int16_t *>(f_->extended_data[sc])[i];
                else if (f_->format == AV_SAMPLE_FMT_S16) s = reinterpret_cast<const std::int16_t *>(f_->data[0])[i * ch + sc];
                out[static_cast<size_t>(i) * 2u + static_cast<size_t>(c)] = static_cast<std::int16_t>(std::clamp(s, -32768.f, 32767.f));
            }
        av_frame_unref(f_);
        return true;
    }
private:
    AVCodecContext *c_{};
    AVPacket *p_{};
    AVFrame *f_{};
};
#else
class H264 {
public:
    int last_send{0};
    bool open() { return false; }
    void close() {}
    bool decode(const std::uint8_t *, int, std::vector<std::uint8_t> &, int &, int &) { return false; }
};
class AtracDec {
public:
    bool open(int, int, int) { return false; }
    void close() {}
    bool decode(const std::uint8_t *, int, std::vector<std::int16_t> &) { return false; }
};
#endif

// ------------------------------------------------------------------ the movie
struct Mark { std::uint64_t off; std::int64_t pts, dts; };
struct Au { std::vector<std::uint8_t> data; std::int64_t pts{-1}, dts{-1}; };

struct Movie {
    bool created{};
    std::uint32_t ring{};
    std::uint32_t handle{};
    bool eof{};
    // demux input
    std::vector<std::uint8_t> pend;
    // video elementary stream (Annex B), with PTS marks
    std::vector<std::uint8_t> v_es;
    std::size_t v_head{}, v_scan{};
    std::uint64_t v_base{};
    std::deque<Mark> marks;
    std::int64_t last_pts{-1}, last_dts{-1};
    // audio elementary stream (ATRAC3+ frames with 8-byte headers)
    std::vector<std::uint8_t> a_es;
    std::size_t a_head{};
    std::int64_t a_pts0{-1};
    std::uint64_t a_frames{};
    // decode state
    Au cur_video;
    bool has_cur_video{};
    std::vector<std::uint8_t> rgba;
    int w{480}, h{272};
    std::unique_ptr<H264> h264;
    bool h264_ok{};
    std::unique_ptr<AtracDec> atrac;
    bool atrac_ok{};
    int atrac_block{};
    std::vector<std::int16_t> pcm;
    Au cur_audio;
    bool has_cur_audio{};
    int next_stream{0x100};
    int next_esbuf{1};
    std::uint64_t pics{};
    std::uint64_t v_aus{}, v_nodata{}, a_nodata{}, v_run{}, puts{}, dec_fail{}, dec_calls{};
};
Movie g_m;

void reset_movie() {
    const bool was_created = g_m.created;
    const std::uint32_t ring = g_m.ring, handle = g_m.handle;
    g_m = Movie{};
    g_m.created = was_created;
    g_m.ring = ring;
    g_m.handle = handle;
}

std::int64_t read_ts(const std::uint8_t *p) {
    return (static_cast<std::int64_t>((p[0] >> 1) & 7) << 30) | (static_cast<std::int64_t>(p[1]) << 22) |
           (static_cast<std::int64_t>(p[2] >> 1) << 15) | (static_cast<std::int64_t>(p[3]) << 7) |
           static_cast<std::int64_t>(p[4] >> 1);
}

// Parses as many complete program-stream units from `pend` as possible.
void demux() {
    Movie &m = g_m;
    std::size_t i = 0;
    const std::size_t n = m.pend.size();
    const std::uint8_t *d = m.pend.data();
    while (i + 4 <= n) {
        if (!(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) { ++i; continue; }  // resync
        const std::uint8_t sid = d[i + 3];
        if (sid == 0xBA) {  // pack header
            if (i + 5 > n) break;
            std::size_t len;
            if ((d[i + 4] >> 6) == 1) {
                if (i + 14 > n) break;
                len = 14u + (d[i + 13] & 7u);
            } else {
                len = 12;
            }
            if (i + len > n) break;
            i += len;
            continue;
        }
        if (sid == 0xB9) { i += 4; continue; }
        if (sid < 0xBB) { i += 4; continue; }
        if (i + 6 > n) break;
        const std::size_t plen = (static_cast<std::size_t>(d[i + 4]) << 8) | d[i + 5];
        if (i + 6 + plen > n) break;
        if ((sid & 0xF0) == 0xE0 || sid == 0xBD) {
            const std::uint8_t *pes = d + i;
            if (plen >= 3) {
                const std::uint8_t flags = pes[7];
                const std::size_t hl = pes[8];
                std::int64_t pts = -1, dts = -1;
                if ((flags & 0x80) && hl >= 5) pts = read_ts(pes + 9);
                if ((flags & 0x40) && hl >= 10) dts = read_ts(pes + 14);
                if (dts < 0) dts = pts;
                if (3 + hl <= plen) {
                    const std::uint8_t *pay = pes + 9 + hl;
                    std::size_t plen2 = plen - 3 - hl;
                    if ((sid & 0xF0) == 0xE0) {
                        if (pts >= 0)
                            m.marks.push_back({m.v_base + m.v_es.size(), pts, dts});
                        m.v_es.insert(m.v_es.end(), pay, pay + plen2);
                    } else if (plen2 > 4) {
                        if (m.a_pts0 < 0 && pts >= 0) m.a_pts0 = pts;
                        m.a_es.insert(m.a_es.end(), pay + 4, pay + plen2);  // 4-byte private header skipped
                    }
                }
            }
        }
        i += 6 + plen;
    }
    m.pend.erase(m.pend.begin(), m.pend.begin() + static_cast<std::ptrdiff_t>(i));
}

void feed(const std::uint8_t *p, std::size_t n) {
    g_m.pend.insert(g_m.pend.end(), p, p + n);
    demux();
}

// Position of the next AUD NAL start code at or after `from` (including a leading zero byte), or npos.
std::size_t find_aud(const std::vector<std::uint8_t> &v, std::size_t from) {
    for (std::size_t i = from; i + 4 <= v.size(); ++i) {
        if (v[i] == 0 && v[i + 1] == 0 && v[i + 2] == 1 && (v[i + 3] & 0x1F) == 9) return (i > 0 && v[i - 1] == 0) ? i - 1 : i;
    }
    return std::string::npos;
}

bool next_video_au(Au &out) {
    Movie &m = g_m;
    if (m.v_head >= m.v_es.size()) return false;
    std::size_t end = find_aud(m.v_es, std::max(m.v_head + 4u, m.v_scan));
    if (end == std::string::npos) {
        m.v_scan = m.v_es.size() > 4u ? m.v_es.size() - 4u : 0u;
        if (!m.eof) return false;
        end = m.v_es.size();
    }
    out.data.assign(m.v_es.begin() + static_cast<std::ptrdiff_t>(m.v_head), m.v_es.begin() + static_cast<std::ptrdiff_t>(end));
    const std::uint64_t abs_start = m.v_base + m.v_head;
    out.pts = out.dts = -1;
    bool have_ts = false;
    while (!m.marks.empty() && m.marks.front().off <= abs_start) {
        out.pts = m.marks.front().pts;
        out.dts = m.marks.front().dts;
        m.marks.pop_front();
        have_ts = true;
    }
    // Only some PES packets carry a time stamp; the pictures in between follow at 29.97 fps.
    if (have_ts) {
        m.last_pts = out.pts;
        m.last_dts = out.dts;
    } else if (m.last_pts >= 0) {
        m.last_pts += 3003;
        m.last_dts += 3003;
        out.pts = m.last_pts;
        out.dts = m.last_dts;
    }
    m.v_head = end;
    m.v_scan = end;
    if (m.v_head > (1u << 22)) {  // drop consumed bytes now and then
        m.v_es.erase(m.v_es.begin(), m.v_es.begin() + static_cast<std::ptrdiff_t>(m.v_head));
        m.v_base += m.v_head;
        m.v_head = 0;
        m.v_scan = 0;
    }
    return true;
}

bool next_audio_frame(Au &out) {
    Movie &m = g_m;
    while (m.a_es.size() - m.a_head >= 8u) {
        const std::uint8_t *p = m.a_es.data() + m.a_head;
        if (p[0] != 0x0F || p[1] != 0xD0) { ++m.a_head; continue; }  // resync on the frame header
        const std::size_t data = ((static_cast<std::size_t>(p[2] & 3u) << 8) | p[3]) * 8u + 8u;
        if (m.a_es.size() - m.a_head < 8u + data) return false;
        out.data.assign(p, p + 8u + data);
        out.pts = out.dts = (m.a_pts0 < 0 ? 0 : m.a_pts0) + static_cast<std::int64_t>(m.a_frames) * 2048 * 90000 / 44100;
        ++m.a_frames;
        m.a_head += 8u + data;
        if (m.a_head > (1u << 22)) {
            m.a_es.erase(m.a_es.begin(), m.a_es.begin() + static_cast<std::ptrdiff_t>(m.a_head));
            m.a_head = 0;
        }
        return true;
    }
    return false;
}

// ---- SceMpegRingBuffer in guest memory: packets, read, written, free, packetSize, data, callback, param, upper, sema, mpeg
void update_ring() {
    if (g_m.ring == 0u) return;
    const std::uint32_t packets = rd32(g_m.ring + 0u);
    // Once the stream has ended and both elementary streams have reported "no data", partial leftovers
    // (a cut-off audio frame, a demux fragment) can never be delivered; count only what is still deliverable,
    // so the ring reads as empty like on hardware (the game waits for that before closing the movie).
    const bool drained = g_m.eof && g_m.v_nodata > 0u && g_m.a_nodata > 0u;
    const std::size_t held = drained ? (g_m.v_es.size() - g_m.v_head)
                                     : (g_m.v_es.size() - g_m.v_head) + (g_m.a_es.size() - g_m.a_head) + g_m.pend.size();
    const std::uint32_t used = static_cast<std::uint32_t>(std::min<std::size_t>((held + 2047u) / 2048u, packets));
    const std::uint32_t written = rd32(g_m.ring + 8u);
    wr32(g_m.ring + 4u, written - used);  // "read" = written minus what is still waiting
    wr32(g_m.ring + 12u, packets - used);
}

void write_au(std::uint32_t addr, const Au &au, std::uint32_t es_size) {
    // pts and dts are stored as big-word-first 64-bit values
    auto put = [&](std::uint32_t off, std::int64_t v) {
        wr32(addr + off, static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32u));
        wr32(addr + off + 4u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) & 0xFFFFFFFFu));
    };
    put(0u, au.pts);
    put(8u, au.dts);
    wr32(addr + 20u, es_size);
}


// Debug aid for PERSONA1_MOVIELOG: the game's movie-player globals (addresses are specific to this game build).
void dump_game_state(const char *when) {
    if (!g_log) return;
    static int shown = 0;
    if (++shown > 6) return;
    std::fprintf(stderr, "[movie] state %s: flag@08AF6DE4=%08X pos@08CC261C=%08X src@08CC269C=%08X size@08CC2778=%08X\n", when,
                 rd32(0x08AF6DE4u), rd32(0x08CC261Cu), rd32(0x08CC269Cu), rd32(0x08CC2778u));
    std::fprintf(stderr, "[movie]   08AF6E08..1C: %08X %08X %08X %08X %08X %08X\n", rd32(0x08AF6E08u), rd32(0x08AF6E0Cu),
                 rd32(0x08AF6E10u), rd32(0x08AF6E14u), rd32(0x08AF6E18u), rd32(0x08AF6E1Cu));
    const std::uint32_t b40 = rd32(0x08AF6E18u), b32 = rd32(0x08AF6E0Cu), b8 = rd32(0x08AF6E08u);
    if (b40) std::fprintf(stderr, "[movie]   blk40 @%08X: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n", b40, rd32(b40),
                          rd32(b40 + 4), rd32(b40 + 8), rd32(b40 + 12), rd32(b40 + 16), rd32(b40 + 20), rd32(b40 + 24),
                          rd32(b40 + 28), rd32(b40 + 32), rd32(b40 + 36));
    if (b32) std::fprintf(stderr, "[movie]   blk32 @%08X: %08X %08X %08X %08X\n", b32, rd32(b32), rd32(b32 + 4), rd32(b32 + 8), rd32(b32 + 12));
    if (b8) std::fprintf(stderr, "[movie]   blk8  @%08X: %08X %08X\n", b8, rd32(b8), rd32(b8 + 4));
}

// ------------------------------------------------------------------ handlers
void h_init(Runtime &, AllegrexContext &ctx) { log_call("sceMpegInit", ctx); ret(ctx, 0u); }
void h_finish(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }
void h_query_mem(Runtime &, AllegrexContext &ctx) { ret(ctx, 0x10000u); }
void h_ring_query_mem(Runtime &, AllegrexContext &ctx) { ret(ctx, ctx.gpr[4] * 0x868u); }

void h_ring_construct(Runtime &, AllegrexContext &ctx) {  // (ring, packets, data, size, callback, param)
    log_call("sceMpegRingbufferConstruct", ctx);
    const std::uint32_t r = ctx.gpr[4];
    wr32(r + 0u, ctx.gpr[5]);
    wr32(r + 4u, 0u);
    wr32(r + 8u, 0u);
    wr32(r + 12u, ctx.gpr[5]);
    wr32(r + 16u, 2048u);
    wr32(r + 20u, ctx.gpr[6]);
    wr32(r + 24u, ctx.gpr[8]);
    wr32(r + 28u, ctx.gpr[9]);
    wr32(r + 32u, ctx.gpr[6] + ctx.gpr[5] * 2048u);
    wr32(r + 36u, 0u);
    wr32(r + 40u, 0u);
    ret(ctx, 0u);
}
void h_ring_destruct(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }

void h_create(Runtime &, AllegrexContext &ctx) {  // (mpeg*, data, size, ring, frameWidth, mode, ddrTop)
    g_seen.clear();  // every movie gets its own log budget
    log_call("sceMpegCreate", ctx);
    reset_movie();
    g_m.created = true;
    g_m.ring = ctx.gpr[7];
    g_m.handle = ctx.gpr[5] ? ctx.gpr[5] + 0x30u : 0x08000030u;
    wr32(ctx.gpr[4], g_m.handle);
    wr32(g_m.ring + 40u, g_m.handle);
    g_m.h264 = std::make_unique<H264>();
    g_m.h264_ok = g_m.h264->open();
    if (!g_m.h264_ok) std::fprintf(stderr, "[movie] H.264 decoder unavailable (build without FFmpeg?); movies will be skipped\n");
    ret(ctx, 0u);
}
void h_delete(Runtime &, AllegrexContext &ctx) {
    log_call("sceMpegDelete", ctx);
    if (g_log)
        std::fprintf(stderr,
                     "[movie +%lldms] DELETE summary: dec_fail=%llu video AUs=%llu decoded pics=%llu audio frames=%llu nodata v=%llu a=%llu puts=%llu eof=%d\n",
                     now_ms(), static_cast<unsigned long long>(g_m.dec_fail), static_cast<unsigned long long>(g_m.v_aus), static_cast<unsigned long long>(g_m.pics),
                     static_cast<unsigned long long>(g_m.a_frames), static_cast<unsigned long long>(g_m.v_nodata),
                     static_cast<unsigned long long>(g_m.a_nodata), static_cast<unsigned long long>(g_m.puts), g_m.eof ? 1 : 0);
    reset_movie();
    g_m.created = false;
    ret(ctx, 0u);
}

void h_query_stream_offset(Runtime &, AllegrexContext &ctx) {  // (mpeg, header, *offset)
    log_call("sceMpegQueryStreamOffset", ctx);
    const std::uint8_t *p = host_ptr(ctx.gpr[5], 16u);
    if (!p || std::memcmp(p, "PSMF", 4) != 0) { ret(ctx, kErrInvalid); return; }
    wr32(ctx.gpr[6], be32(p + 8));
    ret(ctx, 0u);
}
void h_query_stream_size(Runtime &, AllegrexContext &ctx) {  // (header, *size)
    log_call("sceMpegQueryStreamSize", ctx);
    const std::uint8_t *p = host_ptr(ctx.gpr[4], 16u);
    if (!p || std::memcmp(p, "PSMF", 4) != 0) { ret(ctx, kErrInvalid); return; }
    const std::uint32_t size = be32(p + 12);
    if (size & 2047u) { ret(ctx, kErrInvalid); return; }
    wr32(ctx.gpr[5], size);
    ret(ctx, 0u);
}
void h_regist_stream(Runtime &, AllegrexContext &ctx) {
    log_call("sceMpegRegistStream", ctx);
    ret(ctx, static_cast<std::uint32_t>(g_m.next_stream++));
}
void h_unregist_stream(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }
void h_flush_all(Runtime &, AllegrexContext &ctx) {
    log_call("sceMpegFlushAllStream", ctx);
    g_m.pend.clear();
    g_m.v_es.clear();
    g_m.v_head = g_m.v_scan = 0;
    g_m.v_base = 0;
    g_m.marks.clear();
    g_m.a_es.clear();
    g_m.a_head = 0;
    g_m.eof = false;
    update_ring();
    ret(ctx, 0u);
}

void h_ring_available(Runtime &, AllegrexContext &ctx) {
    update_ring();
    ret(ctx, rd32(ctx.gpr[4] + 12u));
}

void h_ring_put(Runtime &, AllegrexContext &ctx) {  // (ring, numPackets, available)
    log_call("sceMpegRingbufferPut", ctx);
    const std::uint32_t r = ctx.gpr[4];
    g_m.ring = r;
    update_ring();
    const std::uint32_t packets = rd32(r + 0u), data = rd32(r + 20u), cb = rd32(r + 24u), param = rd32(r + 28u);
    std::uint32_t n = std::min({ctx.gpr[5], ctx.gpr[6], rd32(r + 12u), packets});
    if (n == 0u || cb == 0u) { ret(ctx, 0u); return; }
    // The refill callback is a guest function (it may call into other guest code), so it runs on a helper
    // guest thread that borrows the caller's thread-local block; the caller waits for it.
    dump_game_state("before refill");
    const bool started = kernel_call_guest(ctx, cb, data, n, param, [r, data, n](std::uint32_t status) -> std::uint32_t {
        const std::int32_t got = static_cast<std::int32_t>(status);
        if (g_log) std::fprintf(stderr, "[movie +%lldms] ring refill callback: asked %u packets, got %d\n", now_ms(), n, got);
        ++g_m.puts;
        if (got > 0 && got <= static_cast<std::int32_t>(n)) {
            if (static_cast<std::uint32_t>(got) < n) g_m.eof = true;  // a short read means the file ended
            if (const std::uint8_t *p = host_ptr(data, static_cast<std::uint32_t>(got) * 2048u))
                feed(p, static_cast<std::size_t>(got) * 2048u);
            wr32(r + 8u, rd32(r + 8u) + static_cast<std::uint32_t>(got));
        } else {
            g_m.eof = true;
        }
        update_ring();
        dump_game_state("after refill");
        return status;
    });
    if (!started) ret(ctx, 0u);
}

void h_get_avc_au(Runtime &, AllegrexContext &ctx) {  // (mpeg, stream, au*, attr*)
    log_call("sceMpegGetAvcAu", ctx);
    Au au;
    if (!next_video_au(au)) {
        ++g_m.v_nodata;
        ++g_m.v_run;
        update_ring();
        // The last picture has no following access-unit delimiter; if the stream has gone quiet, release it.
        if (!g_m.eof && g_m.v_run >= 90u && g_m.v_head < g_m.v_es.size()) g_m.eof = true;
        if (g_log && (g_m.v_nodata == 1u || g_m.v_nodata % 120u == 0u) && g_m.ring != 0u)
            std::fprintf(stderr, "[movie]   ring: packets=%u read=%u written=%u free=%u\n", rd32(g_m.ring), rd32(g_m.ring + 4u),
                         rd32(g_m.ring + 8u), rd32(g_m.ring + 12u));
        if (g_log && (g_m.v_nodata == 1u || g_m.v_nodata % 120u == 0u))
            std::fprintf(stderr, "[movie +%lldms] GetAvcAu: no data (#%llu) eof=%d leftover=%zu aus=%llu pics=%llu fail=%llu\n", now_ms(),
                         static_cast<unsigned long long>(g_m.v_nodata), g_m.eof ? 1 : 0, g_m.v_es.size() - g_m.v_head,
                         static_cast<unsigned long long>(g_m.v_aus), static_cast<unsigned long long>(g_m.pics),
                         static_cast<unsigned long long>(g_m.dec_fail));
        ret(ctx, kErrNoData);
        return;
    }
    g_m.v_run = 0;
    ++g_m.v_aus;
    write_au(ctx.gpr[6], au, static_cast<std::uint32_t>(au.data.size()));
    wr32(ctx.gpr[7], 1u);
    g_m.cur_video = std::move(au);
    g_m.has_cur_video = true;
    update_ring();
    ret(ctx, 0u);
}
void h_get_atrac_au(Runtime &, AllegrexContext &ctx) {  // (mpeg, stream, au*, info*)
    log_call("sceMpegGetAtracAu", ctx);
    Au au;
    if (!next_audio_frame(au)) {
        ++g_m.a_nodata;
        update_ring();
        if (g_log && (g_m.a_nodata == 1u || g_m.a_nodata % 120u == 0u))
            std::fprintf(stderr, "[movie +%lldms] GetAtracAu: no data (#%llu) frames=%llu\n", now_ms(),
                         static_cast<unsigned long long>(g_m.a_nodata), static_cast<unsigned long long>(g_m.a_frames));
        ret(ctx, kErrNoData);
        return;
    }
    write_au(ctx.gpr[6], au, static_cast<std::uint32_t>(au.data.size()));
    wr32(ctx.gpr[7], 0u);
    g_m.cur_audio = std::move(au);
    g_m.has_cur_audio = true;
    update_ring();
    ret(ctx, 0u);
}

void h_init_au(Runtime &, AllegrexContext &ctx) {  // (mpeg, esBuffer, au*)
    const std::uint32_t au = ctx.gpr[6];
    wr32(au + 0u, 0xFFFFFFFFu);
    wr32(au + 4u, 0xFFFFFFFFu);
    wr32(au + 8u, 0xFFFFFFFFu);
    wr32(au + 12u, 0xFFFFFFFFu);
    wr32(au + 16u, ctx.gpr[5]);
    wr32(au + 20u, 0u);
    ret(ctx, 0u);
}
void h_malloc_es(Runtime &, AllegrexContext &ctx) { ret(ctx, static_cast<std::uint32_t>(g_m.next_esbuf++)); }
void h_free_es(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }
void h_query_atrac_es(Runtime &, AllegrexContext &ctx) {  // (mpeg, *esSize, *outSize)
    wr32(ctx.gpr[5], 2112u);
    wr32(ctx.gpr[6], 8192u);
    ret(ctx, 0u);
}
void h_ycbcr_size(Runtime &, AllegrexContext &ctx) {  // (mpeg, mode, width, height, int *size)
    log_call("sceMpegAvcQueryYCbCrSize", ctx);
    const std::uint32_t size = (ctx.gpr[6] / 2u) * (ctx.gpr[7] / 2u) * 6u + 128u;
    if (ctx.gpr[8] != 0u) wr32(ctx.gpr[8], size);
    ret(ctx, 0u);
}

void h_avc_decode(Runtime &, AllegrexContext &ctx) {  // (mpeg, au*, buffer*, init*)
    log_call("sceMpegAvcDecodeYCbCr", ctx);
    bool got = false;
    if (g_m.has_cur_video && g_m.h264 && g_m.h264_ok) {
        got = g_m.h264->decode(g_m.cur_video.data.data(), static_cast<int>(g_m.cur_video.data.size()), g_m.rgba,
                               g_m.w, g_m.h);
        if (got) ++g_m.pics;
        else ++g_m.dec_fail;
        if (g_log && g_m.dec_calls < 6u) {
            std::fprintf(stderr, "[movie +%lldms] decode #%llu: au=%zu bytes, nals:", now_ms(),
                         static_cast<unsigned long long>(g_m.dec_calls), g_m.cur_video.data.size());
            const auto &d = g_m.cur_video.data;
            int shown = 0;
            for (std::size_t i = 0; i + 4 < d.size() && shown < 8; ++i)
                if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { std::fprintf(stderr, " %d", d[i + 3] & 31); ++shown; i += 2; }
            std::fprintf(stderr, " -> %s (send rc=%d) %dx%d\n", got ? "picture" : "no picture", g_m.h264->last_send, g_m.w, g_m.h);
        }
        if (got && std::getenv("PERSONA1_MOVIEDUMP") && (g_m.pics == 1u || g_m.pics == 120u || g_m.pics == 600u)) {
            static int movie_no = 0;
            if (g_m.pics == 1u) ++movie_no;
            char name[64];
            std::snprintf(name, sizeof(name), "movie%d_frame%llu.bmp", movie_no, static_cast<unsigned long long>(g_m.pics));
            if (std::FILE *fp = std::fopen(name, "wb")) {
                const int w = g_m.w, h = g_m.h;
                const std::uint32_t isz = static_cast<std::uint32_t>(w * h * 4);
                std::uint8_t hdr[54] = {'B', 'M'};
                auto p32 = [&](int o, std::uint32_t v) { for (int i = 0; i < 4; ++i) hdr[o + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
                p32(2, 54u + isz); p32(10, 54u); p32(14, 40u); p32(18, static_cast<std::uint32_t>(w));
                p32(22, static_cast<std::uint32_t>(h)); hdr[26] = 1; hdr[28] = 32; p32(34, isz);
                std::fwrite(hdr, 1, 54, fp);
                for (int y = h - 1; y >= 0; --y)
                    for (int x = 0; x < w; ++x) {
                        const std::uint8_t *px = g_m.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u;
                        const std::uint8_t bgra[4] = {px[2], px[1], px[0], 255};
                        std::fwrite(bgra, 1, 4, fp);
                    }
                std::fclose(fp);
                std::fprintf(stderr, "[movie] wrote %s\n", name);
            }
        }
    }
    ++g_m.dec_calls;
    g_m.has_cur_video = false;
    wr32(ctx.gpr[7], got ? 1u : 0u);
    ret(ctx, 0u);
}
void h_avc_stop(Runtime &, AllegrexContext &ctx) {  // (mpeg, buffer*, status*)
    log_call("sceMpegAvcDecodeStopYCbCr", ctx);
    if (ctx.gpr[6] != 0u) wr32(ctx.gpr[6], 0u);
    ret(ctx, 0u);
}

void h_avc_csc(Runtime &, AllegrexContext &ctx) {  // (mpeg, source, range*, frameWidth, dest)
    log_call("sceMpegAvcCsc", ctx);
    const std::uint32_t range = ctx.gpr[6], fw = ctx.gpr[7], dest = ctx.gpr[8];
    const int rx = static_cast<int>(rd32(range + 0u)), ry = static_cast<int>(rd32(range + 4u));
    int rw = static_cast<int>(rd32(range + 8u)), rh = static_cast<int>(rd32(range + 12u));
    if (rw <= 0) rw = g_m.w;
    if (rh <= 0) rh = g_m.h;
    if (g_m.rgba.empty() || fw == 0u) { ret(ctx, 0u); return; }
    std::uint8_t *out = host_ptr(dest, fw * static_cast<std::uint32_t>(rh) * 4u);
    if (!out) { ret(ctx, 0u); return; }
    for (int y = 0; y < rh; ++y) {
        const int sy = ry + y;
        if (sy < 0 || sy >= g_m.h) continue;
        for (int x = 0; x < rw && static_cast<std::uint32_t>(x) < fw; ++x) {
            const int sx = rx + x;
            if (sx < 0 || sx >= g_m.w) continue;
            std::memcpy(out + (static_cast<std::size_t>(y) * fw + static_cast<std::size_t>(x)) * 4u,
                        g_m.rgba.data() + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(g_m.w) + static_cast<std::size_t>(sx)) * 4u,
                        4u);
        }
    }
    ret(ctx, 0u);
}

void h_atrac_decode(Runtime &, AllegrexContext &ctx) {  // (mpeg, au*, buffer*, init)
    log_call("sceMpegAtracDecode", ctx);
    std::uint8_t *out = host_ptr(ctx.gpr[6], 8192u);
    if (out) std::memset(out, 0, 8192u);
    if (g_m.has_cur_audio && out) {
        const std::vector<std::uint8_t> &f = g_m.cur_audio.data;
        if (f.size() > 8u) {
            const int block = static_cast<int>(f.size() - 8u);
            if (!g_m.atrac || g_m.atrac_block != block) {
                const int params = (f[2] << 8) | f[3];
                static const int rates[] = {32000, 44100, 48000, 88200, 96000, 0, 0, 0};
                const int rate = rates[(params >> 13) & 7] ? rates[(params >> 13) & 7] : 44100;
                const int ch = std::clamp((params >> 10) & 7, 1, 2);
                g_m.atrac = std::make_unique<AtracDec>();
                g_m.atrac_ok = g_m.atrac->open(ch, rate, block);
                g_m.atrac_block = block;
            }
            if (g_m.atrac_ok && g_m.atrac->decode(f.data() + 8, block, g_m.pcm))
                std::memcpy(out, g_m.pcm.data(), std::min<std::size_t>(g_m.pcm.size() * 2u, 8192u));
        }
    }
    g_m.has_cur_audio = false;
    ret(ctx, 0u);
}
void h_ret_zero(Runtime &, AllegrexContext &ctx) { ret(ctx, 0u); }

struct Entry { std::uint32_t nid; void (*fn)(Runtime &, AllegrexContext &); };
const Entry kTable[] = {
    {0x682A619Bu, h_init},          {0x874624D6u, h_finish},
    {0xC132E22Fu, h_query_mem},     {0xD7A29F46u, h_ring_query_mem},
    {0x37295ED8u, h_ring_construct},{0x13407F13u, h_ring_destruct},
    {0xD8C5F121u, h_create},        {0x606A4649u, h_delete},
    {0x21FF80E4u, h_query_stream_offset}, {0x611E9E11u, h_query_stream_size},
    {0x42560F23u, h_regist_stream}, {0x591A4AA2u, h_unregist_stream},
    {0x707B7629u, h_flush_all},     {0xB5F6DC87u, h_ring_available},
    {0xB240A59Eu, h_ring_put},      {0xFE246728u, h_get_avc_au},
    {0xE1CE83A7u, h_get_atrac_au},  {0x167AFD9Eu, h_init_au},
    {0xA780CF7Eu, h_malloc_es},     {0xCEB870B1u, h_free_es},
    {0xF8DCB679u, h_query_atrac_es},{0x211A057Cu, h_ycbcr_size},
    {0x67179B1Bu, h_ret_zero},      {0xF0EB1125u, h_avc_decode},
    {0xF2930C9Cu, h_avc_stop},      {0x4571CC64u, h_ret_zero},
    {0x31BD0272u, h_avc_csc},       {0x800C44DFu, h_atrac_decode},
};

std::set<std::uint32_t> &handled() {
    static std::set<std::uint32_t> s;
    return s;
}

} // namespace

void install_movie(Runtime &rt) {
    g_rt = &rt;
    g_log = std::getenv("PERSONA1_MOVIELOG") != nullptr;
    for (const Entry &e : kTable) {
        rt.register_hle("sceMpeg", e.nid, e.fn);
        handled().insert(e.nid);
    }
}

bool movie_handles(const std::string &library, std::uint32_t nid) {
    return library == "sceMpeg" && handled().count(nid) != 0u;
}

} // namespace persona1
