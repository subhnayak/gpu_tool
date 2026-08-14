# Exercises — Graded, from table edits to a tiny debugger

These exercises are ordered by difficulty. Each builds on the project and
teaches skills directly relevant to GPU tools development.

---

## Exercise 1: Add an Instruction (Easy — Table Only)

Add a `NOT` instruction (bitwise NOT, `RD = ~RS1`) to the toy ISA.

**What to do:**
- Add one row to the opcode table in `include/toy_isa.h` (use opcode 0x00, func 0x07 is taken — pick an unused slot like opcode 0x03, func 0)
- Add the same row to `python/isa_table.py`
- Verify: the existing tests still pass, and you can assemble/disassemble `NOT R1, R2` (RS2 is ignored)

**Lesson:** Adding an instruction should be a *data* change, not a *code* change. If it requires modifying switch statements, your architecture is wrong.

---

## Exercise 2: Add a New Instruction Format (Medium)

Design a Format S (Shift-Immediate): `opcode(6) + rd(5) + rs1(5) + shamt(5) + unused(7) + pred(4)`.

Add `SHLI` (shift left by immediate) and `SHRI` using this format.

**What to do:**
- Add `InstFormat::S` to the enum
- Define field specs for the new format
- Add entries to the opcode table
- Update the decoder to handle Format S
- Update the assembler to handle Format S
- Write round-trip tests

**Lesson:** Real ISAs evolve by adding formats. Your decoder must be extensible.

---

## Exercise 3: Decision-Tree Decoder (Medium-Hard)

Build a decision-tree decoder that pre-computes a lookup structure indexed
by opcode bits, instead of linearly scanning the table.

**What to do:**
- At startup, build an array of 64 entries (indexed by the 6-bit opcode field)
- For opcode 0x00 (which uses the func field), build a sub-array of 8 entries
- Benchmark against the linear scan on 1M random words

**Lesson:** This is how real disassemblers achieve O(1) decode. The tradeoff: build time vs query time. For 30 entries the linear scan is fine; for 2000 SASS opcodes, you need a tree.

---

## Exercise 4: Interference-Graph Register Allocator (Hard)

Implement a graph-coloring register allocator over the toy ISA IR.

**What to do:**
- Compute liveness intervals for each virtual register in a basic block
- Build an interference graph (two regs interfere if their live ranges overlap)
- Color the graph with K colors (K = number of physical registers)
- If coloring fails, insert spill code (store to memory, reload)
- Verify: the program still produces the same results when interpreted

**Lesson:** Register allocation is the most important compiler optimization for GPUs. Understanding it helps you build better profiling tools.

---

## Exercise 5: Peephole Optimizer (Medium)

Implement a peephole optimizer that scans decoded instructions for common patterns and replaces them.

**Patterns to detect:**
- `ADDI Rx, Ry, 0` → delete (identity operation)
- `ADD Rx, Ry, R0` → `ADDI Rx, Ry, 0` → delete
- Consecutive `LD` and `ST` to the same address with no intervening writes → delete the ST
- `BRA` to the next instruction → delete

**Lesson:** Peephole optimizers are used in every compiler backend. The key is pattern matching on the IR, not the binary.

---

## Exercise 6: Symbolic Executor (Medium-Hard)

Write a symbolic executor for straight-line toy ISA code (no branches).

**What to do:**
- Represent each register as a symbolic expression (e.g., `R1 = param_0 + 42`)
- Execute each instruction symbolically, building expression trees
- At the end, print the symbolic state of each register

**Lesson:** Symbolic execution is the basis for formal verification, test generation, and some debugging techniques.

---

## Exercise 7: Valid-Program Fuzzer (Medium)

Write a fuzzer that generates *valid* assembly programs randomly and round-trips them.

**What to do:**
- Randomly select instructions from the table
- Generate random valid register and immediate operands
- For branches, generate labels that point to valid locations
- Assemble, disassemble, assemble again, verify the binary is identical

**Lesson:** This is a powerful testing technique. The key insight: generating *valid* random programs is much harder than generating random *bytes*, but it tests more interesting properties.

---

## Exercise 8: PTX Liveness Analysis (Hard)

Extend the PTX parser to compute register liveness.

**What to do:**
- For each basic block, compute `def` and `use` sets
- Run iterative dataflow analysis: `live_in(B) = use(B) ∪ (live_out(B) - def(B))`
- `live_out(B) = ∪ live_in(S) for all successors S of B`
- Print the number of live registers at each program point

**Lesson:** Liveness is the foundation of register allocation and is used by profiling tools to estimate register pressure.

---

## Exercise 9: Natural Loop Detection & Nesting (Medium)

Extend the CFG builder to detect natural loops and compute their nesting depth.

**What to do:**
- A natural loop for back edge (u, v) is: {v} ∪ {all nodes w such that v dominates w and w can reach u without going through v}
- Compute the loop body for each back edge
- Determine nesting: loop L1 is nested inside L2 if L1's body ⊂ L2's body
- Print each loop with its nesting depth

**Lesson:** Loop nesting analysis is critical for GPU performance — deeply nested loops often indicate register pressure and occupancy issues.

---

## Exercise 10: PTX-to-ToyISA Translator (Hard)

Write a translator from a subset of PTX to the toy ISA.

**What to do:**
- Map PTX integer registers to toy ISA registers
- Translate `add.s32` → `ADD`, `ld.global` → `LD`, `bra` → `BRA`, etc.
- Handle predication: `@%p1 bra` → `@P1 BRA`
- The translation will be lossy (PTX has types, the toy ISA doesn't) — document the limitations

**Lesson:** ISA translation is a real task in GPU tools (PTX → SASS is done by ptxas). Even a toy version teaches you about impedance mismatches between ISAs.

---

## Exercise 11: DWARF-Style Line Mapping (Hard)

Add source-line tracking to the toy assembler.

**What to do:**
- Record which source line each instruction came from
- Emit a line-number table in the binary (as a trailer after the code, with a sentinel)
- In the disassembler, read the table and annotate output with source lines
- Format: `0x0008: ADDI R1, R1, -1    ; counted_loop.asm:6`

**Lesson:** Debug info (DWARF, PDB) is essential for debuggers. The line table is the simplest piece, but it teaches the core concept: mapping binary addresses to source locations.

---

## Exercise 12: Tiny Debugger (Very Hard)

Build an interpreter for the toy ISA with breakpoint and single-step support.

**What to do:**
- Implement a simple interpreter: fetch → decode → execute loop
- 32 registers, 64KB memory, a PC
- Commands: `run`, `step`, `break <addr>`, `continue`, `regs`, `mem <addr> <count>`, `quit`
- When hitting a breakpoint, print the current instruction and PC
- Support watchpoints: `watch R5` — break when R5 changes

**Lesson:** This is a minimal debugger. Real GPU debuggers (cuda-gdb, Nsight) work the same way but with hardware breakpoint support, warp-level state, and remote debug protocols. The concepts are identical.
