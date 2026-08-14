# Exercises for P2: cmdgen

These exercises are graded from straightforward to challenging. Each one builds
on the codebase you already have running. Do them in order — later exercises
assume you completed the earlier ones.

---

## 1. Add a new opcode `XOR` — and prove the table-driven claim (Level 1)

**Goal:** Validate the project's core architectural claim: adding a new
instruction requires editing *only* `isa_table.cpp` (plus the `Opcode` enum
in `ir.h`). This teaches **table-driven design** — the encoder, decoder,
printer, and builder all derive behaviour from the table, so they need zero
code changes for a new opcode.

**Hint:**
1. Add `XOR` to the `Opcode` enum at the end (before any `COUNT` sentinel).
2. Add a row to each architecture's table in `src/isa_table.cpp`. Choose a
   unique opcode bit pattern and field layout for ARCH_A and ARCH_B. Model it
   on `AND`/`OR` — three register operands (dst, src0, src1).
3. Add a `xor_op()` convenience method to `WorkloadBuilder` (one line, same
   pattern as `bit_and()`).
4. Add a round-trip test: build an instruction, encode it, decode it, and
   verify all operands survive.
5. Run `cmdgen disasm` on a file containing the new opcode and confirm the
   printer already handles it (because the printer walks the decoded IR, which
   got its fields from the table).

**Expected observation:** Your diff touches exactly three files: `ir.h`
(enum), `isa_table.cpp` (table rows), and `builder.h` (convenience method).
The encoder, decoder, and printer required **zero edits**. This is the payoff
of table-driven design — contrast this with an approach where each opcode has
a hand-written `encodeXOR()` / `decodeXOR()` function.

**Interview connection:** *"How would you design an instruction encoder so
that adding a new GPU instruction doesn't require touching every pass?"*
Table-driven architecture is the standard answer. This exercise lets you point
to concrete code.

---

## 2. Add a third architecture `ARCH_C` via the registry (Level 2)

**Goal:** Add `ARCH_C` with a deliberately different encoding for at least one
instruction that ARCH_A and ARCH_B agree on (e.g., swap the field positions of
`src0` and `src1` in `ADD`). Register it through the `EncoderRegistry` without
editing any existing encoder code. This teaches **self-registering factories**
and the **Open/Closed Principle**.

**Hint:**
- Add a new `Architecture` enum value and a third table in `isa_table.cpp`.
- Add a `REGISTER_ENCODER(ArchC, "ARCH_C", Architecture::ARCH_C)` line in
  `encoder.cpp`.
- The tricky part: make sure the decoder also knows about ARCH_C. Since it
  reads the same table, it should "just work" — verify this with a round-trip
  test on every opcode.
- Write a test that encodes an `ADD` on ARCH_A and ARCH_C and asserts the raw
  `uint32_t` words are *different* — this proves the different encoding is
  actually in effect.

**Expected observation:** The registry finds three encoders at runtime. The
round-trip passes. The raw bits differ where you designed them to differ.
Existing tests for ARCH_A/B remain green — you didn't break anything.

**Interview connection:** *"How do you add hardware support for a new GPU
variant without destabilizing existing back-ends?"* Point to the registry
pattern and the fact that ARCH_A/B tests never saw your change.

---

## 3. Add a Decorator that logs every encode call (Level 2)

**Goal:** Wrap an existing `Encoder` in a `LoggingEncoder` that prints every
instruction before forwarding to the real encoder. This demonstrates the
**Decorator pattern** — same interface, augmented behaviour, no inheritance
explosion.

**Hint:**
- Create `include/logging_encoder.h`. The class holds a reference (or
  `unique_ptr`) to an `Encoder` and implements the same interface.
- In `encode()`, write the opcode name and operands to an `std::ostream&`
  (passed at construction), then delegate to the wrapped encoder.
- Plug it in by wrapping the encoder returned by the registry:
  ```cpp
  auto base = registry.create("ARCH_A");
  LoggingEncoder logged(std::move(base), std::cerr);
  ```
- Write a test that captures the log into a `std::ostringstream` and asserts
  it contains the expected opcode names.

**Expected observation:** The encoded bytes are bit-identical with and without
the decorator — it is purely additive. The log stream shows one line per
instruction. This is how real tool chains add tracing without modifying core
code.

**Interview connection:** *"Decorator vs. inheritance — when do you choose
which?"* Decorator composes at runtime, avoids the combinatorial explosion of
`LoggingArchAEncoder`, `LoggingArchBEncoder`, etc.

---

## 4. Add a validation pass that rejects illegal operand combinations (Level 3)

**Goal:** Before encoding, walk the program and reject illegal operand
combinations with clear error messages: immediate out of the field's bit range,
register index exceeding the architecture's register count, wrong operand count
for an opcode. This teaches **defensive programming** and the concept of a
**validation pass** separate from the transform.

**Hint:**
- Create `include/validator.h` / `src/validator.cpp`. The validator takes an
  `Architecture` and walks each `Instruction`.
