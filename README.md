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
tests/test_pool.cpp           MemoryPool unit tests
tests/test_refptr.cpp         RefPtr unit tests
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

Build options:

| Option                          | Default | Effect                                   |
|----------------------------------|---------|-------------------------------------------|
| `MEMPOOL_BUILD_TESTS`            | `ON`    | Build `unit_tests` (GoogleTest via FetchContent) |
| `MEMPOOL_BUILD_BENCHMARKS`       | `ON`    | Build `bench_allocator`                  |
| `MEMPOOL_ENABLE_SANITIZERS`      | `OFF`   | Add `-fsanitize=address,undefined`       |

All first-party targets build with `-Wall -Wextra -Wpedantic -g` (or
`/W4` on MSVC), and compile warning-free.

## Memory layout

`MemoryPool<T>` allocates one contiguous, aligned buffer up front
(`aligned_alloc` / `_aligned_malloc`), sized `slot_size * capacity`, where
`slot_size` is `sizeof(T)` (or `sizeof(void*)`, whichever is larger)
rounded up to the required alignment. There's no per-slot bookkeeping
struct living next to the data. Free slots just reuse their own bytes to
store the free-list link:

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
              (order changes as slots are allocated/freed)
```

`allocate()` pops the head of `free_list_` and returns it as a raw slot.
`deallocate(ptr)` threads that slot back onto the head of `free_list_`.
Both are pointer-chasing, branch-light O(1) operations: no scanning, no
bitmap, no size-class lookup. Steady-state pool overhead beyond the raw
buffer is O(1) too: just the head pointer, a capacity counter, and (in
debug builds) one `bool` per slot for double-free detection.

The pool is fixed-size by design. It never grows or shrinks after
construction. `allocate()` throws `PoolExhausted` when the free list is
empty instead of silently falling back to the heap, so exhaustion is a
first-class, observable error rather than something papered over.

### Alignment

`MemoryPool<T, Alignment>` defaults `Alignment` to
`max(alignof(T), alignof(void*))` (see `AllocatorTraits.hpp::SlotTraits`),
so every slot is aligned correctly for both `T` and the embedded free-list
pointer. Callers can widen it further (e.g. `MemoryPool<T, 64>` for
cache-line alignment); it's `static_assert`-checked to be a power of two.

### Debug safety net

`deallocate()` always checks that a pointer falls within the pool's
buffer and lands exactly on a slot boundary. These checks are cheap (a
pointer compare plus one modulo) and stay compiled in unconditionally,
release or debug.

Double-free / free-list-corruption detection uses a `std::vector<bool>`
side table and is gated behind `MEMPOOL_DEBUG`, which defaults to `1`
whenever `NDEBUG` isn't defined. You get it for free in debug builds, and
it compiles out entirely in a `Release`/`NDEBUG` build where the O(1)
alloc/dealloc path shouldn't have to pay for it. Override explicitly with
`-DMEMPOOL_DEBUG=1` or `=0`.

## Placement new and destructor decoupling

The core design idea here is decoupling where an object's bytes live from
when its constructor/destructor actually run. That's implemented via
`ControlBlock<T>` in `RefPtr.hpp`:

```cpp
template <typename T>
struct ControlBlock {
    std::size_t ref_count = 0;
    alignas(T) std::byte storage[sizeof(T)];   // T's bytes, uninitialized
};
```

A `MemoryPool<ControlBlock<T>>` only ever hands out raw, uninitialized
`ControlBlock<T>`-sized slots. It has no idea what a `T` even is beyond
its size and alignment. `RefPtr<T>::make(pool, args...)` is what stitches
allocation and construction together:

```cpp
void*  raw   = pool.allocate();                 // 1. pool gives raw bytes
auto*  block = ::new (raw) ControlBlockType();   // 2. placement-new the control block
::new (block->storage) T(std::forward<Args>(args)...);  // 3. placement-new T in place
block->ref_count = 1;                            // 4. now it's a live, owned object
```

And symmetrically, when the last `RefPtr<T>` referencing a block is
destroyed:

```cpp
block->object()->~T();          // explicit destructor call, no delete involved
block->~ControlBlockType();     // control block itself has no real work to do here
pool_->deallocate(block);       // slot returns to the free list, ready for reuse
```

Because construction and destruction are explicit function calls rather
than `new`/`delete` expressions, the pool never has to know how to build
a `T`, and `T` never has to know it lives inside a pool. This is also
what gives `RefPtr::make` its strong exception guarantee: if `T`'s
constructor throws, the control block gets unwound and the slot handed
back before the exception escapes. No leaked slot, no half-built object
left for anyone to trip over.

### Copy / move semantics

* **Copy** (`RefPtr(const RefPtr&)`, `operator=(const RefPtr&)`)
  increments `ref_count` and shares the same control block. No
  allocation, no construction.
* **Move** (`RefPtr(RefPtr&&)`, `operator=(RefPtr&&)`) transfers the
  control block pointer and nulls out the source. Zero refcount churn,
  same idea as `std::shared_ptr`'s move behavior.
* **Destruction** decrements `ref_count`. Only the owner that drives it
  to zero runs `~T()` and returns the slot, exactly once, deterministically,
  with no tracing GC and no collection pause.

### Thread safety, a deliberate trade-off

`ref_count` is a plain `std::size_t`, not `std::atomic`, and
`MemoryPool` isn't internally synchronized. That's a conscious choice for
a pool/arena allocator: the common case is one owning thread (or an
externally-synchronized region), and skipping the atomic RMW on every
copy/destroy accounts for a good chunk of `RefPtr`'s speed advantage over
`std::shared_ptr` in the benchmarks below (`libstdc++`'s `shared_ptr`
uses an atomic refcount unconditionally, even single-threaded). If you
need a multi-threaded `GcPtr<T>`, the change is localized: swap
`ref_count` to `std::atomic<std::size_t>` with `fetch_add`/`fetch_sub`,
and either wrap `MemoryPool`'s free-list push/pop in a mutex or replace
it with a lock-free Treiber stack (`std::atomic<FreeNode*>` + CAS loop).

## Diagnostics

`MemoryPool<T>` exposes:

```cpp
total_blocks()      // fixed capacity
allocated_blocks()  // currently live slots
free_blocks()       // currently free slots
high_watermark()     // peak allocated_blocks() ever observed
block_size()         // bytes per slot (static)
owns(ptr)             // does this pointer address a valid slot boundary?
```

See `src/main.cpp` for a walkthrough that prints these at each step of a
construct/copy/move/release/exhaustion sequence.

## Testing

27 GoogleTest cases across `tests/test_pool.cpp` (allocation lifecycle,
alignment, exhaustion, double-free/out-of-bounds detection, high
watermark) and `tests/test_refptr.cpp` (placement-new construction, copy
refcounting, move semantics, deterministic destruction, exception safety,
pool exhaustion propagation). Run with `ctest --test-dir build` or
directly via `./build/unit_tests`.

`scripts/run_sanitizers.sh` builds under `-fsanitize=address,undefined`,
runs the unit tests and demo under it, and (if `valgrind` is installed)
runs a second, independent pass under `valgrind --leak-check=full`. Both
have actually been run against this exact code:

```
==8748== HEAP SUMMARY:
==8748==     in use at exit: 0 bytes in 0 blocks
==8748==   total heap usage: 1,707 allocs, 1,707 frees, 423,114 bytes allocated
==8748== All heap blocks were freed -- no leaks are possible
==8748== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
==> All sanitizer checks passed.
```

## Benchmarks

Measured with `benchmarks/bench_allocator.cpp` (2,000,000 iterations per
scenario), built at `-O3`, on GCC 13.3.0 / Ubuntu 24.04 under WSL2.
Numbers below are one representative run. Run `./build/bench_allocator`
yourself for your own machine's numbers; allocator throughput is
sensitive to CPU, glibc version, and (for `malloc` especially) whether
the workload fits inside glibc's per-thread tcache.

**Methodology note:** individual pool operations here take low
single-digit nanoseconds, fast enough that timestamping every single one
would mostly measure `steady_clock::now()`'s own call overhead rather
than the operation itself, especially under a virtualized clock (WSL2).
So `measure()` in `bench_allocator.cpp` times throughput with one clock
read pair around the whole run, and estimates latency percentiles from
the average cost of small (16-op) batches instead. All scenarios also
wrap their pointer in a `do_not_optimize`/`clobber_memory` barrier (the
same `asm volatile("" ::: "memory")` trick Google Benchmark uses).
Without it, a trivial payload with no consumer lets the compiler prove an
isolated `new`+`delete` (or `malloc`+`free`) pair has no observable
effect and just elide it, which quietly turns "how fast is malloc" into
"how fast is an empty loop". (I actually hit this while building the
suite: the first run reported malloc doing 50 trillion ops/sec, which is
obviously not a real number.)

### Raw allocation throughput (alloc immediately followed by free)

| Scenario                  | ops/sec     | p50 (ns) | p95 (ns) | p99 (ns) |
|----------------------------|------------:|---------:|---------:|---------:|
| `MemoryPool<T>` alloc+free | 512,613,759 |      2.6 |      3.2 |      3.8 |
| `malloc`/`free`            | 122,352,925 |      9.4 |      9.4 |     11.9 |
| `new`/`delete`              |  94,622,065 |     11.2 |     11.3 |     14.4 |

The pool hits roughly 4.2x the throughput of glibc `malloc`/`free` and
5.4x that of `new`/`delete` on this fixed-size workload. That tracks:
the pool's whole allocate/deallocate path is an intrusive free-list
pop/push, with no size-class lookup, no bin search, and no bookkeeping
header per allocation.

### Smart pointer construction+destruction overhead

| Scenario                    | ops/sec     | p50 (ns) | p95 (ns) | p99 (ns) |
|-------------------------------|------------:|---------:|---------:|---------:|
| `RefPtr<T>` (pool-backed)     | 515,431,769 |      3.1 |      3.2 |      3.8 |
| `std::make_shared<T>`         |  82,161,157 |     13.8 |     13.8 |     20.7 |

`RefPtr<T>` runs at roughly 6.3x the throughput of `std::make_shared<T>`
here. Two effects stack on top of each other: the pool skips a
general-purpose allocator call per object, and `RefPtr`'s non-atomic
refcount skips the atomic RMW that `libstdc++`'s thread-safe `shared_ptr`
performs even in single-threaded code (see the thread-safety trade-off
above).

### Heavy churn under sustained live-object pressure

A rolling window of 4,096 live objects, meant to simulate a workload with
real object-graph turnover instead of an immediate alloc-then-free pair:

| Scenario                          | ops/sec     | p50 (ns) | p95 (ns) | p99 (ns) |
|-------------------------------------|------------:|---------:|---------:|---------:|
| `RefPtr<T>` churn (window=4096)      | 217,638,963 |      5.0 |      5.6 |      6.2 |
| `make_shared` churn (window=4096)    |  74,229,431 |     14.4 |     15.0 |     17.6 |

The gap narrows a bit versus the pure construction benchmark (about 2.9x
here vs. ~6.3x above), because the ring buffer's own assignment and
destruction of the previous occupant now eat a bigger share of each op.
The pool-backed path still wins comfortably under sustained churn, just
by a smaller margin.

Re-run `./build/bench_allocator <iterations>` (default 1,000,000) to
reproduce these tables on your own hardware, then copy the printed
tables straight into this section.
