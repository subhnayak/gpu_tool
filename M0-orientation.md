# M0 — Orientation: Decoding the Role

Before you study anything, you need to know what you are studying *for*. Most
candidates fail this loop not because they lack knowledge but because they bring
application-developer knowledge to an infrastructure-and-verification job. This
module is short. Read it once now, and again the week of the interview.

---

## 1. What this team actually builds

Read the job description again, but read it as an engineer rather than a candidate:

> "designing and developing frameworks for creating GPU workloads that enable
> functional and perf verification of GPU"
> "develop the core infrastructure needed to generate stimulus for modeling,
> analysis, and debugging of upcoming general-purpose graphics and computing chips"

Strip the language away and here is the job:

**The chip does not exist yet. You write the software that makes the not-yet-existing
chip do things, so that other engineers can find out whether it works and how fast it is.**

That single sentence reframes everything.

A normal CUDA developer writes a kernel to make *an application* fast on *hardware
that exists*. You will write software that generates *workloads* — "stimulus" — to
drive a *model* of hardware that does not exist yet, and tools to *analyze* what
that model did. The GPU you target may be running as an RTL simulation at a
millionth of real speed. Your "hello world" may take four hours to execute
ten thousand instructions.

That has enormous consequences, and internalizing them is most of what separates a
strong candidate from an average one:

| Application developer | You, in this role |
| --- | --- |
| Hardware is fixed; make software fast | Software is the tool; find out if hardware is correct and fast |
| Wants realistic, large workloads | Wants *small, targeted, dense* workloads — simulation time is precious |
| Nondeterminism is annoying | Nondeterminism is **fatal** — a bug you cannot reproduce cannot be filed |
| Correct = right answer | Correct = right answer **and** the ability to prove it against a golden model |
| Debug with a debugger on real HW | Debug from waveforms, traces, and logs, often post-mortem |
| Users are end customers | Users are chip architects, verification engineers, driver developers — i.e. *experts who will read your code* |

**"Stimulus"** is the word to internalize. Stimulus is any input that drives the
device under test: a compute kernel, a sequence of draw calls, a stream of
register writes, a pushbuffer of GPU methods. A *stimulus generation framework*
is a program whose output is another program (or command stream) that runs on the
GPU or its model. If you have ever written a compiler, a fuzzer, or a test
generator, you already know the shape of this work — it is all three at once.

### The customers matter

The JD lists them explicitly: "chip architects, driver developers, and verification
engineers." These are not end users who file vague bugs. They are engineers who
will read your framework's source, extend it, and be blocked by it. That is why
the JD asks for **object-oriented design patterns** and **API design sense** in the
same breath as C++ — the framework's *extensibility* is a feature on par with its
correctness. When you get a design question in this loop, "how does someone add a
new instruction / new engine / new architecture to this without editing twelve
files?" is a first-class part of the answer, not a bonus point.

---

## 2. Decoding every JD bullet

Each requirement, what it actually means, and where it is covered.

### "3+ years... large portion on C++ based projects" / "Strong C++ programming capability (boost or C++11/14 a plus)"

**What it means:** you will live in a large, long-lived C++ codebase. Not
competitive-programming C++ — *engineering* C++. They care that you understand
ownership, lifetime, move semantics, templates, and what the compiler and linker
actually do. The explicit mention of C++11/14 and boost signals a mature codebase
where knowing *why* `unique_ptr` replaced raw owning pointers is expected
conversation.

**Covered in:** M1, plus P1 and P2.

**The interview shape:** "here is a class, what's wrong with it" (rule of five,
dangling reference, exception safety), "implement a small container/allocator",
"what does this template do", "what happens at link time here".

### "Knowledge of object-oriented design patterns"

**What it means:** not reciting GoF. It means you can structure a framework that
30 people extend for a decade. Factory for creating instructions/engines, Visitor
for walking an IR, Command for representing a GPU method as an object, Builder for
assembling a workload, Strategy for pluggable allocation or randomization policies.

**Covered in:** M2, exercised in P2 and P7.

