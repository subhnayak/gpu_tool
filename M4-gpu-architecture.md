# Module M4: GPU Hardware Architecture

## Why This Matters for This Role

If you're interviewing for "System Software Engineer, GPU Development Tools" at NVIDIA, you're expected to understand the hardware at a level that lets you write, debug, and verify the software that *exercises* that hardware. Every tool — profilers, debuggers, compilers, verification suites — generates workloads that must stress specific hardware structures: warp schedulers, memory controllers, tensor cores, cache hierarchies. You cannot build good stimulus or interpret results if the hardware is a black box.

This module builds the mental model. It's the foundation for everything in CUDA programming (M5), graphics pipelines (M6), and verification methodology (M7). When you walk into that interview, you need to be able to draw an SM on a whiteboard, trace a memory access through the hierarchy, explain warp divergence with numbers, and reason about whether a kernel is bandwidth-bound or compute-bound.

---

## 1. Why GPUs Exist: Throughput vs Latency Machines

### The Core Trade-off

A CPU is a **latency machine**. It's optimized to finish *one* thread of work as fast as possible. To do that, it spends transistors on:

- **Branch prediction** (deep speculative execution pipelines)
- **Out-of-order execution** (reorder buffer, reservation stations)
- **Big caches** (multi-MB L1/L2/L3 per core)
- **Low-latency memory** paths

A GPU is a **throughput machine**. It's optimized to finish *millions of tasks* as fast as possible *in aggregate*, even if each individual task takes longer. It spends transistors on:

- **Many simple cores** (thousands of ALUs)
- **Small caches** per core
- **Massive register files** (to support thousands of concurrent threads)
- **Latency hiding via multithreading** (when one warp stalls on memory, another fires)

```
CPU vs GPU — transistor budget (conceptual)
┌──────────────────────────────────────────┐
│  CPU Core                                │
│  ┌────────────┐ ┌──────────────────────┐ │
│  │  ALU       │ │  Branch Predictor    │ │
│  │  (small)   │ │  OoO Engine          │ │
│  └────────────┘ │  Reorder Buffer      │ │
│  ┌────────────┐ │  Reservation Station │ │
│  │ L1 Cache   │ └──────────────────────┘ │
│  │ (32-64KB)  │ ┌──────────────────────┐ │
│  └────────────┘ │  L2 Cache (256KB-1MB)│ │
│  ┌────────────┐ └──────────────────────┘ │
│  │ L3 (shared)│                          │
│  │ (8-64MB)   │                          │
│  └────────────┘                          │
└──────────────────────────────────────────┘

┌──────────────────────────────────────────┐
│  GPU SM (Streaming Multiprocessor)       │
│  ┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐      │
│  │AL││AL││AL││AL││AL││AL││AL││AL│ ...x64│
│  └──┘└──┘└──┘└──┘└──┘└──┘└──┘└──┘      │
│  ┌──────────────────────────────┐        │
│  │  Register File (256KB)       │        │
│  └──────────────────────────────┘        │
│  ┌──────────────────────────────┐        │
│  │  Shared Mem / L1 (128-228KB)│        │
│  └──────────────────────────────┘        │
│  Tiny control logic, no OoO, no branch  │
│  predictor (just run another warp)      │
└──────────────────────────────────────────┘
```

### Amdahl vs Gustafson

**Amdahl's Law** says speedup is limited by the serial fraction. If 10% of your code is serial, maximum speedup = 10×, no matter how many cores you throw at it.

**Gustafson's Law** is the GPU engineer's rebuttal: in practice, when you get more cores, you increase the *problem size* (more pixels, more particles, bigger models). The serial fraction *shrinks as a proportion* of the total work. GPUs win because real workloads scale.

**When a GPU is the wrong tool:**
- Highly serial, branch-heavy code (tree traversal, recursive algorithms)
- Very small problem sizes (launch overhead dominates)
- Workloads that require low *latency* for a single result (real-time control loops)
- Irregular memory access patterns with no spatial/temporal locality
- When synchronization between tasks dominates compute

### Interview Q&A

**Q: Why can't you just make a CPU with thousands of cores instead of using a GPU?**
A: You could, and some designs try (many-core Xeon Phi was an attempt). But each CPU core carries a huge transistor tax — branch predictors, reorder buffers, big caches — that exist to make *one thread* fast. A GPU strips that away and uses those transistors for more ALUs and a bigger register file. The key insight is that GPU workloads have enough parallelism to hide memory latency via thread switching rather than caches and speculation. The programming model (SIMT) and hardware are co-designed for this. A CPU with 1000 full cores would be enormous and power-hungry for the same throughput.

*Follow-up: When does Amdahl's Law actually kill GPU performance in practice?* — When the kernel has a serial bottleneck like a single-thread reduction, or when the problem is so small that kernel launch overhead (5-20 µs) dominates. Also when the serial host-side fraction (data transfer, setup) can't be overlapped with GPU work.

**Q: What does "latency hiding by multithreading" mean concretely?**
A: A GPU memory access to DRAM takes ~400-800 cycles. Instead of stalling, the warp scheduler switches to another warp that's ready to execute. If you have enough warps in flight (sufficient occupancy), the pipeline stays busy. You need enough warps to cover the memory latency — roughly latency × throughput warps. For example, if memory latency is 400 cycles and you can issue one warp per cycle, you need ~400 warps in flight (across the whole chip) to saturate bandwidth.

*Follow-up: How is this different from SMT on a CPU (e.g., Hyper-Threading)?* — SMT on a CPU runs 2 threads sharing one core's resources. GPU multithreading runs 32-64 warps per SM, each with its own registers, and the hardware switches between them *every cycle* at zero cost. The register file is the key enabler — it's partitioned, not context-switched.

**Q: Give a concrete example of when you'd tell someone NOT to use a GPU.**
A: Linked-list traversal. Each node access depends on the previous pointer, so you can't parallelize across nodes. The access pattern is random (no coalescing), and branch prediction for "is next null?" is useless since every thread's list is different. A single CPU core with a big cache and prefetcher will beat a GPU. Another example: small matrix multiply (say 8×8) — the kernel launch overhead alone exceeds the compute time.

*Follow-up: What about graph algorithms — those have irregular access patterns but people still GPU-accelerate them?* — True, but they require specialized approaches: BFS with frontier expansion, push-pull direction optimization. The GPU wins only when the frontier is large enough to create massive parallelism. For sparse, low-diameter graphs, the GPU often underperforms.

**Q: Explain Gustafson's Law in the context of GPU rendering.**
A: If you're rendering a 1080p frame, some work is serial (scene setup). At 1080p, you have ~2M pixels of parallel work. Going to 4K gives you ~8M pixels. The serial fraction (scene setup) stays roughly fixed, but total work quadrupled. So the percentage that's parallel went up, and Gustafson says your speedup scales with problem size. This is exactly why GPUs shine in rendering — resolution increases mean more parallelism, not more serial work.

*Follow-up: Name a GPU workload where the serial fraction actually grows with problem size.* — Global synchronization-heavy algorithms like certain iterative solvers where each iteration requires a global reduction (barrier + sum). More data means a larger reduction tree with a serial tail. Also prefix-scan where the up-sweep/down-sweep has log(N) serial steps.

**Q: How does GPU latency hiding compare quantitatively to CPU cache hierarchies?**
A: A CPU hides a 100-cycle L3 miss with speculation and out-of-order execution — it can look ahead ~200 instructions in the instruction window. That works for 1-2 outstanding misses. A GPU has no speculation but runs 64 warps per SM. If each warp has one outstanding load, that's 64 independent memory requests. At 400-cycle DRAM latency and one instruction per cycle throughput, 64 warps is theoretically insufficient — but each warp can also have multiple independent loads in flight (ILP). With 2-4 loads per warp, you get 128-256 outstanding requests per SM, which approaches what's needed. CPUs trade transistors for "look deeper into one thread"; GPUs trade transistors for "run more threads." Neither is strictly better — they target different workload shapes.

*Follow-up: Why can't GPUs just add out-of-order execution?* — They can in principle, but OoO requires tracking dependencies per instruction in a large reorder buffer, which costs area and power per thread. Multiply that by 2048 threads per SM and it's infeasible. The GPU's design bet is that there are always enough independent threads to run; OoO is the CPU's design bet that one thread has enough ILP to find.