- For each opcode, look up its ISA table entry and check: (a) the number of
  operands matches the number of field specs, (b) each immediate fits in the
  field's bit width (signed: `-(1 << (w-1))` to `(1 << (w-1)) - 1`), (c)
  register indices are `< register_count` from the table.
- Return a `std::vector<std::string>` of error messages (not exceptions) so
  the caller sees *all* problems, not just the first.
- Wire it into `main.cpp` before the encode step. Write tests with
  deliberately out-of-range operands.

**Expected observation:** A program with register index 999 produces a clear
error like `"Instruction 3 (ADD): src0 register 999 exceeds register file
size 8"`. Encoding never happens. This is how real assemblers behave.

**Interview connection:** *"What validation do you perform before encoding a
GPU command buffer?"* and *"Why return all errors instead of failing on the
first?"* (Better developer UX, same pattern as compiler diagnostics.)

---

## 5. Implement delta-debugging test minimization over the IR (Level 4)

**Goal:** Given a generated program that triggers a "failure" (any predicate
you define — e.g., "the decoded program differs from the original"), shrink it
to a **minimal failing program** by removing instructions and blocks. This is
the single most job-relevant exercise in the entire study pack — test case
minimization is a core technique for GPU tool verification.

**Hint:**
- Implement the classic ddmin algorithm (Zeller, 1999): partition the
  instruction list into N chunks, try removing each chunk, keep the removal if
  the predicate still fails, increase granularity if no single chunk can be
  removed.
- Start simple: treat the program as a flat instruction list (ignoring block
  structure) and rebuild block boundaries after each removal.
- The predicate should be a `std::function<bool(const Program&)>`.
- For testing, create a synthetic predicate like "the program contains an
  instruction with opcode LOAD *and* register 3 as destination" — then
  verify the minimizer produces a program with exactly one instruction.
- Edge case: make sure the minimizer terminates even if the predicate always
  returns true (i.e., every subset "fails").

**Expected observation:** A 100-instruction failing program shrinks to 1–3
instructions. The number of predicate evaluations is O(n log n), not O(2^n).
This is dramatically more useful than "just re-run with a smaller seed".

**Interview connection:** *"A GPU test fails on a 10,000-instruction stimulus.
How do you find the minimal reproducer?"* Delta debugging. This is bread and
butter for NVIDIA's GPU verification teams. Expect follow-ups on how you'd
handle stateful instructions (register dependencies) during minimization.

---

## 6. Add a control-flow-graph builder and emit Graphviz DOT (Level 3)

**Goal:** Build a CFG over the IR — identify basic block leaders, build edges
from branch instructions, detect back edges (loops). Emit the graph in
Graphviz DOT format. This teaches **graph algorithms on IR** — a daily task in
compiler and tool development.

**Hint:**
- Create `include/cfg.h` / `src/cfg.cpp`. A `CFGNode` holds a block index and
  a list of successor indices. Build the graph by scanning each block's last
  instruction: if it's a `BRANCH`, add an edge to the target block; also add
  a fall-through edge to the next block unless it's an unconditional branch.
- For back-edge detection, do a DFS and classify edges as tree/forward/back/
  cross. A back edge implies a loop.
- Emit DOT:
  ```
  digraph CFG {
    bb0 -> bb1;
    bb1 -> bb2;
    bb2 -> bb1 [style=dashed, label="back"];
  }
  ```
- Add a `cmdgen cfg --out graph.dot file` subcommand.

**Expected observation:** `dot -Tpng graph.dot -o graph.png` produces a
readable control-flow diagram. Back edges are visually distinct. This is how
real compiler visualization tools work (LLVM's `-dot-cfg`).

**Interview connection:** *"How would you detect loops in a GPU kernel's
control flow?"* DFS-based back-edge detection on the CFG. Follow-up: what
about irreducible control flow?

---

## 7. Add golden-file tests with `--update-golden` (Level 3)

**Goal:** Capture the printer's disassembly output as a "golden file" and
compare against it in CI. Add an `--update-golden` flag that overwrites the
golden file with current output. This teaches **snapshot/golden-file testing**
— when it helps and when it hurts.

**Hint:**
- Create a `tests/golden/` directory with `.expected` files.
- In the test, generate a known program (fixed seed), run the printer into a
  string, and compare against the `.expected` file byte-for-byte.
- If `--update-golden` is passed (check `argv`), write the current output to
  the `.expected` file instead of comparing.
- The tricky part: golden tests are brittle. If you change the printer format,
  every golden file breaks. Document this trade-off in a comment.
- Discuss in a comment: when are golden tests valuable? (Catching unintended
  regressions in user-visible output.) When do they hurt? (Trivial format
  changes cause noisy failures; they test the output format, not the semantics.)

**Expected observation:** `cmdgen_tests` passes when the golden file matches.
Changing the printer format causes a clear diff-style failure message. Running
with `--update-golden` regenerates the file and the next run passes.

**Interview connection:** *"How do you test a disassembler's output?"* Golden
tests are the industry standard. Follow-up: how do you keep them maintainable
as the ISA evolves?

---

## 8. Add a JSON serializer/deserializer for the IR (Level 3)

**Goal:** Serialize a `Program` to JSON and deserialize it back. Round-trip
test it. This teaches **serialization** and forces you to think about the IR's
type system (how do you represent a tagged Operand in JSON?).

**Hint:**
- Don't pull in a JSON library — hand-write a simple emitter (it's just string
  concatenation with proper escaping) and a simple parser (recursive descent
  or even just `std::string::find` for this constrained schema).
