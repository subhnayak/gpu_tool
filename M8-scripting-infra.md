# M8 — Scripting, Infrastructure & Engineering Practice

> **Why this matters for this role.** The job description says "strong scripting
> skills desired," and in chip-development organizations that is not a soft
> preference. The C++ framework is the engine, but everything around it — test
> harnesses, regression runners, log and trace parsers, result databases, report
> generators, build glue, bisection scripts — is Python and shell. A tools engineer
> who can only write C++ ships half a tool. Worse, they end up writing 400 lines of
> C++ for something that should have been 30 lines of Python, which nobody on the
> team will maintain.
>
> **Study this module continuously, not as a block.** You learn CMake and pytest by
> using them in P1 through P7, not by reading about them. Read the sections here
> when you hit the corresponding need in a project.

---

## 1. Python for tooling

You do not need to be a Python expert. You need to be *fluent and idiomatic* in the
specific slice of Python that infrastructure work uses: CLIs, file and binary
parsing, data classes, subprocess orchestration, and tests.

### 1.1 Command-line interfaces

Every tool you write will be invoked from a script, from CI, and by a frustrated
engineer at 2am. The CLI is the user interface, so it deserves care.

```python
#!/usr/bin/env python3
"""Run a generated stimulus suite and report results."""
from __future__ import annotations
import argparse
import sys
from pathlib import Path


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="runsuite",
        description="Generate, run and check GPU stimulus workloads.",
        # shows defaults in --help, which saves an enormous amount of user pain
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("suite", type=Path, help="Path to the suite definition (YAML).")
    p.add_argument("--seed", type=int, default=0,
                   help="Master RNG seed. Same seed must reproduce the run exactly.")
    p.add_argument("--count", type=int, default=100, help="Number of tests to generate.")
    p.add_argument("--jobs", "-j", type=int, default=1, help="Parallel workers.")
    p.add_argument("--out", type=Path, default=Path("results"), help="Output directory.")
    p.add_argument("--dry-run", action="store_true",
                   help="Generate and validate, but do not execute.")
    p.add_argument("-v", "--verbose", action="count", default=0,
                   help="Increase log verbosity (repeatable).")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.suite.exists():
        print(f"error: suite not found: {args.suite}", file=sys.stderr)
        return 2
    ...
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

Points worth internalising, because they come up in code review:

- **Return an exit code from `main`, and `raise SystemExit(main())`.** This makes
  the module importable and testable — you can call `main(["suite.yaml", "--dry-run"])`
  from a pytest without spawning a process.
- **Errors to stderr, results to stdout.** Someone will pipe your stdout into another
  tool. If you mix diagnostics into it, you break them.
- **Exit codes are an API.** 0 success, 1 test failure, 2 usage error, and stick to it
  forever. CI depends on it.
- **`--dry-run` is not a luxury** in a stimulus framework. It lets a user validate a
  huge generated suite in seconds without burning simulation hours.
- **`pathlib.Path` over string paths.** Especially on Windows.

`click` is the common third-party alternative and is nicer for nested subcommands
(`tool gen`, `tool run`, `tool report`). Use `argparse` when you want zero
dependencies, which in a locked-down chip-development environment is often the
deciding factor.

### 1.2 Logging, not `print`

```python
import logging

log = logging.getLogger(__name__)

def configure_logging(verbosity: int) -> None:
    level = {0: logging.WARNING, 1: logging.INFO}.get(verbosity, logging.DEBUG)
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

log.info("generated %d tests from seed %d", count, seed)   # lazy %-formatting
log.debug("constraint solver took %.3fs", elapsed)
```

Use `log.info("... %s", x)` rather than f-strings in log calls: the formatting is
skipped entirely when the level is disabled, which matters when you are logging
inside a loop over a million generated instructions.

### 1.3 Dataclasses for structured records

```python
from dataclasses import dataclass, field, asdict
from enum import Enum
import json


class Status(str, Enum):          # str mixin => JSON-serializable directly
    PASS = "pass"
    FAIL = "fail"
    TIMEOUT = "timeout"
    ERROR = "error"


@dataclass(frozen=True, slots=True)
class TestResult:
    name: str
    seed: int
    status: Status
    duration_s: float
    sim_cycles: int = 0
    tags: tuple[str, ...] = ()
    message: str = ""

    @property
    def failed(self) -> bool:
        return self.status is not Status.PASS


