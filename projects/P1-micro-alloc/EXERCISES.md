# Exercises for P1: micro-alloc

These exercises are designed to be done in order. Each one has a goal, a concrete deliverable, and a suggested success criterion.

---

## 1) Reading pass (Level 1)

**Goal:** build a mental model of all four core components.

**Do:**
- Read `arena.h`, `pool.h`, `intrusive_list.h`, and `tracking_allocator.h`.
- For each file, write a 2-3 sentence explanation of the main idea.

**Deliverable:** four short summaries.

**What you should learn:** why each tool exists and what problem it is trying to simplify.

---

## 2) Arena trace by hand (Level 1)

**Goal:** understand pointer bumping and alignment padding.

**Do:**
- Pick a block size of 64 bytes.
- Simulate allocations of sizes/alignment: `(5, 1)`, `(8, 8)`, `(3, 4)`, `(16, 16)`.
- Draw the used bytes and every padding gap.

**Deliverable:** a diagram of the block layout after each allocation.

**What you should learn:** alignment can create internal fragmentation even in a very fast allocator.

---

## 3) Add a typed arena helper benchmark (Level 2)

**Goal:** practice using placement new with the arena.

**Do:**
- In `bench.cpp`, replace one raw `allocate()` path with `arena.create<T>()`.
- Re-run the benchmark and verify behaviour is unchanged.

**Deliverable:** working code and a short note about why the timing should be almost identical.

**What you should learn:** construction cost and allocation cost are separate concerns.

---

## 4) Pool page sizing experiment (Level 2)

**Goal:** see how batching affects allocator cost.

**Do:**
- Benchmark the pool with page sizes 32, 128, 1024, and 4096 chunks.
- Record timing and memory footprint observations.

**Deliverable:** a small result table.

**What you should learn:** larger pages can reduce page-allocation overhead, but they also retain more memory.

---

## 5) Enable debug poisoning and catch a bug (Level 2)

**Goal:** connect allocator debugging features to real mistakes.

**Do:**
- Temporarily create a use-after-free bug in a small local experiment.
- Turn on pool poisoning and inspect the corrupted bytes in a debugger.

**Deliverable:** a short explanation of what changed in memory and why poisoning helps.

**What you should learn:** debug allocators are slower on purpose because observability matters.

---

## 6) Extend the intrusive list tests (Level 3)

**Goal:** reason about invariants.

**Do:**
- Add tests for `push_front`, `clear`, and iterator decrement from `end()`.
- Verify that erased nodes become self-linked again.

**Deliverable:** new passing tests.

**What you should learn:** data structure correctness is mostly about maintaining invariants under every edge case.

---

## 7) Prove vector growth behaviour (Level 3)

**Goal:** move from intuition to measurement.

**Do:**
- In `tests.cpp`, add a test that checks `std::vector` without `reserve()` performs more than one allocation for a moderate `N`.
- Print the exact count using `tracking_allocator`.

**Deliverable:** a passing test and the observed count.

**What you should learn:** standard containers are predictable if you instrument them.

---

## 8) Compare `std::list` locality to arena-backed nodes (Level 3)

**Goal:** connect memory layout to traversal cost.

**Do:**
- In `bench.cpp`, add a second pass that traverses each list multiple times after construction.
- Compare whether the gap widens when traversal dominates.

**Deliverable:** updated benchmark output and a short interpretation.

**What you should learn:** pointer-heavy structures often lose because the CPU cache sees a scattered access pattern.

---

## 9) Add a freelist-backed object cache (Level 4)

**Goal:** design a small allocator-powered subsystem.

**Do:**
- Build a tiny object cache on top of `FixedPool` for a single struct type.
- Expose `acquire()` / `release()` and track how many objects are active.

**Deliverable:** a compilable implementation and tests.

**What you should learn:** allocators become more useful when wrapped in a domain-specific API.

---

## 10) Interview-style writeup (Level 4)

**Goal:** turn implementation details into explanation skills.

**Do:** write concise answers to these prompts:
- Why is `reserve()` often enough to fix vector reallocation churn?
- When is an arena allocator the wrong choice?
- Why can intrusive containers be faster *and* more dangerous?
- What forms of fragmentation are visible in this project?
- Why can SoA beat AoS in a streaming loop?

**Deliverable:** a one-page writeup.

**What you should learn:** strong low-level engineers explain trade-offs, not just code.