**The interview shape:** "design the class hierarchy for X", "how would you let a
new architecture be added without modifying existing code", "when is inheritance
wrong here".

### "Good understanding of system software and Operating Systems"

**What it means:** you understand what happens beneath the API. Virtual memory,
pinned memory and DMA, PCIe, the user-mode/kernel-mode driver split, how a kernel
launch actually reaches the hardware, processes and threads, linking and loading.
Tools work constantly crosses these boundaries.

**Covered in:** M3.

**The interview shape:** "what happens from `cudaMemcpy` to bits landing in device
memory", "why does pinned memory make transfers faster", "what is in a page table
and what is a TLB shootdown", "how does a debugger set a breakpoint".

### "Strong expertise in design and development of complex massively parallel algorithms"

**What it means:** you can decompose a problem across thousands of threads and
reason about the memory system. Reduction, scan, sort, histogram, tiling — not
because you will ship them, but because they are the vocabulary for discussing
parallel decomposition, and because stimulus *is* parallel code.

**Covered in:** M4 and M5, exercised in P3 and P6.

**The interview shape:** "write a parallel reduction", "now remove the divergence",
"now do it with warp shuffles", "why is your transpose slow", "compute the
occupancy of this kernel".

### "Experience with chip and/or system simulation a huge plus"

**What it means:** this is the differentiator bullet. Most candidates have none of
it. Understanding the pre-silicon flow — architectural model → RTL simulation →
emulation → FPGA prototype → silicon bring-up — and what each stage needs from
stimulus is what makes you look like you already do the job. Even conceptual
fluency here separates you from a strong-but-generic C++ candidate.

**Covered in:** M7. **Do not skip M7.** It is the module that wins the loop.

**The interview shape:** "how would you test a feature before silicon exists",
"how do you make a random test reproducible", "your test fails after 6 hours of
simulation — what now", "how do you know the model and the RTL agree".

### "Strong scripting skills desired"

**What it means:** Python (and some Perl/shell in legacy chip flows). Test harnesses,
log/trace parsing, regression runners, report generation, binding C++ to Python.
Infrastructure is glued together with scripts and nobody has time to write it in C++.

**Covered in:** M8, used continuously in every project.

### "Graphics or CUDA knowledge a plus" / "OpenGL, Vulkan, Direct3D, CUDA APIs a plus"

**What it means:** the JD says the framework is "similar to OpenGL and CUDA" — you
are building something in that family. You need to know what these APIs *are* at
the design level: what state they manage, why Vulkan is explicit where OpenGL is
implicit, how a draw call or kernel launch is translated into hardware commands.
Deep app-level graphics programming is *not* required for a CUDA-weighted path;
being able to draw the pipeline and explain the API model is.

**Covered in:** M5 (CUDA), M6 (graphics).

### "Design and develop GPU stimulus analysis tools such as debuggers & disassemblers"

**What it means:** instruction encoding and decoding, opcode tables, control flow
reconstruction, symbolization, debug info, breakpoints, and inspecting the state
of a massively parallel machine. This is concrete, buildable, and rarely prepared
for by candidates.

**Covered in:** M7, built in P4.

### "Excellent interpersonal skills" / "Coordinate with GPU architects" / "Work closely with HW & SW teams"

**What it means:** genuinely weighted here, because your users are internal experts
with strong opinions and your framework mediates between hardware and software
teams. They will probe for: how you handled a disagreement about a design, how you
support users of your code, how you work when requirements are ambiguous (they
*always* are pre-silicon — the spec changes under you).

**Covered in:** M9.

---

## 3. What the interview loop typically looks like

Expect roughly this shape. Details vary by team and region, but the *categories*
are stable:

1. **Recruiter screen** — motivation, background, logistics. Have a 90-second
   version of "why GPU tools / why NVIDIA" ready that is not generic.
2. **Technical phone screen (~1 hr)** — C++ fundamentals and one coding problem.
   Often shared-editor. Expect pointers/memory/ownership questions and a moderate
   algorithm or data-structure task. This is a filter; it is passed on C++ fluency.