r = TestResult("randc_0042", seed=1234, status=Status.FAIL,
               duration_s=812.4, sim_cycles=19_204_113,
               tags=("atomics", "divergence"), message="golden mismatch at pixel (17,3)")
print(json.dumps(asdict(r)))
```

`frozen=True` gives you hashability and prevents accidental mutation of a result
record; `slots=True` cuts memory noticeably when you hold a million of them.

### 1.4 Generators and streaming

Simulation logs are enormous. Never load one into memory.

```python
from typing import Iterator, Iterable
import re

_LINE = re.compile(
    r"^\[(?P<cycle>\d+)\]\s+(?P<engine>\w+)\s+(?P<op>\w+)\s+(?P<rest>.*)$"
)

def parse_log(path) -> Iterator[dict]:
    """Stream a trace log; O(1) memory regardless of file size."""
    with open(path, "r", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            m = _LINE.match(line)
            if m is None:
                continue                      # or count and report skipped lines
            d = m.groupdict()
            d["cycle"] = int(d["cycle"])
            d["lineno"] = lineno
            yield d


def stalls_by_engine(path) -> dict[str, int]:
    out: dict[str, int] = {}
    for rec in parse_log(path):               # never materialises the whole file
        if rec["op"] == "STALL":
            out[rec["engine"]] = out.get(rec["engine"], 0) + 1
    return out
```

Generator pipelines compose: `filter(pred, parse_log(p))`, `itertools.islice`,
`itertools.groupby`. Reach for `itertools` before writing index arithmetic.

### 1.5 Binary parsing with `struct` — the tool-engineer's core skill

You will parse trace files, cubins, ELF sections, and register dumps. `struct` is
how.

```python
import struct
from dataclasses import dataclass

# Trace record layout (little-endian, packed to 24 bytes):
#   u32 magic | u16 version | u16 kind | u64 timestamp | u32 payload_len | u32 crc
_HDR = struct.Struct("<IHHQII")
assert _HDR.size == 24

MAGIC = 0x54524331  # 'TRC1'


@dataclass(frozen=True, slots=True)
class Record:
    kind: int
    timestamp: int
    payload: bytes


def read_records(path) -> "Iterator[Record]":
    with open(path, "rb") as f:
        while True:
            hdr = f.read(_HDR.size)
            if not hdr:
                return                      # clean EOF
            if len(hdr) < _HDR.size:
                raise ValueError(f"truncated header: {len(hdr)} bytes")
            magic, ver, kind, ts, plen, crc = _HDR.unpack(hdr)
            if magic != MAGIC:
                raise ValueError(f"bad magic 0x{magic:08x} at {f.tell()-_HDR.size}")
            if ver != 1:
                raise ValueError(f"unsupported trace version {ver}")
            payload = f.read(plen)
            if len(payload) != plen:
                raise ValueError("truncated payload")
            yield Record(kind, ts, payload)
```

The format-string vocabulary you must know cold:

| Code | Type | Size |
|------|------|------|
| `b` / `B` | int8 / uint8 | 1 |
| `h` / `H` | int16 / uint16 | 2 |
| `i` / `I` | int32 / uint32 | 4 |
| `q` / `Q` | int64 / uint64 | 8 |
| `f` / `d` | float / double | 4 / 8 |
| `s` | char[n] (`16s`) | n |
| `x` | pad byte | 1 |

Prefixes: `<` little-endian **and no alignment padding**, `>` big-endian, `=` native
byte order without padding, `@` native with native alignment (the default — and the
one that will silently ruin your day when the struct size differs between compilers).
**Always write `<` or `>` explicitly in a file format.** Use `memoryview` and
`Struct.unpack_from` to avoid copying when scanning a large mmapped buffer.

For bit-level fields inside an instruction word, use plain integer masking (and note
how directly this mirrors the disassembler work in M7/P4):

```python
def decode(word: int) -> dict:
    return {
        "opcode": (word >> 0)  & 0x7F,
        "dst":    (word >> 7)  & 0x1F,
        "src0":   (word >> 12) & 0x1F,
        "src1":   (word >> 17) & 0x1F,
        "pred":   (word >> 22) & 0x07,
        "imm":    sign_extend((word >> 25) & 0x7F, 7),
    }

def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)
```

### 1.6 Subprocess orchestration

Tool flows shell out constantly — to `nvcc`, `cuobjdump`, a simulator, a job scheduler.

```python
import subprocess, shlex

