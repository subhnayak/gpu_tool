# GPU Development Tools — Interview Mastery Pack

Target role: **NVIDIA — System Software Engineer, GPU Development Tools**

This pack is built to take you from "comfortable with C/C++ basics" to "can hold a
technical conversation with a GPU architect and write the tooling they need."
It is CUDA-heavy with solid graphics-pipeline fundamentals, and it treats the
*tooling / pre-silicon verification* dimension as a first-class subject rather
than an afterthought — because that is what the job actually is.

---

## Prerequisites — read this first

Your machine was checked while this pack was built. Current state:

| Tool | Status | Needed for |
|------|--------|-----------|
| **MSVC / C++17 compiler** | ✅ Installed and verified | M1, M2, and projects P1, P2, P4 (C++ side), P5 |
| **CMake** | ✅ Installed and verified | All C++ projects |
| **NVIDIA GPU** | ✅ RTX 4000 Ada Generation, **sm_89**, driver 581.42 | P3, P4 (Part C) |
| **CUDA Toolkit** | ❌ **Not installed** | P3 (all of it), P4 Part C, M5 hands-on |
| **Python 3** | ❌ **Not installed** | M8, P4 Python parts, P6, P3's `run_all.py` |

Two things to install before you get far:

1. **CUDA Toolkit** — from developer.nvidia.com/cuda-downloads. Your *driver* is
   already present and working (`nvidia-smi` runs), but the toolkit is a separate
   package and is what provides `nvcc`, Nsight Compute, Nsight Systems, `cuobjdump`
   and `nvdisasm`. On Windows it needs Visual Studio Build Tools with the C++
   workload, because `nvcc` uses `cl.exe` as its host compiler. Target `sm_89`.
2. **Python 3.9+** — from python.org. Note that `python` currently resolves to the
   Microsoft Store *alias stub*, which is not an interpreter and produces a
   confusing error message rather than running. Install the real thing and add
   `pytest` (`pip install pytest`).

**You do not need either to start.** M0–M4 and M9 are reading and reasoning, and
projects P1, P2 and P4's C++ half build and pass their tests with what you already
have. Install CUDA before you reach M5/P3, which is where the pack's centre of
gravity is.

**Verification status of the code in this pack:** P1, P2 and P4's C++ components
were compiled and their test suites run and passed on this machine. P3 (CUDA),
P4's Python components, P5 and P6 were written and statically reviewed but could
not be executed here for want of the toolchains above — each carries a note saying
so. Expect to fix a small compile error or two on first build; doing so is
genuinely part of the exercise.

---

## How to use this pack

1. **Read the module notes.** They are written the way you would *say* the answer
   out loud, not as academic prose. That is deliberate — you are training speech,
   not just recognition.
2. **Do the project.** Every module has a matching project under `projects/`.
   Reading about coalescing teaches you nothing; measuring a 10x difference on
   your own GPU makes it permanent.
3. **Re-derive the Q&A from memory.** Each module ends with an interview Q&A bank
   where every question has a **follow-up**. You must survive two levels deep.
   If you can only answer level one, you have memorized rather than understood.
4. **Whiteboard checklist.** Each module ends with a list of diagrams and derivations
   you must be able to produce from a blank page. That is the actual exit criterion.

> **The 20-minute rule.** If you have read a section and cannot explain it to an
> imaginary peer in 20 minutes without looking, you have not finished it. Go back.

---

## Study order

The order is not arbitrary. C++ and systems come first because they are the hard
gate in an NVIDIA loop and because everything else in this pack is *written* in
them. GPU architecture comes before CUDA because CUDA only makes sense once you
can see the SM. Tooling comes last because it composes everything before it.

| # | Module | File | Focus | Suggested time |
|---|--------|------|-------|----------------|
| M0 | Orientation & role decoding | [M0-orientation.md](M0-orientation.md) | What the job is, how the loop works, where you stand | 1 day |
| M1 | Modern C++ mastery | [M1-modern-cpp.md](M1-modern-cpp.md) | Move semantics, templates, allocators, memory model, ABI | 3 weeks |
| M2 | Design patterns for tools | [M2-design-patterns.md](M2-design-patterns.md) | Factory/Visitor/Builder/Command, type erasure, API design | 1 week |
| M3 | Systems software & OS | [M3-systems-os.md](M3-systems-os.md) | VM, DMA, PCIe, GPU driver model, linking, debugging | 2.5 weeks |
| M4 | GPU hardware architecture | [M4-gpu-architecture.md](M4-gpu-architecture.md) | SIMT, SM internals, memory hierarchy, roofline | 2 weeks |
| M5 | CUDA programming | [M5-cuda.md](M5-cuda.md) | Model, patterns, perf engineering, PTX/SASS toolchain | 4 weeks |
| M6 | Graphics pipeline | [M6-graphics-pipeline.md](M6-graphics-pipeline.md) | Pipeline stages, OpenGL vs Vulkan, SPIR-V, stimulus | 2 weeks |
| M7 | Tooling, simulation & verification | [M7-tooling-simulation-verification.md](M7-tooling-simulation-verification.md) | Pre-silicon flow, stimulus gen, disassemblers, debuggers | 3 weeks |
| M8 | Scripting & infrastructure | [M8-scripting-infra.md](M8-scripting-infra.md) | Python tooling, pybind11, CMake, CI | Continuous |
| M9 | Capstone & behavioral | [M9-capstone-and-behavioral.md](M9-capstone-and-behavioral.md) | System design, STAR stories, review schedule | 1 week |

