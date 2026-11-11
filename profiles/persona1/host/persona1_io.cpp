// Persona 1 host: IoFileMgrForUser. Files come from the extracted folder, or from the
// ISO itself for raw-sector paths such as disc0:/sce_lbn0xe020_size0x126b0.
#include "persona1_io.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace persona1 {
namespace rewards { extern bool resync; void on_load(const std::uint8_t *data, std::size_t n, std::uint32_t buf, std::uint32_t buf_size); }
namespace {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;

constexpr std::uint32_t kErrNoEnt = 0x80010002u;
constexpr std::uint32_t kErrBadFd = 0x80010009u;
constexpr std::uint32_t kErrIo = 0x80010005u;
constexpr std::uint64_t kSector = 2048u;

struct File {
    std::FILE *f{};
    bool iso{};          // virtual file inside the ISO
    std::uint64_t base{};  // byte offset in the ISO
    std::uint64_t size{};  // size of the virtual file
    std::uint64_t pos{};   // position inside the virtual file
};

struct Io {
    Runtime *rt{};
    IoConfig cfg;
    std::map<std::int32_t, File> files;
    std::map<std::int32_t, std::uint64_t> async_results;
    std::map<std::int32_t, std::uint64_t> async_last;   // last result per handle, kept after the game collects it
    std::map<std::int32_t, unsigned> async_miss;        // consecutive polls of a handle with nothing pending
    std::int32_t next_fd{0x40000};
    std::set<std::string> warned;
};

Io g;

void ret(AllegrexContext &ctx, std::uint32_t v) { ctx.set_gpr(2, v); }
void ret64(AllegrexContext &ctx, std::uint64_t v) {
    ctx.set_gpr(2, static_cast<std::uint32_t>(v));
    ctx.set_gpr(3, static_cast<std::uint32_t>(v >> 32));
}

std::string read_cstr(std::uint32_t addr) {
    std::string s;
    if (addr == 0u) return s;
    const auto m = g.rt->memory().aot_fast_view();
    for (std::uint32_t i = 0; i < 1024u; ++i) {
        const char c = static_cast<char>(m.aot_load8(addr + i));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

void write_guest(std::uint32_t addr, const std::uint8_t *data, std::size_t n) {
    const auto m = g.rt->memory().aot_fast_view();
    for (std::size_t i = 0; i < n; ++i) m.aot_store8(addr + static_cast<std::uint32_t>(i), data[i]);
}
void read_guest(std::uint32_t addr, std::uint8_t *data, std::size_t n) {
    const auto m = g.rt->memory().aot_fast_view();
    for (std::size_t i = 0; i < n; ++i) data[i] = m.aot_load8(addr + static_cast<std::uint32_t>(i));
}
void store32(std::uint32_t addr, std::uint32_t v) { g.rt->memory().aot_fast_view().aot_store32(addr, v); }

std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Parses "sce_lbn0x<lba>_size0x<size>" anywhere in the path.
bool parse_raw(const std::string &path, std::uint64_t &lba, std::uint64_t &size) {
    const std::string p = lower(path);
    const auto pos = p.find("sce_lbn0x");
    if (pos == std::string::npos) return false;
    unsigned long long l = 0, s = 0;
    if (std::sscanf(p.c_str() + pos, "sce_lbn0x%llx_size0x%llx", &l, &s) != 2) {
        // Some titles omit the size.
        if (std::sscanf(p.c_str() + pos, "sce_lbn0x%llx", &l) != 1) return false;
        s = 0;
    }
    lba = l;
    size = s;
    return true;
}

std::filesystem::path resolve(const std::string &path) {
    std::string p = path;
    std::filesystem::path root = g.cfg.game_root;
    const auto colon = p.find(':');
    if (colon != std::string::npos) {
        const std::string dev = lower(p.substr(0, colon));
        p = p.substr(colon + 1);
        if (dev.rfind("ms", 0) == 0 || dev.rfind("fatms", 0) == 0) root = g.cfg.save_root;
    }
    while (!p.empty() && (p[0] == '/' || p[0] == '\\')) p.erase(p.begin());
    std::filesystem::path out = root / p;
    return out;
}

std::uint64_t iso_size() {
    std::error_code ec;
    const auto s = std::filesystem::file_size(g.cfg.iso_path, ec);
    return ec ? 0u : static_cast<std::uint64_t>(s);
}

void warn_once(const std::string &key, const std::string &msg) {
    if (g.warned.insert(key).second) std::cerr << "[io] " << msg << "\n";
}


// ---- ISO9660 lookup: lets any disc0:/ file resolve straight from the ISO, even if it was not extracted ----
struct IsoEntry { std::uint64_t lba{}, size{}; bool dir{}; };
std::map<std::uint32_t, std::vector<std::uint8_t>> g_iso_dirs;

bool iso_read(std::uint64_t off, void *dst, std::size_t n) {
    std::FILE *f = std::fopen(g.cfg.iso_path.string().c_str(), "rb");
    if (!f) return false;
    bool ok = _fseeki64(f, static_cast<long long>(off), SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n;
    std::fclose(f);
    return ok;
}
inline std::uint32_t le32(const std::uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }

bool is_disc_path(const std::string &path) {
    const std::string p = lower(path);
    return p.rfind("disc0:", 0) == 0 || p.rfind("umd0:", 0) == 0 || p.rfind("disc:", 0) == 0 || p.rfind("umd:", 0) == 0;
}

bool iso_lookup(const std::string &path, IsoEntry &out) {
    if (g.cfg.iso_path.empty()) return false;
    std::uint8_t pvd[2048];
    if (!iso_read(16ull * kSector, pvd, sizeof pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    IsoEntry cur{le32(pvd + 156 + 2), le32(pvd + 156 + 10), true};
    std::string p = path.substr(path.find(':') + 1);
    std::size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && (p[i] == '/' || p[i] == '\\')) ++i;
        std::size_t j = i;
        while (j < p.size() && p[j] != '/' && p[j] != '\\') ++j;
        if (j == i) break;
        const std::string comp = lower(p.substr(i, j - i));
        i = j;
        if (!cur.dir) return false;
        auto &data = g_iso_dirs[static_cast<std::uint32_t>(cur.lba)];
        if (data.empty()) {
            data.resize(static_cast<std::size_t>(cur.size));
            if (!iso_read(cur.lba * kSector, data.data(), data.size())) { data.clear(); return false; }
        }
        bool found = false;
        for (std::size_t k = 0; k + 34 <= data.size();) {
            const std::uint8_t len = data[k];
            if (len == 0) { k = (k / kSector + 1) * kSector; continue; }
            const std::uint8_t nl = data[k + 32];
            if (k + 33 + nl <= data.size()) {
                std::string name(reinterpret_cast<const char *>(&data[k + 33]), nl);
                const auto semi = name.find(';');
                if (semi != std::string::npos) name.resize(semi);
                if (!name.empty() && name.back() == '.') name.pop_back();
                if (lower(name) == comp) {
                    cur = IsoEntry{le32(&data[k + 2]), le32(&data[k + 10]), (data[k + 25] & 2u) != 0};
                    found = true;
                    break;
                }
            }
            k += len;
        }
        if (!found) return false;
    }
    out = cur;
    return true;
}

// Returns a descriptor or a negative-as-u32 error code.
std::uint32_t do_open(const std::string &path, std::uint32_t flags) {
    std::uint64_t lba = 0, size = 0;
    if (parse_raw(path, lba, size)) {
        if (g.cfg.iso_path.empty()) {
            warn_once("noiso", "raw sector path needs the ISO; pass it as the 4th argument or set PERSONA1_ISO: " + path);
            return kErrNoEnt;
        }
        std::FILE *f = std::fopen(g.cfg.iso_path.string().c_str(), "rb");
        if (!f) {
            warn_once("badiso", "cannot open ISO " + g.cfg.iso_path.string());
            return kErrNoEnt;
        }
        File file;
        file.f = f;
        file.iso = true;
        file.base = lba * kSector;
        const std::uint64_t total = iso_size();
        if (size == 0u) size = total > file.base ? total - file.base : 0u;
        file.size = size;
        const std::int32_t fd = g.next_fd++;
        g.files[fd] = file;
        if (g.cfg.trace) std::cerr << "[io] open raw " << path << " -> fd " << fd << "\n";
        return static_cast<std::uint32_t>(fd);
    }

    const std::filesystem::path real = resolve(path);
    if (is_disc_path(path)) {
        std::error_code ec0;
        if (!std::filesystem::exists(real, ec0)) {
            IsoEntry e;
            if (iso_lookup(path, e) && !e.dir) {
                char raw[96];
                std::snprintf(raw, sizeof raw, "sce_lbn0x%llx_size0x%llx", static_cast<unsigned long long>(e.lba), static_cast<unsigned long long>(e.size));
                if (g.cfg.trace) std::cerr << "[io] " << path << " found in ISO at lba " << e.lba << "\n";
                return do_open(raw, flags);
            }
        }
    }
    const bool write = (flags & 2u) != 0u;
    const bool creat = (flags & 0x200u) != 0u;
    const bool trunc = (flags & 0x400u) != 0u;
    const bool append = (flags & 0x100u) != 0u;
    const char *mode = "rb";
    if (write) {
        std::error_code ec;
        if (creat) std::filesystem::create_directories(real.parent_path(), ec);
        const bool exists = std::filesystem::exists(real, ec);
        if (trunc || (!exists && creat)) mode = (flags & 1u) ? "w+b" : "wb";
        else if (exists) mode = "r+b";
        else {
            warn_once("nf:" + path, "open failed (not found): " + path);
            return kErrNoEnt;
        }
    }
    std::FILE *f = std::fopen(real.string().c_str(), mode);
    if (!f) {
        warn_once("nf:" + path, "open failed (not found): " + path + "  -> " + real.string());
        return kErrNoEnt;
    }
    File file;
    file.f = f;
    if (append) std::fseek(f, 0, SEEK_END);
    const std::int32_t fd = g.next_fd++;
    g.files[fd] = file;
    if (g.cfg.trace) std::cerr << "[io] open " << path << " -> fd " << fd << "\n";
    return static_cast<std::uint32_t>(fd);
}

std::uint32_t do_close(std::int32_t fd) {
    auto it = g.files.find(fd);
    if (it == g.files.end()) return kErrBadFd;
    if (it->second.f) std::fclose(it->second.f);
    g.files.erase(it);
    return 0u;
}

std::uint32_t do_read(std::int32_t fd, std::uint32_t buf, std::uint32_t n) {
    auto it = g.files.find(fd);
    if (it == g.files.end()) return kErrBadFd;
    File &f = it->second;
    std::vector<std::uint8_t> tmp;
    std::size_t got = 0;
    if (f.iso) {
        if (f.pos >= f.size) return 0u;
        const std::uint64_t want = std::min<std::uint64_t>(n, f.size - f.pos);
        tmp.resize(static_cast<std::size_t>(want));
        std::fseek(f.f, 0, SEEK_SET);
        if (_fseeki64(f.f, static_cast<long long>(f.base + f.pos), SEEK_SET) != 0) return kErrIo;
        got = std::fread(tmp.data(), 1, tmp.size(), f.f);
        f.pos += got;
    } else {
        tmp.resize(n);
        got = std::fread(tmp.data(), 1, n, f.f);
    }
    write_guest(buf, tmp.data(), got);
    return static_cast<std::uint32_t>(got);
}

std::uint32_t do_write(std::int32_t fd, std::uint32_t buf, std::uint32_t n) {
    auto it = g.files.find(fd);
    if (it == g.files.end() || it->second.iso) return kErrBadFd;
    std::vector<std::uint8_t> tmp(n);
    read_guest(buf, tmp.data(), n);
    return static_cast<std::uint32_t>(std::fwrite(tmp.data(), 1, n, it->second.f));
}

std::int64_t do_seek(std::int32_t fd, std::int64_t off, std::uint32_t whence) {
    auto it = g.files.find(fd);
    if (it == g.files.end()) return -static_cast<std::int64_t>(0x7FFFFFFF);
    File &f = it->second;
    if (f.iso) {
        std::int64_t np = 0;
        if (whence == 0u) np = off;
        else if (whence == 1u) np = static_cast<std::int64_t>(f.pos) + off;
        else np = static_cast<std::int64_t>(f.size) + off;
        if (np < 0) np = 0;
        f.pos = static_cast<std::uint64_t>(np);
        return np;
    }
    const int w = whence == 0u ? SEEK_SET : whence == 1u ? SEEK_CUR : SEEK_END;
    if (_fseeki64(f.f, off, w) != 0) return -1;
    return _ftelli64(f.f);
}

// SceIoStat: mode, attr, size(8), ctime/atime/mtime (16 each), private[6]
void fill_stat(std::uint32_t out, bool dir, std::uint64_t size) {
    for (std::uint32_t i = 0; i < 0x58u; i += 4u) store32(out + i, 0u);
    store32(out + 0, dir ? 0x11FFu : 0x21FFu);
    store32(out + 4, dir ? 0x10u : 0x20u);
    store32(out + 8, static_cast<std::uint32_t>(size));
    store32(out + 12, static_cast<std::uint32_t>(size >> 32));
}

std::uint32_t do_getstat(const std::string &path, std::uint32_t out) {
    std::uint64_t lba = 0, size = 0;
    if (parse_raw(path, lba, size)) {
        if (size == 0u) {
            const std::uint64_t total = iso_size();
            size = total > lba * kSector ? total - lba * kSector : 0u;
        }
        fill_stat(out, false, size);
        return 0u;
    }
    std::error_code ec;
    const auto real = resolve(path);
    if (!std::filesystem::exists(real, ec)) {
        IsoEntry e;
        if (is_disc_path(path) && iso_lookup(path, e)) { fill_stat(out, e.dir, e.dir ? 0u : e.size); return 0u; }
        return kErrNoEnt;
    }
    const bool dir = std::filesystem::is_directory(real, ec);
    fill_stat(out, dir, dir ? 0u : static_cast<std::uint64_t>(std::filesystem::file_size(real, ec)));
    return 0u;
}

// ---- handlers ----------------------------------------------------------------------
void h_open(Runtime &, AllegrexContext &ctx) {
    ret(ctx, do_open(read_cstr(ctx.gpr[4]), ctx.gpr[5]));
}
void h_close(Runtime &, AllegrexContext &ctx) { ret(ctx, do_close(static_cast<std::int32_t>(ctx.gpr[4]))); }
void h_read(Runtime &, AllegrexContext &ctx) {
    ret(ctx, do_read(static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5], ctx.gpr[6]));
}
void h_write(Runtime &, AllegrexContext &ctx) {
    ret(ctx, do_write(static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5], ctx.gpr[6]));
}
void h_lseek(Runtime &, AllegrexContext &ctx) {
    const std::int64_t off = static_cast<std::int64_t>((static_cast<std::uint64_t>(ctx.gpr[7]) << 32) | ctx.gpr[6]);
    ret64(ctx, static_cast<std::uint64_t>(do_seek(static_cast<std::int32_t>(ctx.gpr[4]), off, ctx.gpr[8])));
}
void h_getstat(Runtime &, AllegrexContext &ctx) {
    const std::string path = read_cstr(ctx.gpr[4]);
    std::uint32_t r = do_getstat(path, ctx.gpr[5]);
    if (r != 0u) {
        static std::map<std::string, unsigned> misses;
        const unsigned n = ++misses[path];
        if (n == 1) std::cerr << "[io] getstat failed (file not found): '" << path << "'\n";
    }
    ret(ctx, r);
}

// Async calls run synchronously; the result is parked until the game polls for it.
void h_open_async(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t r = do_open(read_cstr(ctx.gpr[4]), ctx.gpr[5]);
    const std::int32_t key = static_cast<std::int32_t>(r) >= 0 ? static_cast<std::int32_t>(r) : 0;
    g.async_results[key] = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(r)));
    g.async_last[key] = g.async_results[key];
    g.async_miss[key] = 0;
    ret(ctx, static_cast<std::int32_t>(r) >= 0 ? r : 0u);
}
void h_close_async(Runtime &, AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    g.async_results[fd] = do_close(fd);
    g.async_last[fd] = g.async_results[fd];
    g.async_miss[fd] = 0;
    ret(ctx, 0u);
}
void h_read_async(Runtime &, AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t r = do_read(fd, ctx.gpr[5], ctx.gpr[6]);
    g.async_results[fd] = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(r)));
    g.async_last[fd] = g.async_results[fd];
    g.async_miss[fd] = 0;
    ret(ctx, 0u);
}
void h_lseek_async(Runtime &, AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::int64_t off = static_cast<std::int64_t>((static_cast<std::uint64_t>(ctx.gpr[7]) << 32) | ctx.gpr[6]);
    g.async_results[fd] = static_cast<std::uint64_t>(do_seek(fd, off, ctx.gpr[8]));
    g.async_last[fd] = g.async_results[fd];
    g.async_miss[fd] = 0;
    ret(ctx, 0u);
}
// sceIoPollAsync(fd, s64 *res) / sceIoWaitAsyncCB(fd, s64 *res)
void h_poll_async(Runtime &, AllegrexContext &ctx) {
    const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t out = ctx.gpr[5];
    auto it = g.async_results.find(fd);
    if (it == g.async_results.end()) {
        // Nothing parked for this handle. Report "finished" with the last known result instead of "busy" forever,
        // otherwise a game that polls in a sleep loop never leaves it.
        const unsigned miss = ++g.async_miss[fd];
        if (miss >= 3) {
            const auto last = g.async_last.find(fd);
            const std::uint64_t r64 = last != g.async_last.end() ? last->second : 0u;
            if (out) { store32(out, static_cast<std::uint32_t>(r64)); store32(out + 4, static_cast<std::uint32_t>(r64 >> 32)); }
            std::cerr << "[io] sceIoPollAsync(fd=" << fd << ") had nothing pending for " << miss << " polls - reporting finished, result=0x"
                      << std::hex << r64 << std::dec << (last != g.async_last.end() ? "" : " (no prior result)") << "\n";
            g.async_miss[fd] = 0;
            ret(ctx, 0u);
            return;
        }
        if (out) { store32(out, 0u); store32(out + 4, 0u); }
        ret(ctx, 1u);  // nothing pending
        return;
    }
    if (out) {
        store32(out, static_cast<std::uint32_t>(it->second));
        store32(out + 4, static_cast<std::uint32_t>(it->second >> 32));
    }
    g.async_results.erase(it);
    g.async_miss[fd] = 0;
    ret(ctx, 0u);
}

