# SCOPE — Precision C++20 Debugger (macOS)

A **macOS-native C++20** debugger built on **`liblldb` (SB API)**, **Dear ImGui (Docking + FreeType)**, **GLFW + Metal**, and **Tree-sitter (`tree-sitter-cpp`)**, designed with a **Precision Bench Instrument** aesthetic and dedicated **HFT / low-latency C++** inspection tools.

## Highlights

- **Dedicated `liblldb` Worker Architecture**: All debugger operations run off the UI thread (`std::jthread`), publishing immutable `StepSnapshot` frames into a 256-stop history ring buffer so the 120 FPS Metal interface never blocks.
- **Step-Delta Timeline Scrubber**: Records every stop and highlights mutated variables, registers, and memory bytes (`old → new`). Click any earlier stop to inspect historical state snapshots.
- **First-Class Hardware Watchpoints**: Right-click any variable, struct field, or memory range to arm hardware Break-on-Write / Read-Write watchpoints with optional LLDB conditions and a live 4-slot hardware gauge (`WP`).
- **HFT & Low-Latency Memory Inspector (`03 MEMORY`)**:
  - Automatic pointer provenance tags (`[STACK]`, `[HEAP]`, `[TEXT]`, `[RODATA]`, `[NULL]`, `[UNMAPPED!]`) via lazy per-page `GetMemoryRegionInfo`.
  - **64-byte cache-line dividers**, **struct field overlays** (`SBType`), **padding-hole highlighting**, and **false-sharing warnings** for shared atomic cache lines.
  - Live **LE / BE** endianness toggle, **fixed-point price decoder** (`1e-4`, `1e-6`, `1e-8`), and **epoch `ns`/`µs` → UTC** wire-packet decoder.
- **Live Data Probes (`06 PROBES`)**:
  - **Waveform**: Oscilloscope graticule for numeric vectors/arrays.
  - **2D Pixel**: Direct Metal texture upload (`MTLTexture`) with zoom & `(x, y)` RGBA crosshair.
  - **Pointer Graph**: Node-link traversal for linked lists/trees with automatic cycle detection.
  - **Ring Buffer**: SPSC/MPSC circular occupancy strip (`head`/`tail`/wrap-around) and payload table.
  - **Order Book L2**: Live Bid/Ask depth ladder with spread/mid telemetry and crossed-book detection.
- **Systems & Optimized (`-O2`/`-O3`) Inspection**:
  - Tree-sitter C++20 syntax highlighting, `SRC | MIXED | ASM` modes, branch-target margin arcs, and click-to-sync Source ↔ Assembly highlighting.
  - SIMD lane decoder (`F32 | F64 | I32 | I64 | U8`) for 128/256/512-bit vector registers + `NZCV`/`RFLAGS` LEDs.
  - Smart runtime/stdlib stack folding, `[INL]` inlined frame badges, lazy background thread unwinding, expression-interpolated logpoints with `+Δt` timestamps, **Core Dump** loading (`LoadCore`), and **Remote `lldb-server` / `rr`** reverse debugging.

## Prerequisites (macOS Apple Silicon / Homebrew)

```bash
brew install llvm glfw freetype tree-sitter cmake ninja
```

## Build & Test

```bash
git clone --recursive <repo-url>
cd cppdebugger
git config core.hooksPath .githooks   # enable pre-commit & pre-push hooks

cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build build

# Run full CTest suite (unit tests + 7-suite liblldb integration tests)
ctest --test-dir build --output-on-failure

# Generate LLVM source-based code coverage (LCOV + HTML + strict gates: 85% lines, 95% functions, 82% regions, 62% branches)
cmake -S . -B build-cov -G Ninja -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DSCOPE_ENABLE_COVERAGE=ON
cmake --build build-cov --target scope_unit_tests scope_engine_test fixture_step_basics fixture_stl_containers fixture_memory_watch fixture_multithread fixture_crash_autopsy fixture_hft_orderbook
./scripts/coverage.sh build-cov coverage

# Build & verify macOS SCOPE.app release bundle
./scripts/package_macos.sh v0.1.0 build dist
./scripts/verify_package.sh dist

# Launch SCOPE on a fixture or your own C++ binary
./build/scope ./build/fixtures/hft_orderbook
```

## Keybindings

| Shortcut | Action |
| :--- | :--- |
| `F5` / `F6` | Continue / Pause |
| `F10` | Step Over |
| `F11` / `Shift+F11` | Step Into / Step Out |
| `Cmd+Shift+F10` | Step Instruction (Assembly) |
| `Cmd+R` | Restart Target |
| `Cmd+K` | Command, Fixture & Live Symbol/File Palette |
