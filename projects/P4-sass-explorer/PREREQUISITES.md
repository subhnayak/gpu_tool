# Prerequisites

This document lists exactly what must be installed to run each part of the
project. **Part A works immediately with just a C++ compiler and CMake** —
you do not need Python or the CUDA Toolkit to get started.

---

## Part A: Toy ISA (C++)

**Requirements**: A C++17 compiler and CMake ≥ 3.14.

**Verification**:
```
cmake --version
cl /? 2>&1 | findstr /C:"Version"     # MSVC (Windows)
g++ --version                           # GCC (Linux)
```

**Status on this machine**: ✅ Verified working (MSVC 19.50, CMake, VS 2026).

**What works without anything else**: The assembler, disassembler, CFG builder,
and all 18 C++ tests. This is the core of the project.

---

## Part A': Python Mirror (Toy ISA) and Part B: PTX Tools

**Requirements**: Python 3.9+ and `pytest`.

**Verification**:
```
python --version        # Must print "Python 3.9" or higher
pip install pytest      # Or: pip install --user pytest
pytest --version
```

**Status on this machine**: ❌ Not installed. The `python` and `python3`
commands on PATH are Microsoft Store alias stubs that print an error message
and open the Store app — they are **not** a working Python interpreter.

**Installation**: Download from https://www.python.org/downloads/ and install.
During installation, check "Add Python to PATH". After installation, verify
with `python --version`. Then install pytest: `pip install pytest`.

> **Windows gotcha**: After installing Python, you may need to disable the
> Microsoft Store alias stubs. Go to Settings → Apps → Advanced app settings
> → App execution aliases, and turn off the "python.exe" and "python3.exe"
> aliases. Otherwise the stubs shadow the real interpreter on PATH.

**What works without Python**: All of Part A (C++). The Python mirror and
PTX tools are independent — they do not require the C++ side to be built
(except for the differential test, which skips gracefully if the C++ binary
is absent).

---

## Part C: Real Toolchain Exploration (PTX/SASS extraction)

**Requirements**: The NVIDIA CUDA Toolkit, which provides `nvcc`, `cuobjdump`,
and `nvdisasm`. Also requires Python 3.9+ (for the scripts).

**Verification**:
```
nvcc --version
cuobjdump --version
nvdisasm --version
```

**Status on this machine**: ❌ CUDA Toolkit not installed. The machine has an
**NVIDIA RTX 4000 Ada Generation** GPU (sm_89 / Ada Lovelace) with a working
driver, so CUDA programs can run once the toolkit is installed.

**Installation**: Download from https://developer.nvidia.com/cuda-downloads.
Select your OS and follow the installer. The toolkit installs to:
- Windows: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\vX.Y\bin`
- Linux: `/usr/local/cuda/bin`

Ensure the `bin` directory is on your PATH after installation.

**Architecture flag**: This machine's GPU is **sm_89** (Ada Lovelace). Use
`-arch=sm_89` in all nvcc commands. The scripts default to this.

**What works without CUDA Toolkit**: Everything in Parts A, A', and B. The
Part C scripts (`dump_ptx_sass.py`, `compare_ptx_sass.py`) detect missing
tools and print clear error messages instead of crashing. The sample PTX
file (`tests/sample.ptx`) is included so you can exercise the PTX parser
and CFG builder without generating PTX yourself.

---

## Optional: Graphviz (for viewing DOT files)

The CFG builders emit Graphviz DOT files. To render them as images:

```
dot -Tpng output.dot -o output.png
```

**Installation**: https://graphviz.org/download/

This is purely optional — the text CFG output works without Graphviz.

---

## Summary

| Part | C++17 + CMake | Python 3.9+ | pytest | CUDA Toolkit | Graphviz |
|------|:---:|:---:|:---:|:---:|:---:|
| A: Toy ISA (C++) | **Required** | — | — | — | Optional |
| A': Toy ISA (Python) | — | **Required** | **Required** | — | — |
| B: PTX Tools | — | **Required** | For tests | — | Optional |
| C: Real Toolchain | — | **Required** | — | **Required** | Optional |