// ---- sceUtility savedata: real saving to <ms0>/PSP/SAVEDATA/<game><slot>/ ---------------
// The Sony save dialog is replaced by direct file access. Every slot is an ordinary folder, so a save can be
// copied or backed up by hand (for example to keep one copy per story route).
struct Savedata {
    int status{0};  // 0 none, 2 running, 3 finished
    int updates{0};
    std::uint32_t params{};
} sd;

std::uint32_t sd_u32(std::uint32_t off) { return g.rt->memory().aot_fast_view().aot_load32(sd.params + off); }
std::string sd_str(std::uint32_t addr, std::uint32_t max) {
    std::string r;
    if (addr < 0x08000000u || addr > 0x0A000000u) return r;
    const auto m = g.rt->memory().aot_fast_view();
    for (std::uint32_t i = 0; i < max; ++i) {
        const char c = static_cast<char>(m.aot_load8(addr + i));
        if (c == 0) break;
        r.push_back(c);
    }
    return r;
}
void sd_put_str(std::uint32_t addr, const std::string &v, std::uint32_t max) {
    const auto m = g.rt->memory().aot_fast_view();
    for (std::uint32_t i = 0; i < max; ++i) m.aot_store8(addr + i, i < v.size() ? static_cast<std::uint8_t>(v[i]) : 0u);
}

