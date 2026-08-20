// main.cpp
//
// Small CLI driver that exercises MemoryPool<T> and RefPtr<T> end to end
// and prints pool diagnostics, so the library can be sanity-checked
// without pulling in the test or benchmark suites.

#include "MemoryPool.hpp"
#include "RefPtr.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

struct Widget {
    int id;
    std::string name;

    Widget(int id_, std::string name_) : id(id_), name(std::move(name_)) {
        std::cout << "  Widget(" << id << ", \"" << name << "\") constructed\n";
    }

    ~Widget() {
        std::cout << "  Widget(" << id << ", \"" << name << "\") destroyed\n";
    }
};

void print_diagnostics(const char* label, const mempool::MemoryPool<mempool::ControlBlock<Widget>>& pool) {
    std::cout << label << ": total=" << pool.total_blocks()
              << " allocated=" << pool.allocated_blocks()
              << " free=" << pool.free_blocks()
              << " high_watermark=" << pool.high_watermark()
              << " slot_size=" << pool.block_size() << " bytes\n";
}

} // namespace

int main() {
    using mempool::MemoryPool;
    using mempool::ControlBlock;
    using mempool::RefPtr;

    std::cout << "=== Custom Memory Pool + RefPtr demo ===\n\n";

    MemoryPool<ControlBlock<Widget>> pool(/*capacity=*/8);
    print_diagnostics("initial", pool);

    std::cout << "\n-- constructing widgets via RefPtr::make --\n";
    RefPtr<Widget> a = RefPtr<Widget>::make(pool, 1, "alpha");
    RefPtr<Widget> b = RefPtr<Widget>::make(pool, 2, "beta");
    print_diagnostics("after 2 allocations", pool);

    std::cout << "\n-- copy semantics (refcount increments, no new slot used) --\n";
    RefPtr<Widget> a_copy = a;
    std::cout << "a.use_count() = " << a.use_count() << "\n";
    print_diagnostics("after copy", pool);

    std::cout << "\n-- move semantics (ownership transfers, refcount unchanged) --\n";
    RefPtr<Widget> a_moved = std::move(a_copy);
    std::cout << "a_moved.use_count() = " << a_moved.use_count()
              << ", a_copy is now " << (a_copy ? "non-null" : "null") << "\n";

    std::cout << "\n-- dropping references --\n";
    a.reset();
    a_moved.reset();
    std::cout << "b.use_count() = " << b.use_count() << " (only remaining owner)\n";
    print_diagnostics("after dropping a's owners", pool);

    b.reset();
    print_diagnostics("after dropping b", pool);

    std::cout << "\n-- pool exhaustion handling --\n";
    std::vector<RefPtr<Widget>> holders;
    try {
        for (int i = 0; i < 9; ++i) {
            holders.push_back(RefPtr<Widget>::make(pool, 100 + i, "bulk"));
        }
    } catch (const mempool::PoolExhausted& e) {
        std::cout << "caught expected exception: " << e.what() << "\n";
    }
    print_diagnostics("after filling pool to capacity", pool);

    holders.clear();
    print_diagnostics("after releasing all holders", pool);

    std::cout << "\nDone.\n";
    return 0;
}