Plus:
- **[qa-bank.md](qa-bank.md)** — consolidated rapid-fire question bank across every module (~260 questions).
- **[cheatsheet.md](cheatsheet.md)** — one page, for the day before the interview.

**Total: roughly 16–20 weeks at ~12 hrs/week.** Compress by cutting project depth,
never by cutting the Q&A recall step.

M8 (scripting/infra) is marked *continuous* on purpose — you learn CMake, Python
and pytest by using them in every project, not by studying them in a block.

---

## Projects

Projects compound. P2 becomes the host for P4, and P7 assembles P2+P3+P4 into
something that genuinely resembles what the team builds.

| # | Project | Modules | Exit criteria |
|---|---------|---------|---------------|
| P1 | [micro-alloc](projects/P1-micro-alloc/) — arena/pool allocator + intrusive containers | M1 | Beats `new`/`std::vector` on a stated benchmark; ASan/UBSan clean; unit tested |
| P2 | [cmdgen](projects/P2-cmdgen/) — command/IR object model emitting a fake GPU method stream | M1, M2, M7 | Encode→decode round-trips; adding an opcode touches one file |
| P3 | [cuda-ladder](projects/P3-cuda-ladder/) — vector add → coalescing → tiled SGEMM → reduction → scan → histogram → radix sort | M4, M5 | Every stage profiled; % of roofline documented |
| P4 | [sass-explorer](projects/P4-sass-explorer/) — toy-ISA table-driven decoder + real PTX parser with CFG reconstruction | M5, M7 | Disassembles, reconstructs control flow, prints a CFG |
| P5 | [graphics-stimulus](projects/P5-graphics-stimulus/) — deterministic triangle → textured → compute path | M6, M7 | Bit-identical output image hash across runs |
| P6 | [perfmodel](projects/P6-perfmodel/) — analytical model predicting kernel runtime | M4, M5, M7 | Predicts P3 kernels within a stated error band |
| P7 | [capstone](projects/P7-capstone/) — full stimulus framework with Python front-end, constrained-random generation, regression runner | All | Generates + runs + self-checks N workloads reproducibly from a seed |

---

## Progress tracker

Mark these off honestly. "Read it" is not a checkbox; "explained it from memory" is.

```
[ ] M0  Orientation            [ ] P1  micro-alloc
[ ] M1  Modern C++             [ ] P2  cmdgen
[ ] M2  Design patterns        [ ] P3  cuda-ladder
[ ] M3  Systems & OS           [ ] P4  sass-explorer
[ ] M4  GPU architecture       [ ] P5  graphics-stimulus
[ ] M5  CUDA                   [ ] P6  perfmodel
[ ] M6  Graphics pipeline      [ ] P7  capstone
[ ] M7  Tooling & verification
[ ] M8  Scripting & infra      [ ] Q&A bank — full pass
[ ] M9  Capstone & behavioral  [ ] Cheat sheet — memorized
```

---

## Ground rules

- **Never quote a performance fact you have not measured.** You have a GPU. Use it.
  "I measured 780 GB/s of a theoretical 936" beats "coalescing is faster" in every
  interview ever conducted.
- **Keep a bug journal.** Every non-trivial bug you hit during the projects becomes
  a behavioral-interview story. You will need 6–8 of them; you cannot invent them
  the night before.
- **SASS is not publicly documented.** P4 therefore builds a decoder for a *toy ISA*
  plus a parser for real PTX. That is the transferable skill, and being able to say
  clearly *why* you did it that way is itself a good interview answer.
- **Say "I don't know" cleanly, then reason.** For a tools/architecture role,
  watching you reason from the memory hierarchy to a plausible answer is worth more
  than a memorized fact.