**Q: What is the fundamental programming model difference that enables GPU throughput?**
A: The GPU programming model (CUDA's SIMT) assumes you'll launch thousands to millions of threads that execute *the same function* on different data. This allows the hardware to amortize instruction fetch/decode across 32 threads (a warp) and to organize memory accesses for coalescing. CPUs assume threads are doing *different things* and optimize accordingly with branch prediction and OoO. The "same program, different data" assumption is what lets GPUs trade per-thread complexity for thread count.

*Follow-up: Is it strictly SPMD? Can GPU threads do completely different things?* — Yes, they can branch independently (especially post-Volta). But divergence within a warp serializes execution, so performance degrades toward 1/32 throughput in the worst case. The programming model *permits* divergence but *rewards* uniformity.

---

## 2. SIMT Execution Model

### Threads, Warps, and Blocks

In CUDA, you launch a **grid** of **thread blocks** (also called CTAs — Cooperative Thread Arrays). Each block contains up to 1024 threads (architecture-dependent). The hardware groups threads into **warps** of 32 consecutive threads.

```
Grid
├── Block (0,0)
│   ├── Warp 0:  threads  0-31
│   ├── Warp 1:  threads 32-63
│   ├── Warp 2:  threads 64-95
│   └── ...
├── Block (1,0)
│   ├── Warp 0:  threads  0-31
│   └── ...
└── ...
```

The warp is the fundamental scheduling unit. All 32 threads in a warp execute the same instruction at the same time (on the same clock cycle), but each thread operates on its own data and registers.

### SIMT vs SIMD vs SMT — The Precise Differences

| Property | SIMD (e.g., AVX-512) | SIMT (NVIDIA GPU) | SMT (e.g., Hyper-Threading) |
|---|---|---|---|
| **Instruction stream** | 1 instruction, explicit vector width | 1 instruction, 32 threads execute it | Multiple independent instruction streams |
| **Programmer visibility** | Must explicitly vectorize | Writes scalar code per-thread | Writes independent thread code |
| **Divergence** | N/A (no branching per lane) | Threads can branch independently; HW masks inactive threads | Each thread has its own PC |
| **Register file** | Vector registers shared | Each thread has private scalar registers | Shared/partitioned registers |
| **Key distinction** | Width is in the ISA | Width is in the hardware; ISA is scalar per-thread | True independence |

**SIMT is NOT "just SIMD."** The critical difference: in SIMD, the programmer sees and manages vector width explicitly. In SIMT, the programmer writes scalar per-thread code, and the hardware *happens* to execute 32 threads in lockstep. This means SIMT can handle divergence (inefficiently), while SIMD cannot — a divergent branch in SIMD is a programming error.

### Warp Scheduling and Instruction Issue

Each SM has multiple **warp schedulers** (2 per SM partition on Ampere, 4 total per SM). Each cycle, each warp scheduler selects an eligible warp and issues one or more instructions from it.

An eligible warp is one where:
- Its next instruction's operands are ready
- The required functional unit is available
- It's not waiting on a memory operation or barrier

```
Warp Scheduler (simplified, 1 scheduler of 4 in an Ampere SM)
┌─────────────────────────────────┐
│  Scoreboard: tracks operand     │
│  readiness for each warp        │
│                                 │
│  Warp 0: READY ──→ Issue FP32  │
│  Warp 1: STALL (mem)           │
│  Warp 2: STALL (dep)           │
│  Warp 3: READY ──→ Issue LD    │
│  Warp 4: READY (not selected)  │
│  ...                            │
│  Each cycle: pick one ready     │
│  warp, issue its instruction    │
└─────────────────────────────────┘
```

### Warp Divergence and Reconvergence

When threads in a warp take different paths at a branch:

```c
if (threadIdx.x < 16) {
    A();  // threads 0-15
} else {
    B();  // threads 16-31
}
C();      // all threads
```

```
Warp execution timeline (pre-Volta):

Cycle:  1    2    3    4    5    6    7
        ├────┤    ├────┤    ├────┤
        │ A  │    │ B  │    │ C  │
        │t0-15│   │t16-31│  │all │
        │active│  │active│  │    │

Active mask during A: 0x0000FFFF  (threads 0-15)
Active mask during B: 0xFFFF0000  (threads 16-31)
Active mask during C: 0xFFFFFFFF  (all)

Throughput penalty: 2× for the divergent section
```

**Predication:** For short if/else blocks, the compiler may use predicated execution instead of branching. Both paths execute, but results are only written for threads where the predicate is true. No divergence penalty, but both paths always execute.

### Pre-Volta vs Volta+ Reconvergence

**Pre-Volta (stack-based reconvergence):**
- Hardware maintains a reconvergence stack
- At a divergent branch, pushes the reconvergence point (the post-dominator)
- Executes one path, then the other, then pops and reconverges
- **Problem:** Threads in the same warp on different paths *cannot* communicate. If thread 0 (path A) tries to `__shfl` with thread 16 (path B), deadlock or undefined behavior results because they're never simultaneously active.

**Volta+ (independent thread scheduling):**
- Each thread has its own **program counter** and **call stack**
- The scheduler can interleave instructions from different paths within the same warp
- Threads can be at different points in the program and still cooperate
- **Consequence:** Old warp-synchronous code that assumed lockstep execution within a warp is now broken. `__syncwarp()` was introduced to create explicit reconvergence points.

```
Pre-Volta divergence (stack-based):
     ┌─── if ───┐
     │          │
   Path A    Path B
   (0-15)    (16-31)
     │          │
     └─── reconverge (post-dominator) ───┘
     Strict serialization: all of A, then all of B

Volta+ divergence (independent thread scheduling):
     ┌─── if ───┐
     │          │
   Path A    Path B
   (0-15)    (16-31)
     │    ↔    │   ← threads can interleave!
     └─── __syncwarp() ───┘
     Interleaved execution possible; explicit sync needed
```

### Why __syncwarp() Became Necessary

Classic warp-synchronous code pattern (now broken on Volta+):

```c
// BROKEN on Volta+: assumes lockstep after branch
if (threadIdx.x % 2 == 0) {
    shared[threadIdx.x] = compute_even();
}
// No __syncwarp() here!
// Odd threads read shared[threadIdx.x - 1] — may not be written yet on Volta+
val = shared[threadIdx.x - 1];
```

Fixed version:

```c
if (threadIdx.x % 2 == 0) {
    shared[threadIdx.x] = compute_even();
}
__syncwarp();  // ensure all threads have reconverged
val = shared[threadIdx.x - 1];
```

### Interview Q&A

**Q: Explain the difference between SIMT and SIMD. Why does it matter?**
A: SIMD is explicit in the ISA — you write vector instructions that operate on, say, 8 floats at once, and the programmer manages the vector width. SIMT is transparent — you write scalar code per thread, and the hardware groups 32 threads into a warp that executes in lockstep. The key consequence is that SIMT can handle divergence: if threads branch differently, the hardware masks inactive threads and serializes the paths. In SIMD, you can't meaningfully branch per lane. This matters for tools engineers because you need to understand that SIMT divergence is a performance issue, not a correctness issue (post-Volta), and your verification tools need to test divergent code paths.

*Follow-up: If SIMT handles divergence automatically, why should a programmer care?* — Because divergence kills throughput. A warp with 2 divergent paths runs at 50% efficiency. With 32-way divergence (each thread on its own path), you're at 1/32 = 3.1% efficiency. For verification, you need to create stimuli that exercise worst-case divergence patterns to stress the warp scheduler.

**Q: What changed in Volta's thread scheduling model and why?**
A: Pre-Volta used a stack-based reconvergence model where threads on different paths of a branch were strictly serialized — all threads on path A execute, then all on path B, then reconverge. Volta introduced independent thread scheduling: each thread has its own program counter, and the scheduler can interleave instructions from different paths. This was needed to support cooperative patterns between diverged threads (like producer-consumer within a warp) and to enable starvation-free algorithms. The cost is that old warp-synchronous code that assumed lockstep behavior broke, requiring explicit `__syncwarp()` calls.

*Follow-up: Can you give a concrete example of code that worked pre-Volta but breaks on Volta?* — Warp-level reduction without __syncwarp. Pre-Volta, after each shuffle step, you could assume all threads had completed the shuffle. On Volta, a thread might still be executing the shuffle while another thread proceeds to read the result. Without __syncwarp(), you get a data race.

**Q: What is the active mask and how does it relate to predication?**
A: The active mask is a 32-bit value where each bit indicates whether the corresponding thread in a warp is active for the current instruction. During divergence, inactive threads are masked off — their functional units are idle. Predication is a related but different mechanism: the compiler uses predicate registers to conditionally write results. With predication, both paths execute for all threads, but only the appropriate results are committed. Predication avoids the overhead of branch divergence for short if/else blocks but wastes compute on the untaken path.

*Follow-up: How would a verification engineer test active mask behavior?* — Create test kernels with every possible divergence pattern: 1 thread vs 31, alternating threads, nested divergence, divergence inside loops with varying trip counts. Verify that masked-off threads don't corrupt the register file or shared memory, and that the reconvergence point is correct.

**Q: What does "warp-synchronous programming" mean, and why is it now considered dangerous?**
A: Warp-synchronous programming means writing code that relies on all 32 threads in a warp executing in lockstep without explicit synchronization. For example, using `__shfl` and assuming the result is immediately visible to all threads, or writing to shared memory and assuming other threads in the same warp can read it without `__syncwarp()`. Pre-Volta, this worked because the hardware enforced lockstep. Post-Volta, with independent thread scheduling, threads can progress independently, so these assumptions can cause data races. NVIDIA now mandates explicit synchronization via `__syncwarp()` or cooperative groups.

*Follow-up: Is there any case where warp-synchronous code is still safe on Volta+?* — Within a single straight-line basic block with no branches, threads are still executing the same instruction. But the moment you have any control flow, including function calls that might diverge internally, you need explicit sync. Best practice is to always use `__syncwarp()` — the cost is negligible and correctness is guaranteed.

**Q: Walk through what happens cycle-by-cycle when a warp hits a divergent branch with 3 paths.**
A: Suppose threads 0-10 take path A, threads 11-20 take path B, and threads 21-31 take path C. Pre-Volta: the hardware pushes three entries onto the reconvergence stack. It executes path A with an 11-bit active mask, then path B with a 10-bit mask, then path C with an 11-bit mask, then reconverges. Total cost: sum of all three path lengths — worst case is 3× a single path. On Volta+, the scheduler *may* interleave instructions from different paths. For instance, if path A has a long-latency load, the scheduler might switch to path B's instructions while path A waits. But eventually all threads must reach the reconvergence point (explicit `__syncwarp()` or the post-dominator). The key difference is Volta+ can expose more parallelism within the diverged warp.

*Follow-up: Does 3-way divergence cost 3× or less?* — Pre-Volta, it costs exactly the sum of the three paths (sequential execution). On Volta+, it can cost less if the scheduler overlaps independent instructions from different paths, or if one path stalls on memory while another computes. In practice, the improvement depends on the specific instruction mix and latencies.

**Q: How many warps can be in-flight simultaneously on one SM?**
A: It depends on the architecture. On Ampere (GA100), each SM supports up to 64 warps (2048 threads). On Hopper (GH100), it's also 64 warps per SM. The actual number of active warps depends on resource usage — registers per thread, shared memory per block, and block size. The ratio of active warps to maximum warps is *occupancy*. For example, if your kernel uses 64 registers per thread, each warp needs 64×32 = 2048 registers. An Ampere SM has 65,536 registers, so max warps = 65536/2048 = 32, giving 50% occupancy.

*Follow-up: Does higher occupancy always mean better performance?* — No! Volkov showed that low occupancy with high ILP (instruction-level parallelism) can match or exceed high-occupancy kernels. More warps mean more register pressure, which can cause spills to local memory. The key is having enough parallelism (warps or ILP or both) to hide latency.

---

## 3. SM Internals

### SM Block Diagram (Ampere GA100)

```
SM (Streaming Multiprocessor) — Ampere GA100
┌──────────────────────────────────────────────────────────────────┐
│  Instruction Cache (128KB)                                       │
│  ┌───────────────────┐  ┌───────────────────┐                    │
│  │  SM Partition 0   │  │  SM Partition 1   │                    │
│  │ ┌───────────────┐ │  │ ┌───────────────┐ │                    │
│  │ │Warp Sched. ×1 │ │  │ │Warp Sched. ×1 │ │                    │
│  │ │Dispatch  ×1   │ │  │ │Dispatch  ×1   │ │                    │
│  │ ├───────────────┤ │  │ ├───────────────┤ │                    │
│  │ │ 16 FP32 cores │ │  │ │ 16 FP32 cores │ │                    │
│  │ │ 16 INT32 cores│ │  │ │ 16 INT32 cores│ │                    │
│  │ │  8 FP64 cores │ │  │ │  8 FP64 cores │ │                    │
│  │ │  1 Tensor Core│ │  │ │  1 Tensor Core│ │                    │
│  │ │  4 LSU (ld/st)│ │  │ │  4 LSU (ld/st)│ │                    │
│  │ │  4 SFU        │ │  │ │  4 SFU        │ │                    │
│  │ ├───────────────┤ │  │ ├───────────────┤ │                    │
│  │ │Register File  │ │  │ │Register File  │ │                    │
│  │ │  16384 × 32b  │ │  │ │  16384 × 32b  │ │                    │
│  │ └───────────────┘ │  │ └───────────────┘ │                    │
│  └───────────────────┘  └───────────────────┘                    │
│  ┌───────────────────┐  ┌───────────────────┐                    │
│  │  SM Partition 2   │  │  SM Partition 3   │                    │
│  │  (same as above)  │  │  (same as above)  │                    │
│  └───────────────────┘  └───────────────────┘                    │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │  Shared Memory / L1 Data Cache (configurable, 192KB)     │    │
│  │  (unified: shared mem + L1 split configurable)           │    │
│  └──────────────────────────────────────────────────────────┘    │
│  ┌────────────────────┐  ┌────────────────────┐                  │
│  │  Texture units ×4  │  │  RT Core ×1 (Turing+)│               │
│  └────────────────────┘  └────────────────────┘                  │
└──────────────────────────────────────────────────────────────────┘
```

*Note: Numbers are for GA100 (Ampere data-center). Consumer GPUs (GA102) differ slightly. SM partition count and per-partition resources vary by generation.*

### Instruction Issue and Dual-Issue

On Turing and Ampere, each SM partition can **concurrently issue FP32 and INT32** instructions from the *same* warp in the *same* cycle. Pre-Turing, a warp could issue either FP32 or INT32 but not both.

Example: if your kernel interleaves float math and integer address calculations:
```c
float val = a[i] * b[i];    // FP32 pipe
int next = base + stride*i;  // INT32 pipe — runs in parallel with above
```
Both instructions can issue in the same cycle. This effectively doubles throughput for mixed FP/INT code. However, if your code is pure FP32, the INT32 cores sit idle (they cannot do FP32 on Turing; on Ampere GA102, the second datapath CAN do FP32, giving 2× FP32 cores).

### Warp Scheduler Details

Each SM partition's warp scheduler maintains a **scoreboard** — a table tracking, for each warp, which registers are pending (waiting on a long-latency operation like a memory load). Each cycle:

1. The scheduler scans its assigned warps (up to 16 on Ampere) for eligible ones
2. An eligible warp has: all source operands ready (no scoreboard hazards), the target functional unit free, and no pending barrier
3. The scheduler picks one warp (selection policy is implementation-specific — likely oldest-first or round-robin)
4. Issues 1-2 instructions from that warp to the functional units
5. If a dual-issue pair is possible (FP32 + INT32), both are issued

The zero-cost context switch between warps is enabled by the fact that each warp's registers are statically allocated in the register file — there's nothing to save or restore.

### Functional Units

| Unit | What it does | Throughput (per SM partition per cycle, Ampere GA100) |
|---|---|---|
| **FP32 CUDA cores** | Single-precision float add/mul/FMA | 16 ops |
| **INT32 cores** | Integer arithmetic | 16 ops |
| **FP64 cores** | Double-precision float (half-rate on GA100) | 8 ops |
| **Tensor Core** | Matrix multiply-accumulate (MMA): D = A×B + C | 1 MMA op per cycle, but operates on a tile (e.g., 4×4×4 FP16→FP32) |
| **SFU** | sin, cos, rsqrt, log2, exp2 | 4 ops (multi-cycle) |
| **LSU** | Load/store to memory | 4 addresses per cycle |

### Tensor Cores: What They Compute

Tensor cores perform matrix-multiply-accumulate (MMA) on small tiles:

```
D[m×n] = A[m×k] × B[k×n] + C[m×n]
```

Supported shapes and precisions vary by generation:
- **Volta/Turing:** 4×4×4 FP16→FP32
- **Ampere:** 4×4×4 FP16, BF16, TF32, INT8, INT4, binary; also 8×8×4 TF32
- **Hopper:** FP8 (E4M3, E5M2), plus all prior types

**Why MMA shapes matter:** The warp-level MMA API (wmma) requires specific tile sizes (e.g., 16×16×16 for FP16 on Ampere). If your matrix dimensions don't align, you need padding, which wastes compute. For verification, you need to test all supported shapes and precision combinations.

### The Register File

The register file is the largest on-chip storage. On Ampere GA100:
- **65,536 × 32-bit registers per SM** (256 KB per SM)
- 16,384 registers per SM partition
- Banked for multi-port access — typically 4 banks with 2 read + 1 write ports

Registers are **partitioned** across warps, not shared. Each thread gets a fixed allocation (determined at compile time, max 255 registers per thread on modern architectures).

### Occupancy Math — Worked Example

**Given (Ampere GA100 SM):**
- Max threads per SM: 2048 (= 64 warps)
- Max blocks per SM: 32
- Max shared memory per SM: 164 KB (configurable, up to 164KB for shared on GA100)
- Registers per SM: 65,536

**Your kernel:**
- Block size: 256 threads (= 8 warps per block)
- Registers per thread: 48
- Shared memory per block: 8 KB

**Step 1: Register limit**
Registers per warp = 48 × 32 = 1,536
Warps fitting in register file = floor(65,536 / 1,536) = 42 warps
But max is 64, so register limit → 42 warps

**Step 2: Shared memory limit**
Blocks fitting in shared memory = floor(164 KB / 8 KB) = 20 blocks
Warps from shared memory = 20 blocks × 8 warps/block = 160 warps
Max is 64, so shared memory is not the bottleneck.

**Step 3: Block limit**
Max 32 blocks per SM, each block = 8 warps → 32 × 8 = 256 warps
Not the bottleneck.

**Step 4: Take the minimum**
Active warps = min(42, 64, 160, 256) = 42
But active warps must be a multiple of warps-per-block (8):
floor(42 / 8) × 8 = 5 × 8 = 40 warps = 5 blocks

**Occupancy = 40 / 64 = 62.5%**

**What if we reduce to 32 registers per thread?**
Registers per warp = 32 × 32 = 1,024
Warps = floor(65,536 / 1,024) = 64 warps → 100% occupancy
But the compiler might generate spills to local memory (slow DRAM access), so this isn't necessarily better.

### Occupancy Worked Example 2: Shared Memory Bottleneck

**Your kernel:**
- Block size: 128 threads (= 4 warps per block)
- Registers per thread: 32
- Shared memory per block: 48 KB

**Register limit:** 32 × 32 = 1,024 regs/warp. floor(65,536 / 1,024) = 64 warps. Not bottleneck.

**Shared memory limit:** floor(164 KB / 48 KB) = 3 blocks. 3 blocks × 4 warps = 12 warps.

**Block limit:** 32 blocks × 4 warps = 128. Not bottleneck.

**Active warps = 12. Occupancy = 12 / 64 = 18.75%**

This is severely shared-memory-limited. Options:
1. Reduce shared memory usage (smaller tiles, staged loading)
2. Increase block size (amortize shared memory over more warps)
3. Accept low occupancy and ensure high ILP per thread

### Interview Q&A

**Q: Walk me through the SM block diagram of a modern GPU.**
A: An SM is divided into partitions — 4 on Ampere. Each partition has its own warp scheduler, dispatch unit, a set of FP32 cores (16), INT32 cores (16), FP64 cores (8), a tensor core, SFUs, and load/store units. They share a large register file (16K registers per partition) and a unified shared memory/L1 cache (up to ~192KB on Ampere). The instruction cache sits at the top. Each cycle, each warp scheduler picks an eligible warp and issues its instruction to the appropriate functional unit. There are also texture units shared across the SM and, on Turing+, an RT core.

*Follow-up: Why partition the SM instead of having one big scheduler?* — Partitioning reduces the complexity of the warp scheduler. Scheduling 64 warps is hard; scheduling 16 warps per partition is easier. Each partition's scheduler only looks at its own warps. It also allows different partitions to execute different instructions in the same cycle, increasing throughput.

**Q: Do occupancy calculations for a kernel using 96 registers/thread, 256 threads/block, and 16KB shared memory on an Ampere SM.**
A: Registers per warp: 96 × 32 = 3,072. Warps from register budget: floor(65,536 / 3,072) = 21. Shared memory: floor(164 / 16) = 10 blocks × 8 warps/block = 80 warps. Block limit: 32 blocks × 8 warps = 256. Minimum is 21 warps. Rounding down to block-multiple of 8: 2 blocks × 8 = 16 warps. Occupancy = 16/64 = 25%. This is quite low, driven by the high register usage. The developer might consider using `__launch_bounds__` to cap registers, or accept low occupancy and rely on ILP.

*Follow-up: What happens if you force lower register count with `__launch_bounds__`?* — The compiler caps register usage by spilling excess values to local memory (which is actually DRAM, going through L1/L2). Spills turn fast register accesses into ~400-cycle memory accesses. So you trade occupancy for memory traffic — sometimes the net effect is negative. Profile both configurations.

**Q: What does a tensor core physically compute?**
A: A tensor core performs a small matrix multiply-accumulate: D = A × B + C, where A, B, C, D are small tiles. On Ampere, a common shape is 16×16×16 in FP16 with FP32 accumulation, processed across a warp. Physically, one tensor core can do a 4×4×4 FMA per cycle. The warp-level MMA instruction orchestrates data across all 32 threads to feed larger tiles. The shapes are fixed in hardware, which is why the wmma/mma APIs expose specific tile sizes. For FP16 on Ampere, the throughput is ~256 TFLOPS per GPU — about 16× the FP32 rate.

*Follow-up: Why do MMA shapes matter for verification?* — Because each shape exercises different data paths in the tensor core and different register file access patterns. A verification engineer needs coverage across all supported shapes (m×n×k), all precisions (FP16, BF16, TF32, FP8, INT8, INT4), overflow/underflow cases, denorms, NaN/Inf inputs, and accumulator saturation.

**Q: Explain register file banking and why it matters.**
A: The register file is organized into banks (typically 4) to allow multiple simultaneous reads. If two source operands for the same instruction map to the same bank, you get a bank conflict and a one-cycle stall. The compiler tries to avoid this by assigning registers to different banks, but it's not always possible. For tools engineers, this matters because profiling tools need to report bank conflicts, and the compiler team optimizes register allocation to minimize them.

*Follow-up: How is the register file different from shared memory banking?* — Register file banking is for the warp scheduler's operand fetch — it affects instruction issue rate. Shared memory banking is for load/store operations — it affects memory throughput. Register bank conflicts cause 1-cycle stalls; shared memory bank conflicts serialize accesses. Both are performance hazards but in different parts of the pipeline.

**Q: What's the difference between CUDA cores and tensor cores from a verification standpoint?**
A: CUDA cores (FP32/INT32/FP64) execute scalar operations per thread. Each core takes two operands, produces one result — straightforward datapath. Tensor cores execute matrix operations that span an entire warp: 32 threads collectively provide the tile operands, and the tensor core does the MMA. Verification for CUDA cores focuses on per-instruction correctness (rounding, overflow, special values). Verification for tensor cores must additionally cover tile layout, cross-thread operand routing, accumulation precision, and the interaction between warp-level MMA scheduling and other instructions.

*Follow-up: Name a corner case that only affects tensor cores, not CUDA cores.* — Mixed-precision accumulation overflow. When accumulating many FP16 products into an FP32 accumulator over a large K dimension, the intermediate FP32 sums can overflow or lose precision in ways that depend on the order of accumulation within the tile. CUDA core FMA has deterministic ordering per thread; tensor core accumulation order within the tile is implementation-defined.

---

## 4. Memory Hierarchy

### The Pyramid

```
                    ┌───────────┐
                    │ Registers │  0 cycle (operand read)
                    │  ~256 KB  │  ~20 TB/s (per SM, internal)
                    └─────┬─────┘
                    ┌─────┴─────┐
                    │  Shared   │  ~20-30 cycles
                    │  Memory   │  ~10-19 TB/s (aggregate)
                    │ 64-228 KB │
                    └─────┬─────┘
                    ┌─────┴─────┐
                    │  L1 Cache │  ~30-40 cycles
                    │  (unified │  (shared with above)
                    │  with smem│
                    └─────┬─────┘
               ┌──────────┴──────────┐
               │    L2 Cache         │  ~200 cycles
               │    4-50 MB          │  ~6-12 TB/s (Ampere-Hopper)
               └──────────┬──────────┘
          ┌────────────────┴────────────────┐
          │       Device DRAM               │  ~400-800 cycles
          │  HBM2e: ~2 TB/s (A100)          │
          │  HBM3:  ~3.35 TB/s (H100)      │
          │  GDDR6X: ~1 TB/s (RTX 3090)    │
          └────────────────┬────────────────┘
     ┌─────────────────────┴─────────────────────┐
     │        PCIe / NVLink to Host               │
     │  PCIe 4.0 x16: ~25 GB/s (bidirectional)   │
     │  PCIe 5.0 x16: ~50 GB/s                   │
     │  NVLink 4: ~900 GB/s (H100)               │
     └────────────────────────────────────────────┘

Latencies are approximate, in GPU core cycles (~1-2 GHz clock).
```

### Memory Coalescing

When a warp executes a load, the 32 threads each provide an address. The hardware combines these into the minimum number of **cache-line transactions** (128 bytes on most architectures, broken into 32-byte sectors).

**Example 1: Perfect coalescing (stride-1 access)**
```c
// float *data; each thread loads data[threadIdx.x]
// Thread 0: addr 0x1000, Thread 1: 0x1004, ..., Thread 31: 0x107C
// All 32 addresses fit in one 128-byte cache line.
// Result: 1 transaction (4 sectors × 32 bytes)
```

**Example 2: Stride-2 access**
```c
// float *data; each thread loads data[2 * threadIdx.x]
// Thread 0: 0x1000, Thread 1: 0x1008, ..., Thread 31: 0x10F8
// Addresses span 256 bytes → 2 cache lines → 8 sectors
// Only half the data in each line is used.
// Result: 2 transactions, 50% bandwidth utilization
```

**Example 3: Random access (worst case)**
```c
// each thread loads data[random_index[threadIdx.x]]
// 32 random addresses, potentially 32 different cache lines
// Result: up to 32 transactions, ~3% utilization
```

**Example 4: Broadcast (all threads access same address)**
```c
// all threads load data[0]
// 1 cache line, 1 transaction, hardware broadcasts
// Result: 1 transaction
```

**Example 5: Struct-of-Arrays vs Array-of-Structs**
```c
// Array-of-Structs (AoS) — bad for GPUs:
struct Particle { float x, y, z, w; }; // 16 bytes
Particle particles[N];
// Thread i loads particles[i].x
// Thread 0: offset 0, Thread 1: offset 16, Thread 2: offset 32 ...
// Stride-4 (in float terms) → 4 transactions per warp, 25% efficiency

// Struct-of-Arrays (SoA) — good for GPUs:
float x[N], y[N], z[N], w[N];
// Thread i loads x[i]
// Thread 0: offset 0, Thread 1: offset 4, Thread 2: offset 8 ...
// Stride-1 → 1 transaction, 100% efficiency
```

**The SoA transformation is one of the most impactful GPU optimizations.** Profiling tools must report coalescing efficiency to guide this decision.

```
Coalescing patterns (32 threads, 4-byte words):

Perfect (stride-1):   |████████████████████████████████|  → 1 txn
                       └── 128 bytes (1 cache line) ──┘

Stride-2:             |█ █ █ █ █ █ █ █ █ █ █ █ █ █ █ █ |█ █ █ █ █ █ █ █ █ █ █ █ █ █ █ █ |
                       └── cache line 1 ──┘└── cache line 2 ──┘  → 2 txns

Stride-32:            Each thread hits a different line → 32 txns

Random:               Scattered across memory → up to 32 txns
```

### Shared Memory Banks and Bank Conflicts

Shared memory is divided into **32 banks**, each 4 bytes wide. Successive 4-byte words go to successive banks.

```
Bank:     0    1    2    3   ...  31   0    1    2   ...
Address: 0x00 0x04 0x08 0x0C ... 0x7C 0x80 0x84 0x88 ...
Word:     0    1    2    3   ...  31   32   33   34  ...
```

**No conflict:** Each thread accesses a different bank.
```
Thread 0 → Bank 0, Thread 1 → Bank 1, ..., Thread 31 → Bank 31
All 32 accesses served in 1 cycle.
```

**2-way bank conflict:** Two threads access different words in the same bank.
```
Thread 0 → Bank 0 (word 0), Thread 16 → Bank 0 (word 32)
These serialize → 2 cycles for those threads.
```

**Broadcast:** Multiple threads read the *same* word in the same bank.
```
Thread 0 → word 5, Thread 1 → word 5, ..., Thread 31 → word 5
Hardware broadcasts — NO conflict. 1 cycle.
```

**Stride-1 access (no conflict):**
```
shared[threadIdx.x]  →  thread i → bank i%32  → all different banks
```

**Stride-2 access (no conflict, accesses only 16 banks but no duplicates within a half-warp):**
Actually, stride-2 with 32 threads: thread i → bank (2i)%32. Thread 0→bank 0, thread 16→bank 0. This IS a 2-way conflict.

**Padding trick to avoid bank conflicts:**
```c
// Problem: column-major access to a 32×32 matrix causes 32-way bank conflicts
__shared__ float tile[32][32];     // tile[row][col]: accessing column = all hit same bank
// Fix: pad each row by 1 element
__shared__ float tile[32][32 + 1]; // now stride between rows is 33, breaking the bank pattern
```

```
Bank conflict diagram (stride-2 access):

Thread:  0  1  2  3  4  ... 15  16 17 ... 31
Bank:    0  2  4  6  8  ... 30   0  2 ...  30
         ↑                       ↑
         └───── CONFLICT ────────┘  (2-way on even banks)
```

### Sector and Cache-Line Granularity

Modern NVIDIA GPUs (Volta+) use **sector-based** caching. A cache line is 128 bytes, but it's divided into 4 **sectors** of 32 bytes each. When a warp's memory access touches only part of a cache line, only the needed sectors are fetched — not the full 128 bytes.

This matters for verification and performance:
- A scattered access pattern within a single cache line may fetch only 1-2 sectors (64 bytes) instead of 128 bytes
- L2 cache tracks validity per sector, not per line
- Cache miss penalties are per-sector, so partial-line accesses save bandwidth
- Profiling tools report sector transactions, not line transactions (Nsight Compute metrics like `l1tex__t_sectors_pipe_lsu_mem_global_op_ld`)

Example: if a warp of 32 threads reads bytes 0-31 and 64-95 of a cache line, only sectors 0 and 2 are fetched (64 bytes), skipping sectors 1 and 3.

### Other Memory Spaces

**Constant memory:** 64 KB, cached in a dedicated constant cache. All threads reading the same address get a broadcast (1 cycle). If threads read different addresses, accesses serialize. Good for lookup tables accessed uniformly.

**Texture/read-only path:** Goes through texture cache with spatial locality optimization (2D/3D patterns). Also accessible via `__ldg()` or declaring pointers as `const __restrict__`. Uses a separate cache from L1.

**Local memory:** Not a separate hardware — it's per-thread private memory that overflows to DRAM. Register spills go here. Accessed through L1/L2, so it's slow (~400 cycles uncached). The hardware coalesces local memory accesses across threads in a warp.

**Atomics:** Execute at different levels depending on scope:
- **Shared memory atomics:** Execute in the shared memory unit, ~few cycles
- **L2 atomics:** Added in Maxwell+, execute in L2 cache, ~100-200 cycles
- **Global atomics (DRAM):** ~400-800 cycles, and contention serializes threads

### Interview Q&A

**Q: A kernel does `data[threadIdx.x * 4]` for a float array. How many memory transactions per warp load?**
A: Stride-4 access with 4-byte floats means each thread is 16 bytes apart. Thread 0 is at offset 0, thread 1 at offset 16, thread 31 at offset 496. That's 512 bytes total, spanning 4 cache lines (128 bytes each). The warp issues 4 transactions. Each cache line is only 25% utilized (8 out of 32 floats in each line are actually used by the warp). This wastes 75% of bandwidth. To fix this, consider restructuring data so that consecutive threads access consecutive addresses (AoS → SoA transformation).

*Follow-up: What if it's `data[threadIdx.x * 32]`?* — Then each thread is 128 bytes apart, and each thread hits a *different* cache line. 32 transactions per warp. This is the absolute worst case for coalescing — throughput drops to 1/32 of peak bandwidth.

**Q: Explain shared memory bank conflicts with a concrete example.**
A: Say you have a 32×32 float matrix in shared memory and you're reading a column: `tile[threadIdx.x][col]`. Thread 0 reads row 0 (word 0+col), thread 1 reads row 1 (word 32+col), thread 2 reads row 2 (word 64+col). Since banks repeat every 32 words, thread 0 and all others map to the same bank (bank = col). That's a 32-way bank conflict — the worst case — taking 32 cycles instead of 1. The fix is to pad the declaration: `float tile[32][33]`. Now the stride between rows is 33 words, so thread i hits bank (33*i + col) % 32, which distributes across banks.

*Follow-up: Does a bank conflict cause incorrect results or just slower performance?* — Just slower performance. The hardware serializes conflicting accesses but produces correct results. However, in verification, bank conflicts are interesting because they exercise the shared memory arbiter's serialization logic, which is a complex piece of hardware to verify.

**Q: What's the difference between L1 cache and shared memory on a modern GPU?**
A: Since Volta, they're physically the same SRAM but logically separate. You can configure the split — for example, on Ampere, the 192KB can be divided with more going to shared memory or more to L1. Shared memory is explicitly managed by the programmer (you allocate and address it), while L1 is a hardware-managed cache. Shared memory gives you deterministic latency (~20-30 cycles) and no eviction surprises. L1 cache is subject to eviction policies and cache misses. For data that's reused with known access patterns, shared memory wins; for irregular access patterns, L1 caching may be more convenient.

*Follow-up: When would you prefer L1 over shared memory?* — When the access pattern isn't known at compile time, or when you don't want the programming overhead of explicit tiling. Also, L1 caching is "free" in the sense that it doesn't require code changes. On Ampere+, the L1 cache also supports async copy from global memory to shared memory, which overlaps data movement with compute.

**Q: Where do atomics execute in the memory hierarchy, and what are the performance implications?**
A: Shared memory atomics execute in the SM's shared memory unit — fast, ~few cycles, contention only within a block. L2 atomics (Maxwell+) execute at the L2 cache and avoid going to DRAM — ~100-200 cycles, contention across the entire GPU but better than DRAM. Global/DRAM atomics go all the way to the memory controller — ~400+ cycles, worst case. High contention (many threads atomically updating the same location) serializes the operations. For a histogram where many threads increment the same bin, you'd privatize in shared memory first, then do a single L2 atomic per block.

*Follow-up: What changed with Volta's atomicAdd for FP32?* — Volta added native FP32 atomicAdd in hardware. Previously it was emulated with CAS loops, which could fail and retry under contention. The native version is much faster and deterministic. For verification, you need to test both the native path and the CAS-fallback path (used for types without native support).

**Q: Explain the difference between GDDR and HBM and why it matters.**
A: GDDR (e.g., GDDR6X) uses traditional memory chips on the PCB, connected via a wide bus (384-bit on RTX 3090). It's cheaper but limited in bandwidth (~1 TB/s). HBM (High Bandwidth Memory) stacks DRAM dies vertically on a silicon interposer right next to the GPU die. This gives a much wider bus (4096-bit on A100's HBM2e) and higher bandwidth (~2 TB/s) with lower power per bit. The trade-off is cost and total capacity — HBM is expensive and typically maxes out at 80-120 GB. Consumer GPUs use GDDR; data-center GPUs use HBM.

