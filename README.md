# Custom Fixed-Size Memory Pool Allocator + RefPtr

A header-only, fixed-size memory pool (arena) allocator for C++17, paired
with `RefPtr<T>`, a deterministic, intrusive reference-counting smart
pointer that constructs objects directly inside pool slots via placement
new. No heap fragmentation, O(1) allocate/deallocate, deterministic
destruction, no GC pause.

```
include/AllocatorTraits.hpp   alignment & sizing helpers, portable aligned alloc
include/MemoryPool.hpp        the pool allocator itself
include/RefPtr.hpp            ControlBlock<T> + RefPtr<T>
src/main.cpp                  demo / CLI driver
tests/                        GoogleTest unit tests
benchmarks/bench_allocator.cpp throughput + latency benchmark suite
scripts/run_sanitizers.sh     ASan/UBSan + Valgrind verification
```

## Building

Requires CMake 3.16+ and a C++17 compiler. Tested with GCC 13.3.0 on
Ubuntu 24.04 (WSL2).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/memory_pool_demo   # demo driver
ctest --test-dir build     # unit tests (fetches GoogleTest on first configure)
./build/bench_allocator    # benchmarks (1,000,000 iterations by default)
```

All first-party targets build with `-Wall -Wextra -Wpedantic -g` (or
`/W4` on MSVC) and compile warning-free. `MEMPOOL_BUILD_TESTS`,
`MEMPOOL_BUILD_BENCHMARKS`, and `MEMPOOL_ENABLE_SANITIZERS` are CMake
options if you want to skip a target or add `-fsanitize=address,undefined`.

## Memory layout

`MemoryPool<T>` allocates one contiguous, aligned buffer up front
(`aligned_alloc` / `_aligned_malloc`), sized `slot_size * capacity`. There's
no per-slot bookkeeping struct living next to the data: free slots reuse
their own bytes to store the free-list link.

```
buffer_ (one aligned_alloc call)
┌───────────┬───────────┬───────────┬───────────┬───────────┐
│  slot 0   │  slot 1   │  slot 2   │  slot 3   │  slot 4   │   ...
└───────────┴───────────┴───────────┴───────────┴───────────┘

slot layout while FREE (unused bytes double as a free-list node):
┌─────────────────────────────┐
│ FreeNode { FreeNode* next; }│   <- first sizeof(void*) bytes
│ ...... unused ......        │
└─────────────────────────────┘

slot layout while LIVE (holds a ControlBlock<T>, itself holding T):
┌─────────────────────────────┐
│ std::size_t ref_count        │
│ alignas(T) std::byte storage[sizeof(T)]   <- T lives here
└─────────────────────────────┘

