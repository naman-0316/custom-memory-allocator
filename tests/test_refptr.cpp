// test_refptr.cpp
//
// Unit tests for RefPtr<T>: construction via placement new, copy/move
// semantics, deterministic destruction, and pool interaction.

#include "MemoryPool.hpp"
#include "RefPtr.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using mempool::ControlBlock;
using mempool::MemoryPool;
using mempool::PoolExhausted;
using mempool::RefPtr;

namespace {

struct Tracked {
    static inline int live_count = 0;
    static inline int construct_count = 0;
    static inline int destruct_count = 0;

    int value;

    explicit Tracked(int v) : value(v) {
        ++live_count;
        ++construct_count;
    }

    ~Tracked() {
        --live_count;
        ++destruct_count;
    }

    static void reset_counters() {
        live_count = 0;
        construct_count = 0;
        destruct_count = 0;
    }
};

struct MoveOnly {
    std::string data;
    explicit MoveOnly(std::string d) : data(std::move(d)) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
};

} // namespace

class RefPtrTest : public ::testing::Test {
protected:
    void SetUp() override { Tracked::reset_counters(); }
};

TEST_F(RefPtrTest, MakeConstructsObjectInPlace) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> p = RefPtr<Tracked>::make(pool, 42);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->value, 42);
    EXPECT_EQ(Tracked::live_count, 1);
    EXPECT_EQ(p.use_count(), 1u);
}

TEST_F(RefPtrTest, ForwardsConstructorArgumentsForMoveOnlyTypes) {
    MemoryPool<ControlBlock<MoveOnly>> pool(2);
    RefPtr<MoveOnly> p = RefPtr<MoveOnly>::make(pool, std::string("hello"));
    EXPECT_EQ(p->data, "hello");
}

TEST_F(RefPtrTest, DestructorRunsWhenLastReferenceDrops) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    {
        RefPtr<Tracked> p = RefPtr<Tracked>::make(pool, 1);
        EXPECT_EQ(Tracked::live_count, 1);
    }
    EXPECT_EQ(Tracked::live_count, 0);
    EXPECT_EQ(Tracked::destruct_count, 1);
    EXPECT_EQ(pool.allocated_blocks(), 0u);
}

TEST_F(RefPtrTest, CopyIncrementsRefCountAndSharesObject) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 7);
    RefPtr<Tracked> b = a;

    EXPECT_EQ(a.use_count(), 2u);
    EXPECT_EQ(b.use_count(), 2u);
    EXPECT_EQ(a.get(), b.get());
    EXPECT_EQ(Tracked::construct_count, 1) << "copy must not re-run T's constructor";
    EXPECT_EQ(Tracked::live_count, 1);
}

TEST_F(RefPtrTest, DestructorOnlyRunsAfterAllCopiesDrop) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 7);
    {
        RefPtr<Tracked> b = a;
        EXPECT_EQ(Tracked::live_count, 1);
    }
    EXPECT_EQ(Tracked::live_count, 1) << "object must survive while a is still alive";
    EXPECT_EQ(a.use_count(), 1u);
}

TEST_F(RefPtrTest, MoveTransfersOwnershipWithoutRefcountChurn) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 9);
    Tracked* raw = a.get();

    RefPtr<Tracked> b = std::move(a);
    EXPECT_EQ(b.get(), raw);
    EXPECT_EQ(b.use_count(), 1u) << "move must not increment refcount";
    EXPECT_FALSE(static_cast<bool>(a)) << "moved-from RefPtr must be null";
    EXPECT_EQ(Tracked::construct_count, 1);
    EXPECT_EQ(Tracked::live_count, 1);
}

TEST_F(RefPtrTest, MoveAssignmentReleasesPreviousOwnedObject) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 1);
    RefPtr<Tracked> b = RefPtr<Tracked>::make(pool, 2);
    EXPECT_EQ(Tracked::live_count, 2);

    b = std::move(a);
    EXPECT_EQ(Tracked::live_count, 1) << "b's original object must be destroyed";
    EXPECT_EQ(b->value, 1);
}

TEST_F(RefPtrTest, CopyAssignmentReleasesPreviousOwnedObject) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 1);
    RefPtr<Tracked> b = RefPtr<Tracked>::make(pool, 2);

    b = a;
    EXPECT_EQ(Tracked::live_count, 1);
    EXPECT_EQ(a.use_count(), 2u);
    EXPECT_EQ(b->value, 1);
}

TEST_F(RefPtrTest, SelfAssignmentIsSafe) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 5);
    a = a;
    EXPECT_EQ(a.use_count(), 1u);
    EXPECT_EQ(Tracked::live_count, 1);
}

TEST_F(RefPtrTest, ResetReleasesEarly) {
    MemoryPool<ControlBlock<Tracked>> pool(4);
    RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 3);
    a.reset();
    EXPECT_FALSE(static_cast<bool>(a));
    EXPECT_EQ(Tracked::live_count, 0);
    EXPECT_EQ(pool.allocated_blocks(), 0u);
}

TEST_F(RefPtrTest, NullRefPtrHasZeroUseCount) {
    RefPtr<Tracked> p;
    EXPECT_FALSE(static_cast<bool>(p));
    EXPECT_EQ(p.use_count(), 0u);
    EXPECT_EQ(p, nullptr);
}

TEST_F(RefPtrTest, SlotIsReturnedToPoolAndReusable) {
    MemoryPool<ControlBlock<Tracked>> pool(1);
    {
        RefPtr<Tracked> a = RefPtr<Tracked>::make(pool, 1);
        EXPECT_THROW(RefPtr<Tracked>::make(pool, 2), PoolExhausted);
    }
    RefPtr<Tracked> c = RefPtr<Tracked>::make(pool, 3);
    EXPECT_EQ(c->value, 3);
}

TEST_F(RefPtrTest, ManyObjectsAllDestroyedExactlyOnce) {
    constexpr std::size_t N = 100;
    MemoryPool<ControlBlock<Tracked>> pool(N);
    {
        std::vector<RefPtr<Tracked>> owners;
        owners.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            owners.push_back(RefPtr<Tracked>::make(pool, static_cast<int>(i)));
        }
        // Fan out extra references, then drop them, to exercise the
        // refcounting path before the final release.
        std::vector<RefPtr<Tracked>> extra = owners;
        EXPECT_EQ(Tracked::live_count, static_cast<int>(N));
        extra.clear();
        EXPECT_EQ(Tracked::live_count, static_cast<int>(N));
    }
    EXPECT_EQ(Tracked::live_count, 0);
    EXPECT_EQ(Tracked::construct_count, static_cast<int>(N));
    EXPECT_EQ(Tracked::destruct_count, static_cast<int>(N));
    EXPECT_EQ(pool.allocated_blocks(), 0u);
}
