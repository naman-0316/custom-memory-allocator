// MemoryPool.hpp
//
// Fixed-size, single-owner memory pool (arena) allocator.
//
// Design summary
// ---------------
// * A single contiguous, aligned buffer is allocated once up front
//   (aligned_alloc / _aligned_malloc), sized as `capacity * slot_size`.
// * The buffer is carved into `capacity` equal-size slots. While a slot is
//   free, its first sizeof(void*) bytes are reinterpreted as a `FreeNode`
//   and threaded into a singly linked intrusive free list -- there is no
//   separate bookkeeping array for free slots, so the steady-state memory
//   overhead of the pool is O(1) regardless of capacity.
// * allocate() pops the head of the free list; deallocate() pushes the
//   slot back onto the head. Both are O(1) with no branching on slot
//   index, no scanning, and no locking (the pool is intentionally single
//   threaded -- see README for the rationale).
// * The pool never grows and never shrinks: this is what makes it a
//   *fixed-size* pool. Exhaustion is reported via the PoolExhausted
//   exception rather than silently falling back to the heap.
//
// Debug safety net
// -----------------
// When MEMPOOL_DEBUG is enabled (on by default unless NDEBUG is defined,
// i.e. debug builds get it for free), the pool keeps a side `std::vector<bool>`
// recording which slots are currently live. This turns two classic pool bugs
// into immediate, diagnosable exceptions instead of silent corruption:
//   * double free            -> PoolCorruption("double free detected")
//   * free-list corruption   -> PoolCorruption("double allocation ...")
// Pointer bounds/alignment checks (out-of-pool pointer, misaligned pointer)
// are cheap enough that they are always enabled, debug or release.

#pragma once

#include "AllocatorTraits.hpp"

#include <cstddef>
#include <stdexcept>
#include <vector>

#ifndef MEMPOOL_DEBUG
#  ifndef NDEBUG
#    define MEMPOOL_DEBUG 1
#  else
#    define MEMPOOL_DEBUG 0
#  endif
#endif

namespace mempool {

// Thrown by allocate() when the pool has no free slots left.
class PoolExhausted : public std::runtime_error {
public:
    PoolExhausted() : std::runtime_error("MemoryPool: pool exhausted") {}
};

// Thrown by deallocate() when a pointer is out of bounds, misaligned, or
// (in debug builds) identified as a double free / free-list corruption.
class PoolCorruption : public std::runtime_error {
public:
    explicit PoolCorruption(const char* what) : std::runtime_error(what) {}
};

template <typename T, std::size_t Alignment = traits::SlotTraits<T>::alignment>
class MemoryPool {
public:
    static_assert(traits::is_power_of_two(Alignment), "MemoryPool: Alignment must be a power of two");
    static_assert(Alignment >= alignof(void*), "MemoryPool: Alignment must be able to hold a free-list pointer");

    // Size in bytes of a single slot, after rounding sizeof(T) up to hold a
    // free-list pointer and aligning to `Alignment`.
    static constexpr std::size_t slot_size = traits::align_up(
        (sizeof(T) > sizeof(void*)) ? sizeof(T) : sizeof(void*), Alignment);

    explicit MemoryPool(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("MemoryPool: capacity must be > 0");
        }

        buffer_ = static_cast<std::byte*>(
            traits::aligned_alloc_bytes(slot_size * capacity_, Alignment));
        if (buffer_ == nullptr) {
            throw std::bad_alloc();
        }

#if MEMPOOL_DEBUG
        live_.assign(capacity_, false);
#endif
        build_free_list();
    }

