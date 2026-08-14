# P1: micro-alloc

A compact, heavily commented C++17 project for learning **memory ownership**, **allocation strategies**, **container growth**, and **cache behaviour** by building and measuring a few small allocators and data structures.

## Learning objectives

By the end of this project you should be able to explain:

- **Why allocation is expensive**: every `new`/`delete` can touch the general-purpose heap, metadata, synchronization, and fragmentation control.
- **Ownership trade-offs**: when lifetime is naturally "all at once", an arena is often simpler and faster than per-object `delete`.
- **Allocator-aware containers**: `std::vector` is fast, but growth strategy and reallocation count still matter.
- **Cache locality**: data layout often matters as much as algorithmic complexity.
- **When intrusive containers matter**: tools, schedulers, game engines, and runtimes often want links embedded in objects to avoid extra allocations.

## Project layout

- `include/arena.h`  
  Bump / arena allocator with aligned allocation, block chaining, `reset()`, and a `std::allocator`-compatible adapter.
- `include/pool.h`  
  Fixed-size pool allocator with an intrusive free list and optional debug poisoning.
- `include/intrusive_list.h`  
  Intrusive doubly linked list using a sentinel node and bidirectional iterators.
- `include/tracking_allocator.h`  
  Allocator adapter that counts allocations, deallocations, bytes, and peak usage.
- `src/bench.cpp`  
  Benchmark harness comparing heap allocation, pool/arena allocation, intrusive structures, cache-friendly layouts, and `std::vector` growth.
- `src/tests.cpp`  
  Tiny no-dependency test runner.
- `EXERCISES.md`  
  Ten graded exercises that build on the code.

## What each exercise teaches

1. Arena basics: why "free everything at once" matches request-scoped or frame-scoped lifetimes.
2. Alignment: why allocators must respect `alignof(T)` and why padding exists.
3. Pool allocation: why fixed-size reuse eliminates most general heap traffic.
4. Intrusive free lists: how freed memory can temporarily store allocator metadata.
5. Intrusive lists: why no-per-insert-allocation is valuable in low-level code.
6. Tracking allocators: how to prove, not guess, what containers are doing.
7. `std::vector::reserve`: how one API call changes reallocation behaviour.
8. AoS vs SoA: how layout changes traversal speed through cache locality.
9. Block chaining: how arenas grow without invalidating prior allocations.
10. Benchmark reading: why medians, warmups, and ratios matter more than single runs.

## Build instructions

### Windows (MSVC)

```powershell
cd C:\Users\sunayak\.copilot\session-state\08b80c37-1925-49f6-964d-67f7cf9e1d95\files\gpu-tools-study\projects\P1-micro-alloc
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Windows (Ninja or single-config generator)

```powershell
cd C:\Users\sunayak\.copilot\session-state\08b80c37-1925-49f6-964d-67f7cf9e1d95\files\gpu-tools-study\projects\P1-micro-alloc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Linux / macOS (GCC or Clang)

```bash
cd /path/to/P1-micro-alloc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Running benchmarks

After a Release build:

### Multi-config generators (Visual Studio)

```powershell
.\build\Release\bench.exe
```

### Single-config generators (Ninja / Makefiles)

```bash
./build/bench
```

The benchmark prints:

- median timings after a warmup,
- baseline vs optimized comparisons,
- speedup ratios,
- allocation counts for `std::vector` growth.

## ASan / UBSan instructions

The project exposes `ENABLE_ASAN` for **GCC/Clang** and adds:

- `-fsanitize=address,undefined`
- `-fno-omit-frame-pointer`

Example:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
./build-asan/tests
./build-asan/bench
```

Notes:

- `ENABLE_ASAN` is intended for GCC/Clang builds.
- On MSVC, use a normal Debug build, or use Clang on Windows if you specifically want the sanitizer option from this project.

## Acceptance criteria

You are done when all of the following are true:

1. `tests` passes with zero failures.
2. GCC/Clang sanitizer build is clean under AddressSanitizer and UBSan.
3. On the included benchmark in **Release** mode:
   - pool allocation beats raw `new`/`delete` in the many-small-allocation test,
   - arena + intrusive list beats or is clearly competitive with `std::list` for the large linked-structure build,
   - `std::vector` with `reserve()` performs exactly **one allocation** in the tracked growth test,
   - AoS vs SoA results are measured and explained, even if the exact winner varies by CPU and compiler.
4. You can explain *why* the numbers came out that way.

## What to observe (and why)

### 1. Many small allocations: `new`/`delete` vs pool vs arena

Expected result: **pool** and **arena** should usually beat the general heap.

Why:

- `new`/`delete` must manage a flexible heap that works for many sizes and lifetimes.
- A **pool** reuses same-sized chunks, so allocate/free becomes mostly pointer popping/pushing.
- An **arena** turns many allocations into pointer bumping, then reclaims everything with one `reset()`.
- Less allocator metadata traffic usually means better locality and fewer branches.

### 2. `std::list` vs intrusive list in arena

Expected result: the intrusive arena-backed structure should usually win.

Why:

- `std::list` typically performs one heap allocation per node.
- Intrusive nodes store the links *inside* the object; there is no separate link node.
- Arena-backed nodes tend to sit closer together in memory, improving traversal locality.
- Fewer allocations also means less fragmentation pressure.

### 3. AoS vs SoA traversal

Expected result: **SoA** often wins for field-wise streaming loops.

Why:

- In **AoS** (`struct Particle { x, y, z, ... }`), each iteration loads fields you may not need.
- In **SoA**, each field sits in its own dense array, so the CPU prefetcher gets a cleaner pattern.
- Better cache line utilization means more useful bytes per load.

### 4. `std::vector` growth with and without `reserve()`

Expected result: `reserve()` reduces reallocations dramatically and usually improves time.

Why:

- Without `reserve()`, vector grows geometrically and copies/moves elements multiple times.
- With `reserve(N)`, capacity is acquired once, so pushes do not trigger repeated reallocations.
- The tracking allocator lets you **prove** the reallocation count instead of guessing.

## Questions to answer after finishing

These are deliberately close to common interview prompts:

1. When would you choose an arena over `std::pmr::monotonic_buffer_resource` or the general heap?
2. Why can a pool allocator be O(1) for both allocation and free?
3. What lifetime assumptions make arena allocation safe? What bugs appear when the assumptions are wrong?
4. Why does `std::vector` usually outperform `std::list` even though `std::list` has O(1) insertion once you have an iterator?
5. What does "intrusive" mean, and why is it common in kernels, engines, schedulers, or GUI frameworks?
6. Why can SoA outperform AoS even when both are technically O(N)?
7. What is fragmentation, and why do custom allocators sometimes reduce it?
8. Why must allocators care about alignment? What breaks if they do not?
9. Why should benchmarks use warmups, multiple iterations, and medians?
10. If a benchmark result surprises you, how would you validate whether the code or the measurement is wrong?

## Suggested workflow

1. Build `tests` and make it pass first.
2. Run `bench` in Release mode.
3. Read the comments in each header and connect the design to the numbers you see.
4. Modify one variable at a time: block size, chunk count, reserve amount, object size, traversal pattern.
5. Re-run benchmarks and explain the change before moving on.