std::filesystem::path sd_dir(const std::string &game, const std::string &slot) {
    return g.cfg.save_root / "PSP" / "SAVEDATA" / (game + slot);
}
std::vector<std::string> sd_slot_list() {
    std::vector<std::string> names;
    const std::uint32_t list = sd_u32(0x60);
    if (!list || list < 0x08000000u || list > 0x0A000000u) return names;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const std::string n = sd_str(list + i * 20u, 20);
        if (n.empty()) break;
        names.push_back(n);
    }
    return names;
}
bool sd_exists(const std::filesystem::path &dir, const std::string &file) {
    std::error_code ec;
    return std::filesystem::exists(dir / file, ec);
}
std::filesystem::file_time_type sd_mtime(const std::filesystem::path &p) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    return ec ? std::filesystem::file_time_type::min() : t;
}

int g_active_slot = 0;  // chosen with F3/F4; used by the list save/load modes

std::uint32_t sd_do_save(const std::filesystem::path &dir, const std::string &file) {
    const std::uint32_t buf = sd_u32(0x74), bufsize = sd_u32(0x78), size = sd_u32(0x7C);
    const std::uint32_t n = size ? std::min(size, bufsize ? bufsize : size) : bufsize;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<std::uint8_t> tmp(n);
    if (n && buf) read_guest(buf, tmp.data(), n);
    std::FILE *f = std::fopen((dir / file).string().c_str(), "wb");
    if (!f) return 0x80110385u;  // SAVE_ACCESS_ERROR
    std::fwrite(tmp.data(), 1, tmp.size(), f);
    std::fclose(f);
    // Human-readable description (the real PARAM.SFO is not needed).
    if (std::FILE *m = std::fopen((dir / "PARAM.TXT").string().c_str(), "wb")) {
        const std::string title = sd_str(sd.params + 0x80, 0x80), stitle = sd_str(sd.params + 0x100, 0x80), detail = sd_str(sd.params + 0x180, 0x400);
        std::fprintf(m, "title=%s\nsavedata_title=%s\ndetail=%s\n", title.c_str(), stitle.c_str(), detail.c_str());
        std::fclose(m);
    }
    std::cerr << "[sd] saved " << n << " bytes (game memory 0x" << std::hex << buf << std::dec << ") -> " << (dir / file).string() << "\n";
    return 0u;
}

