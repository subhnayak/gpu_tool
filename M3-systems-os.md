# Module M3: Systems Software & Operating Systems

## Why This Matters for This Role

NVIDIA's GPU Development Tools team builds the infrastructure that lets GPU engineers verify, debug, and profile silicon—from pre-silicon simulation through post-silicon bring-up. You'll write system software that lives in the narrow space between the OS kernel, the GPU driver stack, and the hardware itself. Every topic in this module is something you'll either touch directly or need to reason about when debugging failures at 2 AM:

- **Processes, threads, scheduling** — Your tools instrument GPU workloads; you must understand what the CPU side is doing while the GPU executes.
- **Virtual memory, pinned pages, DMA** — GPU DMA engines transfer data between host and device. If you don't understand page tables, TLB shootdowns, and why pinned memory exists, you can't debug transfer stalls.
- **PCIe** — It's the physical link between CPU and GPU. BAR mappings, doorbells, and MSI-X interrupts are daily vocabulary.
- **GPU driver architecture** — You may build tools that hook into the UMD or KMD, intercept command buffers, or replay GPU submissions. Understanding the pushbuffer model is non-negotiable.
- **Concurrency** — Lock-free ring buffers are literally how commands reach the GPU. You'll design producer-consumer pipelines in your tools.
- **ELF, linking, debugging** — Your tools may parse GPU binaries (cubin/ELF), inject instrumentation via LD_PRELOAD, or generate DWARF debug info for GPU code.
- **Linux tooling** — You'll use perf, strace, gdb, and sanitizers every day.

This module covers all of it at interview depth.

---

## 1. Processes, Threads, and Scheduling

### Processes vs Threads

A **process** is the OS unit of isolation: it owns a virtual address space, file descriptor table, signal handlers, and credentials. A **thread** is a schedulable entity within a process—threads share the address space and file descriptors but each has its own stack, register set, and thread-local storage (TLS).

On Linux, both are implemented as `task_struct`; `clone()` with different flags creates either. `fork()` = new process (COW address space). `pthread_create()` = new thread (shared address space via `CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND`).

### Address Space Layout

```
┌─────────────────────────┐  0xFFFFFFFFFFFFFFFF (64-bit)
│   Kernel Space           │  (upper half on x86-64)
├─────────────────────────┤  Canonical hole boundary
│   [unmapped]             │
├─────────────────────────┤
│   Stack ↓                │  grows downward, RLIMIT_STACK
├─────────────────────────┤
│   mmap region            │  shared libs, mmap'd files, anon pages
│   (grows down or up)     │
├─────────────────────────┤
│   Heap ↑                 │  brk()/sbrk(), malloc arena
├─────────────────────────┤
│   BSS (zero-init)        │
│   Data (initialized)     │
│   Text (code, r-x)       │
├─────────────────────────┤
│   [guard page / NULL]    │
└─────────────────────────┘  0x0000000000000000
```

Each thread gets its own stack (default 8 MB on Linux, allocated via mmap in the mmap region). The main thread's stack is at the top of user space.

### Scheduling: CFS Basics

Linux's Completely Fair Scheduler models each task as accumulating "virtual runtime" (`vruntime`). Tasks are stored in a red-black tree keyed by `vruntime`. The scheduler always picks the leftmost node (lowest vruntime). Tasks that use less CPU accumulate vruntime slower and get prioritized—this naturally favors interactive tasks.

Key concepts:
- **Nice values** (-20 to +19): Affect the *rate* of vruntime accumulation. Lower nice = slower vruntime growth = more CPU.
- **Time slices**: CFS computes a "target latency" divided among runnable tasks, weighted by nice.
- **CPU affinity**: `sched_setaffinity()` / `taskset`. Pin threads to cores to avoid migration overhead—critical for latency-sensitive GPU submission threads.
- **SCHED_FIFO / SCHED_RR**: Real-time policies. Higher priority than any CFS task. Used in some driver/runtime paths where latency matters.

### Context Switch Cost

A context switch involves:
1. Save registers (GP, FP/SSE/AVX—lazily via `CR0.TS` bit or eagerly on modern kernels using XSAVE).
2. Switch page tables (write `CR3` → TLB flush on x86, though PCID mitigates this).
3. Flush pipeline.
4. Cache pollution (working sets evict each other).

Cost: ~1–5 µs direct, but indirect cost (cache/TLB misses) can be 10–100 µs for large working sets. This matters when your tool is profiling GPU kernel launch latency—a context switch on the submission thread adds measurable jitter.

### Syscall Mechanics

User → Kernel transition on x86-64 Linux:
1. User code places syscall number in `RAX`, arguments in `RDI, RSI, RDX, R10, R8, R9`.
2. `SYSCALL` instruction: saves `RIP` → `RCX`, `RFLAGS` → `R11`, loads kernel `RIP` from `LSTAR` MSR, switches to ring 0.
3. Kernel entry (`entry_SYSCALL_64`): swaps to kernel stack (per-CPU), saves user registers, calls `sys_call_table[rax]`.
4. Return via `SYSRET` (fast) or `IRET` (for signals/ptrace).

**Syscall vs ioctl**: A syscall is a numbered kernel entry point (`read`, `write`, `mmap`). An `ioctl` is a *single* syscall (`__NR_ioctl`) that multiplexes commands via a command number and a pointer to a driver-specific struct. GPU drivers heavily use ioctls—every command submission, memory allocation, and context switch goes through ioctls on the device file (e.g., `/dev/nvidia0`).

```c
// Example: simplified GPU ioctl pattern
struct my_gpu_alloc_args {
    uint64_t size;
    uint64_t handle_out;  // returned by driver
};
int fd = open("/dev/nvidia0", O_RDWR);
struct my_gpu_alloc_args args = { .size = 4096 };
ioctl(fd, MY_GPU_IOCTL_ALLOC, &args);
```

### Signals

Signals are asynchronous notifications delivered to a process or thread. Key for tools:
- `SIGSEGV` — your tool might install a handler for copy-on-write tricks or guard-page-based memory tracking.
- `SIGSTOP/SIGCONT` — used by debuggers (via `ptrace`).
- `SIGUSR1/SIGUSR2` — common for triggering profiler dumps.
- Signal delivery interrupts syscalls (unless `SA_RESTART`). This can cause spurious `EINTR` in GPU driver ioctls—your code must handle this.

```c
// Signal handler that triggers a profiler snapshot
void handler(int sig) {
    // Async-signal-safe only! No malloc, no printf.
    write(STDOUT_FILENO, "snapshot\n", 9);
    __atomic_store_n(&snapshot_requested, 1, __ATOMIC_RELEASE);
}
struct sigaction sa = { .sa_handler = handler, .sa_flags = SA_RESTART };
sigaction(SIGUSR1, &sa, NULL);
```

### Interview Q&A

**Q: What's the difference between a process and a thread on Linux at the kernel level?**
A: On Linux, both are represented by a `task_struct`. The difference is what they share. When you call `fork()`, the kernel creates a new `task_struct` with a copy-on-write clone of the parent's address space, file descriptor table, and signal handlers—each is independent. When you call `pthread_create()`, it uses `clone()` with flags like `CLONE_VM` so both tasks share the same address space, the same file descriptors, and so on. The kernel scheduler treats them identically—it schedules `task_struct` objects, not "processes" or "threads."
*Follow-up:* What's the implication for `mmap` in a multithreaded process? — Any thread can call `mmap` and the mapping is visible to all threads immediately, because they share the same `mm_struct` (page tables). This is why you can allocate GPU-visible memory in one thread and access it from another without any IPC.

**Q: Walk me through what happens when a user-space program executes a syscall on x86-64.**
A: The program loads the syscall number into `RAX` and arguments into the standard registers (`RDI`, `RSI`, etc.), then executes the `SYSCALL` instruction. This instruction is a hardware-assisted transition: it saves the return address into `RCX` and flags into `R11`, loads the kernel entry point from the `LSTAR` MSR, and flips the privilege level to ring 0. The kernel entry code swaps to a per-CPU kernel stack, saves all user registers onto it, then dispatches through `sys_call_table` to the actual handler. On return, `SYSRET` restores the user state and drops back to ring 3.
*Follow-up:* Why do GPU drivers use ioctls instead of adding new syscalls? — Adding a syscall requires modifying the kernel's syscall table, which means mainline kernel changes and ABI stability guarantees. An ioctl multiplexes many commands through a single syscall on a device file descriptor, so the driver can define its own command set without touching core kernel ABI. It also naturally scopes operations to a file descriptor, which maps well to a GPU context.

**Q: Why does CPU affinity matter for a GPU workload submission thread?**
A: The submission thread is typically latency-sensitive—it builds command buffers and rings a doorbell to notify the GPU. If the scheduler migrates it to another core, you pay a context switch cost plus cache and TLB refill. On a NUMA system, migrating to a core on a different socket also means the memory containing the pushbuffer may now be remote, adding latency. Pinning the thread to a core near the GPU's PCIe root port minimizes both migration overhead and NUMA effects.
*Follow-up:* How does CFS handle a thread that's mostly sleeping waiting for GPU fences? — It accumulates very little `vruntime`, so when it wakes up, it's far to the left in the red-black tree and gets scheduled almost immediately. CFS naturally prioritizes I/O-bound and event-driven tasks, which is exactly what a GPU fence-waiting thread is.

**Q: What are the dangers of signal handlers in a GPU tools context?**
A: Signal handlers run asynchronously and can interrupt almost any code—including code holding locks. Only a small set of functions are async-signal-safe (no `malloc`, no `printf`). If your tool is intercepting GPU API calls and a signal fires mid-interception, you can deadlock on your own internal locks. The safe pattern is to set a flag (using an atomic or `sig_atomic_t`) in the handler and check it in your main loop. Also, signals can interrupt blocking ioctls with `EINTR`, so any GPU driver interaction code must retry on `EINTR`.
*Follow-up:* How does `ptrace` interact with signals? — A ptraced process stops on *every* signal delivery (the debugger gets notified via `waitpid`), allowing GDB to inspect state. The debugger can then suppress or inject the signal. This is how breakpoints work: `SIGTRAP` is generated by the `INT3` instruction at the breakpoint, the debuggee stops, and GDB handles it.

**Q: Explain the cost of a context switch beyond just saving/restoring registers.**
A: The direct cost—saving and restoring registers, switching the kernel stack—is maybe 1–5 microseconds. But the indirect cost dominates. When you switch to a different task, you load CR3 with a new page table base, which flushes the TLB (unless PCID is used, which tags TLB entries per process). The new task's working set isn't in cache, so you get a burst of cache misses. On a large-footprint application, the warming-up penalty can be tens of microseconds. For GPU tools, this is why we pin submission threads: even a few extra microseconds of jitter per kernel launch adds up across millions of launches.
*Follow-up:* What is PCID and how does it help? — Process Context Identifiers tag TLB entries with a 12-bit ID so the CPU doesn't have to flush the entire TLB on CR3 switch. The kernel assigns PCIDs to recently-used address spaces. This significantly reduces context switch cost for processes with large TLB footprints.

---

## 2. Virtual Memory in Depth

### Paging and Multi-Level Page Tables

x86-64 uses a 4-level page table (5-level with LA57):

```
┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐
│  PML4  │──>│  PDPT  │──>│   PD   │──>│   PT   │──> Physical Page
│(PGD)   │   │(PUD)   │   │(PMD)   │   │(PTE)   │
│9 bits  │   │9 bits  │   │9 bits  │   │9 bits  │   12 bits offset
└────────┘   └────────┘   └────────┘   └────────┘
   [47:39]     [38:30]      [29:21]      [20:12]      [11:0]
```

Each level is a 4 KB page containing 512 8-byte entries. Each entry holds a physical frame number plus flags (present, writable, user-accessible, NX, accessed, dirty, etc.).

**Huge pages**: Instead of walking all 4 levels, the PD entry can directly point to a 2 MB physical page (or the PDPT entry to a 1 GB page), setting a "page size" bit. Fewer TLB entries needed, fewer page walks, critical for large GPU buffer mappings.

### TLB and TLB Shootdown

The TLB caches virtual→physical translations. On x86, each core has its own TLB. When a page table entry is modified (e.g., unmapping a page), *all* cores that might have cached that translation must be notified. This is a **TLB shootdown**: the initiating core sends an IPI (inter-processor interrupt) to affected cores, each flushes the relevant TLB entry, and acknowledges. This is expensive—it's an IPI plus the cost of the remote core handling it.

