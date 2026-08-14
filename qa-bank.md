# Consolidated Rapid-Fire Q&A Bank

**This is a drill, not a textbook.** The module files contain the long-form answers with
follow-ups; this file contains ~260 short questions with the *compressed* answer you
should be able to produce in 15–45 seconds.

**How to use it:**
1. Cover the answer column. Say your answer **out loud**.
2. Mark each question ✅ (clean), ⚠️ (fumbled), ❌ (no idea).
3. Re-drill only ⚠️ and ❌ after 1 day, 3 days, 7 days, 16 days.
4. Anything you fumble twice — go back to the module and re-read that section.

Speaking the answer matters. The gap between recognizing an answer and producing one
under mild stress is where interviews are lost.

---

## Section 1 — Modern C++ (M1)

**1. What is the rule of five?**
If you define any of destructor, copy constructor, copy assignment, move constructor, or move assignment, you probably need to define all five, because the presence of one implies non-trivial resource ownership. Rule of zero is better: use RAII members so you need none of them.

**2. What does `std::move` actually do?**
Nothing at runtime. It is a static_cast to an rvalue reference — it only changes overload resolution so a move constructor or move assignment is selected instead of a copy.

**3. Difference between `std::move` and `std::forward`?**
`move` unconditionally casts to rvalue. `forward` casts *conditionally*, preserving the original value category of a forwarding reference — it is what makes perfect forwarding work.

**4. What is a forwarding (universal) reference?**
`T&&` where `T` is a deduced template parameter. Reference collapsing means it binds to both lvalues and rvalues, deducing `T` as `U&` for lvalues and `U` for rvalues.

**5. Reference collapsing rules?**
`& &` → `&`, `& &&` → `&`, `&& &` → `&`, `&& &&` → `&&`. Any lvalue reference in the pair wins.

**6. What is copy elision / RVO?**
The compiler constructs the return value directly in the caller's storage, skipping copy/move entirely. Since C++17 it is *mandatory* for prvalues; NRVO (returning a named local) is still optional but usually done.

**7. Why must a move constructor be `noexcept`?**
`std::vector` growth needs the strong exception guarantee. If the move constructor may throw, vector uses `move_if_noexcept`, which falls back to *copying* — so a missing `noexcept` silently costs you the performance you wrote the move for.

**8. What is the state of a moved-from object?**
Valid but unspecified. You may destroy it or assign to it; you may not assume anything about its value. Standard library types are typically left empty, but that is not a general guarantee.

**9. `unique_ptr` vs `shared_ptr` cost?**
`unique_ptr` is zero-overhead — same size as a raw pointer with a stateless deleter. `shared_ptr` is two pointers wide and does atomic refcount increments/decrements, which is genuinely expensive when copied across threads.

**10. Why `make_shared` over `shared_ptr<T>(new T)`?**
One allocation instead of two — the control block and object are allocated together. Caveat: the object's memory is not freed until the last *weak* reference dies, which matters for large objects with long-lived weak refs.

**11. What breaks a `shared_ptr` cycle?**
`weak_ptr`. A cycle of shared_ptrs never reaches zero refcount and leaks; the back-edge should be weak.

**12. What is `enable_shared_from_this` for?**
So a member function can hand out a `shared_ptr` to itself that shares the *existing* control block instead of creating a second, independent one (which would cause a double free).

**13. What is SFINAE?**
"Substitution Failure Is Not An Error" — if template argument substitution produces an invalid type in the immediate context, that overload is removed from the candidate set rather than being a hard error. Used with `enable_if` to constrain templates. C++20 concepts replace it far more readably.

**14. What is CRTP and when is it useful?**
Curiously Recurring Template Pattern: `class D : public Base<D>`. It gives static polymorphism — the base can call into the derived without a virtual call. Useful in hot paths and for injecting shared behavior; costs are code bloat and no runtime heterogeneity.

**15. Cost of a virtual call?**
A load of the vptr, a load from the vtable, then an indirect call — but the real cost is that it is an inlining barrier and a possible branch misprediction. In a tight loop over homogeneous objects it matters; in a loop dominated by memory traffic it is usually noise.

**16. What is the object layout of a class with virtual functions?**
A vptr (typically first), then the members. Each polymorphic class has one vtable; the vptr points into it. Multiple inheritance gives multiple vptrs and requires `this`-pointer adjustment (thunks).

**17. Why is a virtual destructor needed?**
Deleting a derived object through a base pointer with a non-virtual destructor is undefined behavior — the derived destructor never runs. Make it virtual, or make the destructor protected and non-virtual to prevent that usage.

**18. What is the ODR?**
One Definition Rule: exactly one definition of any entity across the program, and if it appears in multiple translation units (inline functions, templates, class definitions) the definitions must be token-identical. Violations are IFNDR — the linker often will not tell you.

**19. What does `inline` actually mean today?**
Primarily a linkage relaxation: multiple identical definitions across TUs are allowed and merged. Whether the compiler actually inlines the body is an independent decision.

**20. What is strict aliasing?**
You may not access an object through a pointer of an unrelated type. The compiler assumes `int*` and `float*` never alias and reorders accordingly. Legal type punning is via `memcpy` (or `std::bit_cast` in C++20), not via pointer casts or reading an inactive union member.

**21. How do you legally reinterpret the bits of a float as an int?**
`std::memcpy` into an `uint32_t` (or `std::bit_cast`). The compiler optimizes it to a register move; it is free and correct.

**22. What is undefined behavior and why is it dangerous?**
Behavior the standard imposes no requirements on. It is dangerous because the compiler *assumes it cannot happen* and optimizes on that assumption — so the visible symptom often appears far from the cause, and behavior can change with optimization level.

**23. Name several sources of UB.**
Out-of-bounds access, dereferencing null, use-after-free, signed integer overflow, uninitialized reads, data races, strict aliasing violations, misaligned access, shifting by ≥ width, and infinite loops without side effects.

**24. `alignas` and `alignof`?**
`alignof(T)` queries the required alignment; `alignas(N)` demands it. Relevant for SIMD types, DMA buffers, and avoiding false sharing by padding to a cache line.

**25. What is false sharing?**
Two threads writing distinct variables that share a cache line, causing the line to ping-pong between cores. Fixed by padding/aligning hot per-thread data to `hardware_destructive_interference_size` (typically 64 bytes).

**26. What is a data race vs a race condition?**
A data race is two unsynchronized accesses to the same location with at least one write — undefined behavior. A race condition is a logic bug where the outcome depends on timing; it can exist even in fully synchronized, well-defined code.

**27. Explain `memory_order_relaxed`.**
Atomicity only, no ordering or visibility guarantees with respect to other operations. Fine for counters where you only need the final total.

