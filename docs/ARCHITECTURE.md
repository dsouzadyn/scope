# SCOPE Architecture & Engineering Guide

SCOPE is a macOS-native C++20 debugger engineered around a **Precision Bench Instrument** UI and **HFT / low-latency C++** workflows.

---

## 1. Threading & Concurrency Model

SCOPE enforces a strict two-thread separation so the 120 FPS Metal interface never blocks on DWARF symbolication, expression evaluation, or target I/O:

```text
┌─────────────────────────────────────────────────────────────────────────┐
│ Main Thread (120 FPS GLFW + Metal + Dear ImGui Docking + FreeType)      │
│  • Renders all 7 Instrument Bays from immutable shared_ptr<StepSnapshot>│
│  • Never calls liblldb SB APIs directly                                 │
│  • Enqueues async commands onto DebuggerEngine::cmd_queue_              │
└───────────────────────────┬──────────────────────────▲──────────────────┘
                            │ mutex + condvar          │ mutex (shared_ptr swap)
                            ▼                          │
┌─────────────────────────────────────────────────────────────────────────┐
│ Dedicated Worker Thread (std::jthread inside DebuggerEngine)            │
│  • Owns SBDebugger, SBTarget, SBProcess, SBListener                     │
│  • Drains cmd_queue_, polls LLDB events (non-blocking), drains stdout   │
│  • Refreshes active Memory & Data Probes BEFORE publishing StepSnapshot │
│  • Maintains 256-stop immutable StepSnapshot ring buffer for scrubbing  │
└─────────────────────────────────────────────────────────────────────────┘
```

### Key Invariants
1. **Atomic Snapshot Consistency**: On every stop, `DebuggerEngine::publish_snapshot()` refreshes all configured memory views and data probes (`MemorySnapshot`, `StructTableData`, `WaveformProbeData`, `PixelProbeData`, `PointerGraphData`, `RingBufferProbeData`, `OrderBookProbeData`) **before** swapping `latest_snapshot_`. The UI therefore never renders a frame where locals belong to stop `#N` while memory or probes still show stop `#N-1`.
2. **Zero-Overhead History Scrubbing**: Every recorded stop is stored as an immutable `std::shared_ptr<const StepSnapshot>` (up to 256 stops). Scrubbing backward in the Step-Delta Timeline simply swaps the pointer rendered by `UiContext::snap` with zero IPC or target interaction.
3. **Ref-Counted LLDB Lifetime**: `SBDebugger::Initialize()` / `SBDebugger::Terminate()` are reference-counted across `DebuggerEngine` instances so tests can construct and destroy engines safely.

---

## 2. Module Map

| Path | Responsibility |
| :--- | :--- |
| `src/main.mm` | macOS Cocoa/GLFW window bootstrap, Retina `CAMetalLayer` (`contentsScale = backingScaleFactor`), 120Hz Metal render loop, and `MetalTextureBridge` (`MTLTexture` RGBA uploader for 2D pixel probes). |
| `src/app/App.{hpp,cpp}` | Main frame coordinator, default `DockBuilder` 7-bay workbench layout, global keyboard shortcuts (`F5`/`F6`/`F10`/`F11`/`Cmd+K`/`Cmd+R`), and fixture discovery (including inside `SCOPE.app/Contents/Resources/fixtures`). |
| `src/engine/Types.hpp` | Pure POD/STL immutable data transfer structs shared between engine and UI (zero LLDB header leakage into UI translation units). |
| `src/engine/DebuggerEngine.{hpp,cpp}` | Dedicated `liblldb` SB API worker: launch, attach, core dumps (`LoadCore`), remote `gdb-remote`/`rr` (`ConnectRemote`), stepping, breakpoints, hardware watchpoints, expression-interpolated logpoints, lazy per-page provenance, SIMD extraction, and all 5 data probes. |
| `src/engine/TypeFormat.{hpp,cpp}` | C++ ABI template noise simplifier (`std::__1::basic_string<...>` → `std::string`, default allocator/hash/deleter stripping), stdlib/runtime frame classifier, nibble-grouped hex formatting, and provenance helpers. |
| `src/engine/SyntaxHighlighter.{hpp,cpp}` | Cached `tree-sitter-cpp` AST highlighter producing sorted, bounds-clamped per-line `SyntaxSpan` lists. |
| `src/ui/Theme.{hpp,cpp}` | **Anodized Lab** (dark) & **Technical Schematic** (light) palettes, FreeType `LightHinting` font tiers (`code`, `bold`, `micro`, `display`), and zero-rounding rectilinear ImGui styling. |
| `src/ui/Widgets.{hpp,cpp}` | Custom `ImDrawList` vector icons, segmented controls, hardware LED status pills, watchpoint slot gauge (`WP`), dimmed-zero nibble hex links, provenance badges, and unified bay toolbars. |
| `src/ui/TransportAndTimeline.cpp` | Top transport deck, target/fixture switcher, reverse-execution controls (remote `rr`), Step-Delta Timeline Scrubber, and Crash Autopsy alert banner. |
| `src/ui/CodeCanvas.cpp` | `01 SOURCE` — custom virtualized `ImDrawList` source canvas, `SRC | MIXED | ASM` modes, branch-target margin arcs, click-to-sync `-O3` source↔asm highlighting, breakpoint/logpoint gutter, inline ghost values (executed lines only), and debounced hover evaluation. |
| `src/ui/InspectorBay.cpp` | `02 INSPECTOR` — Locals, Watches, Struct-Table view, provenance badges, amber step diffs, and right-click context menu for hardware watchpoints and probes. |
| `src/ui/MemoryBay.cpp` | `03 MEMORY` — virtualized hex + ASCII canvas, 64B cache-line dividers, `SBType` struct field overlays, padding-hole highlights, false-sharing warning strip, `LE`/`BE` toggle, fixed-point price & epoch `ns`/`µs` UTC wire-protocol decoder. |
| `src/ui/StackAndRegisters.cpp` | `04 STACK & THREADS` (runtime frame folding, `[INL]` badges, lazy Thread Matrix, BP/WP condition manager) and `05 REGISTERS` (`GEN | SIMD | ALL`, `F32`/`F64`/`I32`/`I64`/`U8` vector lane decoder, `NZCV`/`RFLAGS` LEDs). |
| `src/ui/ProbesAndConsole.cpp` | `06 PROBES` (Waveform, 2D Pixel `MTLTexture`, Pointer Graph with cycle detection, SPSC/MPSC Ring Buffer, L2 Order Book with crossed-book alert), `07 CONSOLE` (Target I/O + LLDB REPL), launch/attach/core/remote modals, and `Cmd+K` command & symbol palette. |