def run(cmd: list[str], *, timeout: float | None = None, cwd=None) -> subprocess.CompletedProcess:
    log.debug("exec: %s", shlex.join(cmd))
    try:
        cp = subprocess.run(
            cmd, cwd=cwd, timeout=timeout,
            capture_output=True, text=True,    # text=True => str not bytes
            check=False,                       # inspect returncode ourselves
        )
    except subprocess.TimeoutExpired:
        log.error("timeout after %ss: %s", timeout, shlex.join(cmd))
        raise
    if cp.returncode != 0:
        log.error("command failed (%d): %s\n%s", cp.returncode, shlex.join(cmd), cp.stderr)
    return cp
```

Rules: **pass a list, never `shell=True`** with interpolated user data; **always set a
timeout** for anything touching a simulator or a farm; capture stderr separately so
you can attach it to a failure record; log the exact command line so a user can copy-paste
and reproduce. That last one is worth more goodwill than any amount of documentation.

### 1.7 Interview Q&A

**Q: Why do infrastructure teams write tooling in Python instead of C++?**
A: Development speed and maintainability for code whose runtime is dominated by
something else. A regression runner spends 99.9% of its wall time inside a simulator,
so the harness language's speed is irrelevant, while the harness changes weekly as
requirements shift. Python also has batteries for exactly this work — argparse,
struct, subprocess, pathlib, json, sqlite3 — and, importantly, everyone on a chip team
can read and patch it. The C++ goes where the performance actually is: the generator's
inner loops and the runtime.
*Follow-up:* **When would you move a Python tool to C++?** — When profiling shows the
tool itself is the bottleneck: parsing multi-gigabyte traces, generating millions of
instructions, or anything in an inner loop that runs per-instruction. The usual move is
not a rewrite but pushing the hot kernel into C++ behind a pybind11 binding, or using
numpy for bulk numeric work, and keeping the orchestration in Python.

**Q: How do you parse a 20 GB simulation log?**
A: Stream it. A generator that yields one parsed record per line keeps memory constant
regardless of file size, and it composes with filters so the whole pipeline stays lazy.
If the format is binary and fixed-size I'd mmap it and use `Struct.unpack_from` over a
memoryview to avoid per-record copies. If I have to do it repeatedly I'd do one pass to
build an index — offsets of interesting events into a sqlite file — so later queries are
seeks instead of scans. And I'd make the parser tolerant: log and count malformed lines
rather than aborting on line 19 million.
*Follow-up:* **The parse is still too slow. Now what?** — Profile first to see whether
it is regex, I/O, or object construction. Regex is usually the culprit: precompile,
anchor the pattern, and cheap-reject lines with a substring check before running the
regex. Object construction is next: use `slots=True` dataclasses or plain tuples.
After that, parallelise across files or byte ranges with multiprocessing, and only then
consider moving the parser to C++.

**Q: What's the difference between `<I` and `@I` in a struct format string?**
A: `<` means little-endian with no alignment padding; `@` means native byte order with
native alignment, and it is the default. For a file or wire format you must always
specify `<` or `>` explicitly, because `@` makes the layout depend on the machine and
the compiler's padding rules — the same script would then read a file differently on
two hosts, and in a verification flow that is a nondeterminism bug that is very hard to
find.
*Follow-up:* **How would you handle a format that changed between tool versions?** —
Put a magic number and a version field in the header, and refuse to guess: read the
version first and dispatch to a per-version reader, with a clear error for versions you
do not support. Keep old readers around so old traces stay analysable, and never
silently reinterpret an unknown version.

**Q: Why avoid `shell=True` in subprocess?**
A: It hands the string to a shell, so any metacharacter in an interpolated value —
a semicolon, a backtick, a space in a path — changes the meaning of the command. That is
a command-injection bug and, more mundanely, a portability and quoting nightmare. Passing
a list gives the arguments straight to `exec` with no interpretation. The only reason to
use a shell is when you genuinely need shell features like pipelines, and then it is
better to build the pipeline with multiple `Popen` objects.
*Follow-up:* **How do you make failures from a subprocess debuggable?** — Log the exact
command with `shlex.join` so it is copy-pasteable, capture stdout and stderr separately,
record the return code and the working directory, and attach all of it to the failure
record. Also always set a timeout, and on timeout kill the whole process group, not just
the child, or you leak simulator processes onto the farm.

**Q: How do you make a Python tool testable?**
A: Separate the pure logic from I/O and process launching. Parsing should be a function
from an iterable of lines to records, not something that opens files itself; the
subprocess call should be behind a small interface I can substitute in a test. Then
`main(argv)` returns an exit code rather than calling `sys.exit`, so tests drive the CLI
directly. That way most of the test suite runs in milliseconds without a simulator, and
only a thin integration layer needs the real tools.
*Follow-up:* **How do you test the part that does need the real tool?** — Mark those as
integration tests and gate them (a pytest marker, skipped when the tool is not on PATH),
so the fast suite still runs everywhere. For the boundary itself, record real tool output
once and replay it as a fixture — that catches parser regressions without needing the tool.

---

## 2. Binding C++ to Python — pybind11

This is the pattern for a real stimulus framework: a fast C++ core, driven by a Python
front end where tests are actually described. It is exactly what P7 builds.

```cpp
// bindings.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>        // std::vector <-> list, std::string, std::optional
#include <pybind11/numpy.h>      // py::array_t for zero-copy numeric data
#include "workload.h"