*Follow-up: How does memory bandwidth affect kernel performance analysis?* — Bandwidth is the most common bottleneck. If your kernel's arithmetic intensity (FLOPS/byte) is below the machine's compute-to-bandwidth ratio, you're bandwidth-bound. With A100 doing ~19.5 TFLOPS FP32 and ~2 TB/s bandwidth, the ridge point is ~10 FLOPS/byte. Any kernel below that is bandwidth-bound and benefits more from memory optimizations than compute optimizations.

---

## 5. Chip-Level Organization

### GPC / TPC / SM Hierarchy

```
GPU Chip (e.g., GA100 - Ampere)
├── GPC 0 (Graphics Processing Cluster)
│   ├── Raster Engine
│   ├── TPC 0 (Texture Processing Cluster)
│   │   ├── SM 0
│   │   │   └── (4 partitions, 64 FP32 cores, etc.)
│   │   └── SM 1
│   ├── TPC 1
│   │   ├── SM 2
│   │   └── SM 3
│   └── ...  (typically 4-8 TPCs per GPC on GA100)
├── GPC 1
│   └── ...
├── ...
├── GPC 7 (GA100 full chip has 8 GPCs, 128 SMs)
│
├── L2 Cache (40 MB on GA100, partitioned)
├── Memory Controllers × 12 (HBM2e stacks)
├── NVLink interfaces × 12
├── PCIe Gen4 x16
├── GigaThread Engine (global work distributor)
├── Copy Engines (DMA) × 3-7
└── NVDEC / NVENC / Display Engine
```