Why it matters: When the GPU driver pins/unpins pages or changes memory mappings, TLB shootdowns hit every core running threads of that process. In a profiling tool that maps/unmaps buffers frequently, this can be a significant overhead.

### Page Faults

- **Minor fault**: Page table entry exists but the page is already in memory (e.g., a COW page that needs to be copied, or a zero-fill page on first access). No disk I/O.
- **Major fault**: Page must be fetched from disk (swap or a file-backed mmap). Hundreds of microseconds to milliseconds.

**Demand paging**: Pages aren't allocated until first access. `malloc` returns immediately; the kernel only allocates physical pages on the first page fault. This is "memory overcommit"—the kernel promises more memory than physically available, betting not everyone will use it all. (`vm.overcommit_memory` controls this.)

### mmap

`mmap` maps files or anonymous memory into the address space. Key for GPU tools:

```c
// Map a file into memory (e.g., reading a GPU binary/cubin)
void *p = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

// Anonymous mapping (like malloc but page-aligned, good for large buffers)
void *buf = mmap(NULL, size, PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

// Shared mapping (IPC or device memory)
void *shared = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
```

GPU drivers use `mmap` on the device file to map BARs (device memory) or command buffers into user space.

### Copy-on-Write (COW)

After `fork()`, both parent and child share the same physical pages, marked read-only. On a write, a page fault triggers, the kernel copies the page, and updates the faulting process's page table to point to the copy. This is a *minor* fault. COW is also used for `MAP_PRIVATE` file mappings.

### Pinned (Page-Locked) Memory and GPU DMA

**This is a critical interview topic.**

When a GPU performs DMA (Direct Memory Access), its DMA engine reads/writes physical memory addresses. If the OS can freely page out or relocate the host memory, the GPU would DMA to the wrong physical location—or to a page that's been swapped out. The GPU has no mechanism to handle a page fault on host memory (the GPU's DMA engine doesn't participate in the CPU's demand paging).

Therefore, memory used as a DMA source or destination must be **pinned** (locked): the kernel guarantees the pages stay at fixed physical addresses and are never swapped out.

```c
// CUDA-style pinned memory allocation
cudaHostAlloc(&ptr, size, cudaHostAllocDefault);
// Under the hood, the driver:
// 1. Allocates pages (malloc or mmap)
// 2. Calls get_user_pages() / pin_user_pages() to pin them
// 3. Translates to physical/IOMMU addresses for GPU DMA
// 4. May also map them as write-combining for faster CPU writes
```

**Why not pin everything?** Pinned pages cannot be reclaimed by the kernel under memory pressure. Too much pinned memory starves the rest of the system. Also, pinning is expensive—it walks the page tables, faults in any demand-paged pages, and increments reference counts.

**Unified/Managed Memory** (CUDA `cudaMallocManaged`) avoids explicit pinning by using a page-fault-based migration scheme. The driver installs page protections and migrates pages between host and device on demand. But for peak bandwidth, explicit pinned transfers are still faster because they avoid fault handling overhead.

### Interview Q&A

**Q: Walk me through a page table walk for a virtual address on x86-64.**
A: Take a 48-bit virtual address. Bits [47:39] index into the PML4 (Page Map Level 4), which is a 4 KB table of 512 entries pointed to by the CR3 register. That entry gives the physical address of a PDPT (Page Directory Pointer Table). Bits [38:30] index into the PDPT to find the PD (Page Directory). Bits [29:21] index into the PD for the PT (Page Table). Finally, bits [20:12] index into the PT, giving the physical frame number. The bottom 12 bits are the offset within the 4 KB page. Each table entry also has permission bits—present, writable, user/supervisor, NX—checked at every level. If any entry has the present bit clear, you get a page fault.
*Follow-up:* Where do huge pages fit in? — For 2 MB huge pages, the PD entry has the "page size" (PS) bit set and directly encodes the physical address of a 2 MB frame. The walk stops one level early. For 1 GB pages, the PDPT entry has the PS bit set. Fewer TLB entries are consumed, which is a big win for large contiguous allocations like GPU buffers.

**Q: Why does GPU DMA require pinned memory? What goes wrong if you DMA into unpinned memory?**
A: A GPU DMA engine operates on physical addresses. It programs a scatter-gather list of physical pages and reads/writes them directly. If those pages aren't pinned, the OS can swap them out or migrate them at any time—the physical addresses in the DMA descriptor become stale. The GPU would then DMA data into whatever now occupies those frames, corrupting memory. Unlike a CPU load/store, DMA doesn't go through the CPU's MMU, so there's no page fault mechanism to transparently fix it. Pinning ensures the physical-to-virtual mapping is stable for the duration of the transfer.
*Follow-up:* What is `get_user_pages()` and why was `pin_user_pages()` introduced? — `get_user_pages()` was the original API to pin user pages for DMA. But it only elevated the page's reference count, which conflicted with memory management operations like direct I/O and page migration. `pin_user_pages()` was introduced to explicitly distinguish "pinned for DMA" from "temporary GUP reference," allowing the MM subsystem to make correct decisions about page migration and compaction.

**Q: Explain copy-on-write. When does it help, and when does it hurt?**
A: After `fork()`, parent and child share the same physical pages, all marked read-only. When either writes to a page, a page fault fires, the kernel copies that page, updates the writer's page table, and marks the new copy writable. This is hugely beneficial for `fork()` because most child processes quickly call `exec()` and never touch most of the parent's pages—so you avoid copying gigabytes of memory. It hurts when both processes write heavily to shared pages, because every write triggers a fault and a copy, plus TLB invalidation. In GPU contexts, this is a concern if you `fork()` a process with large pinned allocations—the pinned pages might get COW-faulted, and the child's copies aren't pinned.
*Follow-up:* What's the interaction between COW and mmap MAP_PRIVATE? — `MAP_PRIVATE` file mappings are also COW. Reads go to the file's page cache. Writes trigger a copy—the process gets a private copy of that page, and further reads come from the copy, not the file.

**Q: What is a TLB shootdown and when is it a performance concern?**
A: When one core modifies a page table entry—say, unmapping a page—other cores may have cached the old translation in their TLBs. The modifying core sends an IPI (inter-processor interrupt) to every core that might have cached it. Each receiving core stops what it's doing, flushes the relevant TLB entry, and acknowledges. The initiator waits for all acknowledgments. This is expensive: IPIs take microseconds, and the more cores involved, the worse it gets. It's a concern in GPU tools when you're frequently mapping and unmapping buffers, because each unmap triggers a shootdown across all cores running threads of your process.
*Follow-up:* How does PCID help, and does it help with shootdowns? — PCID avoids *full* TLB flushes on context switch but does NOT eliminate shootdowns. When you unmap a page, you still need to invalidate that specific entry on all cores, even with PCID. PCID helps with context switches, not with munmap/mprotect.

**Q: Explain memory overcommit. How does the OOM killer relate to GPU workloads?**
A: Linux overcommits by default—`malloc` and `mmap` succeed even if there isn't enough physical RAM, because the kernel bets that not all pages will be touched. When physical memory actually runs out, the OOM killer selects a process to kill based on an heuristic score (`oom_score`). GPU workloads are vulnerable because they often allocate large host-side buffers. If a machine is running multiple GPU jobs, one job's allocation can trigger OOM that kills a different job. This is why many GPU clusters set `vm.overcommit_memory=2` (strict accounting) or use cgroups to limit per-job memory.
*Follow-up:* What's the difference between `vm.overcommit_memory` values 0, 1, and 2? — 0 is the default heuristic (reject obviously insane allocations). 1 always allows overcommit—never fails an allocation. 2 limits total commit to swap + a fraction of RAM (`overcommit_ratio`). For GPU test infrastructure, 2 is safest because it prevents silent OOM situations during long simulation runs.

---

## 3. DMA and Device I/O

### How a Device DMAs into Host Memory

```
                    CPU writes descriptor      Device reads descriptor
                    to device register          from its own memory
                         │                            │
  ┌──────────┐     ┌─────▼────┐     ┌───────────┐    │     ┌────────────┐
  │   CPU    │────>│  PCIe    │────>│  Device   │◄───┘     │  System    │
  │          │     │  Bus     │     │  DMA Eng  │─────────>│  RAM       │
  └──────────┘     └──────────┘     └───────────┘          └────────────┘
                                    Device issues            DMA writes
                                    memory read/write        directly to
                                    transactions on PCIe     physical RAM
```

1. The driver allocates a host buffer and pins it.
2. The driver translates virtual addresses to physical (or IOMMU) addresses.
3. The driver writes a DMA descriptor (source addr, dest addr, length) to the device.
4. The device's DMA engine issues PCIe memory read/write transactions directly to system RAM—bypassing the CPU entirely.
5. When complete, the device raises an interrupt (MSI/MSI-X).

### IOMMU and Address Translation

The IOMMU (Intel VT-d, AMD-Vi) sits between the device and physical memory. It translates device-visible "IOVA" (I/O Virtual Addresses) to physical addresses, similar to how the CPU MMU translates virtual to physical. Benefits:
- **Security**: A buggy device can't scribble on arbitrary physical memory—only pages mapped in its IOMMU page table.
- **Scatter-gather**: The IOMMU can make physically discontiguous pages appear contiguous to the device, simplifying DMA descriptors.
- **Virtualization**: Pass-through devices in VMs use the IOMMU to translate guest physical addresses.

GPU drivers configure IOMMU mappings for pinned host buffers. If the IOMMU translation misses (IOTLB miss), there's a performance penalty—this is why large contiguous allocations (huge pages) help DMA performance.

### Coherent vs Streaming DMA Mappings

In the Linux DMA API:
- **Coherent** (`dma_alloc_coherent`): CPU and device see a consistent view of memory at all times. Implemented by mapping the memory as uncacheable or using hardware coherence (e.g., PCIe with cache-coherent interconnect). Simpler to use, but uncacheable memory is slow for CPU access. Used for command rings, descriptor tables.
- **Streaming** (`dma_map_single/sg`): Memory is cacheable on the CPU side, but you must explicitly sync before and after DMA. Call `dma_sync_single_for_device()` before device access (flushes CPU caches so device sees latest data) and `dma_sync_single_for_cpu()` after (invalidates CPU caches so CPU sees device-written data). Used for bulk data transfers where performance matters.

### Cache Flush/Invalidate Concerns

On x86, PCIe DMA is cache-coherent (the CPU snoops DMA accesses), so explicit cache flushes are usually unnecessary. But on ARM and other architectures, DMA may not be cache-coherent, and the driver must explicitly flush/invalidate caches. This is a portability concern for NVIDIA drivers that also target ARM (Tegra/Jetson).

Even on x86, write-combining buffers (used for GPU BARs) are not coherent—you must use `wmb()` (write memory barrier) or `sfence` to ensure writes are flushed to the device.

### Interview Q&A

**Q: Walk me through how a GPU DMA transfer of a host buffer to device memory works.**
A: The application calls something like `cudaMemcpy(devicePtr, hostPtr, size, HostToDevice)`. The driver first ensures the host buffer is pinned—if it's not already, it pins it via `pin_user_pages()`. Then it translates the host virtual addresses into physical or IOMMU addresses and builds a scatter-gather list if the pages aren't contiguous. It writes a DMA descriptor to the GPU's command buffer or a copy engine channel, specifying source (host physical addresses), destination (GPU virtual address mapped to VRAM), and length. The GPU's copy engine reads from host memory over PCIe and writes to its local VRAM. When complete, the GPU signals a fence/semaphore, and the driver can report completion to the application.
*Follow-up:* Why might a DMA transfer be slower than expected? — Several reasons: the host buffer wasn't pre-pinned (so pinning happens synchronously), IOMMU IOTLB misses cause page-walk stalls on the device side, PCIe link isn't at full width/speed (x8 instead of x16), NUMA effects (buffer on a remote socket from the GPU), or CPU cache line bouncing if the CPU is also writing to the buffer concurrently.