- Structure:
  ```json
  {
    "blocks": [
      { "name": "entry", "instructions": [
        { "opcode": "ADD", "operands": [
          { "kind": "register", "value": 1 },
          { "kind": "register", "value": 2 },
          { "kind": "register", "value": 3 }
        ]}
      ]}
    ]
  }
  ```
- The tricky part: `MemoryObject` operands have two fields (`base_register`,
  `offset`); immediate operands are signed. Make sure the deserializer
  reconstructs the correct `Operand` variant.
- Round-trip test: `program == deserialize(serialize(program))` for several
  programs (hand-built and randomly generated).

**Expected observation:** The JSON is human-readable, the round-trip is exact,
and the serializer handles all operand kinds. This is how real tools exchange
IR (LLVM's JSON-based remarks, SPIR-V's JSON mapping).

**Interview connection:** *"How would you export a GPU command buffer for
offline analysis?"* Serialization to a human-readable format. Follow-up: what
are the trade-offs vs. a binary format? (Size, parse speed, readability,
schema evolution.)

---

## 9. Add coverage tracking for generated test suites (Level 4)

**Goal:** Record which opcodes, operand kinds, and field value ranges were
exercised by a generated suite, and report gaps. This connects directly to
**coverage-driven verification** — a core concept in module M7 (tooling) and
a major part of NVIDIA's GPU verification methodology.

**Hint:**
- Create `include/coverage.h`. Define a `CoverageTracker` that observes a
  program (or a stream of programs) and records:
  - Which opcodes appeared (and how many times).
  - Which operand kinds were used for each operand slot.
  - The min/max values seen for immediates and register indices.
  - Which architecture-specific bit patterns were exercised.
- After tracking, report "holes": opcodes never generated, operand kinds never
  combined with certain opcodes, immediate ranges never reaching the sign-
  extension boundary (e.g., -1, min, max).
- Wire it into `cmdgen stats` as an optional `--coverage` flag.
- Write a test: generate 1000 instructions with a known seed, assert every
  opcode appears at least once (the generator already guarantees this), and
  assert that at least one negative immediate was generated.

**Expected observation:** The coverage report shows 100% opcode coverage but
likely <100% on field-value-range boundaries. This is exactly the gap that
coverage-driven fuzzing addresses — the generator is good but not exhaustive.

**Interview connection:** *"How do you know your GPU test generator is
exercising all instruction encodings?"* Coverage tracking + gap analysis. This
is how NVIDIA's verification teams measure test quality. Follow-up: how would
you use the coverage report to guide the random generator toward uncovered
cases? (Feedback-directed generation.)

---

## 10. Make the printer match a realistic disassembly format (Level 3)

**Goal:** Rewrite the `PrinterVisitor` output to look like a real disassembler:
```
0x0000:  0xA3010203  ADD   R1, R2, R3
0x0004:  0x50040010  LOAD  R4, [R0 + 16]
0x0008:  0xE0000001  BRANCH @bb1 (P0)
```
Each line shows: address (hex), raw encoded word (hex), mnemonic, operands.
Write a test that the output is parseable by a regex or simple line parser,
proving it could be consumed by a downstream tool.

**Hint:**
- The printer now needs access to the *encoded* bytes as well as the decoded
  IR. One approach: pass both the `std::vector<uint32_t>` (encoded) and the
  decoded `Program` to the printer side by side.
- For the address column, multiply the instruction index by 4 (each encoded
  word is 4 bytes).
- For the operand column, format registers as `R<n>`, memory as `[R<n> + offset]`,
  immediates as signed decimal, predicates as `(P<n>)`.
- The tricky part: branch targets should show the block name, not just a raw
  index. Use the decoded program's block names.
- Write a test that parses each output line with a regex like
  `^0x([0-9A-F]+):\s+0x([0-9A-F]+)\s+(\w+)\s+(.*)$` and asserts the address
  increments by 4, the raw word matches the encoded data, and the mnemonic
  matches the opcode.

**Expected observation:** The output looks like something `objdump` or
`nvdisasm` would produce. The parser test proves the format is machine-
readable, not just human-readable. This is the format that P4's CUDA binary
parser would consume in a real tool chain.

**Interview connection:** *"Design a disassembler output format for a GPU ISA."*
This exercise gives you a concrete answer with trade-offs (fixed-width columns
for alignment, hex encoding for debuggability, symbolic names for readability).