3. **Onsite / virtual loop (4–6 rounds)**, drawn from:
    - **C++ deep dive** — move semantics, templates, virtual dispatch and object
     layout, ownership, undefined behavior, "what's wrong with this code".
    - **Coding round** — data structures/algorithms, frequently with a systems or
     parsing flavor (parse this format, build this index, implement an LRU,
     implement a small allocator) rather than pure LeetCode-style puzzles.
    - **Parallel/CUDA round** — write or optimize a kernel, reason about memory,
     compute occupancy, explain divergence.
    - **Systems/architecture round** — OS and GPU architecture, memory hierarchy,
     driver path, how things work under the hood.
    - **Design round** — "design a disassembler / a trace format / a test generator /
     a framework API". This is where M2 + M7 + M9 pay off.
    - **Hiring manager / behavioral** — collaboration, ambiguity, ownership,
     debugging war stories, how you handle being blocked.

**Weighting reality:** C++ depth and clear reasoning about *why* something behaves
as it does carry more weight than breadth of API trivia. Nobody is impressed that
you memorized every `cudaMemcpyKind`; everybody is impressed when you say "that's
bandwidth-bound at about 60% of peak, and here's how I'd confirm it."

### How to answer well in this loop specifically

- **Reason from the machine.** When you don't know, say so and derive: "I don't
  know that specific number, but shared memory is on-die, so it should be an order
  of magnitude lower latency than L2 — call it tens of cycles versus a couple
  hundred." That answer scores nearly as well as knowing it.
- **Quantify.** Ranges, orders of magnitude, and "I measured X on my RTX ___" are
  the currency of this team.
- **Ask about constraints before designing.** In a design round, the first thing
  out of your mouth should be questions: who extends this, how fast must it run,
  does it need determinism, what's the failure mode. That *is* the signal they
  are testing.
- **Bring the verification lens.** When asked to design anything, mention how you'd
  test it, how you'd make it reproducible, and how you'd debug it at 3am. Very few
  candidates do this, and it is exactly this team's value system.

---

## 4. Self-assessment rubric

Rate yourself 0–4 **now**, before studying, and again after each module. Be brutal;
the only person you can fool is yourself.

> 0 = never heard of it · 1 = recognize it · 2 = can use it · 3 = can explain it
> from memory · 4 = can teach it and discuss tradeoffs

**Target before interviewing: 3+ on everything, 4 on C++, CUDA, and one tooling area.**

| # | Skill | Module | Now | Later |
| --- | --- | --- | --- | --- |
| 1 | Move semantics, rule of five, perfect forwarding | M1 |  |  |
| 2 | Templates, SFINAE/CRTP, type traits | M1 |  |  |
| 3 | Smart pointers, custom allocators, arena allocation | M1 |  |  |
| 4 | C++ memory model, atomics, memory ordering | M1 |  |  |
| 5 | UB, aliasing, alignment, binary encoding safety | M1 |  |  |
| 6 | Object layout, vtables, ABI, linking | M1/M3 |  |  |
| 7 | GoF patterns applied to tool/compiler code | M2 |  |  |
| 8 | Framework/API design for extensibility | M2 |  |  |
| 9 | Virtual memory, paging, TLB, pinned memory | M3 |  |  |
| 10 | DMA, PCIe, MMIO, interrupts | M3 |  |  |
| 11 | GPU driver model: UMD/KMD, pushbuffers, fences | M3 |  |  |
| 12 | Lock-free concurrency, producer-consumer | M3 |  |  |
| 13 | ELF, dynamic linking, plugin architectures | M3 |  |  |
| 14 | gdb, sanitizers, perf, post-mortem debugging | M3 |  |  |
| 15 | SIMT, warps, divergence, independent thread scheduling | M4 |  |  |
| 16 | SM internals, occupancy math | M4 |  |  |
| 17 | Memory hierarchy, coalescing, bank conflicts | M4 |  |  |
| 18 | Roofline, bottleneck diagnosis | M4 |  |  |
| 19 | CUDA programming model, streams, events | M5 |  |  |
| 20 | Parallel patterns: reduction, scan, sort, tiling | M5 |  |  |
| 21 | Warp intrinsics, atomics, fences | M5 |  |  |
| 22 | Nsight profiling and interpretation | M5 |  |  |
| 23 | nvcc, PTX vs SASS, cuobjdump/nvdisasm, driver API | M5 |  |  |
| 24 | Graphics pipeline stages and HW mapping | M6 |  |  |
| 25 | OpenGL vs Vulkan model, SPIR-V, barriers | M6 |  |  |
| 26 | Pre-silicon flow: model/RTL/emulation/silicon | M7 |  |  |
| 27 | Directed vs constrained-random stimulus, coverage | M7 |  |  |
| 28 | Disassembler design, instruction decoding, CFG | M7 |  |  |
| 29 | Debugger internals, breakpoints, debug info | M7 |  |  |
| 30 | Performance modeling and trace analysis | M7 |  |  |
| 31 | Python tooling, binary parsing, pybind11 | M8 |  |  |
| 32 | CMake, CI, large-repo git workflow | M8 |  |  |
| 33 | System design under constraints | M9 |  |  |
| 34 | Behavioral stories (6–8, STAR format) | M9 |  |  |