*Note: GA100 full chip has 128 SMs; A100 product ships with 108 enabled (yields).*

### GigaThread Engine and Work Distribution

The **GigaThread Engine** is the top-level work distributor. When you launch a kernel:

1. Host driver sends the kernel descriptor to the GPU (grid dimensions, block size, shared memory, kernel pointer, arguments)
2. GigaThread Engine distributes thread blocks to SMs
3. Blocks are assigned to SMs based on available resources (registers, shared memory, block slots)
4. Once assigned, a block runs to completion on that SM — it does not migrate
5. As blocks complete, the SM's resources are freed and new blocks can be scheduled

### Raster Engines and ROPs

For graphics workloads:
- **Raster Engine:** One per GPC. Converts triangles into pixel fragments (rasterization). Determines which pixels a triangle covers and does edge/attribute interpolation setup.
- **ROPs (Render Output units):** Perform pixel blending, depth testing, antialiasing, and write to framebuffer. Grouped with memory partitions, not with SMs.

### Copy/DMA Engines

Separate engines for host↔device and device↔device memory copies. They run concurrently with compute kernels (if using separate CUDA streams). Multiple copy engines allow overlapping upload/download.

### Memory Partitions and Crossbar

The L2 cache and memory controllers are partitioned. A crossbar network connects SMs to memory partitions. Addresses are interleaved across partitions (typically at cache-line or sector granularity) to distribute load.

