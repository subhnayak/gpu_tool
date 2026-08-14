# P4: SASS Explorer — GPU Disassembler & Analysis Tools

## Why This Project?

> **Prerequisites**: See [PREREQUISITES.md](PREREQUISITES.md) for what to install for each part.

NVIDIA's "System Software Engineer, GPU Development Tools" role asks you to
**"design and develop GPU stimulus analysis tools such as debuggers &
disassemblers."** This project builds that exact skill.

### Why Is It Split Into Three Parts?

> **CRITICAL INTERVIEW POINT:** NVIDIA SASS (Streaming Assembly) instruction
> encodings are **not publicly documented**. The encoding format is proprietary
> and changes between GPU architectures. In an interview, stating this clearly
> and explaining how you'd work around it demonstrates more competence than
> inventing fake SASS encodings.

The three parts are structured to teach the underlying skills correctly:

| Part | What | Why |
|------|------|-----|
| **A: Toy ISA** | A fully-specified 32-bit GPU-style ISA with assembler, disassembler, CFG builder | Teaches instruction encoding/decoding on a spec you control. You learn the algorithms without needing proprietary data. |
| **B: Real PTX** | Parser and CFG reconstructor for NVIDIA's public PTX virtual ISA | PTX is documented, you can generate it from your own CUDA code, and it's what NVIDIA tools engineers actually work with. |
| **C: Real Toolchain** | Scripts to extract and explore real PTX + SASS using nvcc, cuobjdump, nvdisasm | Teaches you the actual NVIDIA toolchain and lets you see real machine code. |

### Architecture

```
                        Part A: Toy ISA
  ┌─────────┐    ┌──────────┐    ┌─────────┐    ┌─────┐    ┌─────────┐
  │ .asm    │───>│ Assembler│───>│ Binary  │───>│Decodr│───>│   IR    │
  │ source  │    │ (2-pass) │    │ (.bin)  │    │(table│    │(DecodedI│
  └─────────┘    └──────────┘    └─────────┘    │driven│    │  nst)   │
                                                └──┬──┘    └────┬────┘
                                                   │            │
                  ┌──────────────────────────────────┘            │
                  │  same table drives both ←─── KEY DESIGN      │
                  └──────────────────────────────────┐            │
                                                     ▼            ▼
                                                ┌─────────┐  ┌───────┐
                                                │ Printer  │  │  CFG  │
                                                │ (format) │  │Builder│
                                                └─────────┘  └───┬───┘
                                                                  │
                                                            ┌─────▼─────┐
                                                            │ DOT / Text│
                                                            │  Output   │
                                                            └───────────┘

                        Part B: Real PTX
  ┌─────────┐    ┌──────────┐    ┌─────────┐    ┌─────────┐
  │ .ptx    │───>│  Parser  │───>│PTXModule│───>│ CFG +   │───> DOT / Stats
  │ (nvcc)  │    │(tolerant)│    │   IR    │    │Dominatr │
  └─────────┘    └──────────┘    └─────────┘    └─────────┘

                        Part C: Toolchain
  ┌─────────┐    ┌──────────────────────────────────────────┐
  │ .cu     │───>│ nvcc -ptx    → .ptx                      │
  │ source  │    │ nvcc -cubin  → .cubin                    │
  │         │    │ cuobjdump    → SASS listing              │
  │         │    │ nvdisasm     → SASS (detailed)           │
  │         │    │ compare_ptx_sass.py → aligned view       │
  └─────────┘    └──────────────────────────────────────────┘
```

## Build & Run

### Part A: C++ (Toy ISA)

```bash
# Build with CMake (Windows or Linux)
mkdir build && cd build
cmake ..
cmake --build . --config Release

# Run tests
ctest --output-on-failure

# Or run directly:
./Release/toyisa_tests        # (Windows: .\Release\toyisa_tests.exe)

# Assemble and disassemble an example
./Release/toyasm ../examples/counted_loop.asm loop.bin
./Release/toydis loop.bin
./Release/toydis --mode both --cfg --dot loop.dot loop.bin
```

### Part A': Python (Toy ISA Mirror)

```bash
cd python

# Run assembler
python toy_asm.py ../examples/counted_loop.asm loop.bin

# Run disassembler
python toy_dis.py loop.bin

# Run tests
pytest test_toy.py -v
```

