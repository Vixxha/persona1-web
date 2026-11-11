#pragma once
// Stand-in for PPSSPP's aligned allocation helpers.
#include <cstddef>
#include <cstdlib>

inline void *AllocateAlignedMemory(std::size_t size, std::size_t alignment) {
    void *ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size == 0 ? 1 : size) != 0) return nullptr;
    return ptr;
}
inline void FreeAlignedMemory(void *ptr) { std::free(ptr); }