### MMU/GMMU and GPU Page Tables

Modern GPUs have a **GMMU (GPU Memory Management Unit)** that supports virtual memory:
- GPU-side page tables (separate from CPU page tables)
- Page faults (on Pascal+) enabling demand paging and unified memory
- Address translation with TLBs (per-SM and shared L2 TLB)
- Support for multi-process sharing via address space isolation

### Context Switching on the GPU

GPU context switching is *not* like a CPU context switch:
- The GPU does not preempt at arbitrary instruction boundaries (pre-Pascal)
- **Compute preemption** (Pascal+): Can preempt at instruction-level granularity for compute workloads
- **Pixel-level preemption** (Pascal+): Can preempt graphics workloads at pixel granularity
- Context switch involves draining pipelines, saving SM state (registers, shared memory), swapping page tables
- Much heavier than CPU context switch — motivation for MIG

### MIG (Multi-Instance GPU)

Introduced in Ampere (A100). Physically partitions the GPU into up to 7 independent instances:
- Each instance gets its own SMs, L2 cache slice, memory controllers, and memory
- Instances are isolated — a fault in one doesn't affect others
- Enables multi-tenant workloads on a single GPU
- **Not** time-slicing — true spatial partitioning
- Important for verification because each partition must be independently testable

### PCIe, NVLink, and NVSwitch

The GPU communicates with the outside world through:

**PCIe:** Standard host-GPU connection. PCIe 4.0 x16 gives ~25 GB/s per direction (total ~50 GB/s bidirectional). PCIe 5.0 doubles this. Every host→device memcpy goes over PCIe unless NVLink is available.

**NVLink:** High-bandwidth GPU↔GPU (and GPU↔CPU on some platforms) interconnect. Evolved significantly:
- NVLink 1.0 (Pascal): 160 GB/s bidirectional
- NVLink 2.0 (Volta): 300 GB/s
- NVLink 3.0 (Ampere): 600 GB/s
- NVLink 4.0 (Hopper): 900 GB/s

**NVSwitch:** A crossbar switch that connects multiple GPUs via NVLink, enabling all-to-all GPU communication at full bandwidth. Critical for multi-GPU training. Each NVSwitch chip can connect up to 8 GPUs (Hopper generation).

For verification/tools: your infrastructure must test data integrity and bandwidth across all interconnect paths, including simultaneous PCIe DMA + NVLink peer transfers + compute.

### Interview Q&A

**Q: How does a thread block get assigned to an SM?**
A: The GigaThread Engine receives the kernel launch descriptor with grid size, block size, and resource requirements. It checks each SM's available resources: free registers, shared memory, and block slots. If an SM can accommodate the block's resource demands, the block is assigned there. Once assigned, the block runs to completion — it never migrates to another SM. As blocks finish, resources are freed and new blocks from the same or different kernels can be dispatched. This is why block execution order is undefined and why you can't assume any particular mapping of blocks to SMs.

*Follow-up: Can two different kernels run on the same SM simultaneously?* — Yes, since Volta with MPS (Multi-Process Service) or concurrent kernels from the same context (if resources permit). The SM's block slots and resources can be shared across kernels. This is important for verification — you need to test inter-kernel resource conflicts.

**Q: What is the crossbar and why does it matter for performance?**
A: The crossbar connects SMs to memory partitions (L2 cache slices and memory controllers). If many SMs access data that maps to the same partition, that partition becomes a bottleneck. The address interleaving scheme distributes consecutive cache lines across partitions to avoid hot spots. For verification, you'd test workloads with non-uniform memory access patterns to stress the crossbar arbitration logic and check for deadlock or starvation.

*Follow-up: How does address interleaving work?* — Typically, the lower bits of the cache-line address (after removing the offset bits within a line) are used to select the partition. This means consecutive cache lines go to different partitions, so sequential access patterns distribute load evenly. Column-major access on large matrices can create partition hotspots.

**Q: What is MIG and why was it introduced?**
A: MIG (Multi-Instance GPU) spatially partitions a GPU into up to 7 independent instances, each with its own SMs, L2 cache, and memory bandwidth. It was introduced on A100 (Ampere) to support multi-tenant cloud deployments where different users need guaranteed isolation — not just performance isolation but fault isolation. Before MIG, GPUs could only time-slice between contexts, which gave unpredictable latency. MIG provides QoS guarantees because each instance has dedicated resources. For a tools engineer, MIG means your tools need to understand partition boundaries and report per-instance metrics.

*Follow-up: How is MIG different from vGPU?* — vGPU time-slices the entire GPU between virtual machines with a hypervisor. MIG physically partitions — each instance is like a smaller GPU with dedicated resources and no interference. vGPU gives flexible fractional allocation but with performance jitter; MIG gives fixed partitions with predictable performance.

**Q: Explain compute preemption and why it matters.**
A: Before Pascal, a long-running kernel could monopolize the GPU — no other context could run until it completed. Pascal introduced instruction-level compute preemption: the hardware can stop a kernel at any instruction boundary, save the SM state (registers, shared memory, program counter for each thread), and schedule another context. This enables interactive use cases (running compute kernels alongside a responsive desktop). For verification, preemption is a nightmare of corner cases: every instruction must be a safe preemption point, the state save/restore must be correct for all pipeline states.

*Follow-up: What state needs to be saved during preemption?* — Per-thread registers (up to 255 × 32 bits × 2048 threads per SM = up to 16MB per SM!), shared memory contents, the program counter for each warp, warp state (active mask, convergence state), barrier state, and any in-flight memory operations. The amount of state is large, which is why GPU context switches are expensive.

**Q: What are copy engines and why do they exist as separate hardware?**
A: Copy engines (also called DMA engines) handle host-to-device and device-to-host memory transfers independently of the compute SMs. They exist so that data transfer can overlap with kernel execution — while the SMs are crunching numbers, the copy engine can be uploading the next batch of data. Modern GPUs have multiple copy engines (one for each direction on A100, plus peer-to-peer). This enables the classic triple-buffering pattern: compute on batch N, download results from batch N-1, upload data for batch N+1 — all simultaneously.

*Follow-up: How does a CUDA stream interact with copy engines?* — CUDA streams provide ordering guarantees: operations in the same stream execute in order; operations in different streams can overlap. If you put a memcpy and a kernel in different streams, the runtime can schedule the copy on a DMA engine while the kernel runs on SMs. The driver manages the dependency tracking.

---

## 6. Architecture Generations

### What Changed and Why It Matters

| Gen | Year | Key Changes (tools-engineer level) |
|---|---|---|
| **Fermi** | 2010 | First "modern" GPU: unified shader architecture, ECC, L1/L2 caches, 64-bit addressing. Established the SM model. |
| **Kepler** | 2012 | Dynamic parallelism (device-side kernel launches), Hyper-Q (multiple CPU→GPU work queues), shuffle instructions, 255 registers/thread. |
| **Maxwell** | 2014 | Shared memory/L1 separation redesigned, improved energy efficiency, native shared memory atomics. Fewer FP64 units (consumer-focused). |
| **Pascal** | 2016 | Unified Memory with page faults and migration, NVLink 1.0 (GPU↔GPU/CPU), compute preemption, FP16 support (half rate on GP100). |
| **Volta** | 2017 | **Independent thread scheduling** (broke warp-sync code), Tensor Cores (first gen, FP16), unified L1/shared memory, `__syncwarp()`. |
| **Turing** | 2018 | RT Cores (ray-triangle intersection HW), concurrent FP32+INT32 execution, new L1 cache design, tensor cores with INT8/INT4. |
| **Ampere** | 2020 | Async copy (`cp.async`, `cuda::memcpy_async`), FP64 tensor cores, sparsity support (2:4 structured), 3rd gen tensor cores, MIG. |
| **Hopper** | 2022 | Thread Block Clusters (cooperative launch across SMs), TMA (Tensor Memory Accelerator), Distributed Shared Memory (smem across SMs), FP8 tensor cores, DPX instructions. |
| **Blackwell** | 2024 | Dual-die GPU (two dies on one package connected by 10 TB/s link), 5th gen tensor cores, FP4, 2nd gen transformer engine, decompression engine for compressed data, 192GB HBM3e. *Note: detailed microarchitectural specifics are still emerging; I'm less certain about internal pipeline changes.* |

### Detailed Architecture Comparison Table

| Feature | Pascal (GP100) | Volta (GV100) | Ampere (GA100) | Hopper (GH100) |
|---|---|---|---|---|
| SMs | 60 | 80 | 108 | 132 |
| FP32 cores/SM | 64 | 64 | 64 | 128 |
| Regs/SM | 65,536 | 65,536 | 65,536 | 65,536 |
| Shared mem/SM | 64 KB | 96 KB (configurable) | 164 KB (configurable) | 228 KB (configurable) |
| L2 cache | 4 MB | 6 MB | 40 MB | 50 MB |
| Memory | HBM2 16GB | HBM2 32GB | HBM2e 80GB | HBM3 80GB |
| BW | ~720 GB/s | ~900 GB/s | ~2 TB/s | ~3.35 TB/s |
| Tensor Cores | No | Yes (FP16) | Yes (FP16/BF16/TF32/FP64/INT8) | Yes (+FP8) |
| NVLink BW | 160 GB/s | 300 GB/s | 600 GB/s | 900 GB/s |

*Numbers are for the full data-center chip. Actual product SKUs may have some SMs disabled for yield.*

### Key Transitions a Tools Engineer Must Know

1. **Volta's independent thread scheduling** — Changed the execution model. Any tool that assumes warp-level lockstep is broken. Debuggers need per-thread PC tracking.

2. **Tensor cores** — New functional units requiring new instruction coverage in verification. Each generation adds precisions and shapes.