**28. Explain acquire/release.**
A release store publishes everything sequenced before it; an acquire load that reads that value sees all of it. That pairing is how you hand data between threads — the classic "fill the buffer, release-store the flag; acquire-load the flag, read the buffer."

**29. What is `seq_cst` and what does it cost?**
Sequential consistency: a single total order over all seq_cst operations, on top of acquire/release. It is the default and the easiest to reason about, but on x86 it forces an expensive store-load fence (`mfence`/`xchg`), and on weaker architectures like ARM it costs more still.

**30. Is `volatile` useful for threading?**
No. It prevents the compiler from eliding accesses — useful for memory-mapped I/O registers — but provides no atomicity and no ordering. Use `std::atomic`.

**31. Exception safety guarantees?**
Basic: invariants hold, no leaks. Strong: the operation either completes or has no effect (commit-or-rollback). Nothrow: it cannot fail. `push_back` gives strong; that is why the noexcept move question matters.

**32. What is copy-and-swap?**
Implement assignment by constructing a copy and swapping with it. You get the strong guarantee and self-assignment safety for free, at the cost of one extra copy.

**33. What is `constexpr`?**
Indicates the function/variable *can* be evaluated at compile time when given constant arguments. `consteval` (C++20) requires it. Useful for building lookup tables — e.g. an opcode decode table — at compile time.

**34. Why is `std::vector` growth amortized O(1)?**
Because capacity grows geometrically (typically ×1.5 or ×2), so the total copy work across n insertions is O(n). Any constant growth factor > 1 works; growing by a fixed amount would be O(n²).

**35. AoS vs SoA?**
Array of Structs keeps a whole record contiguous — good when you touch all fields. Struct of Arrays keeps each field contiguous — much better when you stream one field, because you don't drag unused fields through cache, and it vectorizes.

**36. What is an arena/bump allocator and why use one?**
Allocate by incrementing a pointer; free everything at once with a reset. Allocation is a handful of instructions, objects are contiguous so traversal is cache-friendly, and there is no fragmentation. Ideal for the per-test lifetime of a generated workload.

**37. What is placement new?**
Constructing an object in already-allocated storage: `new (ptr) T(args)`. You must destroy it by calling the destructor explicitly. It is how containers and allocators separate allocation from construction.