---

## 3. HFT & Systems Design Notes

### Lazy Per-Page Provenance (`classify_address_lazy`)
Enumerating all VM regions (`SBProcess::GetMemoryRegions()`) on large HFT binaries can scan thousands of mappings per step. Instead, `DebuggerEngine` clears a per-stop `page_prov_cache_` (`addr & ~0xFFF`) and queries `SBProcess::GetMemoryRegionInfo(addr, info)` strictly on demand for visible pointers and fault addresses, keeping stepping O(visible pointer pages).

### Lazy Thread Unwinding
On multi-threaded binaries, `publish_snapshot()` unwinds up to 64 frames for the active thread and only 4 frames for background threads unless `expand_all_threads_` is enabled (automatically toggled when the `TH` Thread Matrix tab is open).

### Cache-Line Padding & False-Sharing Analysis
When inspecting a pointer or struct in `03 MEMORY`, `DebuggerEngine` walks `SBType::GetFieldAtIndex()`, sorts members by byte offset, identifies internal and trailing padding gaps (`PaddingRange`), groups fields by 64-byte cache line (`offset / 64`), and flags any cache line shared by multiple fields where at least one is `std::atomic`.

### Safe Expression-Interpolated Logpoints
Logpoints do **not** use `SBBreakpoint::SetAutoContinue(true)` (which resumes the process before emitting the stop event and races expression evaluation). Instead, the worker thread handles the stop event synchronously, evaluates `{expr}` placeholders in frame `#0`, computes microsecond/millisecond `Δt` since the previous hit, appends the formatted line to console, and immediately calls `process_.Continue()` without recording a stop in the UI history.

### Crash Autopsy on macOS LLDB
On macOS ARM64, `assert()` reports `eStopReasonSignal` with `signo = SIGABRT (6)` and description `"hit program assert"`. `extract_crash_info()` checks both numeric POSIX signal numbers (`SIGSEGV`, `SIGBUS`, `SIGABRT`, `SIGILL`, `SIGFPE`) and exception descriptions, extracts exact fault addresses (e.g., `EXC_BAD_ACCESS` `address=0x18`), disassembles the offending instruction, and locates the nearest non-runtime user frame.

---

## 4. Packaging, Testing & Release Gates

- **Unit Tests** (`tests/unit_tests.cpp` → `scope_unit_tests`): Fast, deterministic tests for type simplification, runtime frame detection, hex formatting, provenance logic, and Tree-sitter highlighting.
- **Engine Integration Tests** (`tests/engine_test.cpp` → `scope_engine_test`): 7 end-to-end suites driving real `liblldb` sessions across all 6 `-g -O0` fixtures in `fixtures/`.
- **LLVM Source-Based Coverage** (`scripts/coverage.sh`): Instruments `scope_engine` with `-fprofile-instr-generate -fcoverage-mapping`, exports LCOV + HTML, and enforces minimum thresholds (**85% lines, 95% functions, 82% regions, 62% branches**).
- **macOS Bundle Packaging** (`scripts/package_macos.sh` & `scripts/verify_package.sh`): Builds `SCOPE.app`, recursively bundles `libglfw`, `libfreetype`, `libpng16`, and `libtree-sitter` into `Contents/Frameworks/` with `@executable_path/../Frameworks/...` install names (leaving `liblldb` linked against Homebrew LLVM so `debugserver` entitlements stay intact), ad-hoc signs with `codesign --deep`, and verifies `plutil`, `otool -L`, `codesign --verify`, and `SHA256SUMS.txt`.