3. **Async copy (Ampere)** — Decouples data movement from compute. The memory pipeline changed fundamentally. Verification must cover async-copy in-flight state during preemption.

4. **Thread Block Clusters (Hopper)** — Blocks can now cooperate across SMs. This changes the work distributor, shared memory model, and synchronization primitives. A new layer of verification complexity.

5. **MIG (Ampere+)** — Hardware partitioning means the chip has multiple independent failure domains. Each must be testable.

### Interview Q&A

**Q: What was the most significant architectural change in Volta from a software compatibility perspective?**
A: Independent thread scheduling. Before Volta, all threads in a warp that took the same branch executed in perfect lockstep. Programmers (and tools) relied on this for warp-synchronous primitives — warp-level reductions, leader-thread patterns, lock-free algorithms within a warp. Volta gave each thread its own program counter, allowing threads on different paths to interleave. This broke existing code silently (data races that worked by luck). NVIDIA introduced `__syncwarp()` and cooperative groups as the correct replacement. For a tools engineer, this means debuggers, profilers, and verification suites had to be updated to track per-thread PCs instead of per-warp PCs.

*Follow-up: How did this affect the compiler?* — The compiler could no longer assume reconvergence at the post-dominator. The new model uses a more flexible schedule that the compiler generates with explicit synchronization barriers. The `__syncwarp` intrinsic compiles to a hardware barrier instruction. The compiler also had to update its optimization passes to not reorder instructions across warp-sync points.

**Q: What are Thread Block Clusters in Hopper, and why do they matter?**
A: A Thread Block Cluster is a group of thread blocks (up to 16 on Hopper) that are guaranteed to be co-scheduled on nearby SMs. Blocks in a cluster can directly access each other's shared memory through Distributed Shared Memory (DSMEM) without going through global memory. They also get cluster-level barriers for synchronization. This enables algorithms that need inter-SM cooperation (like large matrix multiply tiles that span multiple SMs) without the overhead of global memory. For verification, clusters add a new layer: you must test cross-SM shared memory coherence, cluster barrier behavior, and TMA (Tensor Memory Accelerator) operations that load directly from global memory into shared memory.

*Follow-up: What is TMA?* — The Tensor Memory Accelerator is a DMA engine that copies multi-dimensional tiles from global memory directly into shared memory without SM intervention. You describe the tile shape and base address, and TMA handles the address calculation and data movement. This frees up compute cycles and reduces instruction overhead. For verification, TMA is a complex new data path with its own address translation and error handling.

**Q: What does "structured sparsity" mean on Ampere and why is it a hardware feature?**
A: Ampere's tensor cores support 2:4 structured sparsity: out of every 4 values in a weight matrix, exactly 2 must be zero. The hardware stores only the non-zero values plus a 2-bit index per pair, achieving 2× compression. The tensor core then skips the zero multiplications, effectively doubling throughput for sparse matrices. This is a hardware feature because the decompression and index lookup happen inside the tensor core pipeline — software just provides the compressed data and the index metadata. Verification must test all 6 possible sparsity patterns per group of 4 values, and edge cases like all-zero groups.

*Follow-up: Why 2:4 and not arbitrary sparsity?* — Because 2:4 is a simple, fixed ratio that the hardware can decompress in a single cycle without complex gather logic. Arbitrary sparsity would require a full gather unit with variable latency, which is much harder to pipeline. The 2:4 pattern was chosen as a sweet spot between compression ratio (50%) and hardware simplicity.

**Q: Trace the evolution of tensor core precision support across generations.**
A: Volta introduced FP16 input with FP32 accumulation (4×4×4 MMA). Turing added INT8 and INT4 for inference quantization. Ampere added BF16 (better dynamic range for training), TF32 (19-bit format that fits in FP32 containers for transparent acceleration), FP64 for scientific computing, and structured sparsity. Hopper added FP8 (E4M3 and E5M2 formats) for training with reduced precision, plus the Transformer Engine that dynamically chooses precision. Blackwell reportedly adds FP4. Each new precision requires verification of the datapath, rounding behavior, overflow/underflow handling, and interaction with the accumulator.

*Follow-up: What is TF32?* — TF32 uses 10 bits of mantissa (like FP16) with 8 bits of exponent (like FP32), giving FP32's range with FP16's precision. It fits in a 32-bit container, so FP32 code gets automatic acceleration on tensor cores without code changes — just a cuBLAS flag or environment variable. The key insight is that many deep learning workloads don't need FP32's full 23-bit mantissa.

**Q: What's the significance of NVLink for a GPU tools engineer?**
A: NVLink provides high-bandwidth, low-latency connections between GPUs (and between GPU and CPU on some systems). For tools engineers, NVLink means your verification and profiling infrastructure must handle multi-GPU memory accesses, peer-to-peer transfers, and cache coherence protocols across GPUs. NVLink bandwidth has grown from 160 GB/s (Pascal) to 900 GB/s bidirectional (Hopper). A workload might access memory on a peer GPU through NVLink with different latency characteristics than local memory, and your tools need to attribute these correctly.

*Follow-up: How does NVLink interact with unified memory?* — With NVLink, page migration between GPUs is fast, so the unified memory runtime can migrate pages to the GPU that's accessing them most frequently. Without NVLink, migration goes through PCIe and is much slower. The GMMU on each GPU handles page faults and triggers migration. Tools need to distinguish between local memory accesses, NVLink accesses, and PCIe accesses for accurate performance attribution.

**Q: Explain the role of the raster engine and ROPs for graphics workloads.**
A: The raster engine (one per GPC) converts triangle primitives into fragments — it determines which pixels each triangle covers, does edge tests, and sets up attribute interpolation. The ROPs (Render Output units) sit at the memory partition end and handle the final pixel operations: depth/stencil testing, alpha blending, and writing the final color to the framebuffer. ROPs are grouped with memory controllers, not SMs, because they need high memory bandwidth for framebuffer access. For a tools engineer working on graphics pipeline verification, you need to stress the raster engine with degenerate triangles (subpixel, zero-area, very large) and test ROP blending with all blend modes and edge cases (NaN colors, zero-alpha).

*Follow-up: Why are ROPs at the memory partitions rather than at the SMs?* — Because ROPs write to the framebuffer, which lives in DRAM. Placing ROPs next to memory controllers minimizes the distance for those writes and gives each ROP a dedicated bandwidth path. If ROPs were at the SMs, framebuffer writes would need to traverse the crossbar, adding latency and congestion.

---

## 7. Performance Reasoning

### The Roofline Model

The roofline model plots achievable performance (FLOPS) as a function of **arithmetic intensity** (FLOPS per byte of DRAM traffic).

```
Performance (TFLOPS)
    ^
    |                              _______________
    |                             /               Peak Compute (19.5 TFLOPS FP32, A100)
    |                            /
    |                           /
    |                          /
    |                         / ← Bandwidth roof (2 TB/s × AI)
    |                        /
    |                       /
    |                      /
    |                     /
    |                    /
    |                   /
    |                  /
    |                 /
    |________________/
    |
    +──────────────────────────────────────────→ Arithmetic Intensity
    0    2    4    6    8   10   12   14          (FLOPS / Byte)
                               ^
                               |
                       Ridge point ≈ 9.75 FLOP/Byte
                       (19.5 TFLOPS / 2 TB/s)

    Below ridge point: BANDWIDTH-BOUND
    Above ridge point: COMPUTE-BOUND
```

### Worked Roofline Example

**Kernel: SAXPY (y = a*x + y)**
- Per element: 1 FMA = 2 FLOPS
- Data traffic: read x (4 bytes), read y (4 bytes), write y (4 bytes) = 12 bytes
- Arithmetic intensity: 2 / 12 = 0.167 FLOPS/byte

This is **far below** the ridge point (~10 FLOP/byte on A100). SAXPY is massively bandwidth-bound.

Expected performance: 0.167 × 2 TB/s = 0.33 TFLOPS (1.7% of peak compute)

**Kernel: Stencil (5-point 2D)**
- Per output element: 5 multiplies + 4 adds = 9 FLOPS (or 5 FMAs = 10 FLOPS if the compiler fuses)
- Data traffic (naive, no tiling): read 5 floats = 20 bytes, write 1 float = 4 bytes = 24 bytes
- Arithmetic intensity: 9/24 ≈ 0.375 FLOPS/byte → bandwidth-bound
- With shared memory tiling (halo cells loaded once): traffic drops to ~8 bytes/element → AI = 9/8 ≈ 1.125 FLOPS/byte — still bandwidth-bound but 3× better

**Kernel: Dense matrix multiply (SGEMM), N×N×N**
- FLOPS: 2N³
- Data traffic (assuming tiled): ~3N² × 4 bytes (read A, B; write C, if tiled well)
- Arithmetic intensity: 2N³ / (12N²) = N/6 FLOPS/byte
- For N=1024: AI = 170 FLOPS/byte → deeply compute-bound

### Little's Law for Memory-Level Parallelism

**Little's Law:** `Concurrency = Throughput × Latency`

To saturate memory bandwidth:
- A100 DRAM bandwidth: 2 TB/s
- DRAM latency: ~400 ns at 1.4 GHz clock
- Bytes in flight needed: 2 TB/s × 400 ns = 800 KB

Each outstanding memory request is a sector (32 bytes), so you need:
800 KB / 32 bytes = 25,600 outstanding requests across the chip.

With 108 SMs, that's ~237 outstanding requests per SM. Each warp can have ~1-2 outstanding loads, so you need ~120-240 active warps per SM. Max is 64 warps per SM, so you need **high occupancy and instruction-level memory parallelism** (multiple independent loads per thread) to saturate bandwidth.

**Little's Law applied to L2 cache:**
If a kernel mostly hits L2 (AI is medium, data set fits in L2):
- L2 bandwidth: ~6 TB/s (A100 aggregate)
- L2 latency: ~200 cycles at 1.4 GHz ≈ ~143 ns
- Bytes in flight: 6 TB/s × 143 ns ≈ 860 KB

This is comparable to the DRAM case — you still need massive concurrency. The advantage is the lower latency means each in-flight request resolves faster, so warps become eligible sooner.

**Little's Law for shared memory:**
- Shared mem bandwidth: ~10+ TB/s aggregate (across all SMs)
- Shared mem latency: ~20-30 cycles ≈ ~15-20 ns
- Bytes in flight: ~150-200 KB
Much less concurrency needed. This is why shared memory is so powerful — low latency means you don't need as many warps to keep the pipe full.

### Volkov's Argument: Low Occupancy Can Still Saturate

Vasily Volkov showed that **occupancy is not the only way to hide latency**. You can also use:

1. **ILP (Instruction-Level Parallelism):** Multiple independent instructions per thread. If each thread has 4 independent loads, that's equivalent to 4× the concurrency from half the warps.

2. **TLP (Thread-Level Parallelism):** More warps (traditional occupancy).

The product of ILP × TLP matters, not TLP alone. A kernel with 25% occupancy but 4× ILP can match a kernel with 100% occupancy and 1× ILP.

**Practical implication:** Don't over-optimize for occupancy at the expense of register usage. Sometimes using more registers (lower occupancy) gives each thread more ILP and better performance.

### Diagnosing the Bottleneck

| Symptom | Likely bottleneck | Diagnostic |
|---|---|---|
| Achieved bandwidth near peak, compute far from peak | Bandwidth-bound | nsight compute: memory throughput, L2 hit rate |
| Achieved compute near peak, bandwidth under-utilized | Compute-bound | nsight compute: SM utilization, pipe utilization |
| Both bandwidth and compute under-utilized | Latency-bound | Low occupancy, insufficient ILP, dependency chains |
| Very short kernels dominate | Launch-bound | nsight systems: kernel duration < ~10 µs, launch overhead visible |
| SM busy but few instructions issued | Stall-bound | Warp stall reasons: memory, execution dependency, barrier |

