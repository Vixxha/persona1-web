#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace psprecomp { class Runtime; }
namespace persona1::states {
constexpr std::size_t maxBytes = 256u * 1024u * 1024u;

struct Writer {
    std::vector<std::uint8_t> bytes;
    void raw(const void *data, std::size_t size) {
        if (!size) return;
        if (size > maxBytes - bytes.size()) throw std::runtime_error("El estado supera el tama\u00f1o permitido.");
        const auto *first = static_cast<const std::uint8_t *>(data);
        bytes.insert(bytes.end(), first, first + size);
    }
    template<class... T> void operator()(const T &...value) { (write(value), ...); }
    template<class T> void write(const T &value) {
        if constexpr (requires(T &v) { v.archive(*this); }) const_cast<T &>(value).archive(*this);
        else if constexpr (requires { value.first; value.second; }) { write(value.first); write(value.second); }
        else if constexpr (std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>) raw(&value, sizeof(T));
        else if constexpr (std::is_array_v<T>) { for (const auto &entry : value) write(entry); }
        else {
            const std::uint32_t size = static_cast<std::uint32_t>(value.size());
            write(size);
            if constexpr (requires { value.data(); }) {
                using V = typename T::value_type;
                if constexpr (std::is_trivially_copyable_v<V>) raw(value.data(), value.size() * sizeof(V));
                else for (const auto &entry : value) write(entry);
            } else for (const auto &entry : value) write(entry);
        }
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset{};
    void raw(void *data, std::size_t size) {
        if (!size) return;
        if (size > bytes.size() - offset) throw std::runtime_error("El estado est\u00e1 incompleto.");
        std::memcpy(data, bytes.data() + offset, size);
        offset += size;
    }
    template<class... T> void operator()(T &...value) { (read(value), ...); }
    template<class T> void read(T &value) {
        if constexpr (requires { value.archive(*this); }) value.archive(*this);
        else if constexpr (requires { value.first; value.second; }) { read(value.first); read(value.second); }
        else if constexpr (std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>) raw(&value, sizeof(T));
        else if constexpr (std::is_array_v<T>) { for (auto &entry : value) read(entry); }
        else {
            std::uint32_t size{};
            read(size);
            if (size > bytes.size() - offset) throw std::runtime_error("El estado contiene un tama\u00f1o inv\u00e1lido.");
            value.clear();
            if constexpr (requires { typename T::mapped_type; }) {
                for (std::uint32_t i = 0; i < size; ++i) {
                    typename T::key_type key{};
                    typename T::mapped_type entry{};
                    read(key); read(entry);
                    if (!value.emplace(std::move(key), std::move(entry)).second) throw std::runtime_error("?ndice duplicado en el estado.");
                }
            } else if constexpr (requires { value.resize(size); }) {
                using V = typename T::value_type;
                if constexpr (std::is_trivially_copyable_v<V> && requires { value.data(); }) {
                    if (size > (bytes.size() - offset) / sizeof(V)) throw std::runtime_error("Bloque de estado inv\u00e1lido.");
                    value.resize(size);
                    raw(value.data(), size * sizeof(V));
                } else {
                    value.resize(size);
                    for (auto &entry : value) read(entry);
                }
            } else {
                for (std::uint32_t i = 0; i < size; ++i) {
                    typename T::value_type entry{};
                    read(entry); value.insert(std::move(entry));
                }
            }
        }
    }
    void finish() const {
        if (offset != bytes.size()) throw std::runtime_error("El formato del estado no es compatible.");
    }
};

using Commit = std::function<void()>;
bool kernel_safe();
std::uint64_t kernel_time();
void kernel_rebase(std::uint64_t time);
void save_kernel(Writer &);
Commit load_kernel(Reader &);
void save_io(Writer &);
Commit load_io(Reader &);
void save_audio(Writer &);
Commit load_audio(Reader &);
void save_movie(Writer &);
Commit load_movie(Reader &);
void save_graphics(Writer &);
Commit load_graphics(Reader &);
void install(psprecomp::Runtime &);
}