**38. What is pImpl and what is it for?**
Hiding the implementation behind a pointer to an incomplete type. It decouples compilation (callers don't recompile when internals change) and stabilizes ABI, since the size of the public class no longer depends on private members. Costs an indirection and an allocation.

**39. What breaks ABI compatibility?**
Adding/removing/reordering data members, changing member types, adding a virtual function (changes the vtable layout), changing inheritance, changing function signatures or calling convention, and changing anything about an inline function that callers already baked in.

**40. Static vs dynamic linking, tradeoffs?**
Static: one binary, no runtime resolution cost, no version skew, but bigger and requires relinking to patch. Dynamic: shared memory across processes, upgradeable independently, supports plugins via `dlopen` — but pays symbol resolution cost and exposes you to ABI skew.

---

## Section 2 — Design Patterns & Framework Design (M2)

**41. Which patterns actually show up in compiler/tool code?**
Factory and registry (creating instructions/back ends), Visitor (IR traversal, printing, analysis passes), Builder (assembling a workload), Command (a GPU method as an object — queueable, serializable, replayable), Strategy (pluggable policies), Composite (nested blocks/scene graphs), Flyweight (interning repeated operand descriptors).

**42. Why Visitor for an IR?**
It lets you add new *operations* over a fixed set of node types without touching the node classes — which is exactly the compiler situation, where node types are stable and passes proliferate. The cost is that adding a new node type requires touching every visitor.

**43. What is the expression problem?**
You want to extend both the set of types and the set of operations without modifying existing code. Virtual functions make adding types easy and operations hard; Visitor makes adding operations easy and types hard. `std::variant` + `std::visit` is the closed-set alternative.

**44. When would you use `std::variant` + `std::visit` instead of Visitor?**
When the set of node types is genuinely closed and known at compile time. You get value semantics, no heap allocation, no virtual dispatch, and exhaustiveness checking — but you lose the ability for a plugin to add a new node type.

**45. What is a self-registering factory and why does it matter here?**
A static object whose constructor registers a creator function with a global registry. It matters because adding a new architecture or opcode becomes "add a file," with no edit to any existing file — which is the Open/Closed Principle made concrete.

**46. What's the danger with static registration?**
Static initialization order across translation units is unspecified, so a registry that is itself a namespace-scope object may not exist yet. Fix with a Meyers singleton (function-local static), which is initialized on first use and is thread-safe since C++11.

**47. What is type erasure?**
Providing a single concrete type that can hold any type satisfying an interface, without those types sharing a base class — `std::function` and `std::any` are the canonical examples. Implemented with an internal abstract base plus a templated derived wrapper, usually with a small-buffer optimization.

**48. CRTP vs virtual dispatch — how do you choose?**
CRTP when the type is known at compile time and the call is in a hot loop: no indirection, full inlining. Virtual when you need runtime heterogeneity — a container of different back ends, or a plugin loaded at runtime. Most framework boundaries need virtual; the inner loops behind them can be CRTP.

**49. Why is Singleton usually a mistake?**
It is global mutable state: it hides dependencies, makes testing hard (you cannot substitute it), creates initialization-order problems, and in this domain it is a determinism hazard because shared state couples otherwise-independent tests. Prefer an explicit context object passed in.

**50. What is the Open/Closed Principle, concretely, in a stimulus framework?**
Adding a new opcode, engine, or architecture requires *adding* a table row or a plugin file, not *modifying* a switch statement in the core. If you have to edit a central switch, the design has failed the principle.

**51. Why table-driven encoding rather than per-instruction code?**
An ISA change becomes a data change, reviewable by an architect and diffable against the spec. Crucially, the encoder and disassembler share the table, so they cannot drift apart — and you get round-trip testing almost for free.

**52. What is the Decorator pattern good for in tools?**
Layering behavior on an existing interface without changing it: wrapping an encoder to log every call, to validate, to count coverage, or to inject faults. Each concern stays separate and composable.

**53. What is Flyweight and where would you use it?**
Sharing immutable data between many objects to save memory — interning operand or instruction descriptors when you have tens of millions of instruction objects in memory.

**54. API vs ABI compatibility?**
API is source compatibility: existing code still *compiles*. ABI is binary compatibility: existing compiled code still *links and runs*. You can break ABI without breaking API (add a data member) and vice versa.

**55. How do you version a plugin interface?**
Put a version number in the interface and in the plugin's entry point; refuse to load a mismatched plugin loudly at load time rather than crashing later. Use a C ABI for the entry point, because C++ ABI compatibility across compiler versions is not something you want to depend on.

**56. Exceptions vs error codes for a framework API?**
Exceptions for genuinely exceptional conditions and constructor failures; error codes or `expected`-style returns for expected failures like "this instruction is not supported on this architecture," which callers must handle routinely. Whatever you choose, be consistent — mixed models are what actually hurt users.

**57. What is Liskov substitution and how is it violated in hardware modeling?**
A subtype must be usable anywhere the base is. It is violated when a derived "architecture" silently doesn't support an operation the base promises, and throws or no-ops. The fix is capability queries — ask "do you support X?" — rather than modeling capability differences with inheritance.

**58. Why prefer composition over inheritance?**
Inheritance couples you to the base's implementation and layout, is single-shot in C++ for a given base, and encourages deep hierarchies that mirror a taxonomy rather than a behavior. Composition lets you mix capabilities and swap them at runtime.

**59. Name anti-patterns specific to this domain.**
Deep inheritance mirroring the hardware taxonomy; switch-on-architecture-enum sprawl; hidden global state destroying determinism; over-templating that makes compile times and error messages unbearable; leaky abstractions where architecture-specific details bleed into the neutral IR.

---

## Section 3 — Systems & OS (M3)

**60. Process vs thread?**
A process owns an address space and resources; threads share the address space within a process and have their own stack and registers. Context switching between processes is more expensive because it changes the page table root and typically flushes TLB entries.

**61. What happens on a system call?**
A trap instruction switches to kernel mode, saves user state, dispatches through the syscall table, executes in kernel context with the kernel stack, then returns and restores. The cost is the mode transition plus cache/TLB pollution — which is why batching (submitting many commands at once) matters so much for driver design.

**62. What is virtual memory for?**
Isolation between processes, the illusion of a large contiguous address space, and the ability to relocate/share/swap physical pages independently of the addresses programs use.

**63. Walk a virtual address translation.**
Split the address into per-level indices plus an offset. Check the TLB first; on a hit you have the physical frame immediately. On a miss the hardware page walker traverses the multi-level page table from the root register, caches the result in the TLB, and if the entry isn't present it raises a page fault for the OS to handle.

**64. What is a TLB shootdown?**
When one core changes a page mapping, other cores may hold stale TLB entries. The OS must interrupt them (IPI) to invalidate — expensive, and a reason why frequent mapping changes hurt.

**65. Minor vs major page fault?**
Minor: the page is in memory but not mapped in this page table (e.g. copy-on-write, or first touch of an already-cached page). Major: the page must be read from storage — orders of magnitude slower.

**66. What is pinned (page-locked) memory and why does the GPU need it?**
Memory the OS guarantees will not be paged out or moved, so it has a stable physical address. DMA engines work with physical addresses and cannot handle a page vanishing mid-transfer, so the driver either requires pinned memory or must stage through a pinned bounce buffer.

**67. Why is a pinned `cudaMemcpy` faster than a pageable one?**
Pageable memory must be copied into a pinned staging buffer first, so you pay an extra CPU-side copy and cannot fully overlap. Pinned memory can be DMA'd directly and enables true async transfer overlapping compute.

**68. What is the downside of pinning?**
It removes pages from the OS's pageable pool. Over-pinning degrades system-wide memory management and can cause failures elsewhere — it is a shared resource.

**69. What does an IOMMU do?**
Translates device-visible addresses to physical addresses, so a device sees its own address space. It provides isolation (a buggy or malicious device cannot DMA anywhere), enables safe device passthrough to VMs, and lets a device see contiguous addresses over scattered physical pages.

**70. What is MMIO?**
Device registers mapped into the physical address space, so the CPU accesses them with ordinary loads and stores. Those accesses must not be cached or reordered arbitrarily, hence uncached or write-combining mappings.

**71. What is a PCIe BAR?**
Base Address Register — declares a region of device memory/registers the host maps into its address space. Resizable BAR lets the host map the GPU's whole VRAM instead of a small window, which speeds up host writes to device memory.

**72. What is a doorbell register?**
An MMIO location the driver writes to tell the device "there is new work in the queue." The command data itself sits in memory; the doorbell is just the notification, avoiding per-command MMIO round trips.

**73. Roughly what does a PCIe round trip cost, and why does it matter?**
Microseconds — far more than a memory access. It is why drivers batch commands into a pushbuffer and use a single doorbell write, and why per-launch overhead is a real limit for small kernels.

**74. UMD vs KMD in a GPU driver?**
The user-mode driver runs in the application's process: it tracks API state, compiles shaders, validates, and builds command buffers. The kernel-mode driver handles what must be privileged: memory management, page tables, scheduling between contexts, and interrupts. Keeping work in the UMD avoids syscalls.

**75. What is a pushbuffer / command buffer?**
A region of memory the driver fills with a stream of commands (in NVIDIA terminology, "methods" — writes to engine method addresses with data). The GPU's front end fetches and executes it. Batching into a buffer amortizes submission cost.

**76. Trace `cudaMemcpy` from call to bits landing in device memory.**
The runtime validates arguments and, for pageable source, stages through a pinned buffer. It builds a copy command into the pushbuffer, and rings the doorbell. The GPU's copy engine fetches the command, translates addresses through the GPU MMU, and DMAs the data over PCIe. Completion signals a fence/semaphore, which raises an interrupt the KMD handles, and the runtime observes the fence value to satisfy the synchronization.

**77. What is a fence or semaphore in GPU submission?**
A monotonically increasing value the GPU writes to memory when it reaches a point in the command stream. The CPU (or another engine) waits on that value to know work has completed, without polling the device over PCIe.

**78. What is MESI?**
Cache coherence states: Modified, Exclusive, Shared, Invalid. A write requires exclusive ownership, invalidating other copies — which is exactly the mechanism that makes false sharing expensive.

**79. What is NUMA and why does it matter for GPU transfers?**
Memory is attached to specific sockets; access to a remote node's memory costs more. If your pinned buffer is allocated on a node far from the CPU socket that owns the GPU's PCIe root complex, transfer bandwidth drops measurably. Pin threads and memory near the device.

**80. What is the ABA problem?**
In a lock-free algorithm, a value read as A, changed to B, then back to A, so a compare-and-swap succeeds even though the structure changed underneath. Fixed with tagged pointers/version counters or with safe memory reclamation like hazard pointers or RCU.

**81. Spinlock vs mutex?**
A spinlock burns CPU waiting and is right only for very short critical sections on a dedicated core. A mutex sleeps, yielding the CPU, at the cost of a syscall and context switch when contended.

**82. What is in an ELF file?**
A header, program headers (segments — what the loader maps), section headers (what the linker uses: `.text`, `.data`, `.bss`, `.rodata`, symbol table, relocations), and optionally debug sections. Sections are the link-time view; segments are the run-time view.

**83. What is the GOT and PLT?**
Global Offset Table holds resolved addresses for external symbols; the Procedure Linkage Table provides stubs enabling lazy binding — the first call goes through the dynamic linker, which patches the GOT so later calls jump directly.

**84. How would you build a plugin system in C++?**
`dlopen`/`LoadLibrary` the shared object, look up a versioned `extern "C"` factory function, and get back a pointer to an abstract interface. The C linkage avoids C++ ABI/name-mangling fragility across compiler versions, and the version check must fail loudly at load time.

**85. How does a debugger set a breakpoint?**
Software breakpoints overwrite the instruction with a trap (`int3` on x86), catch the resulting signal, and restore the original byte to step over it. Hardware breakpoints use debug registers, are limited in number, but can watch data addresses without modifying memory.

**86. What is DWARF?**
The debug info format: a tree of DIEs describing types, variables and scopes, plus a line table mapping addresses to source lines, and location expressions saying where a variable lives at a given PC. It is what lets a debugger show source-level state for optimized machine code.

**87. What do ASan, UBSan and TSan each catch?**
ASan: out-of-bounds, use-after-free, leaks, via shadow memory and redzones (~2× slowdown). UBSan: undefined behavior like signed overflow, misaligned access, bad shifts. TSan: data races, via a happens-before shadow (~5–15× slowdown). ASan and TSan cannot be combined.

**88. How do you profile a CPU-bound C++ program on Linux?**
`perf record`/`perf report` for sampling profiles, `perf stat` for cache misses/branch misses/IPC, and a flamegraph to see call-stack attribution. Then form a hypothesis about the mechanism — memory bound, branch bound, allocation bound — and confirm with counters before changing code.

---

## Section 4 — GPU Architecture (M4)

**89. CPU vs GPU in one sentence?**
The CPU minimizes latency for one thread using speculation and big caches; the GPU maximizes throughput across thousands of threads by hiding latency with multithreading.

**90. What is SIMT and how does it differ from SIMD?**
SIMT executes one instruction across a warp of threads, but each thread has its own registers and program counter semantics, so threads may diverge and be masked off. SIMD is a single thread operating on a vector register with no per-lane control flow — divergence isn't even expressible.

**91. What is a warp?**
32 threads that are scheduled and issued together on NVIDIA hardware. It is the true unit of execution; block size should be a multiple of 32 or you waste lanes.

**92. What is warp divergence and what does it cost?**
When threads in a warp take different branch paths, the paths execute serially with inactive lanes masked. Cost is proportional to the number of distinct paths taken — a fully divergent 32-way branch is up to 32× slower for that region.

**93. Does an `if` always cause divergence?**
No. If every thread in the warp takes the same side, there is no divergence — the branch is uniform. Divergence is per-warp, not per-block, which is why aligning branch conditions to warp boundaries fixes so many kernels.

**94. What changed with Volta's independent thread scheduling?**
Threads got individual program counters, so the hardware no longer guarantees lockstep re-convergence within a warp. That broke implicit warp-synchronous programming, which is why the `_sync` intrinsics with explicit masks and `__syncwarp()` became mandatory.

**95. What's in an SM?**
Warp schedulers and dispatch units, FP32/INT32/FP64 pipelines, tensor cores, SFUs, load/store units, a large register file, a combined shared memory/L1, an instruction cache, and the texture units — partitioned into processing blocks each with its own scheduler.

**96. What is occupancy?**
Active warps per SM divided by the maximum supported. It is limited by registers per thread, shared memory per block, block size, and hardware limits on blocks/warps per SM.

**97. Is higher occupancy always better?**
No. Occupancy is a means of hiding latency, not an end. A kernel with high instruction-level parallelism and heavy register use can saturate memory bandwidth at 25–30% occupancy — Volkov's result. Past the point where latency is hidden, extra occupancy buys nothing and the registers it cost may hurt.

**98. Order the memory hierarchy by latency.**
Registers (~1 cycle) → shared memory / L1 (tens of cycles) → L2 (~200) → device DRAM (~400–800) → host over PCIe (microseconds). Roughly an order of magnitude at each step.

**99. What is memory coalescing?**
The hardware combines the addresses requested by a warp into the minimum number of memory transactions. If 32 consecutive threads read 32 consecutive 4-byte words, that's one or a few transactions; scattered addresses can require up to 32.

**100. What does a stride-2 access pattern cost?**
Roughly half the effective bandwidth, because each fetched cache line/sector delivers only half its bytes usefully. Larger strides degrade further until every thread pulls its own line.

**101. What are shared memory bank conflicts?**
Shared memory is divided into 32 banks of 4-byte words. If two threads in a warp access different addresses in the same bank, the accesses serialize. Same-address access is broadcast and is free.

**102. How do you fix bank conflicts in a transpose?**
Pad the shared tile: declare `__shared__ float tile[32][33]`. The extra column shifts each row into a different bank, converting a 32-way conflict into none, for the cost of a little shared memory.

**103. What is the roofline model?**
Attainable performance = min(peak compute, arithmetic intensity × peak bandwidth). Plotting it shows a bandwidth-limited slope and a compute-limited ceiling; the ridge point is the arithmetic intensity at which you transition.

**104. What is arithmetic intensity, and what is it for SAXPY?**
FLOPs per byte of DRAM traffic. SAXPY (`y = a*x + y`) does 2 FLOPs and moves 12 bytes (read x, read y, write y), so AI ≈ 0.167 — deeply memory bound on any GPU.

**105. Why is matrix multiply compute-bound while vector add is memory-bound?**
SGEMM does O(n³) work on O(n²) data, so with tiling each loaded element is reused O(tile) times, pushing arithmetic intensity high. Vector add touches each byte once for one operation.

**106. How do you tell if a kernel is memory-bound or compute-bound?**
Compute achieved bandwidth and achieved FLOP/s and compare each against peak — whichever is near its ceiling is your limiter. In Nsight Compute the SOL section tells you directly. If neither is near peak, you're latency- or occupancy-limited, and the stall reasons will say which.

**107. Apply Little's Law to GPU memory.**
Required in-flight bytes = bandwidth × latency. At roughly 500 GB/s and ~500 ns latency you need ~250 KB of memory requests in flight to saturate. That is why you need many warps or many independent loads per thread — insufficient memory-level parallelism leaves bandwidth on the table regardless of how fast the DRAM is.

**108. What is a GPC/TPC?**
Graphics Processing Cluster and Texture Processing Cluster — the levels of chip hierarchy grouping SMs, with raster engines at the GPC level. It matters for work distribution and for understanding partial-chip (binned) products.

**109. What do copy engines do?**
Dedicated DMA engines that move data independently of the SMs, enabling transfers to overlap with compute. Having multiple lets you overlap H2D and D2H simultaneously with kernel execution.

**110. What did Ampere add that's relevant to programmers?**
Asynchronous copy from global to shared memory bypassing registers (`cp.async`), third-gen tensor cores with sparsity support, and a larger, more configurable L1/shared.

**111. What did Hopper add?**
Thread block clusters (a new level of hierarchy allowing blocks on different SMs to cooperate), distributed shared memory across a cluster, and the Tensor Memory Accelerator for bulk asynchronous data movement.

**112. What makes a workload "interesting" for GPU verification?**
Corner cases the hardware must handle correctly and that common code paths never hit: maximum divergence, worst-case bank conflicts, heavy atomic contention on one address, memory aliasing, cache-thrashing working-set sizes, boundary-sized blocks, and mixed engine concurrency.

---

## Section 5 — CUDA (M5)

**113. Grid, block, thread — what maps to hardware?**
A thread runs on a lane; 32 threads form a warp; a block is scheduled entirely onto one SM and can use shared memory and `__syncthreads`; the grid is all blocks and is distributed across SMs. Blocks cannot synchronize with each other (without cooperative groups).

**114. Why must a block fit on one SM?**
Because shared memory is physically per-SM and `__syncthreads()` is a barrier implemented within an SM. That is also why block size is limited (1024 threads) and why shared memory per block is capped.

**115. How do you choose a block size?**
A multiple of 32, usually 128–512. Start at 256, then use the occupancy API or Nsight to check whether registers or shared memory are the binding constraint, and measure — the answer is kernel-specific and not worth guessing.

**116. What is a grid-stride loop and why use it?**
Each thread processes multiple elements by striding by `gridDim.x * blockDim.x`. It decouples the launch configuration from the problem size, handles any N without a boundary special case, and lets you tune grid size for the device rather than the data.

**117. Is a kernel launch synchronous?**
No — it is asynchronous; control returns to the host immediately. Errors from within the kernel surface later, which is why you check `cudaGetLastError()` after launch *and* synchronize to catch execution errors.

**118. Difference between `cudaGetLastError` and `cudaPeekAtLastError`?**
Both return the last error; `cudaGetLastError` also *resets* it to success. Errors are sticky per-context for some failure classes, so a real fault can poison every subsequent call.

**119. Where does local memory live?**
In device DRAM, despite the name — it is per-thread private storage used for register spills and for arrays that can't be register-allocated (e.g. dynamically indexed). It is cached, but spilling is still a performance problem worth eliminating.

**120. What causes register spilling and how do you see it?**
Too many live values per thread, or the compiler trading registers for occupancy under `__launch_bounds__`/`maxrregcount`. Compile with `-Xptxas=-v` and it prints registers, spill stores and spill loads per kernel.

**121. What is constant memory good for?**
Small read-only data accessed uniformly across a warp — it broadcasts in a single cycle from the constant cache. If threads in a warp read *different* constant addresses, the accesses serialize and you have lost the benefit.

**122. What does `__restrict__` buy you?**
It promises the pointers do not alias, letting the compiler reorder loads/stores, keep values in registers across stores, and use the read-only data path. On memory-bound kernels it can be a significant win for a one-word change.

**123. What is `__syncthreads()` and what is the correctness rule?**
A barrier across all threads in a block, plus a memory fence for shared memory. Every thread in the block must reach the *same* `__syncthreads()` — calling it inside divergent control flow is undefined behavior and can hang or corrupt.

**124. What does `__shfl_down_sync` do?**
Reads a value from a lane `delta` higher within the warp, directly through the register file, with no shared memory and no barrier. It is how you write a modern warp-level reduction.

**125. Why does the mask argument on `_sync` intrinsics exist?**
It names the threads that must participate. Since Volta's independent thread scheduling, the hardware no longer guarantees which threads are converged, so the mask makes the participating set explicit and the intrinsic will reconverge them.

**126. Write a warp reduction.**
`for (int off = 16; off > 0; off >>= 1) v += __shfl_down_sync(0xffffffff, v, off);` — after this, lane 0 holds the sum of the warp.

**127. Why is the classic `volatile` last-warp unroll unsafe now?**
It relied on the 32 threads of a warp executing in lockstep so no barrier was needed. Independent thread scheduling removed that guarantee, so the code has a real race on Volta and later. Use `__shfl_down_sync` or `__syncwarp()`.

**128. Walk through optimizing a reduction.**
Start with interleaved addressing using modulo — divergent and slow. Switch to non-divergent indexing, then to sequential addressing to fix bank conflicts, then add the first add during the global load to halve the block count, then unroll with a templated block size, and finally replace the last stages with warp shuffles plus one atomic per block.

**129. Hillis-Steele vs Blelloch scan?**
Hillis-Steele is simple and O(n log n) work — fine within one warp/block where you have idle lanes anyway. Blelloch is a two-phase upsweep/downsweep tree with O(n) work — better for large arrays where work efficiency dominates.

**130. How do you scan an array larger than one block?**
Scan each block, write per-block sums to an array, scan that array (recursively if needed), then add each block's exclusive prefix back into its elements. The modern single-pass alternative is decoupled look-back.

**131. Why is a naive histogram with global atomics slow?**
All threads hitting a few bins serialize on the same addresses. With skewed input it collapses entirely. Fix by privatizing a histogram in shared memory per block and doing one atomic per bin at the end.

**132. What makes tiled SGEMM fast?**
Each tile is loaded into shared memory once and reused by every thread in the block, so global traffic drops by roughly the tile dimension and arithmetic intensity rises above the ridge point. Register tiling (each thread computing a small output sub-tile) raises reuse further.

**133. What is a stream?**
An ordered queue of operations. Work in the same stream is serialized; work in different streams may overlap. This is how you overlap copies with compute.

**134. What's special about the default stream?**
The legacy default stream is implicitly synchronizing — it blocks other blocking streams. Use `cudaStreamNonBlocking` streams, or compile with per-thread default stream, or you'll silently serialize what you thought you had overlapped.

**135. Why use CUDA Graphs?**
They capture a whole dependency graph of kernels and copies once and replay it with a single submission, removing per-launch CPU overhead. It matters when you launch many small kernels and are launch-bound.

**136. Runtime API vs Driver API?**
The runtime API (`cudaMalloc`, `<<<>>>`) is higher level with implicit context management. The driver API (`cuModuleLoad`, `cuLaunchKernel`) is explicit about contexts, modules and functions — which is what tools need when loading generated or runtime-compiled code rather than statically linked kernels.

**137. PTX vs SASS?**
PTX is a virtual, forward-compatible, documented ISA — the compiler's output and a stable target. SASS is the actual machine ISA of a specific architecture, produced by `ptxas`, and is not publicly documented. PTX can be JIT-compiled to SASS by the driver for a newer GPU.

**138. What is a fatbinary?**
A container embedding multiple compiled versions of a kernel — several architecture-specific cubins and optionally PTX. The driver picks the matching cubin, or JITs the PTX if none matches, which is how one binary runs on multiple GPU generations.

**139. `compute_XX` vs `sm_XX`?**
`compute_XX` is a virtual architecture — the PTX target. `sm_XX` is a real architecture — the SASS target. `-gencode arch=compute_80,code=sm_80` compiles PTX for 8.0 and assembles SASS for 8.0; embedding the PTX too gives forward compatibility via JIT.

**140. How do you inspect the generated code?**
`nvcc -ptx` for PTX, `nvcc -cubin` plus `cuobjdump -sass` or `nvdisasm` for SASS, and `-Xptxas=-v` for register/shared/spill counts. Add `--generate-line-info` to correlate back to source in the profiler.

**141. What does compute-sanitizer do?**
Four tools: `memcheck` for out-of-bounds and misaligned access, `racecheck` for shared memory races, `initcheck` for uninitialized reads, `synccheck` for barrier misuse. It is the first thing to run on any intermittent CUDA bug.

**142. Nsight Systems vs Nsight Compute?**
Systems gives the timeline across the whole application — where the gaps are, whether transfers overlap, whether you're CPU-bound. Compute profiles an individual kernel in depth — memory throughput, occupancy, stall reasons. Use Systems to find *which* kernel, Compute to find *why*.

**143. Your kernel is slow. Walk me through your process.**
Verify correctness first, then measure with events to get a reliable baseline. Compute achieved bandwidth and FLOP/s against peak to classify the bottleneck. Profile with Nsight Compute and read the SOL section and stall reasons. Then attack the specific limiter: coalescing and vectorized loads for memory, tiling for reuse, occupancy or ILP for latency, and fusion or graphs if launch-bound. Re-measure after each single change.

**144. Unified memory — what is it and when does it hurt?**
A single pointer valid on host and device, with the driver migrating pages on demand. Convenient and good for oversubscription and irregular access, but page-fault-driven migration can be far slower than an explicit copy for streaming access. Prefetch hints (`cudaMemPrefetchAsync`) recover most of the gap.

**145. What is `__launch_bounds__` for?**
It tells the compiler the maximum threads per block (and optionally minimum blocks per SM) so `ptxas` can cap register usage to achieve that occupancy. It's how you trade registers against occupancy deliberately rather than accepting the compiler's guess.

---

## Section 6 — Graphics (M6)

**146. Name the pipeline stages in order.**
Input assembly → vertex shader → tessellation (hull, tessellator, domain) → geometry shader → clipping → perspective divide → viewport transform → culling → rasterization → early-Z → fragment shader → late-Z/stencil → blending → framebuffer write.

**147. Which stages are programmable vs fixed function?**
Programmable: vertex, hull/domain, geometry, fragment, compute (and mesh/task, ray tracing shaders). Fixed function: input assembly, tessellator, clipping, rasterization, depth/stencil test, blending, and the ROP write.

**148. Walk the coordinate spaces.**
Object → world (model matrix) → view (view matrix) → clip (projection) → NDC (perspective divide by w) → window (viewport transform). Depth range is −1..1 in classic OpenGL and 0..1 in D3D and Vulkan.

**149. What is the perspective divide and why after clipping?**
Dividing x, y, z by w to project. Clipping happens in homogeneous clip space *before* the divide because dividing by a negative or zero w for vertices behind the eye produces garbage.

**150. Why is perspective-correct interpolation needed?**
Screen-space linear interpolation of attributes is wrong under perspective, because equal screen distances are not equal object-space distances. You interpolate attribute/w and 1/w linearly, then divide — that's the classic warped-texture bug when omitted.

**151. What is the top-left fill rule?**
A tie-breaking rule for pixels exactly on a shared edge: the pixel belongs to the triangle if the edge is a top or left edge. Without it, shared edges are either double-shaded (visible with blending) or leave gaps.

**152. Why are fragments processed in 2×2 quads?**
So screen-space derivatives (ddx/ddy) can be computed by finite differences between neighbors, which is what mipmap LOD selection needs. The cost is helper lanes: a triangle covering one pixel still shades four, which is why lots of tiny triangles waste shading throughput.

**153. What is early-Z and what disables it?**
Depth testing before the fragment shader, to skip shading occluded fragments. It's disabled when the shader can change the result the test depends on: writing `gl_FragDepth`, `discard`/alpha-test (in some configurations), and certain side-effecting shaders like those with image stores.

**154. What is hierarchical Z?**
A coarse per-tile depth range kept alongside the depth buffer, so whole tiles can be rejected before per-pixel testing. It's why front-to-back rendering order is faster than back-to-front.

**155. What is z-fighting and how do you fix it?**
Two coplanar surfaces whose interpolated depths quantize inconsistently, causing flickering. Fixes: increase depth precision, use reversed-Z with a floating-point depth buffer, move the near plane out, or apply polygon offset.

**156. Why does reversed-Z improve precision?**
Float has far more representable values near zero, while a standard perspective projection concentrates depth precision near the near plane — the two work against each other. Mapping near→1 and far→0 aligns float's density with where precision is scarce, dramatically improving distant-object precision.

**157. Why did the industry move from OpenGL to Vulkan/D3D12?**
The implicit, validating, state-machine model forced drivers to guess intent, do shader recompiles at draw time, and serialize submission — making performance unpredictable and blocking multithreaded command recording. Explicit APIs move that work to the application and to build time, giving thin drivers, predictable cost, and parallel recording.

**158. What is a pipeline state object?**
An immutable, pre-baked bundle of all the fixed-function and shader state for a draw. Baking it at build time eliminates the draw-time state validation and shader recompilation that caused hitches in older APIs.

**159. What is a descriptor set?**
A pre-built group of resource bindings (buffers, images, samplers) the shader will access, laid out according to a descriptor set layout. Binding one set is cheap compared to binding resources individually, and layouts let you share sets between compatible pipelines.

**160. Fence vs semaphore vs barrier in Vulkan?**
A fence synchronizes GPU→CPU. A semaphore synchronizes GPU queue→queue (timeline semaphores add counter values and CPU waits). A pipeline barrier synchronizes *within* a command buffer, expressing execution and memory dependencies and image layout transitions.

**161. What is an image layout transition and why does it exist?**
Images are stored in different hardware-optimal formats depending on use — a compressed render-target layout differs from a shader-read layout. The API makes the transition explicit so the driver knows exactly when to decompress or reformat instead of guessing.

**162. What is a render pass / subpass for?**
It declares the attachments and their load/store behavior up front. On tile-based GPUs that lets the driver keep an entire tile's attachments in on-chip memory across subpasses, never writing intermediates to DRAM — a large bandwidth win on mobile.

**163. What is SPIR-V?**
A binary intermediate representation for shaders. Shaders are compiled offline from GLSL/HLSL to SPIR-V, then the driver compiles SPIR-V to its own ISA — removing a full source compiler from the driver and making shader compilation reproducible.

**164. Compute shader vs CUDA kernel?**
The same SMs execute both; the difference is the front end and the ecosystem. Workgroup ≈ thread block, `groupshared`/`shared` ≈ `__shared__`, barriers correspond. Compute shaders integrate with the graphics pipeline and are portable across vendors; CUDA gives deeper hardware access and a richer tool ecosystem.

**165. What makes rendering non-deterministic, and why does it matter for verification?**
Floating-point reassociation and FMA contraction by the compiler, fast-math, differing rasterization rules between vendors, multithreaded triangle submission order, and uninitialized framebuffer contents. It matters because golden-image comparison is the primary checking method, so nondeterminism directly destroys your ability to check anything.

**166. How do you check a graphics test's result?**
Ideally an exact hash/CRC of the framebuffer when bit-exactness is guaranteed. Otherwise a tolerance-based comparison reporting differing pixel count and maximum channel delta, with per-vendor or per-driver golden images and a documented tolerance policy.

**167. What graphics corner cases would you target in verification stimulus?**
Degenerate and zero-area triangles, pixels exactly on shared edges (fill rule), near-plane clipping, guard-band overflow with huge triangles, thin slivers, coplanar z-fighting geometry, backfacing primitives, blend order with overlapping transparency, and MSAA resolve behavior.

---

## Section 7 — Tooling, Simulation & Verification (M7)

**168. Describe the pre-silicon flow.**
Architectural/performance models come first for design exploration, then a functional C-model as the reference, then RTL, verified by simulation, then emulation on special-purpose hardware, then FPGA prototyping, then silicon bring-up and post-silicon validation. Speed rises and observability falls at each step.

**169. Roughly how slow is RTL simulation?**
Six or more orders of magnitude slower than real hardware — think a few hertz to kilohertz of simulated clock. That single fact drives everything about how stimulus is designed.

**170. Why not just run real applications pre-silicon?**
They're far too slow at simulation speeds, their coverage is accidental, they can't be aimed at a specific new feature, and when they fail they can't easily be shrunk to a debuggable case. Real apps become useful at emulation and bring-up where you have the speed.

**171. Directed vs constrained-random testing?**
Directed tests target a specific known behavior — precise, readable, and limited to what you thought of. Constrained-random generates within legality constraints and finds bugs nobody anticipated, but needs coverage measurement to know what it hit and minimization to be debuggable.

**172. What is functional coverage vs code coverage?**
Code coverage measures which RTL lines/branches/toggles executed — necessary but weak. Functional coverage measures whether *architecturally meaningful scenarios* occurred (this instruction with that operand type while that engine was busy). You can have 100% code coverage and have never tested the interesting interaction.

**173. What is coverage closure?**
Driving functional coverage to the target by analyzing holes, then writing directed tests or tightening random constraints to hit them. It's the main loop of a verification schedule.

**174. What is a scoreboard?**
A checker that independently predicts the expected result of each transaction and compares it against what the design produced, decoupled from the stimulus itself.

**175. Five things you must capture to reproduce a random test.**
The master seed, the tool version (git SHA), the ISA/config tables version, the full configuration and command line, and the backend and its version. One command must rebuild the exact failing test from those.

**176. Name sources of nondeterminism to eliminate.**
Wall-clock time, pointer or address values, uninitialized memory, hash-map iteration order, per-process hash randomization, thread completion order, filesystem directory order, and uncaptured environment variables.

**177. How do you seed sub-components without coupling them?**
Derive each component's seed from a hash of the master seed and a stable component identifier. Then adding a new randomized component doesn't perturb the random stream consumed by existing ones, so your regression baseline doesn't shift.

**178. What is test minimization and why does it matter?**
Automatically shrinking a failing test to the smallest case that still fails — delta debugging. It matters because a 100,000-instruction random failure is unfileable, and a 6-instruction one is a bug report. Having the workload in a structured IR is what makes shrinking tractable.

**179. How do you tell whether a mismatch is a hardware bug, a model bug, or a bad test?**
Cross-check independent implementations: run the same stimulus against the functional model and the RTL. Also validate that the stimulus itself is architecturally *legal* — a surprising fraction of "hardware bugs" are illegal stimulus, and shipping those destroys the verification team's trust in your generator.

**180. Why should the generator validate its own output?**
Because an illegal stimulus produces undefined hardware behavior, so any mismatch is meaningless. Every false bug filed costs an engineer a day and costs your tool credibility.

**181. Table-driven vs hand-written decoder?**
Table-driven scales, stays in sync with the encoder because they share the table, and turns ISA changes into data changes. Hand-written switch code is faster to write for a tiny ISA and easier to special-case, but drifts and rots. For anything real, table-driven, with a decision tree over discriminating bits if lookup speed matters.

**182. Linear sweep vs recursive descent disassembly?**
Linear sweep decodes sequentially from the start — complete coverage, but it misinterprets data embedded in code and can desynchronize on variable-length ISAs. Recursive descent follows control flow from known entry points — accurate, but misses code reachable only via indirect branches. Real tools do both and flag disagreements, because disagreement is exactly where data-in-code lives.

**183. What's the robustness contract for a disassembler?**
Never crash, never infinite-loop, always make forward progress, and emit an explicit `.unknown` for undecodable words rather than aborting. It will be pointed at corrupt and adversarial input constantly.

**184. How do you reconstruct a CFG?**
Identify leaders — the entry point, every branch target, and every instruction following a branch. Cut basic blocks at leaders, add edges for branch targets and fallthroughs, mark indirect branch targets as unknown rather than guessing, and find loops via back edges in a DFS.

**185. How do you test a disassembler with no reference implementation?**
Round-trip properties (encode→decode→compare) with property-based random generation, "every byte accounted for" on real binaries, no instruction decoding as unknown, a well-formed CFG with no dangling edges, and fuzzing for robustness. Differential testing against a second independent implementation if one exists.

**186. How does debugging a GPU differ from debugging a CPU?**
Massive parallelism means you're inspecting thousands of threads, so the tooling must let you select a warp/lane and reason about divergence rather than showing "the" program counter. Stepping semantics apply to a warp, breakpoints must handle many threads hitting simultaneously, and much of the state is architecturally hidden.

**187. What goes into designing a trace format?**
Binary with a magic number and version, a self-describing schema block, fixed-size record headers with variable payloads so you can skip without parsing, explicit endianness and field widths, per-block rather than per-record compression to keep random access, and a sidecar index for repeat queries. And it must be readable by a short script, or nobody will analyze traces.

**188. Analytical vs cycle-accurate models — when do you use each?**
Analytical models are seconds to run and answer "roughly what if" questions during architecture exploration — great for direction, poor for precision. Cycle-accurate models are far slower but predict real behavior and can be correlated against RTL. You use analytical early and broadly, cycle-accurate for decisions that need confidence.

**189. What does "shift left" mean?**
Moving verification and software enablement earlier — finding bugs at model or simulation time rather than at silicon. It's motivated by cost: a bug found post-silicon can mean a respin costing millions and months.

**190. How do you handle a flaky test in a regression suite?**
Treat it as a real bug, not noise, because in this domain flakiness usually means either a genuine hardware/model race or nondeterminism in your infrastructure. Quarantine it so it stops blocking others, but track it and root-cause it. Never just re-run until green.

**191. How would you find which commit broke the nightly?**
`git bisect run` with a script that reproduces the failure and returns an exit code — log₂(n) builds instead of n. Prerequisites: the reproduction must be reliable and reasonably fast, so minimize it first if it takes hours.

---

## Section 8 — Scripting & Infra (M8)

**192. Why Python for tool infrastructure?**
The runtime is dominated by the simulator, so harness speed is irrelevant, while requirements change weekly. Python has exactly the right batteries (argparse, struct, subprocess, sqlite3) and everyone on a chip team can read and patch it.

**193. How do you parse a 20 GB log?**
Stream it with a generator yielding one record at a time — constant memory. For binary formats, mmap and `unpack_from` over a memoryview to avoid copies. Build a sidecar index if you'll query it repeatedly.

**194. Why must a binary format specify `<` or `>` in a struct format string?**
The default `@` uses native byte order *and native alignment padding*, so the same script reads the file differently on different hosts or compilers. In a verification flow that's a nondeterminism bug that's very hard to trace.

**195. Why never use `shell=True`?**
It hands the string to a shell, so metacharacters in interpolated values change the command's meaning — a command injection and quoting hazard. Pass a list and it goes straight to exec.

**196. What must you always do when launching a simulator from a script?**
Set a timeout, kill the whole process group on timeout (not just the child, or you leak simulator processes onto the farm), capture stdout and stderr separately, and log the exact copy-pasteable command line.

**197. What does pybind11 solve, and what are its hazards?**
It exposes real C++ classes to Python with type safety. Hazards: object lifetime across the boundary (a wrong return-value policy gives a use-after-free that looks like an interpreter crash), and the GIL — long C++ calls must release it or they stall all Python threads.

**198. PUBLIC vs PRIVATE vs INTERFACE in CMake?**
PRIVATE: needed to build me, not propagated. INTERFACE: not needed by me, propagated to consumers. PUBLIC: both. Marking everything PUBLIC leaks every include path and macro through the whole build and turns implementation details into de facto public API.

**199. What does `-Xptxas=-v` tell you?**
Registers, shared memory and spill stores/loads per kernel at build time — the cheapest way to notice register pressure without opening a profiler.

**200. How do you make a Python tool testable?**
Separate pure logic from I/O: parsing takes an iterable of lines, not a filename; subprocess calls sit behind a substitutable interface; and `main(argv)` returns an exit code rather than calling `sys.exit`. Then most tests run in milliseconds with no simulator.

---

## Section 9 — Design & Behavioral (M9, M0)

**201. First thing you do in a design round?**
Ask clarifying questions for the first several minutes — users, lifetime, what changes most often, execution target, scale, determinism requirements, failure workflow, and what's explicitly out of scope. Designing before asking is the most common way to fail this round.

**202. How do you design for a spec that keeps changing?**
Separate what from how — an architecture-neutral IR with per-architecture back ends — and make the frequently-changing part data rather than code, so an ISA revision is a table edit. Use registries so new things are added, not edited in.

**203. Why is an IR the centre of a stimulus framework?**
It's the contract: front ends produce it, back ends consume it, passes transform it, analysis reads it. Without it you get an N×M matrix of front ends against targets; with it you get N+M. It's also what makes automatic test minimization possible.

**204. Why must the encoder and disassembler share one table?**
Otherwise they drift, and you spend days chasing a "hardware bug" that is actually your disassembler misreading a field. Sharing the table also gives you round-trip testing essentially for free.

**205. How do you keep hundreds of existing tests working while the framework evolves?**
Version the public interface and keep it narrow; put churn behind the abstraction rather than in it. Use deprecation cycles rather than breaking changes, and maintain a regression suite of existing tests that runs on every framework change — the framework is software and needs its own verification.

**206. How do you test the checker itself?**
Inject a known bug into the reference model or the stimulus and assert the framework *catches* it. Otherwise the checking path is the one piece of the system that's never exercised in the failing direction, and a checker that silently passes everything is worse than no checker.

**207. What makes a good behavioral debugging story here?**
Method, not heroics: how you made it reproducible, how you quantified it, what you ruled out and how, which tool you used or wrote, and what you changed about your process afterwards. Bonus if it involves a race, a memory bug, or a nondeterminism hunt.

**208. What questions should you ask the interviewer?**
What's hardest to change in the current framework; how much of the architecture description is data-driven vs hand-written; how determinism is maintained across generator, model and RTL; what triage looks like when a random test fails overnight; and what bug the infrastructure caught that would have been expensive to find later.

---

## Scoring and next steps

| Result | What it means | Action |
|---|---|---|
| < 60% clean | Foundations aren't set | Return to the modules; don't drill |
| 60–80% clean | Knowledge is there, articulation isn't | Drill out loud daily; this is the fastest-improving zone |
| 80–95% clean | Interview-ready on knowledge | Shift effort to design rounds and behavioral stories |
| > 95% clean | Ready | Maintain with weekly passes; spend time on projects instead |

Remember the rule from the README: **you must survive two levels of follow-up.** This
bank is level one. The module files hold the level-two answers — when a question here
feels easy, open the module and check that you can also answer the follow-up.