### Practical Bottleneck Diagnosis Flow

Here's the step-by-step reasoning a tools engineer should follow:

```
Step 1: Measure achieved FLOPS and achieved bandwidth
        ↓
Step 2: Compare to peak. Is either > 60% of peak?
        ├─ Bandwidth > 60% peak → bandwidth-bound
        ├─ Compute > 60% peak  → compute-bound
        └─ Neither > 60%       → likely latency-bound or launch-bound
        ↓
Step 3: If latency-bound, check:
        - Occupancy (is it < 25%?)
        - Warp stall breakdown (what are warps waiting on?)
        - Instruction-level dependency chains
        - Uncoalesced access patterns (wasting bandwidth, appearing as low utilization)
        ↓
Step 4: If launch-bound, check:
        - Average kernel duration (< 10 µs → suspect)
        - Host-side CPU utilization (high → driver overhead)
        - Consider CUDA Graphs or kernel fusion
```

### How to Tell Which Bottleneck You Have — Summary Table

| Metric | Bandwidth-Bound | Compute-Bound | Latency-Bound | Launch-Bound |
|---|---|---|---|---|
| DRAM BW utilization | >60% | Low | Low-Medium | Low |
| SM compute utilization | Low | >60% | Low | Low |
| Warp occupancy | Medium-High | Medium-High | Low | N/A |
| Top warp stall reason | Long scoreboard | Execution dependency | Long scoreboard or barrier | N/A |
| Kernel duration | Medium-Long | Medium-Long | Medium-Long | <10 µs |
| Primary optimization | Reduce traffic, coalesce | Algorithm improvement | More parallelism, ILP | Fusion, CUDA Graphs |

### Interview Q&A

**Q: Compute the roofline for a kernel that does 8 FLOPS per 16 bytes of DRAM traffic on an A100.**
A: Arithmetic intensity = 8/16 = 0.5 FLOPS/byte. The ridge point on A100 is ~19.5 TFLOPS / 2 TB/s ≈ 9.75 FLOPS/byte. Since 0.5 << 9.75, this kernel is bandwidth-bound. Expected peak performance = 0.5 × 2 TB/s = 1 TFLOPS. That's about 5% of FP32 peak compute. To improve it, you need to either increase arithmetic intensity (do more computation per loaded byte, via tiling or recomputation) or reduce memory traffic (data compression, caching).

*Follow-up: If you add shared memory tiling that reduces DRAM traffic by 4×, what happens?* — Arithmetic intensity becomes 0.5 × 4 = 2.0 FLOPS/byte. Still below the ridge point, so still bandwidth-bound, but now expected performance is 2.0 × 2 = 4 TFLOPS. You've 4× improved performance but are still bandwidth-bound. You'd need to reduce traffic by ~20× to become compute-bound.

**Q: Explain Little's Law in the context of GPU memory bandwidth saturation.**
A: Little's Law says bytes_in_flight = bandwidth × latency. To saturate A100's 2 TB/s bandwidth with ~400ns DRAM latency, you need 800 KB in flight simultaneously across the chip. Each 32-byte sector request is one unit of concurrency. That's ~25,000 outstanding requests chip-wide. This tells you the minimum parallelism needed just for memory access. It comes from occupancy (more warps = more outstanding loads) and ILP (more independent loads per thread = more outstanding loads per warp). If you can't generate enough concurrency, you'll underutilize bandwidth.

*Follow-up: How does the L2 cache affect this analysis?* — The L2 cache reduces effective latency for data with temporal/spatial locality. If 50% of accesses hit L2 (~200 cycles instead of ~400), the average latency drops, and you need less concurrency to saturate. But you also need less bandwidth from DRAM. The roofline shifts — you should compute it using the bottleneck bandwidth tier (DRAM, L2, or L1) depending on where your data comes from.

