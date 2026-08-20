// bench_allocator.cpp
//
// Self-contained benchmark suite (no external benchmark library) comparing:
//   1. Raw allocation throughput: MemoryPool<T> vs malloc/free vs new/delete
//   2. Smart pointer overhead: RefPtr<T> (pool-backed) vs std::make_shared<T>
//
// For each scenario we run N operations, record the wall-clock latency of
// every individual operation, and report throughput plus P50/P95/P99
// latency. Build in Release (-O3) for meaningful numbers -- the CMake
// target for this file forces -O3 regardless of the top-level build type.

#include "MemoryPool.hpp"
#include "RefPtr.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Optimization barriers, in the spirit of Google Benchmark's
// DoNotOptimize/ClobberMemory. Without these, a trivial payload with no
// consumer lets the compiler prove an alloc+free (or new+delete) pair has
// no observable effect and elide it entirely -- the standard explicitly
// permits omitting calls to replaceable global allocation functions when
// the pointer doesn't escape. That turns "how fast is malloc" into "how
// fast is an empty loop", which is not what we want to measure.
template <typename T>
inline void do_not_optimize(T const& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(value) : "memory");
#else
    volatile auto sink = value;
    (void)sink;
#endif
}

inline void clobber_memory() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#endif
}

struct Stats {
    std::string name;
    std::size_t iterations = 0;
    double total_seconds = 0.0;
    double p50_ns = 0.0;
    double p95_ns = 0.0;
    double p99_ns = 0.0;

    [[nodiscard]] double ops_per_sec() const {
        return total_seconds > 0.0 ? static_cast<double>(iterations) / total_seconds : 0.0;
    }
};

// A payload roughly the size of a small game/UI object -- big enough that
// malloc bookkeeping and cache effects are representative, small enough to
// keep 1M+ iterations fast.
struct Payload {
    std::uint64_t a, b, c, d;
    explicit Payload(std::uint64_t seed = 0) : a(seed), b(seed + 1), c(seed + 2), d(seed + 3) {}
};

double percentile(std::vector<double>& sorted_ns, double p) {
    if (sorted_ns.empty()) return 0.0;
    const double rank = p * (static_cast<double>(sorted_ns.size()) - 1.0);
    const auto lo = static_cast<std::size_t>(rank);
    const std::size_t hi = std::min(lo + 1, sorted_ns.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return sorted_ns[lo] + (sorted_ns[hi] - sorted_ns[lo]) * frac;
}

// Runs `op` `iterations` times and reports both throughput and latency
// percentiles.
//
// `op` must perform exactly one allocation-related unit of work per call.
// The individual operations measured here (a few nanoseconds each) are
// fast enough that calling Clock::now() around *every single one* would
// mostly measure the clock read itself rather than the operation -- on a
// virtualized/VM clock (e.g. WSL2) a single now() round-trip can cost as
// much as the operation under test. To avoid that noise floor:
//
//   1. Throughput is measured with ONE pair of clock reads around the
//      entire `iterations`-sized loop (zero per-op timing overhead).
//   2. Latency percentiles are estimated by timing small batches of
//      `kBatchSize` back-to-back operations and recording the *average*
//      per-op cost within each batch as one sample. This amortizes the
//      clock-read overhead across the batch while still capturing
//      run-to-run variance (cache effects, allocator contention, etc.)
//      at batch granularity.
template <typename Fn>
Stats measure(const std::string& name, std::size_t iterations, Fn&& op) {
    constexpr std::size_t kBatchSize = 16;

    Stats stats;
    stats.name = name;
    stats.iterations = iterations;

    // -- Pass 1: throughput, no per-op timing overhead ------------------
    std::size_t counter = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        op(counter++);
    }
    const auto end = Clock::now();
    stats.total_seconds = std::chrono::duration<double>(end - start).count();

    // -- Pass 2: batched latency sampling for percentiles ---------------
    const std::size_t batches = std::max<std::size_t>(1, iterations / kBatchSize);
    std::vector<double> latencies_ns;
    latencies_ns.reserve(batches);

    for (std::size_t b = 0; b < batches; ++b) {
        const auto t0 = Clock::now();
        for (std::size_t j = 0; j < kBatchSize; ++j) {
            op(counter++);
        }
        const auto t1 = Clock::now();
        const double batch_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        latencies_ns.push_back(batch_ns / static_cast<double>(kBatchSize));
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());
    stats.p50_ns = percentile(latencies_ns, 0.50);
    stats.p95_ns = percentile(latencies_ns, 0.95);
    stats.p99_ns = percentile(latencies_ns, 0.99);
    return stats;
}