std::uint32_t sd_do_load(const std::filesystem::path &dir, const std::string &file) {
    const std::uint32_t buf = sd_u32(0x74), bufsize = sd_u32(0x78);
    std::FILE *f = std::fopen((dir / file).string().c_str(), "rb");
    if (!f) return 0x80110307u;  // LOAD_NO_DATA
    std::vector<std::uint8_t> tmp(bufsize ? bufsize : 0x100000u);
    const std::size_t got = std::fread(tmp.data(), 1, tmp.size(), f);
    std::fclose(f);
    if (buf) write_guest(buf, tmp.data(), got);
    store32(sd.params + 0x7Cu, static_cast<std::uint32_t>(got));
    rewards::on_load(tmp.data(), got, buf, bufsize);
    std::cerr << "[sd] loaded " << got << " bytes <- " << (dir / file).string() << "\n";
    return 0u;
}

void sd_run() {
    const std::uint32_t mode = sd_u32(0x30);
    const std::string game = sd_str(sd.params + 0x3C, 13);
    const std::uint32_t slot_ptr = sd.params + 0x4Cu;  // saveName is an inline char[20]
    std::string slot = sd_str(slot_ptr, 20);
    std::string file = sd_str(sd.params + 0x64, 13);
    if (file.empty()) file = "DATA.BIN";
    const std::vector<std::string> names = sd_slot_list();
    std::uint32_t result = 0u;
    switch (mode) {
    case 0: case 2: case 16: case 15:  // auto load / load / read
        result = sd_do_load(sd_dir(game, slot), file);
        // Startup auto-load with no save: keep the code the game already handled correctly (sound/config defaults).
        if (mode == 0 && result == 0x80110307u) result = 0x80110327u;
        break;
    case 1: case 3: case 18: case 17: case 13: case 14:  // auto save / save / write / makedata
        result = sd_do_save(sd_dir(game, slot), file);
        break;
    case 4: {  // list load: the slot chosen with F3/F4, else the most recently written one
        std::vector<std::string> ch;
        for (const auto &n : names) if (n != "SYSI") ch.push_back(n);
        std::string best;
        if (!ch.empty()) {
            const std::string &want = ch[static_cast<std::size_t>(g_active_slot) % ch.size()];
            if (sd_exists(sd_dir(game, want), file)) best = want;
        }
        if (best.empty()) {
            auto bt = std::filesystem::file_time_type::min();
            for (const auto &n : ch) {
                const auto dir = sd_dir(game, n);
                if (!sd_exists(dir, file)) continue;
                const auto t = sd_mtime(dir / file);
                if (best.empty() || t > bt) { best = n; bt = t; }
            }
        }
        if (best.empty()) { result = 0x80110307u; break; }
        sd_put_str(slot_ptr, best, 20);
        slot = best;
        result = sd_do_load(sd_dir(game, best), file);
        break;
    }
    case 5: {  // list save: the slot chosen with F3/F4
        std::vector<std::string> ch;
        for (const auto &n : names) if (n != "SYSI") ch.push_back(n);
        std::string pick = ch.empty() ? (slot.empty() ? std::string("0000") : slot)
                                      : ch[static_cast<std::size_t>(g_active_slot) % ch.size()];
        sd_put_str(slot_ptr, pick, 20);
        slot = pick;
        result = sd_do_save(sd_dir(game, pick), file);
        break;
    }
    case 6: case 7: case 9: case 10: case 21: case 19: case 20: {  // delete / erase
        std::error_code ec;
        if (mode == 6 || mode == 9) for (const auto &n : names) std::filesystem::remove_all(sd_dir(game, n), ec);
        else std::filesystem::remove_all(sd_dir(game, slot), ec);
        break;
    }
    case 8: case 22: case 11: case 12:  // sizes / list / files: not supported
        result = 0x80110327u;
        break;
    default:
        break;
    }
    std::cerr << "[sd] mode=" << mode << " game='" << game << "' slot='" << slot << "' file='" << file << "' slots=" << names.size()
              << " -> result=0x" << std::hex << result << std::dec << "\n";
    store32(sd.params + 0x1Cu, result);
}