**Q: What's the IOMMU and why does a GPU driver care about it?**
A: The IOMMU translates device-visible I/O virtual addresses to physical addresses, providing memory isolation and scatter-gather capability. The GPU driver cares because every host buffer exposed to the GPU for DMA must be mapped in the IOMMU. If the IOMMU is enabled (which it is in most server environments), the driver must call `dma_map_sg()` or equivalent to create IOMMU mappings. If these mappings are fragmented or the IOTLB is thrashing, DMA performance degrades. The driver also needs IOMMU awareness for GPU passthrough in virtualized environments (SR-IOV, VFIO), where the IOMMU provides isolation between virtual functions.
*Follow-up:* What is an IOTLB miss and how does it affect GPU DMA throughput? — Just like a CPU TLB miss triggers a page table walk, an IOTLB miss on the IOMMU triggers an I/O page table walk, stalling the DMA transaction. For GPU workloads that touch many small buffers, this can significantly reduce effective bandwidth. Using huge pages in IOMMU mappings (when supported) reduces IOTLB pressure.

**Q: Explain the difference between coherent and streaming DMA mappings.**
A: Coherent mappings guarantee that any write by the CPU is immediately visible to the device and vice versa—typically by marking the memory uncacheable. This is simple but slow for CPU access. Streaming mappings use cacheable memory but require the driver to explicitly synchronize: flush CPU caches before device reads, invalidate CPU caches before CPU reads after device writes. Streaming is used for bulk data transfers because the CPU accesses the buffer briefly and then the device uses it for a long time, so the sync overhead is amortized. GPU drivers use coherent mappings for command rings and streaming mappings for data buffers.
*Follow-up:* On x86, DMA is cache-coherent via snooping—so why do streaming mappings still exist? — Even though x86 snoops, the DMA API abstractions exist for portability. Also, the API handles bounce buffers (for devices that can't address all of RAM), IOMMU mapping, and ensures proper ordering. The cache sync calls may be no-ops on x86 but are critical on ARM. GPU drivers (especially NVIDIA's, which target Tegra/ARM) must use them correctly.

**Q: What happens if you forget to pin memory before initiating a DMA transfer?**
A: If the memory isn't pinned, the OS can swap or migrate the pages at any time. The physical addresses the device was given become stale. Best case: the DMA reads garbage. Worst case: the DMA writes to physical memory now owned by another process or the kernel, causing silent data corruption or a crash. In practice, GPU driver stacks don't let you make this mistake—`cudaMemcpy` from a non-pinned buffer triggers an internal pinning (slow path) or a bounce-buffer copy. But if you're writing a kernel-mode driver component, forgetting to pin is a serious bug.
*Follow-up:* What is a bounce buffer? — When a device can only DMA to/from low physical memory (legacy 32-bit DMA mask), but the data is in high memory, the kernel allocates a "bounce buffer" in low memory, copies data there, DMAs from it, then copies back. It's a performance penalty. Modern GPUs with 64-bit DMA masks avoid this, but the mechanism still exists in the kernel's DMA layer.

**Q: Why is write-combining important for device memory writes?**
A: When the CPU writes to a device's MMIO region (e.g., GPU BAR), each store typically generates a separate PCIe write transaction—4 or 8 bytes each. PCIe transactions have significant header overhead, so many small writes are very inefficient. Write-combining (WC) allows the CPU to buffer multiple writes in a WC buffer and flush them as a single larger transaction (up to a cache line or more). This dramatically improves throughput for operations like writing doorbell registers or uploading small data to the GPU. However, WC memory is weakly ordered—the CPU may reorder writes, so you need `sfence` or `wmb()` when ordering matters.
*Follow-up:* How does the CPU know to use write-combining for a specific address range? — The memory type is set in the page table entry's PAT (Page Attribute Table) bits or in the MTRRs (Memory Type Range Registers). The driver requests WC when mapping a BAR region via `ioremap_wc()` in the kernel or by setting appropriate flags on the `mmap` of the device file.

---

## 4. PCIe as a Transport

### Config Space and BARs

Every PCIe device has a 256-byte (legacy) or 4 KB (extended) **configuration space** containing device ID, vendor ID, status, command registers, capability lists, and **Base Address Registers (BARs)**.

BARs define regions of device memory or I/O ports that the BIOS/OS maps into the CPU's physical address space. A GPU typically has:
- **BAR0**: MMIO registers (control/status)
- **BAR1**: Framebuffer / VRAM aperture (may be 256 MB or larger with resizable BAR)
- **BAR2/3**: Additional register space or doorbell region

```
┌──────────────────────────────────────────────────────┐
│  PCIe Config Space (first 64 bytes)                   │
│  ┌──────┬──────┬──────────┬──────────┐               │
│  │VendID│DevID │  Status  │ Command  │  (offset 0x0) │
│  ├──────┴──────┴──────────┴──────────┤               │
│  │  ...                              │               │
│  ├───────────────────────────────────┤               │
│  │  BAR0  (MMIO registers)           │  (offset 0x10)│
│  │  BAR1  (VRAM aperture)            │  (offset 0x14)│
│  │  BAR2  (doorbells / other)        │  (offset 0x18)│
│  │  ...                              │               │
│  └───────────────────────────────────┘               │
└──────────────────────────────────────────────────────┘
```

### MMIO vs Port I/O

- **MMIO** (Memory-Mapped I/O): Device registers are mapped into the CPU's physical address space. Accessed via regular load/store instructions (but uncacheable or write-combining). This is what modern GPUs use almost exclusively.
- **Port I/O**: Legacy x86 mechanism using `IN`/`OUT` instructions on I/O port numbers. Only used for legacy PCI config space access. Slow, limited to 64K ports.

### Write-Combining and Doorbell Registers

A **doorbell register** is an MMIO address that, when written, triggers the device to take an action (e.g., "process the next N commands in the ring buffer"). The write itself carries information (typically a head/tail pointer).

Doorbell writes should be a single PCIe transaction to avoid tearing. Write-combining helps batch setup writes but the doorbell itself is often in an uncacheable region to ensure immediate delivery.

### MSI/MSI-X Interrupts

Legacy interrupts use shared physical interrupt lines—slow and requires the OS to poll each device. **MSI** (Message Signaled Interrupts) uses a PCIe write transaction to a specific memory address (programmed by the OS) to signal an interrupt. **MSI-X** extends this to support up to 2048 interrupt vectors, each with its own address and data—enabling per-queue interrupts.

GPU drivers use MSI-X to differentiate interrupts by engine (e.g., graphics engine done, copy engine done, fault interrupt).

### PCIe Bandwidth and Latency

| Config | Bandwidth (per direction) | Round-trip latency |
|--------|---------------------------|-------------------|
| Gen3 x16 | ~16 GB/s | ~0.5–1 µs |
| Gen4 x16 | ~32 GB/s | ~0.5–1 µs |
| Gen5 x16 | ~64 GB/s | ~0.5–1 µs |

Practical throughput is lower due to protocol overhead (TLP headers, DLLP, framing), flow control, and ordering rules. Host-to-device (CPU writes to GPU) is typically slower than device-to-host (GPU reads from CPU) due to:
1. CPU writes are posted (fire-and-forget), but small writes waste bandwidth.
2. CPU reads from device are non-posted (requires a completion), adding round-trip latency per read.
3. Write-combining can help batch CPU writes but is not always applicable.

### Resizable BAR

Traditionally, BAR1 (the VRAM aperture) was limited to 256 MB, meaning the CPU could only directly access 256 MB of (say) 24 GB of VRAM. Accessing other VRAM required the driver to "window" (remap) regions. **Resizable BAR** (a PCIe capability) allows the BAR to cover the entire VRAM, enabling direct CPU access to all of it. This improves texture upload and CPU-side debugging of GPU memory.

### Interview Q&A

**Q: What are BARs and how does a GPU driver use them?**
A: Base Address Registers are entries in the PCIe config space that define windows into device memory. The BIOS or OS assigns physical address ranges to them during enumeration. A GPU typically exposes BAR0 for control/status registers (MMIO), BAR1 as an aperture into VRAM, and possibly BAR2 for doorbells. The kernel driver maps these into kernel virtual address space via `ioremap` or `ioremap_wc`, and may also expose them to user space via `mmap` on the device file. When the driver writes to a BAR0 address, it's actually sending a PCIe memory write transaction to the GPU.
*Follow-up:* What is resizable BAR and why does it matter? — Without resizable BAR, BAR1 is often 256 MB even though the GPU has gigabytes of VRAM. Accessing VRAM beyond the BAR window requires the driver to remap the window, which is slow. Resizable BAR lets the BAR cover all VRAM, so the CPU (and tools like debuggers) can directly access any VRAM location. This is crucial for GPU debugging tools that need to inspect arbitrary GPU memory.

**Q: Why are host-to-device transfers expensive?**
A: Several factors. First, the CPU generates writes that go over PCIe, which has non-trivial latency (~500 ns–1 µs per transaction). If the writes are small (8 bytes each), the per-transaction overhead dominates and you waste bandwidth. Write-combining helps but isn't always used. Second, CPU reads from device memory (e.g., reading back GPU registers) are non-posted: the CPU issues a read, waits for the PCIe completion packet, and stalls. Third, NUMA topology matters—if the CPU socket is far from the GPU's PCIe root port, every transaction crosses the inter-socket link. Finally, the GPU's DMA engine pulling from host memory (device-initiated) is generally more efficient than CPU pushing because the GPU can pipeline many outstanding reads.
*Follow-up:* How does a GPU driver mitigate these costs? — Use DMA (device-initiated transfers) instead of CPU writes. Use pinned memory to avoid copy overhead. Use staging buffers with write-combining. Batch small transfers. Use CUDA streams or copy engines for asynchronous overlapping transfers.

**Q: Explain MSI-X and why GPUs use it.**
A: MSI-X allows a device to signal up to 2048 distinct interrupt vectors by writing different data to different memory addresses. Each vector can be targeted to a different CPU core. GPUs use this because they have multiple engines (graphics, compute, copy, video decode) that complete work independently. With MSI-X, each engine can have its own interrupt, handled on a specific core, avoiding the serialization of a single shared interrupt line. The driver registers different handlers for different vectors—a copy-engine-done interrupt goes to a different handler than a GPU page fault interrupt.
*Follow-up:* How does the OS route an MSI-X interrupt to a specific CPU core? — The MSI-X table entry contains a target address that encodes the APIC ID of the destination core. The OS writes these entries during driver initialization. Linux also has IRQ affinity (`/proc/irq/N/smp_affinity`) to control which core handles each interrupt.

**Q: What is write-combining and when would you use it vs uncacheable MMIO?**
A: Write-combining is a memory type that lets the CPU buffer multiple stores and flush them as a single burst transaction. It's weakly ordered—the CPU may coalesce or reorder writes. You use it for bulk writes to device memory (like uploading data through a BAR) where throughput matters more than ordering. Uncacheable (UC) MMIO is strictly ordered—every store generates an immediate transaction. You use UC for control registers where ordering and immediate visibility matter (e.g., writing a status register then a kick register—they must arrive in order). Doorbell registers are typically UC; BAR1 VRAM apertures might be WC.
*Follow-up:* What instruction ensures a write-combining buffer is flushed? — `SFENCE` (store fence) on x86. In the Linux kernel, `wmb()` or `mmiowb()`. After writing to a WC region, you issue an `SFENCE` to ensure all buffered writes are flushed to the device before proceeding.

**Q: How does PCIe config space access work?**
A: Legacy PCI used port I/O (CF8/CFC mechanism): write the bus/device/function/offset to port 0xCF8, then read/write port 0xCFC. PCIe introduced ECAM (Enhanced Configuration Access Mechanism), which maps the entire config space of all devices into a memory region. Each device's 4 KB config space is at a fixed offset based on bus/device/function number. The OS accesses config space with regular memory loads/stores to the ECAM region. In Linux, user space can read config space via `lspci` or `sysfs` (`/sys/bus/pci/devices/.../config`).
*Follow-up:* What's in the capability list? — A linked list in config space starting at the capability pointer. Each entry has a capability ID and data. Examples: MSI/MSI-X capability (interrupt configuration), power management, PCIe extended capabilities (AER, resizable BAR, SR-IOV). `lspci -vv` dumps all of these.

---

## 5. Memory Hierarchy and Coherence (CPU Side)

### Cache Lines and Hierarchy

Modern x86 CPUs have 64-byte cache lines and typically:
- **L1** (per-core): 32–48 KB I-cache + 32–48 KB D-cache, ~4 cycle latency
- **L2** (per-core): 256 KB–1.25 MB, ~12 cycle latency
- **L3** (shared): 16–96 MB, ~40 cycle latency
- **Main memory**: ~100+ ns (~200+ cycles)

