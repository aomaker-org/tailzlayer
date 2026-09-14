# Path: labs/tailzlayer/README.md
# Hedged Virtual Memory Allocator (`tailzlayer`) Specifications

Notice: `tailzlayer` is the experimental, low-friction research fork of
`tailslayer`. While `tailslayer` serves as a conservative, pass-through fork
for minor compatibility fixes, `tailzlayer` is dedicated to rapid
architectural exploration, H-VMA allocators, assembly-backed optimization,
and telemetry experiments.

---

## 🏛️ Repository Hierarchy & Relationship

```
 Upstream (LaurieWired/tailslayer)
              │
              ▼
 aomaker-org/tailslayer  (Pass-Through Fork: Minor Bugfixes & Compatibility)
              │
              ▼
 aomaker-org/tailzlayer  (Experimental Fork: H-VMA, Dual Rust, Assembly)
```

---

## 🔬 Dual Rust Implementation Strategy

The project implements a two-stage Rust evolution pipeline:

### 1. Stage 1: Straightforward Rust Port (`tailslayer-rs-v1`)
- Idiomatic, 1:1 translation from the baseline C++ codebase.
- Establishes correctness, safety guarantees, and benchmark baseline.

### 2. Stage 2: Artifact-Driven Assembly Optimization (`tailslayer-rs-v2`)
- Both C++ and Rust v1 implementations are compiled with **Extreme Verbose
  Compiler Flags**:
  - `-save-temps=obj -fverbose-asm`
  - Disassembly (`objdump -d`), symbol tables (`nm -C`), ELF headers
    (`readelf -S`)
  - Intermediate assembly (`.s`), preprocessed source (`.i`), and objects (`.o`)
- Build artifacts are retained in a causal ledger and analyzed for register
  spills, vectorization opportunities, and DRAM refresh stall hazards.
- Hot paths are rewritten using hand-tuned assembly (`asm!`) and SIMD vector
  instructions to produce the production-grade `v2` implementation.

---

## 🛠️ Modular Makefile & Lazy Evaluation Architecture

The build system (`tailslayer.mk` / `Makefile`) uses lazy evaluation to avoid
unnecessary recompilation while enforcing strict artifact retention:

```bash
# Build standard H-VMA benchmark binary
make

# Execute Extreme Verbose Build & Artifact Inspection
make extreme

# Execute Rust v1 Baseline Benchmark
make rust-v1

# Execute Assembly-Optimized Rust v2 Benchmark
make rust-v2
```

---

## 📂 Directory Structure

- `include/`: Hedged allocator headers (`hvma.hpp`, `telemetry_compressor.hpp`).
- `src/`: Core C++ implementation files (`hvma.cpp`, `main.cpp`).
- `rust/`: Dual Rust ports (`v1` baseline and `v2` assembly-optimized).
- `Makefile`: Lazy-evaluated, modular build configuration.
- `README.md`: Authoritative specification document.

<!-- end of file: labs/tailzlayer/README.md -->