void h_sd_init(Runtime &, AllegrexContext &ctx) {
    sd.params = ctx.gpr[4];
    sd.status = 2;
    sd.updates = 0;
    ret(ctx, 0u);
}
void h_sd_update(Runtime &, AllegrexContext &ctx) {
    if (sd.status == 2 && ++sd.updates >= 3) {
        sd_run();
        sd.status = 3;
    }
    ret(ctx, 0u);
}
void h_sd_status(Runtime &, AllegrexContext &ctx) {
    // The game polls this without calling Update every time, so advance here too.
    if (sd.status == 2) {
        AllegrexContext dummy = ctx;
        h_sd_update(*g.rt, dummy);
    }
    ret(ctx, static_cast<std::uint32_t>(sd.status));
}
void h_sd_shutdown(Runtime &, AllegrexContext &ctx) {
    sd.status = 0;
    ret(ctx, 0u);
}

// Memory Stick device control: the game asks for free space before saving.
void h_devctl(Runtime &, AllegrexContext &ctx) {
    const std::uint32_t cmd = ctx.gpr[5], indata = ctx.gpr[6], outdata = ctx.gpr[8];
    const auto m = g.rt->memory().aot_fast_view();
    std::uint32_t result = 0u;
    if (cmd == 0x02425818u) {  // get free space; indata -> pointer to SceIoDevInfo-like struct
        const std::uint32_t info = indata ? m.aot_load32(indata) : 0u;
        if (info >= 0x08000000u && info < 0x0A000000u) {
            m.aot_store32(info + 0, 0x3D000u);   // maxClusters
            m.aot_store32(info + 4, 0x20000u);   // freeClusters
            m.aot_store32(info + 8, 0x3D000u);   // maxSectors
            m.aot_store32(info + 12, 0x200u);    // sectorSize
            m.aot_store32(info + 16, 0x40u);     // sectorCount (cluster = 32 KB)
        }
    } else if (cmd == 0x02025806u) {  // memory stick inserted
        if (outdata >= 0x08000000u && outdata < 0x0A000000u) m.aot_store32(outdata, 1u);
    }
    std::cerr << "[io] devctl cmd=0x" << std::hex << cmd << std::dec << " -> " << result << "\n";
    ret(ctx, result);
}

