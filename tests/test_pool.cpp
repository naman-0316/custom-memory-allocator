// test_pool.cpp
//
// Unit tests for MemoryPool<T>: allocation lifecycle, alignment, capacity
// tracking, exhaustion, double-free / out-of-bounds detection.

#include "MemoryPool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

using mempool::MemoryPool;
using mempool::PoolCorruption;
using mempool::PoolExhausted;

namespace {

struct Small {
    int x;
};

struct alignas(64) CacheLineAligned {
    char bytes[64];
};

} // namespace

TEST(MemoryPool, ConstructionReportsCorrectInitialState) {
    MemoryPool<Small> pool(16);
    EXPECT_EQ(pool.total_blocks(), 16u);
    EXPECT_EQ(pool.free_blocks(), 16u);
    EXPECT_EQ(pool.allocated_blocks(), 0u);
    EXPECT_EQ(pool.high_watermark(), 0u);
}

TEST(MemoryPool, ZeroCapacityThrows) {
    EXPECT_THROW((MemoryPool<Small>(0)), std::invalid_argument);
}

TEST(MemoryPool, AllocateDecrementsFreeAndIncrementsAllocated) {
    MemoryPool<Small> pool(4);
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.allocated_blocks(), 1u);
    EXPECT_EQ(pool.free_blocks(), 3u);
    pool.deallocate(p);
    EXPECT_EQ(pool.allocated_blocks(), 0u);
    EXPECT_EQ(pool.free_blocks(), 4u);
}

TEST(MemoryPool, AllAllocatedSlotsAreDistinct) {
    constexpr std::size_t N = 64;
    MemoryPool<Small> pool(N);
    std::set<void*> seen;
    for (std::size_t i = 0; i < N; ++i) {
        void* p = pool.allocate();
        EXPECT_TRUE(seen.insert(p).second) << "slot returned twice";
    }
}

TEST(MemoryPool, ExhaustionThrowsPoolExhausted) {
    MemoryPool<Small> pool(2);
    [[maybe_unused]] void* p1 = pool.allocate();
    [[maybe_unused]] void* p2 = pool.allocate();
    EXPECT_THROW((void)pool.allocate(), PoolExhausted);
}

TEST(MemoryPool, FreedSlotIsReusable) {
    MemoryPool<Small> pool(1);
    void* p1 = pool.allocate();
    pool.deallocate(p1);
    void* p2 = pool.allocate();
    EXPECT_EQ(p1, p2);
}

TEST(MemoryPool, HighWatermarkTracksPeakUsage) {
    MemoryPool<Small> pool(8);
    std::vector<void*> ptrs;
    for (int i = 0; i < 5; ++i) ptrs.push_back(pool.allocate());
    EXPECT_EQ(pool.high_watermark(), 5u);
    for (void* p : ptrs) pool.deallocate(p);
    EXPECT_EQ(pool.high_watermark(), 5u) << "watermark must not decrease on deallocation";

    [[maybe_unused]] void* p1 = pool.allocate();
    [[maybe_unused]] void* p2 = pool.allocate();
    EXPECT_EQ(pool.high_watermark(), 5u) << "watermark must not decrease below prior peak";
}

TEST(MemoryPool, SlotsAreAlignedToRequestedAlignment) {
    MemoryPool<CacheLineAligned> pool(8);
    for (int i = 0; i < 8; ++i) {
        void* p = pool.allocate();
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(CacheLineAligned), 0u);
    }
}

TEST(MemoryPool, SlotSizeAtLeastFitsPointerAndType) {
    EXPECT_GE(MemoryPool<Small>::slot_size, sizeof(void*));
    EXPECT_GE(MemoryPool<Small>::slot_size, sizeof(Small));
    EXPECT_GE(MemoryPool<CacheLineAligned>::slot_size, sizeof(CacheLineAligned));
}

TEST(MemoryPool, DeallocateNullptrIsNoop) {
    MemoryPool<Small> pool(4);
    EXPECT_NO_THROW(pool.deallocate(nullptr));
    EXPECT_EQ(pool.allocated_blocks(), 0u);
}

TEST(MemoryPool, DeallocateOutOfBoundsPointerThrows) {
    MemoryPool<Small> pool(4);
    int stack_var = 0;
    EXPECT_THROW(pool.deallocate(&stack_var), PoolCorruption);
}

TEST(MemoryPool, DeallocateMisalignedPointerThrows) {
    MemoryPool<Small> pool(4);
    void* p = pool.allocate();
    auto* misaligned = reinterpret_cast<std::byte*>(p) + 1;
    EXPECT_THROW(pool.deallocate(misaligned), PoolCorruption);
    pool.deallocate(p);
}

#if MEMPOOL_DEBUG
TEST(MemoryPool, DoubleFreeIsDetected) {
    MemoryPool<Small> pool(4);
    void* p = pool.allocate();
    pool.deallocate(p);
    EXPECT_THROW(pool.deallocate(p), PoolCorruption);
}
#endif

TEST(MemoryPool, OwnsReportsMembershipCorrectly) {
    MemoryPool<Small> pool(4);
    void* p = pool.allocate();
    EXPECT_TRUE(pool.owns(p));
    int stack_var = 0;
    EXPECT_FALSE(pool.owns(&stack_var));
    pool.deallocate(p);
}

TEST(MemoryPool, FullAllocateDeallocateCycleManyTimes) {
    constexpr std::size_t N = 32;
    MemoryPool<Small> pool(N);
    for (int round = 0; round < 1000; ++round) {
        std::vector<void*> ptrs;
        ptrs.reserve(N);
        for (std::size_t i = 0; i < N; ++i) ptrs.push_back(pool.allocate());
        EXPECT_EQ(pool.free_blocks(), 0u);
        for (void* p : ptrs) pool.deallocate(p);
        EXPECT_EQ(pool.free_blocks(), N);
    }
}