Caches can be **inclusive** (L3 contains a copy of everything in L1/L2—simplifies coherence, used in older Intel) or **exclusive/non-inclusive** (L3 doesn't duplicate L1/L2 data—more effective capacity, used in AMD and newer Intel).

### MESI Protocol

Cache coherence on multi-core CPUs is maintained by the MESI protocol (or variants like MESIF/MOESI):

```
        ┌────────────┐
        │  Modified   │  Only copy, dirty — must write back before eviction
        └──────┬─────┘
               │ Remote read (snoop) → write back, go to Shared
               ▼
        ┌────────────┐
        │  Exclusive  │  Only copy, clean — can transition to Modified on write
        └──────┬─────┘
               │ Remote read (snoop) → go to Shared
               ▼
        ┌────────────┐
        │   Shared    │  Multiple copies, clean — must invalidate others to write
        └──────┬─────┘
               │ All copies invalidated
               ▼
        ┌────────────┐
        │  Invalid    │  No valid data — must fetch on access
        └────────────┘
```

### False Sharing

When two threads write to different variables that happen to reside on the same 64-byte cache line, each write invalidates the other core's copy, causing constant cache misses—even though they're logically independent. This is **false sharing**.

```cpp
// BAD: likely on the same cache line
struct Counters {
    uint64_t count_a;  // Thread A writes this
    uint64_t count_b;  // Thread B writes this
};

// GOOD: pad to separate cache lines
struct Counters {
    alignas(64) uint64_t count_a;
    alignas(64) uint64_t count_b;
};
```

This matters in GPU tools: if your profiler has per-thread counters on the same cache line, you'll get mysterious slowdowns.

### Memory Barriers

CPUs reorder loads and stores for performance. x86 is relatively strongly ordered (TSO: Total Store Order), but:
- Stores can be reordered after loads (store buffer).
- Non-temporal stores and WC writes have no ordering guarantees.
- On ARM/POWER, everything can be reordered.

Barriers enforce ordering:
- `mfence` / `__sync_synchronize()` — full barrier (expensive, rarely needed on x86)
- `sfence` — orders stores (needed for WC/non-temporal)
- `lfence` — orders loads (rarely needed on x86 for correctness, used for speculation mitigation)
- C++ `std::atomic` with memory orderings maps to the right barriers.

### NUMA

In multi-socket systems, each socket has its own memory controller. Memory attached to socket 0 is "local" to cores on socket 0 and "remote" to socket 1. Remote access goes over the inter-socket link (UPI/Infinity Fabric) and is ~1.5–2x slower.

**GPU implication**: If a GPU is connected to socket 0's PCIe, but the pinned buffer is allocated on socket 1's memory, every DMA transfer crosses the inter-socket link, reducing bandwidth. Always allocate GPU buffers on the NUMA node closest to the GPU:

```c
// Linux: allocate on specific NUMA node
numactl --cpunodebind=0 --membind=0 ./my_gpu_app

// Or programmatically:
#include <numa.h>
void *buf = numa_alloc_onnode(size, gpu_numa_node);
```

### Interview Q&A

**Q: Explain false sharing and how you'd detect and fix it.**
A: False sharing occurs when two threads write to different variables that share a cache line. Each write invalidates the other core's cached copy, causing cache misses even though the variables are logically independent. You'd detect it with `perf c2c` (cache-to-cache) on Linux, which shows cache lines with high cross-core invalidation traffic. To fix it, align variables to cache line boundaries using `alignas(64)` or padding. In GPU tool code, this often shows up in per-thread statistics counters or per-engine status flags that are packed into the same struct.
*Follow-up:* What's the approximate cost of a false-sharing miss? — It's roughly the same as an L3 miss on a single-socket system (40+ cycles) because the data must be fetched from the other core's cache via the coherence protocol. On a multi-socket system, it's even worse because the snoop goes over the inter-socket link (100+ ns).

**Q: Explain the MESI protocol at a high level.**
A: MESI tracks each cache line in one of four states. Modified means this core has the only copy and it's dirty—the core must write it back before anyone else can read it. Exclusive means this core has the only copy but it's clean—it can silently transition to Modified on a write. Shared means multiple cores have clean copies—to write, a core must first invalidate all other copies. Invalid means the cache line is not present. When core A writes to a Shared line, it broadcasts an invalidation; other cores mark their copies Invalid; core A transitions to Modified. This is all handled by hardware—the CPU's coherence protocol issues snoops on the interconnect.
*Follow-up:* What's the difference between MESI and MOESI? — MOESI adds an "Owned" state: the owner has a dirty copy and other caches have Shared copies. On a read from another core, instead of writing back to memory and going to Shared, the owner directly supplies the data and stays in Owned state. This avoids a memory write-back, which is AMD's approach. Intel uses MESIF which adds a "Forward" state for a similar optimization.

**Q: How does NUMA affect GPU DMA performance?**
A: If a GPU is attached to PCIe lanes on socket 0, DMA transfers target memory through socket 0's memory controller. If the pinned buffer is allocated on socket 1's memory, each DMA read/write crosses the inter-socket link (UPI or Infinity Fabric), which adds latency and reduces bandwidth by 30–50%. The fix is to ensure host buffers are allocated on the same NUMA node as the GPU. CUDA's `cudaHostAlloc` is NUMA-aware in recent driver versions, but custom tools may need explicit `mbind` or `numa_alloc_onnode` to control placement.
*Follow-up:* How do you determine which NUMA node a GPU is on? — Check `/sys/bus/pci/devices/<GPU BDF>/numa_node` on Linux, or use `nvidia-smi topo -m` which shows the GPU's NUMA affinity and interconnect topology.

**Q: When do you need memory barriers on x86?**
A: x86 has a strong memory model (TSO), so loads are not reordered with loads, and stores are not reordered with stores—for regular cacheable memory. You need barriers in three cases: (1) WC or non-temporal stores, which bypass the cache and have no ordering—use `sfence` after them; (2) when you need a store to be visible before a subsequent load from a different address—TSO doesn't guarantee store-load ordering, so you need `mfence` or a locked instruction; (3) compiler barriers (`asm volatile("" ::: "memory")` or `std::atomic_thread_fence`) to prevent the compiler from reordering. In GPU driver code, `wmb()` after writing to a WC pushbuffer and before ringing the doorbell is the classic example.
*Follow-up:* What's the difference between a compiler barrier and a hardware barrier? — A compiler barrier (e.g., `asm volatile("" ::: "memory")`) prevents the compiler from reordering loads/stores across it but generates no instructions—it's a compile-time-only constraint. A hardware barrier (e.g., `mfence`) emits an actual instruction that prevents the CPU's out-of-order execution from reordering memory operations across it. You often need both—the compiler barrier to stop the compiler, and the hardware barrier to stop the CPU.

**Q: What does `perf c2c` show and when would you use it?**
A: `perf c2c` (cache-to-cache) profiles cache line contention. It records memory access samples and identifies cache lines with high hitm (hit-modified) rates—meaning one core accessed a line that was in Modified state on another core. It shows the offending data addresses, the source code lines, and the threads involved. You'd use it when you suspect false sharing or true sharing contention is causing performance issues—for example, if your GPU profiling tool's throughput drops with more threads despite good parallelism.
*Follow-up:* What's a "hitm"? — "Hit modified" means the requesting core's load hit a cache line that was in Modified state on another core. The data must be transferred from that other core's cache, which is expensive. High hitm rates indicate contention. `perf c2c` distinguishes local hitm (same socket) from remote hitm (cross-socket), with remote being much more expensive.

---

## 6. GPU Driver Architecture

> **Note**: The following describes the *conceptual public model* based on open-source drivers (nouveau, Mesa/NVK), public NVIDIA documentation, and published papers. Internal NVIDIA driver details are proprietary. Interviewers expect you to know this model and to be explicit about what is publicly known vs. internal.

### UMD vs KMD Split

```
 User Space                              Kernel Space
┌──────────────────────────┐     ┌──────────────────────────────┐
│  Application             │     │                              │
│  (CUDA / OpenGL / VK)    │     │  Kernel-Mode Driver (KMD)    │
│         │                │     │  ┌────────────────────────┐  │
│         ▼                │     │  │ Memory Manager         │  │
│  ┌──────────────────┐    │     │  │ (alloc, pin, map,      │  │
│  │ User-Mode Driver │    │     │  │  GPU page tables)      │  │
│  │ (UMD)            │    │     │  ├────────────────────────┤  │
│  │ ┌──────────────┐ │    │     │  │ Command Submission     │  │
│  │ │ API state    │ │    │     │  │ (validate, schedule,   │  │
│  │ │ tracking     │ │    │     │  │  write to GPFIFO)      │  │
│  │ ├──────────────┤ │    │     │  ├────────────────────────┤  │
│  │ │ Shader       │ │    │     │  │ Interrupt Handler      │  │
│  │ │ compiler     │ │    │     │  │ (fence completion,     │  │
│  │ ├──────────────┤ │    │     │  │  fault handling)       │  │
│  │ │ Command buf  │ │    │     │  ├────────────────────────┤  │
│  │ │ builder      │ │    │     │  │ GPU Scheduler          │  │
│  │ └──────┬───────┘ │    │     │  │ (channel/context       │  │
│  └────────┼─────────┘    │     │  │  switching, priority)  │  │
│           │ ioctl()      │     │  └────────────────────────┘  │
└───────────┼──────────────┘     └──────────────┬───────────────┘
            │                                    │
            └──────► /dev/nvidia0 ◄──────────────┘
                                                 │
                                           ┌─────▼─────┐
                                           │  GPU HW   │
                                           └───────────┘
```

**User-Mode Driver (UMD)** responsibilities:
- **API state tracking**: Translating OpenGL/Vulkan/CUDA API calls into internal state.
- **Shader compilation**: Compiling GLSL/HLSL/PTX into GPU machine code (SASS for NVIDIA). This is a major component.
- **Command buffer building**: Encoding GPU commands ("methods") into a pushbuffer—a sequence of (address, data) pairs that program GPU registers and trigger operations.

**Kernel-Mode Driver (KMD)** responsibilities:
- **Memory management**: Allocating GPU memory (VRAM), managing GPU page tables (GMMU), pinning host pages, mapping buffers.
- **Command submission**: Validating and scheduling pushbuffers, writing to the GPFIFO, ringing doorbells.
- **Interrupt handling**: Processing GPU completion interrupts, signaling fences.
- **GPU scheduling**: Multiplexing GPU channels across contexts/applications.

### Command/Push Buffers and Ring Buffers

The GPU consumes commands from a **pushbuffer** (or command buffer). This is a linear buffer of GPU commands in host or device memory. Each command is a **(method, data)** pair:

- **Method**: An offset into the GPU's register space. Writing to a method programs a specific GPU function.
- **Data**: The value to write to that method.

```
Pushbuffer layout (simplified):
┌──────────┬──────────┬──────────┬──────────┬─────┐
│ Header   │ Method 0 │ Data 0   │ Method 1 │ ... │
│ (count,  │ (reg     │ (value)  │ (reg     │     │
│  subchan)│  offset) │          │  offset) │     │
└──────────┴──────────┴──────────┴──────────┴─────┘
```

The **GPFIFO** (General Purpose FIFO) is a ring buffer of **GPFIFO entries**, where each entry points to a pushbuffer segment (address + length). The GPU engine reads GPFIFO entries sequentially, fetches the referenced pushbuffer, and executes the methods.

```
                    ┌─────────── GPFIFO (ring buffer) ──────────┐
                    │                                            │
  CPU writes  ──►   │ [PB addr|len] [PB addr|len] [PB addr|len] │
  new entries       │     │              │              │        │
                    └─────┼──────────────┼──────────────┼────────┘
                          │              │              │
                          ▼              ▼              ▼
                    ┌──────────┐  ┌──────────┐  ┌──────────┐
                    │Pushbuf 0 │  │Pushbuf 1 │  │Pushbuf 2 │
                    │(methods) │  │(methods) │  │(methods) │
                    └──────────┘  └──────────┘  └──────────┘
                          │
                    GPU fetches and
                    executes methods
```

### Doorbells, Fences, and Semaphores

After writing new GPFIFO entries, the CPU writes to a **doorbell register** (MMIO in a BAR) to notify the GPU: "there are N new entries to process." This is a single PCIe write.

**Fences/Semaphores**: The GPU writes a value to a memory location (in host or device memory) when it reaches a certain point in the pushbuffer. The CPU polls or waits on this location. This is how completion is tracked:

```c
// Conceptual fence flow
// 1. UMD writes commands to pushbuffer, including:
//    "When you reach here, write value 42 to address FENCE_ADDR"
// 2. UMD submits pushbuffer via ioctl → KMD writes GPFIFO entry → doorbell
// 3. CPU polls FENCE_ADDR until it reads 42 (or uses interrupt-based wait)
```

### GPU Virtual Memory (GMMU)

Modern NVIDIA GPUs have their own MMU (the GMMU—GPU Memory Management Unit). GPU programs use GPU virtual addresses, and the GMMU translates them to physical VRAM or system memory addresses via GPU page tables. This enables:
- GPU programs to use contiguous virtual address ranges backed by discontiguous physical memory.
- Multiple GPU contexts to have isolated address spaces.
- **Unified/Managed Memory**: Mapping both host and device memory into a single GPU virtual address space, with page migration handled by faults.

The GPU page table structure is multi-level (similar to CPU page tables but with different sizes and formats, which are architecture-specific and partially documented in NVIDIA's open-gpu-doc repository).

### Unified/Managed Memory

CUDA Unified Memory (`cudaMallocManaged`) provides a single pointer accessible from both CPU and GPU. Under the hood:
1. The driver allocates pages and maps them in both CPU and GPU page tables.
2. Pages may reside on host or device. Initially, they might be on the device.
3. When the CPU accesses a device-resident page, a CPU page fault triggers; the driver migrates the page to host memory.
4. When the GPU accesses a host-resident page, a GPU page fault triggers (on supported architectures, Pascal+); the driver migrates the page to device memory.
5. Prefetch hints (`cudaMemPrefetchAsync`) avoid fault-driven migration.

This is convenient but has higher overhead than explicit management due to fault handling and migration latency.

### How a Kernel Launch Reaches the Hardware (End-to-End)

```
 Application: kernel<<<grid, block>>>(args)
       │
       ▼
 CUDA Runtime: resolve kernel function, set up parameters
       │
       ▼
 UMD: Build pushbuffer containing:
   - Set shader program address (method writes)
   - Set grid/block dimensions
   - Set parameter memory (constant buffer) with kernel args
   - Set launch method (triggers execution on the GPU)
   - Optionally insert fence: "write value N to FENCE_ADDR when done"
       │
       ▼
 UMD → KMD (ioctl): Submit pushbuffer
       │
       ▼
 KMD: Validate submission, write GPFIFO entry (pushbuffer addr + length)
      into the channel's GPFIFO ring buffer
       │
       ▼
 KMD: Write to doorbell register (MMIO) → PCIe write to GPU
       │
       ▼
 GPU Host Interface: Reads GPFIFO entry, fetches pushbuffer from memory
       │
       ▼
 GPU Command Processor: Decodes methods, programs shader engine registers
       │
       ▼
 GPU Compute Engine: Launches thread blocks (warps) onto SMs
       │
       ▼
 GPU (on completion): Writes fence value to FENCE_ADDR
       │
       ▼
 CPU: Detects fence completion (poll or interrupt) → reports to application
```

### Interview Q&A

**Q: Describe the split between a GPU's user-mode driver and kernel-mode driver.**
A: The UMD runs in the application's address space and handles API translation, shader compilation, and command buffer construction. It takes high-level API calls and converts them into GPU-specific method sequences in a pushbuffer. The KMD runs in the kernel and handles privileged operations: allocating GPU memory, managing GPU page tables, validating and submitting pushbuffers to the hardware (writing GPFIFO entries and ringing doorbells), handling GPU interrupts, and scheduling GPU contexts. The split exists because shader compilation is CPU-intensive and doesn't need kernel privilege, while memory management and hardware access do.
*Follow-up:* Why is the shader compiler in user space? — It's CPU-intensive (compilation can take milliseconds to seconds), and running it in kernel space would be risky—a compiler bug could crash the kernel. In user space, a compiler crash only affects the application. It also simplifies debugging and allows different API layers (CUDA, Vulkan, OpenGL) to have their own compiler front-ends while sharing the KMD.

**Q: What is a pushbuffer and what is a "method" in NVIDIA GPU terminology?**
A: A pushbuffer is a buffer of GPU commands, where each command is essentially a write to a GPU register. A "method" is the register offset within a GPU engine's register space. The pushbuffer encodes (method, data) pairs: "write this data to this method offset." Methods control everything: setting the shader program address, configuring rasterizer state, setting grid dimensions, triggering a kernel launch. The term "method" comes from NVIDIA's object-oriented hardware model where GPU engines expose "objects" with "methods" (register writes). This encoding is documented in the open-gpu-doc headers as class methods.
*Follow-up:* What is a subchannel? — A pushbuffer method header includes a subchannel field (0–7) that selects which GPU object/engine the method targets. The driver binds objects (e.g., 3D engine, compute engine, copy engine) to subchannels, then addresses methods on each via the subchannel selector. This allows a single pushbuffer to interleave commands to different engines.

**Q: Explain the GPFIFO/pushbuffer submission model.**
A: The GPFIFO is a ring buffer in memory (host or device) that contains entries, each pointing to a pushbuffer segment (address + length). When the UMD has built a pushbuffer, it submits it via an ioctl to the KMD. The KMD writes a new GPFIFO entry with the pushbuffer's address and length, then writes to a doorbell register to tell the GPU "there are new entries." The GPU's host interface fetches GPFIFO entries, follows the pointer to fetch the pushbuffer, and executes the methods. The ring buffer structure means the GPU can be executing one pushbuffer while the CPU is building the next—enabling pipelining.
*Follow-up:* What happens if the GPFIFO ring is full? — The CPU must wait (spin or block) until the GPU consumes some entries and the tail pointer advances. The driver typically tracks a fence or semaphore at the GPU's current position to know when entries are freed. This is a back-pressure mechanism—if the CPU is producing work faster than the GPU can consume it, submission stalls.

**Q: How does GPU virtual memory (GMMU) work at a high level?**
A: The GMMU is the GPU's MMU. It translates GPU virtual addresses to physical addresses (VRAM or system memory) using multi-level page tables stored in VRAM. Each GPU context has its own set of page tables, providing address space isolation. The KMD manages these page tables—when you allocate GPU memory (`cudaMalloc`), the KMD allocates physical VRAM and creates page table entries mapping a GPU virtual range to it. The GMMU has its own TLB. Page table formats and levels are architecture-specific and partially documented in NVIDIA's open-gpu-doc repo. Unified Memory extends this by allowing GPU page tables to point to host memory and handling faults when pages need to migrate.
*Follow-up:* What happens on a GPU page fault? — On architectures that support it (Pascal+), if the GPU accesses a page not currently mapped or not resident, the GMMU raises a fault interrupt. The KMD's interrupt handler processes it—migrating the page from host to device, updating GPU page tables, and replaying the faulting access. This is the mechanism behind CUDA Unified Memory's automatic migration. It's conceptually similar to CPU demand paging but much more expensive due to PCIe transfer latency.

**Q: Walk me through what happens end-to-end when a CUDA kernel is launched.**
A: The application calls `kernel<<<grid, block>>>(args)`. The CUDA runtime resolves the kernel's device function pointer and marshals arguments. The UMD encodes this into a pushbuffer: method writes to set the shader program address, configure the grid and block dimensions, write kernel arguments into a constant buffer, and trigger the launch method. This pushbuffer is submitted via ioctl to the KMD, which validates it, writes a GPFIFO entry pointing to it, and rings the doorbell. The GPU's host interface reads the GPFIFO entry, fetches the pushbuffer, and the command processor decodes the methods. The compute engine distributes thread blocks to SMs for execution. When the kernel completes, if a fence was inserted, the GPU writes the completion value to the fence address. The CPU (polling or interrupt-driven) detects completion and signals the application. The whole path is: runtime → UMD → ioctl → KMD → GPFIFO → doorbell → GPU fetch → execute → fence → CPU notification.
*Follow-up:* Where are the biggest latency sources in this path? — The ioctl (user-to-kernel transition + validation) is a few microseconds. The doorbell write is a PCIe transaction (~500 ns). The GPU fetching the GPFIFO and pushbuffer from host memory adds a few microseconds. Total launch overhead is typically 5–15 µs for a simple kernel. For CUDA Graphs, the driver pre-builds and validates the entire pushbuffer, eliminating per-launch ioctl overhead.

---

## 7. Concurrency in Systems Software

### Producer-Consumer

The fundamental pattern in GPU command submission: the CPU (producer) generates commands, the GPU (consumer) executes them. Efficiently bridging the two is the core of the driver's submission path.

```c
// Simple bounded producer-consumer with a ring buffer
#define RING_SIZE 1024
struct RingBuffer {
    alignas(64) uint64_t entries[RING_SIZE];
    alignas(64) _Atomic uint32_t head;  // written by producer
    alignas(64) _Atomic uint32_t tail;  // written by consumer
};

void produce(struct RingBuffer *rb, uint64_t value) {
    uint32_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);
    while ((h + 1) % RING_SIZE ==
           atomic_load_explicit(&rb->tail, memory_order_acquire))
        ; // spin: ring full
    rb->entries[h % RING_SIZE] = value;
    atomic_store_explicit(&rb->head, (h + 1) % RING_SIZE, memory_order_release);
}
```

Note the `alignas(64)` to prevent false sharing between head, tail, and entries.

### Lock-Free Ring Buffers and the ABA Problem

Lock-free data structures use atomic operations (CAS—compare-and-swap) instead of locks. A single-producer single-consumer (SPSC) ring buffer is naturally lock-free: the producer only writes `head`, the consumer only writes `tail`.

The **ABA problem** affects lock-free structures using CAS: a thread reads value A, gets preempted, another thread changes A→B→A, then the first thread's CAS succeeds even though the state changed. Solutions:
- **Tagged pointers**: Pack a counter with the pointer (e.g., upper bits of a 64-bit value). Each CAS increments the counter, so A₁ → B₂ → A₃ doesn't match A₁.
- **Hazard pointers**: Before accessing a node, a thread publishes a "hazard pointer" to it. Other threads check hazard pointers before freeing nodes—ensuring no in-use node is freed.
- **RCU (Read-Copy-Update)**: Readers access data without locks. Writers create a new version, swap a pointer, then wait for all existing readers to finish (a "grace period") before freeing the old version. Used extensively in the Linux kernel.

### Spinlocks vs Mutexes

- **Spinlock**: Busy-waits (spins) until the lock is available. Good when the critical section is very short (< ~microsecond) and the holder won't be preempted. Used in kernel interrupt handlers where sleeping is forbidden.
- **Mutex**: Blocks the thread (context switch to another task) if the lock isn't available. Better when the critical section is long or the holder might be preempted. More efficient under contention because waiting threads don't burn CPU.
- **Adaptive mutexes** (used in some kernels and `pthread_mutex` with `PTHREAD_MUTEX_ADAPTIVE_NP`): Spin briefly, then block. Combines the low-latency of spinlocks for uncontended cases with the CPU-friendliness of mutexes under contention.

```c
// Spinlock — correct implementation with atomic test-and-set
typedef struct { _Atomic int locked; } spinlock_t;

void spin_lock(spinlock_t *l) {
    while (atomic_exchange_explicit(&l->locked, 1, memory_order_acquire))
        while (atomic_load_explicit(&l->locked, memory_order_relaxed))
            __builtin_ia32_pause();  // reduce pipeline pressure
}

void spin_unlock(spinlock_t *l) {
    atomic_store_explicit(&l->locked, 0, memory_order_release);
}
```

The double loop (test-and-test-and-set) avoids hammering the cache line with writes during contention—the inner loop does read-only spins, only attempting the expensive `exchange` when the lock appears free.

### Priority Inversion

A low-priority thread holds a lock needed by a high-priority thread. A medium-priority thread preempts the low-priority thread, preventing it from releasing the lock. The high-priority thread is effectively blocked by the medium-priority thread.

Solutions: **priority inheritance** (the low-priority thread temporarily inherits the high-priority thread's priority while holding the lock) or **priority ceiling** (the lock's priority is set to the highest priority of any thread that might acquire it).

### Deadlock and Livelock

**Deadlock** conditions (all four must hold simultaneously):
1. **Mutual exclusion** — Resources cannot be shared.
2. **Hold and wait** — A thread holds one resource while waiting for another.
3. **No preemption** — Resources cannot be forcibly taken.
4. **Circular wait** — A cycle exists in the wait-for graph.

Classic scenario: Thread A holds lock X and waits for lock Y; Thread B holds lock Y and waits for lock X.

Prevention strategies:
- **Lock ordering**: Always acquire locks in a consistent global order (e.g., by address). Break ties deterministically.
- **Lock hierarchies**: Assign levels to locks; never acquire a lower-level lock while holding a higher-level one.
- **Try-lock with backoff**: Use `pthread_mutex_trylock()`; if it fails, release all held locks, back off, and retry.
- **Lock-free designs**: Eliminate locks entirely on the hot path (the GPFIFO approach).

```c
// Deadlock prevention: always lock in address order
void safe_lock_two(pthread_mutex_t *a, pthread_mutex_t *b) {
    if (a < b) {
        pthread_mutex_lock(a);
        pthread_mutex_lock(b);
    } else {
        pthread_mutex_lock(b);
        pthread_mutex_lock(a);
    }
}
```

**Livelock**: Threads keep retrying operations that conflict with each other, making progress impossible. Example: two threads backing off and retrying a CAS in perfect sync. Unlike deadlock, threads are running (not blocked) but achieving nothing. Solution: add randomized backoff.

### Designing for Throughput

Key principles for GPU tool and driver infrastructure:

- **Minimize lock granularity**: Per-channel locks instead of a global driver lock. Lock only the specific data structure being modified.
- **Use lock-free structures for hot paths**: GPFIFO is essentially a lock-free SPSC ring. Tool trace buffers should be lock-free too.
- **Batch operations**: Submit many commands per ioctl, not one. Amortize syscall and kernel entry overhead.
- **Pipeline CPU and GPU work**: CPU builds batch N+1 while GPU executes batch N. Double or triple buffering of pushbuffers.
- **Reduce contention**: Partition data structures by thread (per-thread trace buffers merged periodically) to eliminate sharing.
- **Use appropriate waiting**: Spin for sub-microsecond waits (doorbell response), block for longer waits (GPU completion), use eventfd/futex for efficient wakeup.
- **Minimize allocations on hot paths**: Pre-allocate pools of command buffers, trace entries, etc. `malloc` takes locks internally and can cause contention.

```cpp
// Per-thread trace buffer pattern — zero contention on hot path
thread_local std::vector<TraceEntry> tl_trace_buffer;

void record_event(const TraceEntry& entry) {
    tl_trace_buffer.push_back(entry);  // no locks, no atomics
    if (tl_trace_buffer.size() >= FLUSH_THRESHOLD) {
        // Batch flush to global buffer (takes a lock once)
        global_trace_buffer.append(std::move(tl_trace_buffer));
        tl_trace_buffer.clear();
        tl_trace_buffer.reserve(FLUSH_THRESHOLD);
    }
}
```

### Interview Q&A

**Q: Describe a lock-free single-producer single-consumer ring buffer and why it doesn't need CAS.**
A: In an SPSC ring buffer, the producer only writes the head index and the consumer only writes the tail index. There's no contention on any single variable—no two threads ever write the same location. The producer reads the tail to check if the ring is full, and the consumer reads the head to check if it's empty, but these are read-only from their perspective. With proper memory ordering (release on writes, acquire on reads), this is naturally lock-free. No CAS needed because there's no concurrent modification of any variable. This is exactly the pattern used in GPU GPFIFO: one CPU thread produces entries, one GPU engine consumes them.
*Follow-up:* What changes for a multi-producer ring buffer? — Multiple producers contend on the head index, so you need CAS (or a ticket-based approach) to atomically claim a slot. Each producer CAS-updates the head, writes to its claimed slot, then you need a mechanism to ensure the consumer doesn't read past an uncommitted slot. This is significantly more complex—Disruptor-style designs handle it with separate "published" sequence tracking.

**Q: What is the ABA problem and how do you solve it?**
A: Say you have a lock-free stack. Thread A reads the top pointer as node X, then gets preempted. Thread B pops X, pops Y, pushes X back. Thread A resumes, sees top is still X, and its CAS succeeds—but the stack is now corrupted because Y is lost. The value was A, changed to B, then back to A—hence "ABA." Solutions include tagged pointers (attach a monotonically increasing counter to the pointer so X₁ ≠ X₃), hazard pointers (defer freeing X until no thread is actively using it), or epoch-based reclamation.
*Follow-up:* Why don't SPSC ring buffers suffer from ABA? — Because the head and tail are monotonically increasing indices (modulo ring size), not pointers to nodes. They always move forward. There's no "reuse" scenario where an old index value reappears with different meaning.

**Q: Explain priority inversion with a concrete example.**
A: Imagine three threads: High, Medium, Low. Low acquires a mutex, then gets preempted by Medium (which doesn't need the mutex). High wakes up and tries to acquire the same mutex—it blocks because Low holds it. But Low can't run because Medium is running (higher priority than Low). So High is effectively blocked by Medium, even though Medium has nothing to do with the mutex. The fix is priority inheritance: when High blocks on the mutex, the kernel temporarily boosts Low's priority to High's level so Low can finish and release the mutex. This is what the `PI_FUTEX` mechanism does in Linux.
*Follow-up:* Where might priority inversion show up in a GPU driver? — A GPU interrupt handler thread (high priority) needs to update a data structure protected by a lock held by a lower-priority user-space submission thread. If a medium-priority thread preempts the submission thread, the interrupt handler is delayed. Real-time scheduling and priority inheritance on the submission lock mitigate this.

**Q: What is RCU and why is it used in the Linux kernel?**
A: RCU (Read-Copy-Update) is a synchronization mechanism optimized for read-heavy workloads. Readers access shared data without any locking or atomic operations—just a regular pointer dereference, protected by `rcu_read_lock()` (which just disables preemption). Writers create a new copy of the data, atomically swap the pointer, and then wait for a "grace period"—until all existing readers have finished their critical sections. Then the old copy is freed. RCU is used extensively in the kernel for routing tables, device lists, and module tracking where reads vastly outnumber writes.
*Follow-up:* What's a "grace period"? — A grace period is the time after a writer publishes a new version and before the old version can be freed. It ends when every CPU has passed through a context switch or explicit quiescent state, meaning no reader can still hold a reference to the old data. The kernel tracks this efficiently using per-CPU counters.

**Q: How would you design a high-throughput command submission system for a GPU tool?**
A: Use an SPSC lock-free ring buffer between the user-space tool thread (producer) and the submission thread (consumer). The tool thread writes captured API calls or command records into the ring without locking. The submission thread drains the ring in batches, builds a consolidated pushbuffer, and submits via a single ioctl—amortizing the syscall overhead. Pipeline the work: while the GPU executes batch N, the submission thread prepares batch N+1 and the capture thread fills batch N+2. Align ring entries to cache lines to avoid false sharing. Use eventfd for signaling (empty→non-empty) to avoid busy-waiting when idle.
*Follow-up:* Why batch submissions? — Each ioctl involves a user-to-kernel transition (~microsecond), lock acquisition in the KMD, GPFIFO entry writing, and a doorbell. Submitting 100 commands in one ioctl is far cheaper than 100 ioctls of 1 command each. CUDA and Vulkan both batch internally for this reason.

---

## 8. Binaries and Tooling

### ELF Structure

```
ELF File Layout:
┌──────────────────┐
│ ELF Header       │  magic, class (32/64), type (EXEC/DYN/REL), entry point
├──────────────────┤
│ Program Headers  │  segments: how to load into memory (LOAD, DYNAMIC, INTERP)
│ (Phdr table)     │
├──────────────────┤
│ .text            │  executable code
│ .rodata          │  read-only data (string literals, constants)
│ .data            │  initialized writable data
│ .bss             │  uninitialized data (zero-filled, occupies no file space)
│ .plt / .got      │  procedure linkage table / global offset table
│ .dynamic         │  dynamic linking info
│ .symtab/.strtab  │  symbol table + string table
│ .debug_*         │  DWARF debug info
│ ...              │
├──────────────────┤
│ Section Headers  │  metadata for linker/debugger (not needed at runtime)
│ (Shdr table)     │
└──────────────────┘
```

**Sections vs Segments**: Sections are the linker's view (`.text`, `.data`, etc.)—they organize the file for linking. Segments are the loader's view—they describe how to map sections into memory (e.g., a single `LOAD` segment might contain `.text` + `.rodata` mapped as read-execute).

GPU binaries (cubins) are also ELF files—NVIDIA uses ELF as the container for GPU code, with custom sections for GPU-specific metadata (shader info, register usage, etc.).

### Static vs Dynamic Linking

- **Static linking**: All library code is copied into the executable at link time. Produces a self-contained binary. Larger size, no runtime dependency issues, but no shared memory for library code across processes.
- **Dynamic linking**: Library code lives in shared objects (`.so` files) loaded at runtime. Smaller binaries, shared memory for library code, but requires the correct `.so` versions at runtime.

### Symbol Resolution and Relocation

When the linker encounters a reference to a symbol defined in another object file or library, it must **resolve** (find the definition) and **relocate** (patch the reference with the actual address).

**PLT/GOT** (Procedure Linkage Table / Global Offset Table): For dynamically linked functions, calls go through the PLT. On first call, the PLT entry jumps to the dynamic linker, which resolves the symbol, writes the actual address into the GOT, and patches the PLT to jump directly to the GOT entry on subsequent calls. This is **lazy binding**.

```
First call to printf():
  call printf@PLT
    → PLT stub: jmp *GOT[printf]  (initially points to resolver)
    → Dynamic linker: resolve printf → libc address
    → Update GOT[printf] = actual address
    → Jump to printf

Subsequent calls:
  call printf@PLT
    → PLT stub: jmp *GOT[printf]  (now points directly to printf in libc)
```

### LD_PRELOAD and dlopen

**LD_PRELOAD**: Load a shared library before all others, allowing you to intercept/replace any dynamically linked function. This is how many GPU tools work—they interpose on CUDA API calls:

```bash
# Intercept all cudaMalloc calls with a custom profiler
LD_PRELOAD=./my_profiler.so ./my_cuda_app
```

```c
// my_profiler.so — interpose cudaMalloc
#define _GNU_SOURCE
#include <dlfcn.h>

cudaError_t cudaMalloc(void **devPtr, size_t size) {
    // Call the real cudaMalloc
    static cudaError_t (*real_cudaMalloc)(void**, size_t) = NULL;
    if (!real_cudaMalloc)
        real_cudaMalloc = dlsym(RTLD_NEXT, "cudaMalloc");

    log("cudaMalloc(%zu)\n", size);
    return real_cudaMalloc(devPtr, size);
}
```

**dlopen**: Load a shared library at runtime. Used for plugin architectures—a tool can discover and load analysis plugins without statically linking them:

```c
void *handle = dlopen("./plugin_memcheck.so", RTLD_LAZY);
analyze_fn fn = dlsym(handle, "analyze");
fn(trace_data);
dlclose(handle);
```

### Name Mangling

C++ compilers encode function signatures into symbol names (e.g., `_Z6myFuncid` for `myFunc(int, double)`). This enables function overloading but complicates interposition—you need to mangle the name correctly or use `extern "C"` to disable mangling at the interface boundary. Use `c++filt` to demangle.

### DWARF Debug Info

DWARF is the standard debug information format stored in `.debug_*` ELF sections. It maps machine code addresses to source lines, describes variable locations (registers, stack offsets), type information, and inlined function boundaries. GPU debug tools generate DWARF-like debug info for GPU code to enable source-level debugging of kernels.

### Interview Q&A

**Q: What's the difference between ELF sections and segments?**
A: Sections are the linker's organizational units—`.text` for code, `.data` for initialized data, `.bss` for zero-initialized data, etc. They're described by section headers and used during linking to merge code from multiple object files. Segments are the loader's view—described by program headers, they tell the OS how to map the file into memory. A single `PT_LOAD` segment might contain multiple sections (`.text` + `.rodata`) mapped with the same permissions (read + execute). At runtime, the kernel only looks at program headers; section headers are optional (and stripped binaries don't have them).
*Follow-up:* Why does NVIDIA use ELF for GPU binaries? — ELF is a well-understood, extensible container format. By using custom sections for GPU metadata (register counts, shared memory size, kernel entry points), they can reuse standard tooling (objdump, readelf) for inspection, and the CUDA loader can use standard ELF parsing to extract and load kernels.

**Q: Explain PLT/GOT and lazy binding.**
A: When you call a dynamically linked function, the call goes to a PLT stub. The PLT stub loads the target address from the GOT. Initially, the GOT entry points back to a resolver stub that invokes the dynamic linker. The dynamic linker looks up the symbol, writes the real address into the GOT, and jumps to it. On subsequent calls, the PLT stub jumps directly to the now-resolved GOT entry—no dynamic linker involvement. This is "lazy binding" because resolution happens on first use. You can force eager binding with `LD_BIND_NOW=1`, which is useful for debugging or when you can't tolerate first-call latency.
*Follow-up:* How does `LD_PRELOAD` interception work with PLT/GOT? — The dynamic linker searches libraries in order: preloaded libraries first, then the application's dependencies. When resolving `cudaMalloc`, it finds the preloaded library's version first and puts that address in the GOT. The preloaded function can call `dlsym(RTLD_NEXT, "cudaMalloc")` to get the *next* definition in the search order—the real one.

**Q: How does LD_PRELOAD work and why is it used in GPU development tools?**
A: `LD_PRELOAD` instructs the dynamic linker to load a specified shared library before any others. Since symbol resolution searches libraries in order, functions in the preloaded library shadow identically-named functions in later libraries. GPU tools use this to intercept CUDA/OpenGL/Vulkan API calls without modifying the application—the tool's library replaces `cudaMalloc`, `cuLaunchKernel`, etc., logs or modifies the call, then forwards to the real implementation via `dlsym(RTLD_NEXT, ...)`. Tools like NVIDIA Nsight Systems, cuda-memcheck, and various profilers use this or similar techniques.
*Follow-up:* What are the limitations? — It doesn't work for statically linked binaries. It doesn't intercept intra-library calls (if libcuda.so calls its own internal function, the preloaded library can't intercept it because it's a direct call, not through the PLT). It also requires `extern "C"` or correct name mangling for C++ functions. And `setuid` binaries ignore `LD_PRELOAD` for security.

**Q: What is DWARF and why does it matter for GPU debugging tools?**
A: DWARF is a standardized debug information format stored in ELF sections (`.debug_info`, `.debug_line`, `.debug_abbrev`, etc.). It maps machine code addresses to source file and line numbers, describes variable types and locations (which register or stack slot holds a variable at each point), and tracks inlining. For GPU debugging tools, generating DWARF-like debug info for GPU kernels enables source-level debugging—setting breakpoints on source lines, inspecting variables, stepping through code—all of which map to GPU instruction addresses and register locations. Without DWARF, you'd be debugging at the assembly level.
*Follow-up:* What makes GPU DWARF harder than CPU DWARF? — GPU has thousands of concurrent threads, each with its own registers. A "variable" might be in a different register for each thread (or in shared memory, or local memory). The debug info must describe per-thread register mapping. Also, GPU compilers aggressively optimize (register reuse, instruction reordering, predication), making location tracking much harder.

**Q: Why would a GPU tool use dlopen for a plugin architecture?**
A: `dlopen` lets you load code at runtime without recompiling or relinking the tool. A profiler might have plugins for different analysis passes—memory checking, race detection, performance counters—each as a `.so` file. At startup, the tool scans a plugin directory, `dlopen`s each `.so`, and `dlsym`s a known entry point (like `plugin_init`). This means third parties or different teams can develop plugins independently, deploy them as `.so` files, and the tool picks them up without rebuilding. It also enables optional features—if a plugin isn't present, the tool still runs, just without that feature.
*Follow-up:* What are the risks of dlopen? — ABI compatibility: the plugin must be compiled with compatible compiler flags, struct layouts, and calling conventions. Symbol collision: a plugin might define a symbol that conflicts with the tool's. Version skew: a plugin compiled against tool API v1 loaded into tool v2 might crash. Mitigate with a versioned plugin API struct and ABI checks at load time.

---

## 9. Debugging and Performance Tooling on Linux

### GDB Workflows

```bash
# Basic debugging
gdb ./my_gpu_tool
(gdb) break main                    # breakpoint
(gdb) run --device 0
(gdb) info threads                  # list threads
(gdb) thread 3                      # switch to thread 3
(gdb) bt                            # backtrace
(gdb) frame 2                       # select stack frame
(gdb) print *pushbuffer@16          # print 16 elements of array
(gdb) watch *(uint64_t*)0x7fff1234  # hardware watchpoint (4 per core on x86)

# Core dump analysis
ulimit -c unlimited
./crashing_app                       # produces core file
gdb ./crashing_app core              # post-mortem analysis

# Attach to running process
gdb -p $(pidof my_gpu_tool)
```

Key for GPU tools: thread debugging (GPU submission threads, interrupt handler threads), watchpoints on fence memory locations, conditional breakpoints on specific command buffer values.

### Sanitizers

```bash
# AddressSanitizer — buffer overflows, use-after-free, leaks
g++ -fsanitize=address -g -o app app.cpp
./app  # reports errors with source location + stack trace

# UndefinedBehaviorSanitizer — signed overflow, null deref, alignment
g++ -fsanitize=undefined -g -o app app.cpp

# ThreadSanitizer — data races
g++ -fsanitize=thread -g -o app app.cpp
```

ASan and TSan cannot be combined (different memory layouts). TSan is invaluable for GPU tool development where multiple threads access shared command buffers—it finds races that only manifest under specific timing.

### Valgrind

```bash
valgrind --tool=memcheck --leak-check=full ./app
```

Runs the program in a software CPU emulator—10–50x slowdown. Catches all memory errors without recompilation. **Caveat**: Valgrind doesn't understand GPU driver ioctls or MMIO mappings, so it produces false positives when the GPU driver maps device memory.

### strace / ltrace

```bash
# Trace syscalls — see all ioctls to the GPU driver
strace -e ioctl -f ./my_cuda_app 2>&1 | head -50

# Trace library calls
ltrace -e cudaMalloc -f ./my_cuda_app
```

`strace` is essential for debugging GPU tool issues: "is the ioctl returning an error?", "which file descriptors is the driver opening?", "is the mmap succeeding?"

### perf

```bash
# CPU profiling with flamegraph
perf record -g -F 99 ./my_gpu_tool
perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg

# Hardware performance counters
perf stat -e cache-misses,cache-references,instructions,cycles ./app

# Specific events
perf record -e dTLB-load-misses ./app

# Per-thread profiling
perf record -t <tid> -g
```

### ftrace

Kernel-level tracing. Useful for tracing driver functions:

```bash
# Trace GPU driver functions
echo function > /sys/kernel/debug/tracing/current_tracer
echo 'nvidia*' > /sys/kernel/debug/tracing/set_ftrace_filter
cat /sys/kernel/debug/tracing/trace_pipe

# Trace specific syscalls with timing
echo 1 > /sys/kernel/debug/tracing/events/syscalls/sys_enter_ioctl/enable
echo 1 > /sys/kernel/debug/tracing/events/syscalls/sys_exit_ioctl/enable
cat /sys/kernel/debug/tracing/trace_pipe

# Function graph tracer — shows call tree with timing
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo 'nvidia_*' > /sys/kernel/debug/tracing/set_graph_function
```

ftrace is the lowest-overhead kernel tracing mechanism—it instruments function entry/exit points with a trampoline. For GPU driver work, it's invaluable when you need to understand the kernel-side execution path of a submission ioctl or an interrupt handler.

**trace-cmd** is the user-friendly frontend:
```bash
trace-cmd record -p function_graph -g nvidia_ioctl ./my_gpu_app
trace-cmd report
```

### /proc Introspection

```bash
cat /proc/<pid>/maps        # memory mappings (find BAR mappings, mmap'd buffers)
cat /proc/<pid>/status      # thread count, memory usage
ls -l /proc/<pid>/fd/       # open file descriptors (device files)
cat /proc/<pid>/smaps_rollup # detailed memory breakdown
cat /proc/<pid>/numa_maps   # NUMA placement of each mapping
cat /proc/<pid>/stat        # CPU time, priority, scheduling info
cat /proc/<pid>/io          # I/O statistics (bytes read/written)
cat /proc/<pid>/stack       # kernel stack trace of the process
```

Example: finding GPU-related mappings:
```bash
# Show all GPU-related memory mappings
grep -i nvidia /proc/$(pidof my_cuda_app)/maps

# Check if memory is pinned (look for "ml" = mlocked flag)
grep -E "Locked:" /proc/$(pidof my_cuda_app)/smaps | sort -u

# Check NUMA placement of GPU buffers
grep nvidia /proc/$(pidof my_cuda_app)/numa_maps
```

### Interview Q&A

**Q: How would you debug a crash in a GPU tool that only reproduces under load?**
A: First, enable core dumps (`ulimit -c unlimited`) and run under load. When it crashes, analyze the core with gdb: `bt` for the backtrace, check which thread crashed, examine the faulting address. If it's a memory corruption issue, rebuild with ASan (`-fsanitize=address`) and reproduce—ASan will catch the corruption at the point it happens, not where it manifests. If it's a race condition (non-deterministic crash), use TSan (`-fsanitize=thread`). If it's a driver interaction issue, strace the ioctls to see if one is returning an unexpected error. Check `/proc/<pid>/maps` to verify that mmap'd regions and BAR mappings are intact. If all else fails, use `rr` (record-and-replay debugger) to capture the exact execution and replay it deterministically.
*Follow-up:* What is `rr` and why is it useful? — `rr` records the entire execution of a process, including all non-deterministic inputs (syscalls, signals, thread scheduling). You can then replay the recording in gdb, stepping forward and backward. This is invaluable for reproducing non-deterministic bugs, because you can replay the exact execution that crashed and debug it at your leisure.

**Q: When would you use strace to debug a GPU-related problem?**
A: When I suspect the issue is at the kernel interface—ioctl failures, mmap problems, or file descriptor issues. For example, if a CUDA application fails to initialize, I'd run `strace -e open,ioctl,mmap -f ./app` to see if it's failing to open `/dev/nvidia0` (permissions issue?), if a specific ioctl returns `ENOMEM` or `ENOSPC` (out of GPU memory?), or if an mmap of the BAR region fails. strace shows the exact syscall arguments and return values, which is faster than reading driver source code to figure out where it failed. The `-f` flag follows child threads, which is important because GPU drivers create internal threads.
*Follow-up:* What's the overhead of strace? — strace uses ptrace to intercept every syscall, which adds two context switches per syscall (stop + restart). Overhead is proportional to syscall rate—a CPU-bound app is barely affected, but an I/O-heavy app with thousands of syscalls per second can slow down 2–5x. For production profiling, perf or eBPF-based tracing is lower overhead.

**Q: How do you generate and use a flamegraph?**
A: Run `perf record -g -F 99 ./app` to sample the call stack at 99 Hz. Then `perf script` dumps the raw samples, which you pipe through `stackcollapse-perf.pl` (folds identical stacks) and `flamegraph.pl` (generates an SVG). Each horizontal bar is a function, width proportional to time spent. The y-axis is call depth. You look for wide bars (time-consuming functions) and tall stacks (deep call chains). For GPU tools, you'd look at whether time is spent in the tool's instrumentation code, in driver ioctls, or in actual computation. If the ioctl bars are wide, your tool might be submitting too frequently.
*Follow-up:* What does the `-g` flag do in perf record? — It records call stacks for each sample (using frame pointers, dwarf, or LBR). Without `-g`, you only get flat profiles (which function is hot) but not *who called it*. For flamegraphs, you need `-g`. If frame pointers are missing (common with optimized builds), use `--call-graph dwarf` or `--call-graph lbr`.

**Q: What's the difference between AddressSanitizer and Valgrind?**
A: ASan is a compiler instrumentation: the compiler inserts checks around every memory access, with ~2x slowdown. It's fast and catches buffer overflows, use-after-free, and stack overflows at the point they happen. Valgrind (memcheck) is a binary translator—it runs the program on a virtual CPU and checks every memory access, with 10–50x slowdown. Valgrind doesn't require recompilation and can check binaries you don't have source for. ASan can't detect reads of uninitialized memory (use MSan for that); Valgrind can. ASan conflicts with TSan (can't combine them); Valgrind has helgrind for thread checking. For GPU tool development, ASan is the daily driver; Valgrind is for when you need to check a third-party library without source.
*Follow-up:* Why does ASan not work well with GPU drivers? — ASan maps shadow memory in a fixed address range and intercepts `mmap`. GPU drivers also `mmap` large address ranges (BARs, GPU memory), and these can conflict with ASan's shadow mapping. ASan also doesn't understand GPU driver memory operations (DMA, device-mapped pages), leading to false positives or missed bugs on the GPU side. You can suppress specific ASan warnings with a suppression file.

**Q: How would you use /proc to diagnose a memory issue in a GPU tool?**
A: Check `/proc/<pid>/maps` to see all memory mappings—look for the GPU BAR mappings (identifiable by the device file path), heap size, mmap'd regions, and their permissions. Check `/proc/<pid>/status` for `VmRSS` (physical memory actually used) vs `VmSize` (virtual size)—a big gap suggests overcommitted or lazily faulted pages. `/proc/<pid>/smaps_rollup` gives detailed per-mapping info like `Rss`, `Pss`, `Locked` (pinned pages). `/proc/<pid>/numa_maps` shows which NUMA node each mapping is on—crucial for GPU performance. If you see GPU buffers on the wrong NUMA node, that explains poor DMA throughput.
*Follow-up:* How do you identify GPU BAR mappings in /proc/pid/maps? — They show up as mappings of the device file (e.g., `/dev/nvidia0`) with specific offsets. The driver uses mmap offsets to encode the BAR number and offset within it. You can cross-reference the mapped physical addresses with BAR addresses from `lspci -v` to identify which BAR a mapping corresponds to.

---

## 10. Build / Toolchain Basics

### Make / CMake

```cmake
# Minimal CMake for a GPU tool
cmake_minimum_required(VERSION 3.18)
project(gpu_profiler LANGUAGES CXX CUDA)

add_library(profiler_interpose SHARED
    src/interpose.cpp
    src/trace_buffer.cpp
)
target_compile_features(profiler_interpose PRIVATE cxx_std_17)
target_link_libraries(profiler_interpose PRIVATE dl)  # for dlsym

# Cross-compilation with sysroot
set(CMAKE_SYSROOT /opt/aarch64-linux/sysroot)
set(CMAKE_C_COMPILER /opt/aarch64-linux/bin/aarch64-linux-gnu-gcc)
```

### Compilation vs Link Errors

- **Compilation error**: A single translation unit fails to compile—syntax errors, undeclared identifiers, type mismatches. Error points to a source file and line.
- **Link error**: All translation units compiled but the linker can't resolve a symbol (`undefined reference to X`) or finds duplicates (`multiple definition of X`). Common in large C++ projects with templates, inline functions, or missing library linkage (`-lcuda`).

### Cross-Compilation and Sysroots

**Cross-compilation**: Building on host architecture (x86-64) for a target architecture (aarch64, e.g., Jetson). You use a cross-compiler toolchain and a **sysroot**—a directory tree containing the target's headers and libraries. CMake `CMAKE_SYSROOT` tells the compiler to look there instead of `/usr/include` and `/usr/lib`.

This is relevant for NVIDIA tools that target both x86 (desktop/server GPUs) and ARM (Tegra/Jetson).

### Interview Q&A

**Q: What's the difference between a compilation error and a link error?**
A: A compilation error happens when a single source file can't be compiled—type mismatches, syntax errors, missing includes. The compiler tells you the exact file and line. A link error happens after all source files compile successfully but the linker can't assemble them into a binary. The most common is "undefined reference to X"—the function was declared (so compilation passed) but never defined in any object file or library. The fix is usually adding the right library (`-lcuda`) or compiling the missing source file. Link errors are harder to debug because they reference mangled symbol names.
*Follow-up:* How do you debug a "multiple definition" link error in a large C++ codebase? — It usually means a function or variable is defined (not just declared) in a header that's included by multiple translation units. The fix is to mark it `inline`, move the definition to a `.cpp` file, or use `static`/anonymous namespace for file-local symbols. Use `nm` on the object files to see which ones define the same symbol.

**Q: What is a sysroot and why is it needed for cross-compilation?**
A: A sysroot is a directory tree that mirrors the target system's `/usr/include` and `/usr/lib`—containing the target architecture's headers and libraries. When you cross-compile (e.g., building on x86 for ARM), the compiler needs ARM-compatible headers (different struct layouts, different system call numbers) and the linker needs ARM libraries. The `--sysroot` flag redirects all header and library searches to this directory. Without it, you'd accidentally use the host's x86 headers and libraries, producing a binary that won't run on the target.
*Follow-up:* How does this relate to NVIDIA's product line? — NVIDIA ships GPUs on both x86 (desktop/server with discrete GPUs) and ARM (Jetson/Orin with Tegra SoCs). GPU development tools must cross-compile for ARM targets, especially for automotive and embedded use cases. The build system uses sysroots with ARM-specific GPU driver headers and libraries.

**Q: Why does a large C++ project use CMake instead of raw Makefiles?**
A: Raw Makefiles become unmaintainable at scale—tracking dependencies between hundreds of source files, handling platform differences, finding libraries, and managing compiler flags manually is error-prone. CMake generates platform-specific build files (Makefiles, Ninja files, Visual Studio projects) from a declarative description. It handles dependency detection (`find_package`), compiler feature detection, and cross-compilation configuration. For GPU tool projects that target multiple platforms (Linux x86, Linux ARM, Windows) with multiple compilers (gcc, clang, MSVC) and need to find CUDA/driver libraries, CMake's abstraction is essential.
*Follow-up:* What's the relationship between CMake and Ninja? — CMake is a meta-build system—it generates build files for a build tool. Ninja is a fast build tool (alternative to Make) that CMake can target. `cmake -G Ninja` generates `build.ninja` files. Ninja is faster than Make for large projects because it has minimal overhead, handles parallel builds better, and tracks dependencies more precisely. Most NVIDIA internal builds use Ninja as the backend.

**Q: What happens when you run `make -j$(nproc)` and a header file changes?**
A: Make checks timestamps. If a header file is newer than its dependent object files, those object files are rebuilt. With `-j$(nproc)`, Make runs up to N compilations in parallel. The Makefile must correctly declare header dependencies (via `-MMD` compiler flag, which generates `.d` dependency files). If dependencies are wrong or missing, changing a header might not trigger recompilation of all affected files, causing stale object files and mysterious bugs. CMake handles this automatically with its dependency scanning.
*Follow-up:* What's a "unity build" and why might a GPU tool project use one? — A unity build concatenates multiple `.cpp` files into a single translation unit. This eliminates redundant header parsing and enables more inlining across files. It dramatically speeds up build times for projects with many small source files and heavy header usage. CMake supports it via `CMAKE_UNITY_BUILD`. The downside is that it can expose name collisions between files that were previously in separate translation units.

**Q: How would you debug a missing symbol error in a cross-compiled GPU tool?**
A: First, identify the missing symbol—`c++filt` demangles it to a readable name. Then figure out where it should come from: is it a function you wrote (check you're compiling and linking the right source file), or from a library (check `target_link_libraries` in CMake)? For cross-compilation, verify you're linking against the target's libraries in the sysroot, not the host's. Use `nm` or `readelf -s` on the sysroot libraries to check if the symbol exists. Common pitfall: the sysroot has an older version of a library that doesn't export the symbol. Also check that the library was built for the target architecture with `file libfoo.so`—mixing x86 and ARM objects gives confusing errors.
*Follow-up:* What tool shows the shared library dependencies of an ELF binary? — `ldd` on the host (but not for cross-compiled binaries, since it tries to load them). For cross-compiled binaries, use `readelf -d <binary> | grep NEEDED` to list dynamic dependencies, and `aarch64-linux-gnu-objdump -p <binary>` for more detail. Or use `patchelf --print-needed`.

---

## Red Flags

Common wrong answers that signal a lack of depth:

| Topic | Red Flag Answer | What's Actually True |
|-------|----------------|---------------------|
| Pinned memory | "It's faster because it's special memory" | Pinned memory is regular RAM that the kernel promises not to swap or migrate. It's required because GPU DMA uses physical addresses and can't handle page faults. |
| TLB shootdown | "It's just a TLB flush" | It's an *inter-processor interrupt* that forces *other* cores to flush specific TLB entries. It's the IPI + remote flush that makes it expensive. |
| PCIe | "PCIe is like a memory bus" | PCIe is a *packet-switched serial network* with headers, flow control, and ordering rules. It has significant per-transaction overhead that doesn't exist on memory buses. |
| UMD vs KMD | "The driver is one monolithic thing in the kernel" | The driver is split: UMD in user space (shader compiler, command building) and KMD in kernel (memory, scheduling, HW access). |
| Context switch | "It costs a few nanoseconds" | The direct cost is microseconds; the indirect cost (cache/TLB misses) can be tens of microseconds. |
| MESI | "Cache coherence is handled by software" | MESI is a *hardware* protocol. Software memory barriers control *ordering*, not coherence itself. |
| mmap | "mmap copies the file into memory" | mmap creates a *mapping*; the data is brought in via demand paging. No immediate copy happens. |
| LD_PRELOAD | "It modifies the binary" | It doesn't touch the binary. It instructs the dynamic linker to search the preloaded library first during symbol resolution. |
| Signals | "Signal handlers can do anything" | Only async-signal-safe functions are allowed. No malloc, no printf, no mutex operations. |
| GPU submission | "The CPU directly writes to GPU registers to launch kernels" | The CPU writes methods to a pushbuffer in memory, submits it via GPFIFO, and rings a doorbell. The GPU fetches and executes. |
| Lock-free | "Lock-free means no synchronization" | Lock-free means no *locks*, but it uses atomic operations and memory ordering. It still requires careful synchronization. |
| ELF sections vs segments | "They're the same thing" | Sections are the linker's view; segments are the loader's view. A segment can contain multiple sections. |

---

## Whiteboard Checklist

Quick-reference for whiteboard/coding rounds:

### Systems & OS
- [ ] Draw address space layout (stack, heap, mmap, text, kernel)
- [ ] Explain page table walk (4 levels, 9-9-9-9-12)
- [ ] Explain TLB shootdown trigger and IPI flow
- [ ] Describe minor vs major page fault
- [ ] Explain COW mechanics after fork()
- [ ] Describe syscall entry path (SYSCALL instruction → LSTAR → kernel stack → dispatch)
- [ ] Explain why pinned memory is needed for DMA (physical addresses, no GPU page fault handling)

### PCIe & DMA
- [ ] Draw PCIe topology (root complex, switch, endpoint)
- [ ] Explain BARs, MMIO, write-combining
- [ ] Explain MSI-X (PCIe write to memory → interrupt)
- [ ] Explain IOMMU (device-side address translation)
- [ ] Know Gen3/4/5 bandwidth numbers (~16/32/64 GB/s per direction x16)

### GPU Driver (Conceptual Model)
- [ ] Draw UMD/KMD split diagram
- [ ] Explain method/pushbuffer/GPFIFO/doorbell flow
- [ ] Describe fence/semaphore completion mechanism
- [ ] Explain GMMU and GPU virtual address spaces
- [ ] Walk through kernel launch end-to-end

### CPU Coherence & NUMA
- [ ] Draw MESI state transitions
- [ ] Explain false sharing with code example
- [ ] Know when memory barriers are needed on x86 (WC, store-load)
- [ ] Explain NUMA effect on GPU DMA bandwidth

### Concurrency
- [ ] Implement SPSC ring buffer with atomics
- [ ] Explain ABA problem and tagged-pointer solution
- [ ] Explain priority inversion and priority inheritance
- [ ] Describe RCU read/update/reclaim pattern
- [ ] Know spinlock vs mutex tradeoff

### Binaries & Tooling
- [ ] Draw ELF layout (header, phdrs, sections, shdrs)
- [ ] Explain PLT/GOT lazy binding
- [ ] Write LD_PRELOAD interposition skeleton
- [ ] Explain dlopen/dlsym plugin pattern

### Debugging
- [ ] GDB: breakpoint, watchpoint, thread debugging, core dump analysis
- [ ] Know ASan vs TSan vs Valgrind tradeoffs
- [ ] Use strace to debug ioctl failures
- [ ] Generate and interpret a perf flamegraph
- [ ] Read /proc/pid/maps to identify BAR mappings and NUMA placement