// ---- sceUtility message dialog: shown by the load screen ("no save data" etc.). Auto-answered, text logged. ----
struct MsgDlg { int status{0}; int updates{0}; std::uint32_t params{}; } md;
void md_finish() {
    // SceUtilityMsgDialogParam: mode@0x34 error@0x38 message@0x3C options@0x23C buttonPressed@0x240
    const std::uint32_t mode = g.rt->memory().aot_fast_view().aot_load32(md.params + 0x34);
    const std::uint32_t opts = g.rt->memory().aot_fast_view().aot_load32(md.params + 0x23C);
    std::cerr << "[msgdlg] mode=" << mode << " options=0x" << std::hex << opts << std::dec << " text='"
              << sd_str(md.params + 0x3C, 512) << "' -> answered\n";
    store32(md.params + 0x240u, 1u);  // "Yes"/OK
    store32(md.params + 0x1Cu, 0u);
}
void md_step() {
    if (md.status == 2 && ++md.updates >= 3) { md_finish(); md.status = 3; }
}
void h_md_init(Runtime &, AllegrexContext &ctx) { md.params = ctx.gpr[4]; md.status = 2; md.updates = 0; ret(ctx, 0u); }
void h_md_update(Runtime &, AllegrexContext &ctx) { md_step(); ret(ctx, 0u); }
void h_md_status(Runtime &, AllegrexContext &ctx) { md_step(); ret(ctx, static_cast<std::uint32_t>(md.status)); }
void h_md_shutdown(Runtime &, AllegrexContext &ctx) { md.status = 0; ret(ctx, 0u); }

struct Def {
    const char *library;
    const char *name;
    std::uint32_t nid;
    void (*fn)(Runtime &, AllegrexContext &);
};

const Def kDefs[] = {
    {"IoFileMgrForUser", "open", 0x109F50BCu, h_open},          {"IoFileMgrForUser", "close", 0x810C4BC3u, h_close},
    {"IoFileMgrForUser", "read", 0x6A638D83u, h_read},          {"IoFileMgrForUser", "write", 0x42EC03ACu, h_write},
    {"IoFileMgrForUser", "lseek", 0x27EB27B8u, h_lseek},        {"IoFileMgrForUser", "getstat", 0xACE946E8u, h_getstat},
    {"IoFileMgrForUser", "openAsync", 0x89AA9906u, h_open_async}, {"IoFileMgrForUser", "closeAsync", 0xFF5940B6u, h_close_async},
    {"IoFileMgrForUser", "readAsync", 0xA0B5A7C2u, h_read_async}, {"IoFileMgrForUser", "lseekAsync", 0x71B19E77u, h_lseek_async},
    {"IoFileMgrForUser", "pollAsync", 0x3251EA56u, h_poll_async}, {"IoFileMgrForUser", "waitAsyncCB", 0x35DBD746u, h_poll_async},
    {"IoFileMgrForUser", "devctl", 0x54F5FB11u, h_devctl},
    {"sceUtility", "MsgDialogInitStart", 0x2AD8E239u, h_md_init},
    {"sceUtility", "MsgDialogUpdate", 0x95FC253Bu, h_md_update},
    {"sceUtility", "MsgDialogGetStatus", 0x9A1C91D7u, h_md_status},
    {"sceUtility", "MsgDialogShutdownStart", 0x67AF3428u, h_md_shutdown},
    {"sceUtility", "MsgDialogShutdownStart2", 0x4928BD96u, h_md_shutdown},
    {"sceUtility", "SavedataInitStart", 0x50C4CD57u, h_sd_init},
    {"sceUtility", "SavedataUpdate", 0xD4B95FFBu, h_sd_update},
    {"sceUtility", "SavedataGetStatus", 0x8874DBE0u, h_sd_status},
    {"sceUtility", "SavedataShutdownStart", 0x9790B33Cu, h_sd_shutdown},
};

} // namespace

void install_io(Runtime &rt, const IoConfig &cfg) {
    g.rt = &rt;
    g.cfg = cfg;
    for (const Def &d : kDefs) rt.register_hle(d.library, d.nid, d.fn);
    if (cfg.iso_path.empty())
        std::cerr << "[io] no ISO given: raw-sector files (sce_lbn...) will fail. Pass the ISO path as 4th argument or PERSONA1_ISO.\n";
}

