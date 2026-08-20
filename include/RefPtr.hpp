// RefPtr.hpp
//
// Deterministic, intrusive-refcount smart pointer whose storage comes from
// a MemoryPool instead of the heap.
//
// Design summary
// ---------------
// RefPtr<T> never allocates T directly. Instead each live object is backed
// by a ControlBlock<T>:
//
//     struct ControlBlock<T> {
//         std::size_t ref_count;
//         alignas(T) std::byte storage[sizeof(T)];   // T lives here
//     };
//
// A `MemoryPool<ControlBlock<T>>` supplies fixed-size slots exactly large
// enough for one ControlBlock<T>. RefPtr<T>::make():
//
//   1. pulls a raw slot from the pool                    (pool.allocate())
//   2. placement-news a ControlBlock<T> into it           (sets ref_count = 0)
//   3. placement-news a T into the control block's
//      storage, forwarding constructor arguments          (new (...) T(...))
//   4. sets ref_count = 1 and returns an owning RefPtr<T>
//
// This is the "decoupling" the project asks for: construction/destruction
// of T (placement new / explicit dtor call) is entirely separate from
// where its bytes live (a pool slot instead of the general heap). Copying
// a RefPtr increments ref_count; moving transfers ownership with no
// refcount traffic at all. When the last owning RefPtr is destroyed,
// ~T() is invoked explicitly, the control block's own destructor runs,
// and the slot is handed back to the pool -- deterministically, exactly
// once, with no GC pause and no heap involved.
//
// Thread safety: ref_count is a plain (non-atomic) std::size_t, and
// MemoryPool is not synchronized. RefPtr<T> is therefore single-threaded
// by design, matching typical arena/pool use (one owning thread, or
// external synchronization) and avoiding the cost of atomic RMW on every
// copy/destroy. See README for the trade-off and how to make it atomic.

#pragma once

#include "MemoryPool.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace mempool {

template <typename T>
struct ControlBlock {
    std::size_t ref_count = 0;
    alignas(T) std::byte storage[sizeof(T)];

    [[nodiscard]] T* object() noexcept { return reinterpret_cast<T*>(storage); }
    [[nodiscard]] const T* object() const noexcept { return reinterpret_cast<const T*>(storage); }
};

template <typename T>
class RefPtr {
public:
    using ControlBlockType = ControlBlock<T>;
    using Pool = MemoryPool<ControlBlockType>;

    // Constructs a new T(args...) in a slot pulled from `pool` and returns
    // an owning RefPtr with use_count() == 1.
    //
    // Strong exception guarantee: if T's constructor throws, the control
    // block is unwound and the slot is returned to the pool before the
    // exception propagates -- no leaked slot, no partially-constructed T
    // left behind.
    template <typename... Args>
    [[nodiscard]] static RefPtr make(Pool& pool, Args&&... args) {
        void* raw = pool.allocate();
        auto* block = ::new (raw) ControlBlockType();

        try {
            ::new (static_cast<void*>(block->storage)) T(std::forward<Args>(args)...);
        } catch (...) {
            block->~ControlBlockType();
            pool.deallocate(raw);
            throw;
        }

        block->ref_count = 1;
        return RefPtr(block, &pool);
    }

    RefPtr() noexcept = default;
    RefPtr(std::nullptr_t) noexcept {}

    RefPtr(const RefPtr& other) noexcept : block_(other.block_), pool_(other.pool_) {
        if (block_ != nullptr) {
            ++block_->ref_count;
        }
    }

    RefPtr(RefPtr&& other) noexcept : block_(other.block_), pool_(other.pool_) {
        other.block_ = nullptr;
        other.pool_ = nullptr;
    }

    RefPtr& operator=(const RefPtr& other) noexcept {
        if (this != &other) {
            if (other.block_ != nullptr) {
                ++other.block_->ref_count;
            }
            release();
            block_ = other.block_;
            pool_ = other.pool_;
        }
        return *this;
    }

    RefPtr& operator=(RefPtr&& other) noexcept {
        if (this != &other) {
            release();
            block_ = other.block_;
            pool_ = other.pool_;
            other.block_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    ~RefPtr() { release(); }

    void reset() noexcept {
        release();
        block_ = nullptr;
        pool_ = nullptr;
    }

    [[nodiscard]] T* get() const noexcept { return block_ != nullptr ? block_->object() : nullptr; }
    [[nodiscard]] T& operator*() const noexcept { return *block_->object(); }
    [[nodiscard]] T* operator->() const noexcept { return block_->object(); }
    [[nodiscard]] explicit operator bool() const noexcept { return block_ != nullptr; }
    [[nodiscard]] std::size_t use_count() const noexcept { return block_ != nullptr ? block_->ref_count : 0; }

    [[nodiscard]] friend bool operator==(const RefPtr& a, const RefPtr& b) noexcept { return a.block_ == b.block_; }
    [[nodiscard]] friend bool operator!=(const RefPtr& a, const RefPtr& b) noexcept { return a.block_ != b.block_; }
    [[nodiscard]] friend bool operator==(const RefPtr& a, std::nullptr_t) noexcept { return a.block_ == nullptr; }
    [[nodiscard]] friend bool operator==(std::nullptr_t, const RefPtr& a) noexcept { return a.block_ == nullptr; }

private:
    RefPtr(ControlBlockType* block, Pool* pool) noexcept : block_(block), pool_(pool) {}

    void release() noexcept {
        if (block_ == nullptr) {
            return;
        }
        if (--block_->ref_count == 0) {
            block_->object()->~T();
            block_->~ControlBlockType();
            pool_->deallocate(block_);
        }
        block_ = nullptr;
        pool_ = nullptr;
    }

    ControlBlockType* block_ = nullptr;
    Pool* pool_ = nullptr;
};

// Free-function spelling in the style of std::make_shared.
template <typename T, typename... Args>
[[nodiscard]] RefPtr<T> make_ref(typename RefPtr<T>::Pool& pool, Args&&... args) {
    return RefPtr<T>::make(pool, std::forward<Args>(args)...);
}

} // namespace mempool