**Q: A kernel achieves only 30% of peak bandwidth and 10% of peak compute. What's wrong?**
A: It's likely latency-bound — there isn't enough parallelism to keep the memory pipeline full. Common causes: low occupancy (check register and shared memory usage), long dependency chains (each instruction depends on the previous one's result), barriers causing all warps to stall, or uncoalesced memory accesses that waste bandwidth. Check warp stall reasons in Nsight Compute: if you see "stall_memory" or "stall_long_scoreboard," you're waiting on memory with insufficient warps to cover the latency. Increase occupancy, add ILP, or restructure the algorithm.

*Follow-up: How would you distinguish between latency-bound and launch-bound?* — Launch-bound kernels are very short (< 10 µs) and the overhead of kernel launch, grid scheduling, and teardown dominates. Check kernel duration in Nsight Systems timeline. If kernels are short, try fusing kernels or using persistent kernels. Latency-bound kernels can be long but underutilized — the warp stall breakdown will show specific stall reasons, not launch overhead.

**Q: Explain Volkov's argument about occupancy and ILP.**
A: Volkov demonstrated with SGEMM that a kernel at 25% occupancy with heavy ILP (many independent multiply-adds per thread before a synchronization) could match the throughput of a kernel at 100% occupancy with minimal ILP. The insight is that occupancy measures thread-level parallelism (TLP), but ILP within each thread also generates pipeline concurrency. Four independent FMAs in one thread generate as much pipeline utilization as four warps each doing one FMA. Lower occupancy means more registers per thread, enabling the compiler to keep more values live and exploit more ILP. The "more occupancy = faster" rule is a myth.

*Follow-up: When IS higher occupancy clearly better?* — For purely memory-bound kernels with no ILP opportunity — when each thread does one load, one operation, one store, and there's nothing to overlap within the thread. Then the only way to hide memory latency is more warps. Also when using tensor cores, which have high latency and need many warps to keep the pipeline full.

**Q: What is "launch-bound" and how do you fix it?**
A: A workload is launch-bound when it consists of many short kernels and the CPU-side launch overhead (~5-20 µs per launch) dominates total runtime. You see it in Nsight Systems as tiny kernel blocks with gaps between them. Fixes include: kernel fusion (combine multiple short kernels into one), CUDA graphs (batch kernel launches to reduce driver overhead), persistent kernels (launch one long kernel that processes a work queue), and increasing per-kernel work. A tools engineer should care because verification suites that test many small kernels may be launch-bound, giving misleading throughput numbers.

*Follow-up: What's the difference between CUDA Graphs and kernel fusion?* — Kernel fusion combines the logic of multiple kernels into one source-code kernel — reducing launch count but requiring code changes. CUDA Graphs record a sequence of existing kernel launches into a graph that can be replayed with a single launch call — reducing launch overhead without code changes. Graphs also enable the driver to optimize the recorded sequence (e.g., eliminating redundant synchronization).

---

## 8. How This Hardware Knowledge Shows Up in Verification/Tools Work

### What Parts of the Machine Need Stimulus Coverage

A verification engineer needs test workloads that exercise every hardware structure:

| Hardware structure | What needs testing | Interesting stimuli |
|---|---|---|
| **Warp scheduler** | Instruction issue, warp selection, stall handling | Divergent warps, warps with dependency chains, barrier-heavy code, mixed instruction types |
| **FP32/FP64/INT ALUs** | Arithmetic correctness, special values | NaN, Inf, denorms, overflow, underflow, rounding modes, division by zero |
| **Tensor cores** | MMA correctness across all shapes and precisions | Every m×n×k shape, all precision combos, accumulator overflow, sparsity patterns (Ampere+) |
| **Register file** | Banking, allocation, spill/fill | Max register pressure (255 regs/thread), bank conflicts, register reuse patterns |
| **Shared memory** | Bank conflicts, broadcast, atomics | 32-way bank conflicts, mixed read/write patterns, atomic contention, padding patterns |
| **L1/L2 cache** | Hit/miss behavior, eviction, coherence | Cache thrashing (working set >> cache size), streaming vs reuse patterns, sector vs line granularity |
| **Memory coalescing HW** | Transaction generation, sector merging | Stride-1 through stride-32 access, random access, misaligned access, partial-warp access |
| **Atomics** | Correctness under contention | Same-address contention from all threads, mixed atomic types, scope (block/device/system) |
| **TMA (Hopper)** | Multi-dim copy correctness | Various tile shapes, boundary conditions, misaligned addresses, concurrent TMA operations |
| **Preemption** | State save/restore at every instruction | Preempt during: divergent code, tensor core MMA, atomic-in-flight, async copy in progress |

### What Makes a Workload "Interesting" to a Verification Engineer

1. **Divergence corner cases:** Worst-case divergence (32-way), nested divergence (divergence inside divergence), divergence at loop boundaries, divergence + synchronization deadlocks.

2. **Bank conflicts:** 32-way shared memory bank conflicts, conflicts combined with atomics, access patterns that create bank conflicts only on certain warp lanes.

3. **Atomics contention:** All 2048 threads atomically updating the same address. Mixed atomic operations on the same address (atomicAdd + atomicCAS). System-scope atomics with concurrent CPU access.

4. **Memory aliasing:** Two different virtual addresses mapping to the same physical page. Concurrent read-write through aliased addresses. Aliasing across different memory spaces (global/texture/surface).

5. **Cache thrash:** Working set sized to exactly evict useful data. Streaming workloads vs reuse workloads on the same cache. Cross-SM cache pollution via L2.

6. **Resource exhaustion:** Max blocks per SM, max registers per thread, max shared memory, max warps — all simultaneously. What happens at the boundary?

7. **Concurrent features:** Tensor core operations overlapping with memory loads. Async copy in progress during a barrier. TMA overlapping with shared memory atomics.

### Verification Coverage Taxonomy

A structured way to think about coverage for GPU hardware verification:

**Functional coverage:**
- Every instruction executed on every functional unit type (FP32, FP64, INT, SFU, tensor core, LSU)
- Every memory space accessed (registers, shared, L1, L2, global, local, constant, texture)
- Every synchronization primitive (__syncthreads, __syncwarp, barrier.arrive/wait, cluster barrier)
- Every atomic operation type × every memory scope × every data type

**Microarchitectural coverage:**
- Warp scheduler: all scheduling transitions (ready→issued, stall→ready, barrier→release)
- Register file: max utilization, cross-bank access, read-after-write hazards at minimum distance
- Shared memory arbiter: all conflict degrees (1-way through 32-way), broadcast, read+write same cycle
- Cache controllers: hit, miss, eviction, fill, sector-partial-hit, tag-way conflicts
- Coalescing unit: every transaction count from 1 to 32 for a single warp load/store

**Stress/corner cases (the interesting ones for finding bugs):**
- Resource exhaustion boundaries: exactly max blocks per SM, exactly max shared mem, 255 registers/thread
- Preemption at every pipeline stage
- Power gating → wake transitions
- ECC error injection and correction during compute
- Clock domain crossings (memory controller clock vs SM clock)

### Interview Q&A

**Q: If you were designing a test for the warp scheduler, what workloads would you create?**
A: I'd create workloads targeting each scheduling decision: (1) A kernel where all warps are ready simultaneously — tests priority/fairness. (2) A kernel with dependency chains that cause warps to go not-ready and become ready at different times — tests the scoreboard. (3) A kernel with heavy barriers where warps stall and resume in waves — tests barrier wakeup. (4) A kernel mixing FP32, INT, tensor, and memory instructions — tests multi-pipe scheduling. (5) A kernel with maximum divergence so each warp has a different active mask — tests interaction between divergence and scheduling. I'd check for correctness (right results), liveness (no deadlocks), and performance (reasonable utilization).

*Follow-up: How would you detect a scheduling bug vs a functional bug?* — A scheduling bug would show as non-deterministic incorrect results (race condition) or as a liveness failure (hang). A functional bug would show as deterministic incorrect results for specific input values. To distinguish, run the same test many times — if results vary, it's likely a scheduling or synchronization issue.

**Q: Why is memory aliasing interesting for GPU verification?**
A: Memory aliasing means two different addresses (virtual or through different memory spaces) map to the same physical location. On a GPU, this creates coherence challenges: if one warp writes through a global pointer and another reads through a texture/surface reference to the same data, the caches may serve stale data because the texture cache doesn't snoop the L1. Also, unified memory can create aliasing between GPU and CPU views of the same page. Verification must ensure that the memory ordering model is respected for all aliasing scenarios, including across different memory scopes (CTA, GPU, system).

*Follow-up: What specific test would you write?* — Allocate a buffer, create both a global pointer and a surface reference to it. Have warp 0 write via the global pointer, issue a __threadfence(), then have warp 1 read via the surface reference. Verify warp 1 sees the updated value. Vary the fence scope (block, device, system) and the ordering of operations. Also test with read-only (__ldg) and texture paths.

**Q: How would you create a worst-case shared memory bank conflict test?**
A: Declare a shared memory array and have all 32 threads in a warp access addresses that map to the same bank. For example: `shmem[threadIdx.x * 32]` — since bank = (address/4) % 32, and 32 × 4 = 128 bytes between accesses, every thread hits the same bank. This is a 32-way conflict. I'd measure the cycle count and verify it's ~32× slower than a conflict-free access pattern. I'd also test the broadcast case (all threads reading the same word from the same bank — should be 1 cycle) and mixed patterns (some threads conflicting, others not).

*Follow-up: How do you verify the broadcast optimization is working?* — Compare the performance of "all threads read address X" vs "32 threads each read a different address in the same bank." The broadcast should complete in 1 cycle; the 32-way conflict should take 32 cycles. If broadcast is broken, both would take 32 cycles.

**Q: What makes GPU preemption hard to verify?**
A: Preemption requires saving the complete SM state at an arbitrary instruction boundary: all thread registers (up to 255 × 32 × 2048 threads = megabytes), shared memory, warp PCs, active masks, barrier state, convergence stack (or independent thread state post-Volta), and any in-flight memory operations. The challenge is that every possible instruction and pipeline state must be a valid preemption point. Edge cases include: preemption during a tensor core MMA (multi-cycle operation), during an async copy (data partially transferred), during an atomic (read-modify-write in progress), or while a warp is diverged with a complex reconvergence state. You need to inject preemption at every possible point and verify that resume produces identical results to uninterrupted execution.

*Follow-up: How would you automate this testing?* — Create a deterministic test kernel, run it without preemption, record the output (golden reference). Then inject preemption signals at every instruction boundary (using hardware debug triggers or simulation), resume, and compare output. You'd also randomize the injection point across thousands of runs. For simulation, you can use a cycle-accurate model; for silicon, you'd use debug features that force preemption at controlled points.

**Q: From a tools engineer's perspective, what's the most important thing to understand about the memory hierarchy?**
A: The layered latency/bandwidth trade-off and how it creates performance cliffs. A kernel that fits in L1 runs at ~10 TB/s effective bandwidth; one that spills to L2 drops to ~6 TB/s; one that goes to DRAM drops to ~2 TB/s. Verification and profiling tools must accurately attribute memory traffic to the right level of the hierarchy. If your profiler reports "bandwidth-bound" but doesn't distinguish between L1 and DRAM bandwidth, the developer gets misleading guidance. Understanding coalescing, bank conflicts, and cache behavior lets you build tools that give actionable advice, not just raw numbers.

*Follow-up: What metrics would your profiling tool expose for the memory hierarchy?* — Per-kernel: L1 hit rate, L2 hit rate, DRAM bytes read/written, sector transactions per request (coalescing efficiency), shared memory bank conflicts per access, outstanding memory requests (for Little's Law analysis), and per-memory-space breakdown (global, local, shared, texture, constant). Ideally also warp stall reasons attributed to memory levels.

---

## Red Flags: Common Wrong Answers

🚩 **"GPUs are just SIMD machines."**
Wrong. SIMT is fundamentally different from SIMD — it allows per-thread branching (with performance cost), each thread has its own registers and PC (on Volta+), and the programmer writes scalar code. Calling it SIMD suggests you don't understand divergence or the programming model.

🚩 **"More occupancy is always better."**
Wrong. Volkov showed that ILP can compensate for low occupancy. High occupancy with register spills can be *slower* than moderate occupancy with more registers per thread. The metric that matters is "enough parallelism (TLP × ILP) to hide latency."

🚩 **"Shared memory is like L1 cache."**
Misleading. They may share the same physical SRAM (since Volta), but they're fundamentally different: shared memory is explicitly managed, software-addressed, has bank conflicts, and persists for the block's lifetime. L1 is hardware-managed with eviction policies. Confusing them suggests you don't understand the programming model.

🚩 **"Warp divergence causes incorrect results."**
Wrong (on correct hardware). Divergence causes performance loss (serialization of paths), not correctness issues. The hardware correctly masks inactive threads. However, warp-synchronous code that assumes lockstep CAN cause correctness issues on Volta+ — that's a software bug, not a divergence problem.

🚩 **"All threads in a block execute simultaneously."**
Wrong. Only warps execute simultaneously (and even then, warps are time-multiplexed on functional units). A block of 1024 threads has 32 warps, and only a few warps execute in any given cycle. The rest are waiting (for operands, memory, barriers).

🚩 **"Register spills go to shared memory."**
Wrong. Register spills go to **local memory**, which is per-thread storage backed by DRAM (going through L1/L2 caches). It's called "local" because it's private to each thread, but it's physically in DRAM — very slow. Shared memory is explicitly allocated by the programmer and shared across the block.

🚩 **"GPU context switching is cheap because there's lots of parallelism."**
Wrong. GPU context switching is *expensive* — megabytes of register and shared memory state must be saved/restored. The whole point of high occupancy and latency hiding is to avoid context switches. Compute preemption (Pascal+) made it possible but not cheap.

🚩 **"Bank conflicts cause incorrect results."**
Wrong. Bank conflicts serialize accesses but produce correct results. They're a performance issue, not a correctness issue. (But they're a verification concern because the serialization logic must be correct.)

🚩 **"The L2 cache is shared across SMs, so all SMs see each other's writes immediately."**
Not exactly. The L2 is partitioned across memory controllers. Cache coherence within the GPU depends on memory fences and scope. Without a `__threadfence()`, there's no guarantee of visibility ordering between SMs. The GPU memory model is weak.

🚩 **"Tensor cores are just faster FP units."**
Wrong. Tensor cores perform matrix-multiply-accumulate on fixed tile shapes — they're fundamentally different from scalar FP units. They take multi-operand matrix inputs from multiple threads in a warp and produce a matrix output. The data layout, instruction format, and precision handling are all different.

🚩 **"Global memory accesses are always slow."**
Misleading. Global memory accesses that hit in L1 are ~30 cycles; hitting L2 is ~200 cycles. Only DRAM misses are ~400-800 cycles. With coalesced access patterns and locality, much global traffic can be served from caches. The key is understanding when and why cache misses occur, not blanket avoidance.

🚩 **"Each CUDA core runs one thread."**
Misleading. A CUDA core (FP32 unit) executes one instruction per clock from one thread, but that thread belongs to a warp of 32. All 32 threads in the warp issue the same instruction — they just execute on 32 different CUDA cores simultaneously. No individual thread "owns" a CUDA core. Warps are multiplexed over the cores.

🚩 **"GPU threads are like CPU threads."**
Wrong in important ways. GPU threads are extremely lightweight (~0 cost to create/switch). They share an instruction stream within a warp. They have no independent stack by default (local memory is used for spillover). They can't do system calls, I/O, or most things OS threads do. The programming model is SPMD — same code, different data — unlike general-purpose CPU threading.

🚩 **"Coalescing only matters for global memory."**
Partially wrong. While coalescing specifically refers to combining global memory requests into fewer transactions, the *principle* of access pattern efficiency applies everywhere. Shared memory has bank conflicts (analogous issue). Local memory (register spills) is also coalesced across warp threads by the hardware. Even texture accesses benefit from spatial locality that the texture cache is designed for. Thinking about access patterns is universal.

---

## Whiteboard Checklist

When you're at the whiteboard in an interview for this role, make sure you can:

- [ ] **Draw an SM block diagram** — 4 partitions, each with warp scheduler, dispatch, FP32/INT/FP64 cores, tensor core, SFU, LSU, register file. Show shared L1/shared memory at the bottom.

- [ ] **Draw the memory hierarchy pyramid** — Registers → Shared/L1 → L2 → DRAM → Host. Label approximate latencies (0, 20-30, ~200, 400-800 cycles) and bandwidths.

- [ ] **Explain warp divergence** — Draw the if/else timeline with active masks. Show the throughput penalty. Distinguish pre-Volta (stack-based, strict serialization) from Volta+ (independent thread scheduling, interleaved).

- [ ] **Do occupancy math** — Given registers/thread, shared memory/block, block size, and SM resource limits, calculate max warps and occupancy. Know the formula and the rounding.

- [ ] **Compute coalescing efficiency** — Given an access pattern (stride-N), determine how many 128-byte transactions are generated and the bandwidth utilization.

- [ ] **Draw a bank conflict diagram** — 32 banks, show which threads hit which banks for stride-1 (no conflict) vs stride-32 (32-way conflict). Show the padding fix.

- [ ] **Sketch a roofline** — Axes (arithmetic intensity vs FLOPS), bandwidth roof (slope), compute roof (horizontal line), ridge point. Place SAXPY (bandwidth-bound) and SGEMM (compute-bound) on it.

- [ ] **Apply Little's Law** — Concurrency = Bandwidth × Latency. Calculate outstanding bytes needed to saturate DRAM bandwidth.

- [ ] **Name the key arch transitions** — Fermi (caches), Kepler (dynamic parallelism), Pascal (unified memory, NVLink), Volta (independent thread scheduling, tensor cores), Turing (RT cores), Ampere (async copy, MIG), Hopper (clusters, TMA, DSMEM).

- [ ] **SIMT vs SIMD vs SMT** — State the precise differences in one sentence each.

- [ ] **Explain what makes a good verification workload** — Divergence, bank conflicts, atomics contention, cache thrashing, memory aliasing, resource exhaustion, preemption during complex operations.

- [ ] **Discuss occupancy vs ILP** — Reference Volkov's work. Explain when low occupancy wins.

- [ ] **Explain tensor core shapes** — Why m×n×k matters, what precisions exist per generation, why verification needs all combos.

- [ ] **Describe MIG at concept level** — Spatial partitioning, independent instances, QoS isolation, verification implications.

- [ ] **Explain async copy and TMA** — Why decoupling data movement from compute matters, what state must be tracked during preemption.

- [ ] **Walk through a bottleneck diagnosis** — Given profiler metrics (achieved bandwidth, compute utilization, warp stall reasons), identify the bottleneck and recommend fixes.

- [ ] **Explain sector-based caching** — 128-byte lines, 32-byte sectors, why only needed sectors are fetched, impact on profiler metrics.

- [ ] **Draw the GPC/TPC/SM hierarchy** — Show how the chip is organized, where the GigaThread Engine sits, how blocks are distributed.

---

*Module M4 complete. Next: M5 — CUDA Programming Model and Runtime.*