// ---- Cheat / memory search tool. Press F5 in the game window: reads persona1\cheat.txt, runs the commands,
// writes persona1\cheat_out.txt. Commands (one per line, numbers decimal or 0x hex):
//   new <1|2|4> <value>    scan RAM for a value of that byte width, start a candidate list
//   eq <value>             keep candidates now equal to value
//   changed | unchanged    keep candidates that changed / did not change since the last step
//   up | down              keep candidates that increased / decreased since the last step
//   list [n]               show up to n candidates (default 40) with their values
//   set <addr> <1|2|4> <value>   write a value into the game's memory
//   dump <addr> <bytes>    hex dump
//   clear                  forget the candidates
namespace cheat {
std::uint32_t rd(std::uint32_t a, int w) {
    const auto m = g.rt->memory().aot_fast_view();
    std::uint32_t v = 0;
    for (int i = 0; i < w; ++i) v |= static_cast<std::uint32_t>(m.aot_load8(a + static_cast<std::uint32_t>(i))) << (8 * i);
    return v;
}
void wr(std::uint32_t a, int w, std::uint32_t v) {
    const auto m = g.rt->memory().aot_fast_view();
    for (int i = 0; i < w; ++i) m.aot_store8(a + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(v >> (8 * i)));
}
struct Cand { std::uint32_t addr; std::uint32_t last; };
std::vector<Cand> cands;
int width = 4;
constexpr std::uint32_t kLo = 0x08800000u, kHi = 0x0A000000u;
std::uint32_t num(const std::string &t) { return static_cast<std::uint32_t>(std::strtoull(t.c_str(), nullptr, 0)); }
}  // namespace cheat

void cheat_run() {
    using namespace cheat;
    if (!g.rt) return;
    const std::filesystem::path dir = g.cfg.save_root.parent_path();
    std::ifstream in(dir / "cheat.txt");
    std::ostringstream out;
    if (!in) out << "cheat.txt not found in " << dir.string() << "\n";
    std::string line;
    while (in && std::getline(in, line)) {
        std::istringstream ls(line);
        std::string cmd;
        if (!(ls >> cmd) || cmd[0] == '#') continue;
        for (auto &c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string a, b, c3;
        ls >> a >> b >> c3;
        out << "> " << line << "\n";
        if (cmd == "new") {
            width = a == "1" ? 1 : a == "2" ? 2 : 4;
            const std::uint32_t v = num(b);
            cands.clear();
            for (std::uint32_t ad = kLo; ad + static_cast<std::uint32_t>(width) <= kHi; ad += static_cast<std::uint32_t>(width)) {
                const std::uint32_t x = rd(ad, width);
                if (x == v) cands.push_back({ad, x});
            }
            out << cands.size() << " candidates\n";
        } else if (cmd == "eq" || cmd == "changed" || cmd == "unchanged" || cmd == "up" || cmd == "down") {
            const std::uint32_t v = num(a);
            std::vector<Cand> keep;
            for (auto &cd : cands) {
                const std::uint32_t x = rd(cd.addr, width);
                const bool ok = cmd == "eq" ? x == v : cmd == "changed" ? x != cd.last : cmd == "unchanged" ? x == cd.last
                              : cmd == "up" ? x > cd.last : x < cd.last;
                if (ok) keep.push_back({cd.addr, x});
            }
            cands.swap(keep);
            out << cands.size() << " candidates\n";
        } else if (cmd == "list") {
            const std::size_t n = a.empty() ? 40u : static_cast<std::size_t>(num(a));
            for (std::size_t i = 0; i < cands.size() && i < n; ++i)
                out << "  0x" << std::hex << cands[i].addr << " = " << std::dec << rd(cands[i].addr, width) << "\n";
            out << cands.size() << " candidates total\n";
        } else if (cmd == "set") {
            wr(num(a), b == "1" ? 1 : b == "2" ? 2 : 4, num(c3));
            out << "wrote\n";
        } else if (cmd == "dump") {
            const std::uint32_t ad = num(a), n = std::min<std::uint32_t>(num(b), 0x1000u);
            for (std::uint32_t i = 0; i < n; i += 16) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%08X: ", ad + i);
                out << buf;
                for (std::uint32_t k = 0; k < 16 && i + k < n; ++k) {
                    std::snprintf(buf, sizeof buf, "%02X ", rd(ad + i + k, 1));
                    out << buf;
                }
                out << "\n";
            }
        } else if (cmd == "clear") {
            cands.clear();
            out << "cleared\n";
        } else {
            out << "unknown command\n";
        }
    }
    std::ofstream(dir / "cheat_out.txt") << out.str();
    std::cerr << "[cheat]\n" << out.str();
}


// ---- Reward multiplier (R3 on the controller cycles off -> 2x -> 4x). Watches the party's EXP and the money in
// guest memory once per controller poll; when EXP rises (a battle reward) it adds the extra EXP, and the money
// gained in the same moment is multiplied too. Selling items etc. (money rising with no EXP) is left alone.
// Layout (checked against the save file, which is a raw copy of this memory): base 0x08A791F4,
// party record i at base + 0x80*i with EXP at +0x10; money at base + 0x1008.
namespace rewards {
constexpr std::uint32_t kSlots = 10;
std::uint32_t kBase = 0x08A791F4u, kMoney = kBase + 0x1008u;  // base is re-located after each save load
int mult = 1;
bool have = false, resync = false;
std::uint32_t prev_money = 0, prev_exp[kSlots] = {};
unsigned long long tick = 0, last_exp_tick = 0, pend_tick = 0;
std::uint32_t pend_money = 0;
std::uint32_t ld(std::uint32_t a) { return g.rt->memory().aot_fast_view().aot_load32(a); }
std::uint8_t sig[0x20];
std::uint32_t sig_money = 0, sig_buf = 0, sig_buf_end = 0;
bool scan_pending = false;
unsigned long long scan_at = 0;
void on_load(const std::uint8_t *data, std::size_t n, std::uint32_t buf, std::uint32_t buf_size) {
    resync = true;
    if (n < 0x1010) return;  // the small system file etc.
    std::memcpy(sig, data, sizeof sig);
    std::memcpy(&sig_money, data + 0x1008, 4);
    sig_buf = buf; sig_buf_end = buf + std::max<std::uint32_t>(buf_size, static_cast<std::uint32_t>(n));
    scan_pending = true;
    scan_at = tick + 90;  // give the game ~1.5 s to copy the save into its live data
}

void st(std::uint32_t a, std::uint32_t v) { g.rt->memory().aot_fast_view().aot_store32(a, v); }
}  // namespace rewards