### Interpreting your score

- **Mostly 0–1:** you are at the start. Follow the module order strictly; do not
  jump to CUDA because it is the fun part.
- **Mostly 2s:** you can use these tools but will crumble under follow-up questions.
  Your bottleneck is *articulation*. Prioritize the Q&A recall step over reading more.
- **Mostly 3s with gaps:** target the gaps and go straight at M7, which most
  candidates never study at all and which is the highest-leverage differentiator here.

---

## Interview Q&A

**Q: Why do you want to work on GPU development tools rather than as a GPU application developer?**
A: Because the leverage is different. An application developer makes one workload
fast; the tools team makes every architect and verification engineer in the company
faster, and does it before the chip exists. I also like the problem shape — it is
compiler, fuzzer, and simulator work at once, which means the C++ has to be genuinely
well-engineered and the hardware understanding has to be real. And the feedback loop
is intellectually honest: either your stimulus caught the bug pre-silicon or it did not.
*Follow-up:* **Doesn't it bother you that you're not shipping to customers?** — The
customers are internal but they are not less demanding; they are more. A framework
used by hundreds of engineers over a decade has stricter requirements around API
stability, extensibility, and debuggability than most consumer code. And the output
does ship — it ships as a chip that works.

**Q: What is "stimulus" in a pre-silicon context?**
A: Stimulus is whatever input drives the device under test. For a GPU that could be a
compute kernel, a stream of draw calls, or, at a lower level, a pushbuffer of methods
and register writes. The point of a stimulus framework is to *generate* those inputs
programmatically — directed at specific hardware features, or randomized within
constraints — so that you can exercise the design's corner cases and check the results
against a golden reference. It is the pre-silicon analogue of a test suite, except the
device under test may be an RTL simulation running a million times slower than real hardware.
*Follow-up:* **Why generate stimulus instead of just running real applications?** — Real
applications are enormous, they run far too slowly under simulation, and their coverage
is accidental. A real app might execute a billion instructions to hit one interesting
corner case; generated stimulus hits it in a thousand. You also cannot easily aim a real
app at a specific new hardware feature, and you cannot easily shrink it when it fails.
Real apps do get used, but later — at emulation and bring-up, where you have the speed.

**Q: Your generated random test fails after six hours of RTL simulation. Walk me through what you do.**
A: First, confirm reproducibility: re-run with the same seed and configuration and
check that I get the identical failure — if it is not reproducible, that is the bug
I chase first, because a non-reproducible failure is nearly unfileable. Then I
minimize: use the seed and the generator's constraint system to shrink the test toward
the smallest sequence that still fails, ideally re-running the reduced case at a much
cheaper simulation level. In parallel I look at what the test was doing at the failure
point using the trace and any waveform dump, and I compare against the golden/functional
model to establish whether the model or the RTL is wrong. Then I file it with the
minimized repro, the seed, and the exact tool versions.
*Follow-up:* **How do you make sure it's reproducible in the first place?** — Everything
random comes from one seeded generator, logged with the test; no dependence on wall-clock
time, addresses, pointer values, hash iteration order, or thread scheduling; the tool
version and configuration are captured with the result. Determinism has to be designed
in from the first commit — you cannot retrofit it once nondeterminism is spread across
a codebase.