namespace py = pybind11;

PYBIND11_MODULE(cmdgen, m) {
    m.doc() = "GPU stimulus generation core";

    py::enum_<Opcode>(m, "Opcode")
        .value("LOAD",  Opcode::Load)
        .value("STORE", Opcode::Store)
        .value("FMA",   Opcode::Fma)
        .export_values();

    py::class_<Instruction>(m, "Instruction")
        .def(py::init<Opcode, uint32_t, uint32_t, uint32_t>(),
             py::arg("op"), py::arg("dst"), py::arg("src0"), py::arg("src1") = 0)
        .def_readwrite("op", &Instruction::op)
        .def_readwrite("dst", &Instruction::dst)
        .def("__repr__", &Instruction::to_string);

    py::class_<Workload>(m, "Workload")
        .def(py::init<std::string>(), py::arg("name"))
        .def("append", &Workload::append, py::arg("insn"),
             "Append an instruction to the workload.")
        .def("encode", [](const Workload& w) {
            auto bytes = w.encode();
            return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }, "Encode to a method stream and return it as bytes.")
        .def("__len__", &Workload::size)
        .def_property_readonly("name", &Workload::name);

    // Release the GIL for long-running C++ work so Python threads keep running.
    m.def("generate_random", &generate_random,
          py::arg("seed"), py::arg("count"),
          py::call_guard<py::gil_scoped_release>());
}
```

```python
# test_gen.py
import cmdgen

w = cmdgen.Workload("smoke")
w.append(cmdgen.Instruction(cmdgen.Opcode.FMA, dst=1, src0=2, src1=3))
blob = w.encode()
assert len(w) == 1 and isinstance(blob, bytes)
```

Things that bite people:

- **The GIL.** Long C++ calls must release it (`py::call_guard<py::gil_scoped_release>`)
  or they serialise every Python thread in the process.
- **Ownership.** Decide deliberately between `py::return_value_policy::copy`,
  `reference`, and `reference_internal`. Returning a raw pointer to a C++-owned object
  with the wrong policy is the classic way to produce a use-after-free that presents as
  a random Python crash.
- **Zero copy for bulk data.** Return `py::array_t<float>` over a buffer with a capsule
  owner rather than copying a 500 MB result into a list.
- **Exceptions translate.** A C++ `std::runtime_error` becomes a Python `RuntimeError`;
  register custom exception translators for your own error types so users get meaningful
  Python-side errors.

Alternatives worth being able to name: `ctypes`/`cffi` (no build step, good for a stable
C ABI, painful for C++ objects), Cython (mature, more machinery), and nanobind
(pybind11's leaner successor). For a C++ codebase with real classes, pybind11 is the
default answer.

### Interview Q&A

**Q: You have a C++ stimulus generator and users want to script it. How do you expose it?**
A: I'd bind the C++ core to Python with pybind11 and keep the test-description layer in
Python. The C++ keeps the performance-critical generation, encoding and checking; Python
gets a fluent API for describing what to generate, which is where the iteration actually
happens. I'd bind a deliberately small, stable surface — the workload builder, the
instruction model, the encode and run entry points — rather than mirroring every internal
class, because every bound symbol becomes an interface I have to keep compatible.
*Follow-up:* **What are the risks of that boundary?** — Object lifetime across the
boundary is the big one: a Python reference outliving the C++ owner gives you a
use-after-free that manifests as a mysterious interpreter crash, so return-value
policies have to be chosen consciously. The GIL is the other: a long C++ call that does
not release it stalls all Python threads. And the build gets heavier — you now ship a
compiled extension that must match the interpreter's ABI and platform.

**Q: When would you use ctypes instead of pybind11?**
A: When the thing I'm calling is already a stable C ABI shared library and I do not
control or want to rebuild it — a driver library, a vendor tool's `.so`. ctypes needs no
compilation and no build integration, which matters in locked-down environments. But it
is the wrong tool for a C++ API with classes, templates or overloads: I'd be
hand-marshalling structs and hoping the layout matches, with no compile-time checking.
pybind11 gives me type safety and real C++ objects at the cost of a build step.
*Follow-up:* **How would you make struct layouts safe with ctypes?** — I would not rely
on guessing: define the layout explicitly with `ctypes.Structure` and `_fields_`, set
`_pack_` to match the C side, and add a runtime assertion comparing `ctypes.sizeof` to a
size the C library reports through an accessor. Better still, have the C library expose
an explicit versioned ABI with getters rather than exposing raw structs at all.

---

## 3. Testing with pytest

```python
# tests/test_decoder.py
import pytest
from cmdgen.decode import decode, DecodeError