    ~MemoryPool() {
        traits::aligned_free_bytes(buffer_);
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    // Slots are addressed by pointers into `buffer_`; anything already
    // holding one of those pointers would be invalidated by a move, so the
    // pool is pinned in place for its lifetime.
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    // Returns a raw, correctly-aligned slot of `slot_size` bytes.
    // Throws PoolExhausted if the pool is full. O(1).
    [[nodiscard]] void* allocate() {
        if (free_list_ == nullptr) {
            throw PoolExhausted();
        }

        FreeNode* node = free_list_;
        free_list_ = node->next;
        --free_count_;

#if MEMPOOL_DEBUG
        const std::size_t index = slot_index(reinterpret_cast<std::byte*>(node));
        if (live_[index]) {
            throw PoolCorruption("MemoryPool::allocate - free list corrupted (slot already live)");
        }
        live_[index] = true;
#endif

        const std::size_t in_use = capacity_ - free_count_;
        if (in_use > high_watermark_) {
            high_watermark_ = in_use;
        }

        return static_cast<void*>(node);
    }

    // Returns a slot previously obtained from allocate() back to the pool.
    // O(1). Safe to call with nullptr (no-op).
    void deallocate(void* ptr) {
        if (ptr == nullptr) {
            return;
        }

        auto* p = static_cast<std::byte*>(ptr);
        if (p < buffer_ || p >= buffer_ + slot_size * capacity_) {
            throw PoolCorruption("MemoryPool::deallocate - pointer is outside this pool");
        }

        const std::size_t offset = static_cast<std::size_t>(p - buffer_);
        if (offset % slot_size != 0) {
            throw PoolCorruption("MemoryPool::deallocate - pointer is not slot-aligned");
        }

#if MEMPOOL_DEBUG
        const std::size_t index = offset / slot_size;
        if (!live_[index]) {
            throw PoolCorruption("MemoryPool::deallocate - double free detected");
        }
        live_[index] = false;
#endif

        // GCC's -Warray-bounds can flag this write as out-of-bounds when a
        // caller passes the address of a small stack object: it reasons
        // about the pointee's static size even though the bounds check
        // above already guarantees `p` lies within our own heap buffer_
        // (and throws before reaching here otherwise). This is a known
        // false-positive pattern for intrusive free lists; the checks
        // above are the real safety net.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
        auto* node = reinterpret_cast<FreeNode*>(p);
        node->next = free_list_;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        free_list_ = node;
        ++free_count_;
    }

    // -- Diagnostics ---------------------------------------------------

    [[nodiscard]] std::size_t total_blocks() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t free_blocks() const noexcept { return free_count_; }
    [[nodiscard]] std::size_t allocated_blocks() const noexcept { return capacity_ - free_count_; }
    [[nodiscard]] std::size_t high_watermark() const noexcept { return high_watermark_; }
    [[nodiscard]] static constexpr std::size_t block_size() noexcept { return slot_size; }
    [[nodiscard]] static constexpr std::size_t alignment() noexcept { return Alignment; }

    // True if `ptr` falls within this pool's backing buffer at a valid
    // slot boundary. Does not indicate whether the slot is currently live.
    [[nodiscard]] bool owns(const void* ptr) const noexcept {
        const auto* p = static_cast<const std::byte*>(ptr);
        if (p < buffer_ || p >= buffer_ + slot_size * capacity_) {
            return false;
        }
        return (static_cast<std::size_t>(p - buffer_) % slot_size) == 0;
    }

private:
    struct FreeNode {
        FreeNode* next;
    };

    void build_free_list() {
        free_list_ = nullptr;
        // Link from the back so slot 0 ends up at the head; purely
        // cosmetic (first-allocated slot has the lowest address) but makes
        // debugging free-list dumps easier.
        for (std::size_t i = capacity_; i-- > 0;) {
            auto* node = reinterpret_cast<FreeNode*>(buffer_ + i * slot_size);
            node->next = free_list_;
            free_list_ = node;
        }
        free_count_ = capacity_;
    }

#if MEMPOOL_DEBUG
    std::size_t slot_index(std::byte* p) const noexcept {
        return static_cast<std::size_t>(p - buffer_) / slot_size;
    }
#endif

    std::byte* buffer_ = nullptr;
    FreeNode* free_list_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t free_count_ = 0;
    std::size_t high_watermark_ = 0;

#if MEMPOOL_DEBUG
    std::vector<bool> live_;
#endif
};

} // namespace mempool