### Part B: PTX Tools

```bash
cd python

# Parse PTX
python ptx_tool.py parse ../tests/sample.ptx -v

# Build CFG
python ptx_tool.py cfg ../tests/sample.ptx

# Print statistics
python ptx_tool.py stats ../tests/sample.ptx

# Generate DOT
python ptx_tool.py dot ../tests/sample.ptx

# Tests
cd ../tests && pytest test_ptx.py -v
```

### Part C: Real Toolchain (requires CUDA Toolkit)

```bash
cd scripts

# Dump PTX and SASS from a CUDA kernel
python dump_ptx_sass.py sample_kernel.cu --arch sm_89 --lineinfo

# Compare PTX vs SASS
python compare_ptx_sass.py sample_kernel_dump/sample_kernel.ptx sample_kernel_dump/sample_kernel_cuobjdump.sass
```

## Toy ISA Specification

See [ISA_SPEC.md](ISA_SPEC.md) for the complete hardware-style specification.

Key features:
- 32-bit fixed-width instructions (4 formats: R, I, B, L)
- One 64-bit (two-word) instruction form (LIMM) for variable-length decoding
- 32 GP registers (R0 = hardwired zero)
- 8 predicate registers (P0 = always true)
- Predicated execution on every instruction
- PC-relative branches with signed offsets
- Load/store with base+offset addressing
- Barrier/sync instruction

## Acceptance Criteria

- [x] **Round-trip**: Every instruction form assembles and disassembles back to the same mnemonic and operands
- [x] **CFG**: Reconstructs correct CFGs for straight-line, if/else diamond, single loop, nested loop, and unreachable-block programs
- [x] **Robustness**: Decoder handles random/corrupt input without crashing, hanging, or infinite loops; always makes forward progress
- [x] **Real PTX**: Parses real nvcc-generated PTX, builds CFG, computes statistics
- [x] **Toolchain**: Scripts compile CUDA, extract PTX/SASS, and handle missing tools gracefully

## C++ vs Python: Design Discussion

Both implementations use the same table-driven architecture. The comparison is instructive:

| Aspect | C++ | Python |
|--------|-----|--------|
| **Bit manipulation** | Natural, zero-cost | Works fine but more verbose |
| **Performance** | Fast enough for multi-GB binaries | Fine for exploration/prototyping |
| **Type safety** | Catches field-width errors at compile time | Runtime errors only |
| **Development speed** | Slower to write | Much faster iteration |
| **Real-world use** | Production tools (nvdisasm, objdump) | Prototyping, scripting, glue |

**In a real flow**, the ISA table would be auto-generated from a machine-readable spec (XML/JSON) into both C++ and Python. This eliminates manual synchronization. The differential test (`test_toy.py::TestDifferential`) validates that the two implementations agree — this is exactly how production disassemblers are validated.

## Questions to Answer After Finishing

*These map directly to interview questions:*

1. **Why is table-driven decoding better than a switch statement?**
   *(Adding instructions, testing, code generation, consistency)*

2. **How do you handle variable-length instructions?**
   *(Look at LIMM: first word's opcode tells you a second word follows)*

3. **What is the difference between linear-sweep and recursive-descent disassembly?**
   *(Run `toydis --mode both data_in_code.bin` and observe)*

4. **How do you ensure an encoder and decoder stay in sync?**
   *(Same table. Round-trip testing. Differential testing.)*

5. **How do you handle corrupt or malicious input in a tools context?**
   *(Robustness contract: never crash, always advance, emit .unknown)*

6. **What is a basic block? How do you identify leaders?**
   *(Entry, branch targets, post-branch instructions)*

7. **How do you detect loops in a CFG?**
   *(DFS back edges; dominator-based natural loop detection)*

8. **What is PTX and how does it relate to SASS?**
   *(Virtual vs real ISA; PTX is target-independent, SASS is architecture-specific)*

9. **Why are SASS encodings not public, and what would you do about it?**
   *(Proprietary IP; reverse-engineering is possible but not necessary — learn the algorithms on a spec you control)*

10. **How would you validate a disassembler at scale?**
    *(Differential testing, fuzz testing, round-trip testing, known-answer tests)*