int rewards_cycle() {
    using namespace rewards;
    mult = mult == 1 ? 2 : mult == 2 ? 4 : 1;
    resync = true;
    std::cerr << "[rewards] EXP/yen multiplier: " << (mult == 1 ? std::string("off") : std::to_string(mult) + "x") << "\n";
    return mult;
}
int rewards_mult() { return rewards::mult; }
void rewards_resync() { rewards::resync = true; }

void rewards_tick() {
    using namespace rewards;
    if (!g.rt) return;
    ++tick;
    if (scan_pending && tick >= scan_at) {
        scan_pending = false;
        std::uint32_t found = 0;
        for (std::uint32_t a = 0x08800000u; a + 0x1010u <= 0x0A000000u && !found; a += 4u) {
            if (a >= sig_buf && a < sig_buf_end) { a = (sig_buf_end - 4u); continue; }
            if (ld(a) != *reinterpret_cast<std::uint32_t *>(sig)) continue;
            bool ok = true;
            for (std::uint32_t k = 4; k < sizeof sig && ok; k += 4) ok = ld(a + k) == *reinterpret_cast<std::uint32_t *>(sig + k);
            if (ok && ld(a + 0x1008u) == sig_money) found = a;
        }
        if (found) { kBase = found; kMoney = found + 0x1008u; resync = true; }
        std::cerr << "[rewards] party data " << (found ? "found" : "NOT found (using default)") << " at 0x" << std::hex << kBase << std::dec << "\n";
    }
    static const bool dbg = std::getenv("PERSONA1_REWARDSLOG") != nullptr;
    static bool said = false;
    if (!said) { said = true; std::cerr << "[rewards] watcher running, debug log " << (dbg ? "ON" : "off") << "\n"; }
    if (dbg) {  // log every changed word of the party/money block so the EXP fields can be found
        static std::vector<std::uint32_t> snap;
        static unsigned lines = 0;
        constexpr std::uint32_t kWords = 0x1100u / 4u;
        static std::uint32_t snap_base = 0;
        if (snap_base != kBase) { snap.clear(); snap_base = kBase; }
        if (snap.empty()) { snap.resize(kWords); for (std::uint32_t i = 0; i < kWords; ++i) snap[i] = ld(kBase + 4u * i); }
        unsigned n = 0;
        for (std::uint32_t i = 0; i < kWords; ++i) {
            const std::uint32_t v = ld(kBase + 4u * i);
            if (v != snap[i]) {
                if (lines < 4000 && n < 24) { std::fprintf(stderr, "[rewards-dbg] +0x%04X (0x%08X): %u -> %u\n", 4u * i, kBase + 4u * i, snap[i], v); ++lines; }
                ++n;
                snap[i] = v;
            }
        }
    }
    const std::uint32_t m = ld(kMoney);
    std::uint32_t e[kSlots];
    for (std::uint32_t i = 0; i < kSlots; ++i) e[i] = ld(kBase + 0x80u * i + 0x10u);
    if (!have || resync || mult == 1) {
        have = true; resync = false;
        prev_money = m;
        for (std::uint32_t i = 0; i < kSlots; ++i) prev_exp[i] = e[i];
        pend_money = 0;
        return;
    }
    constexpr std::uint32_t kMaxDelta = 1000000u, kMoneyCap = 9999999u;
    const std::uint32_t extra = static_cast<std::uint32_t>(mult - 1);
    bool exp_gain = false;
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        if (prev_exp[i] > 0 && e[i] > prev_exp[i] && e[i] - prev_exp[i] <= kMaxDelta) {
            const std::uint32_t d = e[i] - prev_exp[i];
            e[i] += d * extra;
            st(kBase + 0x80u * i + 0x10u, e[i]);
            exp_gain = true;
        }
        prev_exp[i] = e[i];
    }
    constexpr unsigned long long kAfter = 600, kBefore = 1800;  // ~10 s after EXP, ~30 s before it (the results screen pays yen first, EXP later)
    if (exp_gain) last_exp_tick = tick;
    std::uint32_t nm = m;
    if (m > prev_money && m - prev_money <= kMaxDelta) {
        const std::uint32_t d = m - prev_money;
        if (last_exp_tick && tick - last_exp_tick <= kAfter) nm = std::min(kMoneyCap, m + d * extra);
        else { pend_money = d; pend_tick = tick; }  // maybe the EXP shows up next
    } else if (exp_gain && pend_money && tick - pend_tick <= kBefore) {
        nm = std::min(kMoneyCap, m + pend_money * extra);
        pend_money = 0;
    }
    if (nm != m) { st(kMoney, nm); std::cerr << "[rewards] yen " << m << " -> " << nm << "\n"; }
    if (exp_gain) std::cerr << "[rewards] EXP x" << mult << "\n";
    prev_money = nm;
}

int io_save_slot_step(int delta) {
    g_active_slot += delta;
    if (g_active_slot < 0) g_active_slot = 0;
    if (g_active_slot > 127) g_active_slot = 127;
    return g_active_slot;
}

bool io_handles(const std::string &library, std::uint32_t nid) {
    for (const Def &d : kDefs) if (d.nid == nid && library == d.library) return true;
    return false;
}

} // namespace persona1