def test_decodes_fma():
    # opcode=3 dst=1 src0=2 src1=3
    word = 3 | (1 << 7) | (2 << 12) | (3 << 17)
    insn = decode(word)
    assert insn.op == "FMA" and insn.dst == 1

@pytest.mark.parametrize("word,expected", [
    (0x0000_0001, "LOAD"),
    (0x0000_0002, "STORE"),
    (0x0000_0003, "FMA"),
])
def test_opcode_table(word, expected):
    assert decode(word).op == expected

def test_rejects_unknown_opcode():
    with pytest.raises(DecodeError, match="unknown opcode"):
        decode(0x7F)

@pytest.fixture
def sample_trace(tmp_path):
    p = tmp_path / "t.bin"
    p.write_bytes(make_trace(records=10))
    return p

def test_roundtrip(sample_trace):
    assert len(list(read_records(sample_trace))) == 10
```

The techniques that matter for tool work specifically:

- **Parametrize over an opcode table.** A table-driven decoder deserves a table-driven
  test; adding an opcode should add a row, not a test function.
- **`tmp_path`** for anything touching the filesystem, so tests are hermetic and parallel-safe.
- **Round-trip properties.** `decode(encode(x)) == x` for every instruction form is the
  single highest-value test a disassembler can have.
- **Golden/approval tests** for disassembly output: compare against a checked-in expected
  text file, with a `--update-golden` flag for deliberate changes.
- **Property-based testing with Hypothesis.** For encoders and decoders this is
  transformative — generate random legal instructions and assert the round-trip, and it
  will find the sign-extension bug you did not think of.
- **Markers** (`@pytest.mark.slow`, `@pytest.mark.gpu`) so the fast suite stays fast:
  `pytest -m "not slow"`.

```python
# Property-based round-trip — finds edge cases you would never write by hand
from hypothesis import given, strategies as st

@given(op=st.sampled_from(OPCODES),
       dst=st.integers(0, 31), src0=st.integers(0, 31), src1=st.integers(0, 31),
       imm=st.integers(-64, 63))
def test_encode_decode_roundtrip(op, dst, src0, src1, imm):
    insn = Instruction(op, dst, src0, src1, imm)
    assert decode(encode(insn)) == insn