free_list_ (head pointer, O(1) push/pop):
free_list_ ──▶ slot 4 ──▶ slot 1 ──▶ slot 3 ──▶ nullptr
```

`allocate()` pops the head of `free_list_`; `deallocate(ptr)` pushes the
slot back onto it. Both are O(1): no scanning, no bitmap, no size-class
lookup. The pool is fixed-size by design, it never grows or shrinks, and
`allocate()` throws `PoolExhausted` on a full pool instead of silently
falling back to the heap.

Slot alignment defaults to `max(alignof(T), alignof(void*))` and is
`static_assert`-checked to be a power of two; callers can widen it (e.g.
`MemoryPool<T, 64>` for cache-line alignment). `deallocate()` always
checks the pointer is in-bounds and slot-aligned. Double-free detection
via a side `std::vector<bool>` is gated behind `MEMPOOL_DEBUG` (on by
default unless `NDEBUG` is defined), so release builds don't pay for it.

## Placement new and destructor decoupling

The core design idea is decoupling where an object's bytes live from when
its constructor/destructor actually run, implemented via `ControlBlock<T>`
in `RefPtr.hpp`:

```cpp
template <typename T>
struct ControlBlock {
    std::size_t ref_count = 0;
    alignas(T) std::byte storage[sizeof(T)];   // T's bytes, uninitialized
};
```

`MemoryPool<ControlBlock<T>>` only ever hands out raw, uninitialized
slots; it has no idea what a `T` even is beyond its size and alignment.
`RefPtr<T>::make(pool, args...)` stitches allocation and construction
together:

```cpp
void*  raw   = pool.allocate();                 // 1. pool gives raw bytes
auto*  block = ::new (raw) ControlBlockType();   // 2. placement-new the control block
::new (block->storage) T(std::forward<Args>(args)...);  // 3. placement-new T in place
block->ref_count = 1;                            // 4. now it's a live, owned object
```

And symmetrically, when the last `RefPtr<T>` referencing a block drops:

```cpp
block->object()->~T();          // explicit destructor call, no delete involved
block->~ControlBlockType();
pool_->deallocate(block);       // slot returns to the free list, ready for reuse
```

Because construction/destruction are explicit calls rather than
`new`/`delete` expressions, the pool never has to know how to build a
`T`. This also gives `RefPtr::make` a strong exception guarantee: if
`T`'s constructor throws, the control block is unwound and the slot
handed back before the exception escapes.

**Copy** increments `ref_count` and shares the control block (no
allocation). **Move** transfers the pointer and nulls the source (zero
refcount churn). **Destruction** decrements `ref_count`; only the owner
that drives it to zero runs `~T()` and returns the slot, deterministically,
with no tracing GC and no collection pause.

### Thread safety, a deliberate trade-off

`ref_count` is a plain `std::size_t`, not `std::atomic`, and `MemoryPool`
isn't internally synchronized. That's intentional for a pool/arena
allocator built around one owning thread (or an externally-synchronized
region), and it's a real chunk of `RefPtr`'s speed edge over
`std::shared_ptr` below, since `libstdc++`'s `shared_ptr` uses an atomic
refcount even single-threaded. For a multi-threaded `GcPtr<T>`, the
change is localized: swap `ref_count` to `std::atomic<std::size_t>` and
either mutex-guard the free-list push/pop or replace it with a lock-free
Treiber stack.

## Diagnostics

`MemoryPool<T>` exposes `total_blocks()`, `allocated_blocks()`,
`free_blocks()`, `high_watermark()`, `block_size()`, and `owns(ptr)`. See
`src/main.cpp` for a full construct/copy/move/release/exhaustion
walkthrough that prints these at each step.

## Testing

27 GoogleTest cases across `tests/test_pool.cpp` (allocation lifecycle,
alignment, exhaustion, double-free/out-of-bounds detection) and
`tests/test_refptr.cpp` (placement-new construction, copy/move semantics,
deterministic destruction, exception safety). Run via
`ctest --test-dir build` or `./build/unit_tests`.

`scripts/run_sanitizers.sh` builds under `-fsanitize=address,undefined`
and, if `valgrind` is installed, runs a second independent pass under
`valgrind --leak-check=full`. Both have actually been run against this
code:

```
==8748== HEAP SUMMARY:
==8748==     in use at exit: 0 bytes in 0 blocks
==8748==   total heap usage: 1,707 allocs, 1,707 frees, 423,114 bytes allocated
==8748== All heap blocks were freed -- no leaks are possible
==8748== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
```

## Benchmarks

Measured with `benchmarks/bench_allocator.cpp` (2,000,000 iterations per
scenario), built at `-O3`, on GCC 13.3.0 / Ubuntu 24.04. Run
`./build/bench_allocator` for your own machine's numbers; results are
sensitive to CPU, glibc version, and whether the workload fits glibc's
per-thread tcache.

Individual pool operations here take low single-digit nanoseconds, fast
enough that timestamping every single one would mostly measure
`steady_clock::now()`'s own overhead. So `measure()` times throughput
with one clock read pair around the whole run, and estimates latency
percentiles from small (16-op) batches instead. Every scenario also wraps
its pointer in a `do_not_optimize`/`clobber_memory` barrier (the
`asm volatile` trick from Google Benchmark), since a trivial payload with
no consumer otherwise lets the compiler prove an isolated `new`+`delete`
pair has no observable effect and elides it outright. (Caught this the
hard way: the first run reported `malloc` doing 50 trillion ops/sec.)

**Raw allocation throughput** (alloc immediately followed by free)

| Scenario                  | ops/sec     | p50 (ns) | p95 (ns) | p99 (ns) |
|----------------------------|------------:|---------:|---------:|---------:|
| `MemoryPool<T>` alloc+free | 512,613,759 |      2.6 |      3.2 |      3.8 |
| `malloc`/`free`            | 122,352,925 |      9.4 |      9.4 |     11.9 |
| `new`/`delete`              |  94,622,065 |     11.2 |     11.3 |     14.4 |

~4.2x glibc `malloc`/`free`, ~5.4x `new`/`delete`: an intrusive free-list
pop/push has no size-class lookup, no bin search, no per-allocation header.

**Smart pointer construction + destruction**

| Scenario                    | ops/sec     | p50 (ns) | p95 (ns) | p99 (ns) |
|-------------------------------|------------:|---------:|---------:|---------:|
| `RefPtr<T>` (pool-backed)     | 515,431,769 |      3.1 |      3.2 |      3.8 |
| `std::make_shared<T>`         |  82,161,157 |     13.8 |     13.8 |     20.7 |

~6.3x `std::make_shared<T>`: skips both the general-purpose allocator
call and the atomic refcount RMW.

**Heavy churn** (rolling window of 4,096 live objects)

| Scenario                          | ops/sec     | p50 (ns) | p95 (ns) | p99 (ns) |
|-------------------------------------|------------:|---------:|---------:|---------:|
| `RefPtr<T>` churn (window=4096)      | 217,638,963 |      5.0 |      5.6 |      6.2 |
| `make_shared` churn (window=4096)    |  74,229,431 |     14.4 |     15.0 |     17.6 |

The gap narrows to ~2.9x here since the ring buffer's own assignment/
destruction of the previous occupant eats a bigger share of each op, but
the pool-backed path still wins comfortably under sustained churn.