void print_table(const std::string& title, const std::vector<Stats>& rows) {
    std::cout << "\n=== " << title << " ===\n";
    std::cout << std::left << std::setw(28) << "Scenario" << std::right << std::setw(14) << "ops/sec"
              << std::setw(12) << "p50 (ns)" << std::setw(12) << "p95 (ns)" << std::setw(12) << "p99 (ns)" << "\n";
    std::cout << std::string(78, '-') << "\n";
    for (const auto& s : rows) {
        std::cout << std::left << std::setw(28) << s.name << std::right << std::setw(14) << std::fixed
                  << std::setprecision(0) << s.ops_per_sec() << std::setw(12) << std::setprecision(1) << s.p50_ns
                  << std::setw(12) << s.p95_ns << std::setw(12) << s.p99_ns << "\n";
    }
}

// -- Scenario 1: raw alloc/free throughput ----------------------------

Stats bench_pool_alloc_free(std::size_t iterations) {
    mempool::MemoryPool<Payload> pool(1024);
    return measure("MemoryPool<T> alloc+free", iterations, [&](std::size_t i) {
        void* p = pool.allocate();
        ::new (p) Payload(i);
        do_not_optimize(p);
        static_cast<Payload*>(p)->~Payload();
        pool.deallocate(p);
        clobber_memory();
    });
}

Stats bench_malloc_free(std::size_t iterations) {
    return measure("malloc/free", iterations, [&](std::size_t i) {
        void* p = std::malloc(sizeof(Payload));
        ::new (p) Payload(i);
        do_not_optimize(p);
        static_cast<Payload*>(p)->~Payload();
        std::free(p);
        clobber_memory();
    });
}

Stats bench_new_delete(std::size_t iterations) {
    return measure("new/delete", iterations, [&](std::size_t i) {
        auto* p = new Payload(i);
        do_not_optimize(p);
        delete p;
        clobber_memory();
    });
}

// -- Scenario 2: smart pointer overhead --------------------------------

Stats bench_refptr(std::size_t iterations) {
    mempool::MemoryPool<mempool::ControlBlock<Payload>> pool(1024);
    return measure("RefPtr<T> (pool-backed)", iterations, [&](std::size_t i) {
        auto p = mempool::RefPtr<Payload>::make(pool, i);
        do_not_optimize(p.get());
        clobber_memory();
    });
}

Stats bench_make_shared(std::size_t iterations) {
    return measure("std::make_shared<T>", iterations, [&](std::size_t i) {
        auto p = std::make_shared<Payload>(i);
        do_not_optimize(p.get());
        clobber_memory();
    });
}

// -- Scenario 3: heavy churn under sustained pressure -------------------
// Keeps a rolling window of live objects instead of freeing immediately,
// simulating a workload with real allocator/object-graph churn rather
// than a pure alloc-then-free microbenchmark.

Stats bench_pool_churn(std::size_t iterations, std::size_t window) {
    mempool::MemoryPool<mempool::ControlBlock<Payload>> pool(window + 1);
    std::vector<mempool::RefPtr<Payload>> ring(window);
    std::size_t idx = 0;
    return measure("RefPtr<T> churn (window=" + std::to_string(window) + ")", iterations, [&](std::size_t i) {
        ring[idx] = mempool::RefPtr<Payload>::make(pool, i);
        do_not_optimize(ring[idx].get());
        idx = (idx + 1) % window;
        clobber_memory();
    });
}

Stats bench_shared_churn(std::size_t iterations, std::size_t window) {
    std::vector<std::shared_ptr<Payload>> ring(window);
    std::size_t idx = 0;
    return measure("make_shared churn (window=" + std::to_string(window) + ")", iterations, [&](std::size_t i) {
        ring[idx] = std::make_shared<Payload>(i);
        do_not_optimize(ring[idx].get());
        idx = (idx + 1) % window;
        clobber_memory();
    });
}

} // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 1'000'000;
    if (argc > 1) {
        iterations = static_cast<std::size_t>(std::stoull(argv[1]));
    }

    std::cout << "Custom Memory Pool Allocator -- Benchmark Suite\n";
    std::cout << "Iterations per scenario: " << iterations << "\n";

    std::vector<Stats> raw_alloc = {
        bench_pool_alloc_free(iterations),
        bench_malloc_free(iterations),
        bench_new_delete(iterations),
    };
    print_table("Raw allocation throughput (alloc immediately followed by free)", raw_alloc);

    std::vector<Stats> smart_ptr = {
        bench_refptr(iterations),
        bench_make_shared(iterations),
    };
    print_table("Smart pointer construction+destruction overhead", smart_ptr);

    constexpr std::size_t window = 4096;
    std::vector<Stats> churn = {
        bench_pool_churn(iterations, window),
        bench_shared_churn(iterations, window),
    };
    print_table("Heavy churn under sustained live-object pressure", churn);

    std::cout << "\nDone. Copy the tables above into README.md's benchmark section.\n";
    return 0;
}