```

### Interview Q&A

**Q: How would you test a disassembler?**
A: Four layers. First, round-trip properties: assemble then disassemble, or decode then
re-encode, and assert you get back what you started with — ideally with property-based
testing over randomly generated legal instructions, since that finds sign-extension and
field-overlap bugs immediately. Second, a table-driven unit test over the opcode table,
so a new opcode adds a data row. Third, golden-file tests on the printed output to catch
formatting regressions. Fourth, differential testing against an independent reference
disassembler where one exists — if two independently written decoders agree on a million
random words, both are probably right.
*Follow-up:* **You have no reference implementation. What then?** — Then I'd lean harder
on round-tripping and on self-consistency: decode a real binary produced by a known-good
assembler or compiler and check that every byte is consumed, that no instruction decodes
as "unknown," and that control flow reconstructs into a well-formed CFG with no dangling
edges. Those are strong signals without a reference. I would also fuzz with random bit
patterns to ensure the decoder never crashes or loops — robustness matters as much as
correctness for a tool that will be pointed at garbage.

**Q: What's the difference between a fixture and a parametrize in pytest?**
A: A fixture supplies a *dependency* — a temp directory, a parsed file, a device handle
— and handles setup and teardown; parametrize supplies *inputs*, running the same test
body over many values and reporting each as a separate test. They compose: you can
parametrize a fixture so every test using it runs once per configuration, which is how
you run a whole suite against several simulated architectures.
*Follow-up:* **How do you avoid a slow fixture running per test?** — Widen its scope:
`@pytest.fixture(scope="module")` or `"session"` builds it once and shares it. The
tradeoff is isolation — shared mutable state between tests causes order-dependent
failures, which are exactly the kind of nondeterminism this domain cannot tolerate — so
session-scoped fixtures should be immutable or reset explicitly.

---

## 4. Shell, regex and the legacy layer

Chip development flows are old and layered. You will meet Perl, tcsh, and Makefiles
that predate you. You do not need to write new Perl; you need to *read* it, and to be
genuinely good at regex.

### Regex you must know cold

| Construct | Meaning |
|---|---|
| `^ $` | anchors (with `re.M`, per-line) |
| `\b` | word boundary |
| `.*?` | lazy quantifier — the fix for greedy over-matching |
| `(?:...)` | non-capturing group |
| `(?P<name>...)` | named capture — use these, positional groups rot |
| `(?=...)` / `(?!...)` | lookahead / negative lookahead |
| `(?<=...)` / `(?<!...)` | lookbehind / negative lookbehind |
| `re.M / re.S / re.X / re.I` | multiline, dotall, verbose, ignorecase |

```python
# re.X (verbose) makes a serious pattern reviewable
INSN = re.compile(r"""
    ^\s*
    (?P<addr>[0-9a-f]+) : \s+        # address
    (?P<enc>(?:[0-9a-f]{2}\s)+) \s+  # raw encoding bytes
    (?P<mnem>[a-z][\w.]*)            # mnemonic
    (?:\s+ (?P<ops>.*?))?            # optional operands, lazy
    \s*$
""", re.X | re.I)
```

Performance rule: precompile, anchor, and pre-filter with a cheap `in` check before
running an expensive pattern over a hundred million lines. And know when to stop —
if you are parsing nested structure, regex is the wrong tool and you want a real
tokenizer.

### Shell fluency

The pipeline vocabulary that makes log triage fast: `grep -E`, `grep -c`, `sort`,
`uniq -c`, `awk '{print $3}'`, `sed -n '10,20p'`, `head`/`tail -f`, `xargs -P` for
cheap parallelism, `find -name`, `tee`, `cut`, `tr`, `wc -l`, `diff -u`, `comm`.
On Windows/PowerShell the equivalents are `Select-String`, `Sort-Object`,
`Group-Object`, `Measure-Object`, `Select-Object`.

```bash
# Which failing test signature appears most often across a regression run?
grep -h "FAIL:" results/*/log.txt \
  | sed 's/.*FAIL: //' \
  | sort | uniq -c | sort -rn | head -20
```

Being able to produce that line in ten seconds during a debugging conversation is a
real signal of infrastructure experience.

---

## 5. CMake for multi-target C++ projects

Modern (target-based) CMake only. Directory-level `include_directories` and
`add_definitions` are legacy and will be flagged in review.

```cmake
cmake_minimum_required(VERSION 3.20)
project(gputools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # feeds clangd / clang-tidy

# ---- core library -------------------------------------------------------
add_library(cmdgen_core
    src/instruction.cpp
    src/workload.cpp
    src/encoder.cpp)
add_library(gputools::cmdgen_core ALIAS cmdgen_core)

target_include_directories(cmdgen_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE src)

target_compile_features(cmdgen_core PUBLIC cxx_std_17)
target_compile_options(cmdgen_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->)

# ---- sanitizer option ---------------------------------------------------
option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
if(ENABLE_ASAN)
  target_compile_options(cmdgen_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(cmdgen_core    PUBLIC -fsanitize=address,undefined)
endif()

# ---- tool ---------------------------------------------------------------
add_executable(cmdgen_tool src/main.cpp)
target_link_libraries(cmdgen_tool PRIVATE gputools::cmdgen_core)

# ---- tests --------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
```

CUDA in the same build:

```cmake
project(gputools LANGUAGES CXX CUDA)

add_library(kernels STATIC src/reduce.cu src/sgemm.cu)
set_target_properties(kernels PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON        # needed for device linking
    CUDA_ARCHITECTURES "75;86;89")       # or "native" with CMake >= 3.24
target_compile_options(kernels PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--generate-line-info -Xptxas=-v>)
```

`-Xptxas=-v` prints register and shared-memory usage per kernel at build time, which
is how you notice register spills without opening a profiler. `--generate-line-info`
is what makes Nsight Compute and `cuda-gdb` able to attribute back to source lines.

The mental model to carry: **a target owns its requirements.** `PUBLIC` means "I need
this and so does anyone who links me," `PRIVATE` means "only I need this,"
`INTERFACE` means "only my consumers need this." Almost every messy CMake build is a
build that put requirements on directories instead of targets.

### Interview Q&A

**Q: What is the difference between PUBLIC, PRIVATE and INTERFACE in target_link_libraries?**
A: It controls requirement propagation. PRIVATE means the dependency is used in my
implementation only, so it is used to build me but not passed to my consumers. INTERFACE
means I do not use it myself but my consumers need it — typical for header-only
dependencies. PUBLIC is both: I use it and it appears in my headers, so consumers get it
transitively. Getting this right is what keeps include paths and compile definitions from
leaking across a large build and creating accidental coupling.
*Follow-up:* **What breaks if you mark everything PUBLIC?** — It compiles, which is why
people do it, but every consumer inherits every include path, macro and flag in the
graph. Now an internal header of a low-level library is includable from anywhere, so
people include it, and you have lost the ability to change it — you have turned an
implementation detail into a de facto public API. It also slows builds and makes
diagnosing macro conflicts miserable.

**Q: How would you structure the build for a framework with a C++ core, CUDA kernels and Python bindings?**
A: Three targets with clear layering: a core C++ library with no CUDA or Python
dependency, a CUDA library for the device code linked only where needed, and a pybind11
module target linking the core. The core stays testable on a machine with no GPU, which
matters enormously for CI throughput. I'd set `CUDA_ARCHITECTURES` explicitly so builds
are reproducible rather than depending on the build machine's GPU, enable separable
compilation only if I actually need device linking, and gate the Python module behind an
option so people who only want the C++ tool do not need an interpreter.
*Follow-up:* **How do you keep CI fast with a build like that?** — Split the pipeline:
a fast job that builds the core and runs the unit tests with sanitizers on a
CPU-only runner, and a slower GPU job for the CUDA and integration tests. Use ccache
and cache the build directory between runs, keep the compiled-test surface small by
testing logic rather than device code where possible, and mark GPU tests so they can be
skipped locally without failing.

---

## 6. CI, code review and profiling workflow

**CI for a tools project** should run, in order of increasing cost: compile with
warnings-as-errors on at least two compilers, fast unit tests, sanitizer builds
(ASan+UBSan, and TSan separately since it is incompatible with ASan), lint/format
(`clang-format`, `clang-tidy`, `ruff`/`black` for Python), then GPU-gated tests, then
nightly long-running regressions. The fast lane must stay under a few minutes or people
stop paying attention to it.

**Code review** in this domain has some domain-specific asks beyond the usual: does this
change preserve determinism? Does adding a new architecture/opcode require touching this
file (if yes, why is it not table-driven)? What happens when this is fed a corrupt input?
Is the error message good enough to debug from a log alone?

**Profiling workflow** — the discipline, which is the same everywhere and which
candidates routinely skip:

1. Measure first; never optimise on a hunch.
2. Establish a stable, repeatable benchmark with a fixed seed and enough repetitions.
3. Profile (`perf record`/`perf report`, flamegraphs, `py-spy` for Python, Nsight for GPU).
4. Form a hypothesis about *why* — memory-bound, allocation-bound, branch-bound.
5. Change one thing.
6. Re-measure and keep the number in the commit message.

Committing the before/after numbers is a small habit that makes you visibly credible.

---

## 7. Git for large repositories

The commands that actually matter when you are the tools engineer everyone comes to:

```bash
git bisect start HEAD v1.2.0        # find the commit that broke a test
git bisect run ./scripts/check.sh   # automated bisection — the killer feature

git log -S"cudaLaunchKernel" --oneline   # find commits that changed occurrences of a string
git log -L 40,60:src/encoder.cpp         # history of a specific line range
git blame -w -C src/encoder.cpp          # ignore whitespace, follow moved code

git worktree add ../hotfix release-1.2   # second checkout, no re-clone
git rebase -i origin/main                # curate commits before review
git cherry-pick -x <sha>                 # backport, recording the origin
git reflog                               # the undo button people forget exists
```

`git bisect run` is the one to internalise: point it at a script that exits 0 for good
and non-zero for bad, and it finds the offending commit across a thousand-commit range
automatically. In a regression-heavy environment this is a daily tool, and it is also a
great answer to "how do you find which change broke the nightly."

For monorepos: shallow and partial clones (`--filter=blob:none`), sparse checkout for
the subtree you work in, and `git maintenance` to keep things fast.

### Interview Q&A

**Q: The nightly regression started failing and there are 200 commits since the last pass. How do you find the culprit?**
A: `git bisect run` with a script that reproduces the failure and returns an exit code.
That is about eight builds instead of two hundred. Before starting I make sure the
reproduction is reliable and reasonably fast — if the test takes six hours, I first
minimise it or find a cheaper proxy, because bisect multiplies the runtime by log2 of
the range. If the failure is flaky, bisect will lie to me, so I either run the check
multiple times per step or fix the flakiness first.
*Follow-up:* **What if the bug was introduced by a merge or is only present in combination?** —
`git bisect` handles merge commits, but if the culprit is an interaction I'd use
`--first-parent` to bisect at the merge level first, identifying the offending merge,
then bisect within that branch. If it is a genuine interaction between two independently
good changes, bisect will land on whichever came second, and at that point the real
answer is reading the diff of both with the failure in mind.

**Q: How do you keep a huge repository workable?**
A: Partial clone with `--filter=blob:none` so history metadata comes down but file
contents are fetched lazily, sparse checkout so my working tree only contains the
subtree I actually build, and `git worktree` instead of multiple clones when I need to
work on two branches at once. On top of that, keeping the fast-path CI green so I am not
constantly rebasing over broken states.
*Follow-up:* **Why worktree over just cloning again?** — A worktree shares the object
store, so it costs almost nothing in disk and needs no re-fetch, and branches stay
consistent across all worktrees. Two clones means two copies of history, two fetch
cycles, and the very easy mistake of fixing something in the copy you are not building.

---

## Red Flags

- **"I'd write it in C++ because it's faster."** For a harness whose runtime is 99%
  simulator, this signals you have not thought about where time actually goes.
- **Loading a multi-gigabyte log into a list.** Instant credibility loss.
- **`@` (native) struct alignment in a file format**, or unspecified endianness.
- **`shell=True` with interpolated paths.** Injection plus quoting bugs.
- **No timeout on a subprocess** that launches a simulator — this is how you wedge a farm.
- **Directory-level CMake** (`include_directories`, `link_libraries`) in new code.
- **Tests that depend on execution order or on a shared mutable fixture.**
- **Nondeterminism nobody flagged:** iterating a `set`/`dict` and emitting output in that
  order, using `hash()` (randomised per process by default), timestamps in golden files,
  or thread-completion order deciding output order.
- **Print-debugging as the only strategy**, with no profiler, sanitizer, or bisect.
- **A tool that fails with `Traceback (most recent call last)`** as its user-facing error
  message. Your users are experts; they still deserve a real message.

---

## Whiteboard checklist

You should be able to, from a blank page:

- [ ] Write an `argparse` CLI skeleton with proper exit codes and a testable `main(argv)`.
- [ ] Write a streaming log parser as a generator, and explain why it is O(1) memory.
- [ ] Write a `struct.Struct` header definition for a binary trace record and explain
      every character of the format string, including why it starts with `<`.
- [ ] Write the bit-field decode of a 32-bit instruction word, including sign extension.
- [ ] Sketch a pybind11 binding for a C++ class and name the two lifetime/GIL hazards.
- [ ] Write a parametrized pytest and a round-trip property test for an encoder.
- [ ] Write a modern CMakeLists with a library target, PUBLIC/PRIVATE usage requirements,
      a sanitizer option, and CUDA architectures.
- [ ] Explain PUBLIC vs PRIVATE vs INTERFACE without hesitating.
- [ ] Write the `git bisect run` workflow for a failing nightly.
- [ ] Produce a one-line shell pipeline that ranks the most common failure signatures
      across a directory of logs.
- [ ] List five sources of nondeterminism in a Python tool and how to eliminate each.

Next: **[M9 — Capstone, System Design & Behavioral](M9-capstone-and-behavioral.md)**.
