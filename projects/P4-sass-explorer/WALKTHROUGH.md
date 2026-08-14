# Walkthrough: Exploring Real PTX and SASS

This is a guided reading exercise. Follow the steps, answer the questions,
then check the expected answers below.

## Prerequisites

- CUDA Toolkit installed (nvcc, cuobjdump, nvdisasm on PATH)
- Python 3 installed

If you don't have CUDA Toolkit, you can still read the sample PTX file
in `tests/sample.ptx` and work through Part B.

---

## Part 1: Generate PTX from CUDA

```bash
cd scripts
python dump_ptx_sass.py sample_kernel.cu --arch sm_89 --lineinfo
```

Open `sample_kernel_dump/sample_kernel.ptx` in a text editor.

### Questions

**Q1**: How many `.entry` functions (kernels) are in the PTX?
> Expected: 2 (`reduce_sum` and `saxpy`)

**Q2**: Look at `reduce_sum`. How many `.reg` declarations are there?
What register types do you see (`.pred`, `.b32`, `.b64`, `.f32`)?
> Expected: You'll see predicate registers (`%p<N>`), 32-bit registers
> (`%r<N>`), and 64-bit registers (`%rd<N>`) at minimum.

**Q3**: Find the `bar.sync` instruction. What does it do?
> Expected: It's a thread barrier — all threads in the block must reach
> this point before any can proceed. In SASS, this becomes a `BAR` or
> `SYNC` instruction.

**Q4**: Find the `@%p1 bra` instruction. What is `@%p1`?
> Expected: It's a predicate guard. The branch only executes if predicate
> register `%p1` is true. This is how GPUs implement conditionals without
> branching — predicated execution.

**Q5**: Look at the loop body (between `LOOP_HEAD:` and `LOOP_END:`).
How does the PTX implement `stride >>= 1`?
> Expected: `shr.u32 %rN, %rN, 1` — a right shift by 1.

---

## Part 2: Examine the SASS

If you have cuobjdump or nvdisasm, examine the SASS output.

```bash
# In sample_kernel_dump/
```

### Questions

**Q6**: How many SASS instructions correspond to one PTX `ld.global.f32`?
> Expected: Usually 1-3 SASS instructions. The address computation may be
> separate, and there may be a `LDG` (load global) instruction.

**Q7**: What happened to the `bar.sync` in SASS?
> Expected: It becomes a `BAR.SYNC` instruction, possibly with additional
> scheduling information (control codes) in the encoding.

**Q8**: Can you identify the loop back-edge in the SASS? How?
> Expected: Look for a `BRA` instruction that targets an earlier address.
> The target address will be lower than the branch's own address.

**Q9**: How many registers did `ptxas` allocate for `reduce_sum`?
> Expected: Check the cuobjdump header — it reports register usage per
> kernel. Typically 16-32 registers for this kernel.

**Q10**: Compare the PTX `atomicAdd` with its SASS representation.
> Expected: It becomes an `ATOMS` (atomic shared) or `RED` (reduction)
> instruction in SASS.

---

## Part 3: Analyze with the PTX tools

```bash
cd python
python ptx_tool.py stats ../tests/sample.ptx
python ptx_tool.py cfg ../tests/sample.ptx
python ptx_tool.py dot ../tests/sample.ptx
```

### Questions

**Q11**: How many basic blocks does the CFG show for `vector_reduce`?
> Expected: At least 5-6 blocks (entry, bounds check, loop head,
> loop body, loop skip, loop end, early exit).

**Q12**: How many back-edges (loops) are detected?
> Expected: At least 1 (the reduction loop).

**Q13**: What is the instruction mix? What percentage are memory operations?
> Run `stats` and look at the `ld` and `st` counts.

---

## Part 4: Compare PTX vs SASS

```bash
cd scripts
python compare_ptx_sass.py sample_kernel_dump/sample_kernel.ptx \
                           sample_kernel_dump/sample_kernel_cuobjdump.sass
```

### Questions

**Q14**: What is the expansion ratio (SASS instructions per PTX instruction)?
> Expected: Typically 1.5x to 3x. PTX is higher-level; the compiler
> expands, schedules, and may split instructions.

**Q15**: Can you see where the compiler unrolled or optimized?
> Expected: Look for patterns where one PTX instruction becomes multiple
> SASS instructions, or where PTX instructions disappear (constant folding,
> dead code elimination).

---

## Key Takeaways

1. **PTX is a virtual ISA** — it abstracts hardware details. SASS is the
   real machine code, architecture-specific, and changes every generation.

2. **The compiler (ptxas) does significant optimization** between PTX and
   SASS: register allocation, instruction scheduling, loop unrolling,
   predication optimization.

3. **Predicated execution** is fundamental to GPU programming — it avoids
   divergent branches by executing both paths and masking results.

4. **Barriers** are essential for shared memory correctness — they ensure
   all threads have written before any thread reads.

5. **Understanding the toolchain** (nvcc → PTX → ptxas → SASS) is as
   important as understanding the ISA itself.
