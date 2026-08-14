# P2-cmdgen

A compact teaching project for **architecture-neutral command generation**. The code builds a small IR, encodes it with a **table-driven back-end**, decodes it with the **same table**, and inspects the result with **Visitor** objects.

```text
+--------------------+      +-------------------+      +-------------------------+
| front-end builder  | ---> | architecture-     | ---> | encoder back-ends       |
| / random generator |      | neutral IR        |      | (table-driven rules)    |
+--------------------+      +-------------------+      +-------------------------+
                                                                  |
                                                                  v
                                                          +---------------+
                                                          | byte stream    |
                                                          +---------------+
                                                                  |
                                                                  v
+--------------------+      +-------------------+      +-------------------------+
| visitor-based      | <--- | decoder using the | <--- | shared ISA description  |
| printer / stats    |      | same ISA table    |      | (mask/match + fields)   |
+--------------------+      +-------------------+      +-------------------------+
```

## Why this project exists

Real GPU and accelerator tools often need a clean split between:

1. a **portable intermediate representation**,
2. one or more **encoding back-ends**, and
3. **analysis / inspection** passes that should not care about instruction bit layouts.

This project keeps those concerns separate on purpose.

## Learning objectives

After reading the code, you should be able to explain:

- why the IR must not know about bit offsets or opcode masks,
- how a **single ISA table** can drive both encoding and decoding,
- how a **self-registering factory** removes `if/else` chains from back-end selection,
- how the **Visitor pattern** cleanly separates traversal from analysis,
- how a **Builder** makes workload creation readable,
- how to write **deterministic seeded random** generators for tests and repros.

## Project layout

- `include/ir.h` - neutral IR data structures
- `include/isa_table.h`, `src/isa_table.cpp` - all encoding rules
- `include/encoder.h`, `src/encoder.cpp` - table-driven encoders + registry
- `include/decoder.h`, `src/decoder.cpp` - table-driven decoder
- `include/visitor.h` - printer and statistics visitors
- `include/builder.h` - fluent program builder
- `include/random_gen.h`, `src/random_gen.cpp` - deterministic random generator
- `src/main.cpp` - CLI tool
- `src/tests.cpp` - assertion-based tests

## Build

### Configure

```bash
cmake -S . -B build
```

### Build

```bash
cmake --build build
```

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

## Run

Generate a binary command stream:

```bash
build/cmdgen gen --seed 123 --count 32 --arch A --out sample.cmd
```

Dry run without writing a file:

```bash
build/cmdgen gen --seed 123 --count 16 --arch B --dry-run
```

Disassemble a binary stream:

```bash
build/cmdgen disasm sample.cmd
```

Collect statistics:

```bash
build/cmdgen stats sample.cmd
```

## Acceptance criteria

The project is considered complete when all of the following are true:

- **Encode -> decode round-trips work for every instruction form** on both fake architectures.
- **Encoder and decoder share one ISA table**, so there is no duplicated bit layout logic.
- **Changing opcode encodings requires editing only `isa_table` rules** for existing IR forms.
- **The same seed produces byte-identical output** every time.
- **Different seeds usually produce different byte streams**.
- **The encoder registry discovers both back-ends without manual wiring in `main()`**.

## Notes about the fake ISAs

- `ARCH_A` uses the high nibble for opcode selection.
- `ARCH_B` uses a low-byte opcode tag and deliberately moves fields around.
- The differences are intentional: they prove that the IR is not tied to either wire format.

## Questions to answer after finishing

1. What information belongs in the IR, and what information belongs only in the ISA table?
2. Why is using the **same table** for encode and decode safer than duplicating logic?
3. What trade-off does a self-registering factory make compared with explicit construction?
4. How does the Visitor pattern help with the “add new operation vs. add new data type” tension?
5. Which parts of the random generator are most important for reproducibility?
6. If you added `MAD` or `SHL`, what would change, and what should stay untouched?
7. Why is `unordered_map` iteration order a determinism risk in generators and tests?
8. Why do sign-extension bugs often appear first in decoders and tests?
