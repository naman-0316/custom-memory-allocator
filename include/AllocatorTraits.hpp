// AllocatorTraits.hpp
//
// Small, header-only collection of alignment / sizing helpers and a
// portable aligned-allocation wrapper used by MemoryPool<T>.
//
// Nothing in this header allocates from the pool itself -- it only
// provides the compile-time and runtime arithmetic the pool needs to
// carve a raw buffer into fixed-size, correctly aligned slots.

#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace mempool::traits {

// Rounds `n` up to the next multiple of `alignment`. `alignment` must be a
// power of two (enforced by is_power_of_two() at the call site).
[[nodiscard]] constexpr std::size_t align_up(std::size_t n, std::size_t alignment) noexcept {
    return (n + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] constexpr bool is_power_of_two(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

// Derives the slot geometry for a type T stored in a MemoryPool.
//
// Every slot must be large enough to hold either a live T or, while free,
// an embedded intrusive free-list node (a single pointer). It must also be
// aligned to satisfy both T's alignment requirement and the pointer
// alignment the free list relies on.
template <typename T>
struct SlotTraits {
    static constexpr std::size_t alignment =
        (alignof(T) > alignof(void*)) ? alignof(T) : alignof(void*);

    static constexpr std::size_t min_size =
        (sizeof(T) > sizeof(void*)) ? sizeof(T) : sizeof(void*);

    static constexpr std::size_t size = align_up(min_size, alignment);
};

// Portable aligned allocation. Returns nullptr on failure (mirrors
// std::aligned_alloc / _aligned_malloc semantics) rather than throwing, so
// callers can decide how to report the failure.
inline void* aligned_alloc_bytes(std::size_t size, std::size_t alignment) {
    const std::size_t rounded_size = align_up(size, alignment);
#if defined(_WIN32)
    return _aligned_malloc(rounded_size, alignment);
#else
    return std::aligned_alloc(alignment, rounded_size);
#endif
}

inline void aligned_free_bytes(void* ptr) noexcept {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

} // namespace mempool::traits