**Q: How is writing software for a chip that doesn't exist different from normal development?**
A: Three big differences. First, execution is astronomically slow, so everything is
optimized for *small* — small tests, fast iteration, targeted coverage. Second, the
target moves: the spec changes under you, so the framework must be structured so a
hardware change touches one place, not fifty — that is why the design-patterns question
is on the job description. Third, you cannot trust anything, including your own reference
model; correctness comes from cross-checking independent implementations, so self-checking
tests and golden models are structural, not optional.
*Follow-up:* **How do you design for a moving spec?** — Separate the *what* from the
*how*: an architecture-neutral intermediate representation of the workload, with per-architecture
back ends that encode it. Table-driven encoding so an ISA change is a data change.
Registries so a new engine or opcode is added by registering a plugin rather than by
editing a switch statement. And version the interfaces you expose to other teams so
you can evolve without breaking them.

**Q: What do you think the hardest part of this job is?**
A: Debuggability at scale. It is not hard to write a generator that produces a million
random workloads; it is hard that when number 743,912 fails on a Friday night, someone
can reproduce it in one command, shrink it in minutes, and tell whether the bug is in
the hardware, the model, or my generator. That means determinism, good traces, minimization,
and clear triage tooling have to be first-class features, and they are the parts that
are easiest to defer and most expensive to retrofit.
*Follow-up:* **How would you decide whether the bug is in your generator or the hardware?** —
Cross-check with an independent implementation: run the same stimulus against the
functional/architectural model and against the RTL, and see if they disagree. If they
agree with each other but disagree with my expectation, my generator's expectation is
suspect. I'd also check whether the generator produced a *legal* stimulus at all —
a surprising fraction of "hardware bugs" are tests that violate an architectural rule,
which is why the generator should validate its own output against the constraints.

---

## Red flags

Things that will actively hurt you in this loop:

- **Treating verification as QA.** Saying "so the testers use this" signals you don't
  understand that pre-silicon verification is a deep engineering discipline and that
  a chip bug found post-silicon costs millions and months.
- **Ignoring determinism.** Proposing anything that depends on timing, thread scheduling,
  pointer values, or unordered container iteration order, without noticing.
- **Optimizing for the wrong thing.** Proposing huge realistic workloads when simulation
  runs at a millionth of real-time.
- **API trivia over mechanism.** Reciting function names instead of explaining what the
  hardware and driver do. This team wants mechanism.
- **No testing story.** Designing a framework in a design round and never mentioning how
  it is tested, how failures are reproduced, or how a user debugs it.
- **Pretending to know NVIDIA internals.** SASS encodings, internal tool names, and
  microarchitectural specifics are not public. Confidently inventing them is worse than
  saying "that's not public — here is the conceptual model and how I'd reverse it."
- **Only knowing CUDA as an API.** Being able to call `cudaMalloc` but unable to say what
  happens underneath is precisely the gap this role cannot have.

---

## Whiteboard checklist

Before leaving M0, you should be able to, from a blank page:

- [ ] State in one sentence what this team builds and who its customers are.
- [ ] Draw the pre-silicon flow: architectural model → RTL → simulation → emulation →
      FPGA prototype → silicon bring-up, and say what stimulus each stage needs and
      roughly how the speed/fidelity tradeoff shifts across it.
- [ ] Name the two ways stimulus is generated (directed vs constrained-random) and when
      each is the right tool.
- [ ] List the five things that must be captured to make a random test reproducible.
- [ ] Map every bullet of the job description to a module in this pack.
- [ ] Give your 90-second "why this role" answer without notes.
- [ ] Fill in the self-assessment rubric honestly, and name your three weakest rows.

Next: [**M1 — Modern C++ Mastery**](M1-modern-cpp.md).
