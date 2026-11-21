#include "../src/engine/DebuggerEngine.hpp"
#include "../src/engine/TypeFormat.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace {

std::shared_ptr<const scope::StepSnapshot> wait_for_state(
    scope::DebuggerEngine& eng,
    scope::SessionState want,
    uint64_t min_step = 0,
    int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto snap = eng.latest_snapshot();
        if (snap && snap->state == want && snap->step_index >= min_step) {
            return snap;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return eng.latest_snapshot();
}

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);                         \
            for (const auto& l : engine.console_lines()) {                                         \
                std::fprintf(stderr, "  [console] %s\n", l.text.c_str());                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

} // namespace

int main(int argc, char** argv) {
    scope::DebuggerEngine engine;

    // 1) Unit test type simplification & hex formatting
    CHECK(scope::simplify_cpp_type(
              "std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >") ==
              "std::string",
          "simplify std::string");
    CHECK(scope::simplify_cpp_type("std::__1::vector<Particle, std::__1::allocator<Particle> >") ==
              "std::vector<Particle>",
          "simplify std::vector");
    CHECK(scope::format_hex64_grouped(0x16fdff4a0ull) == "0x0000_0001_6fdf_f4a0",
          "grouped hex formatting");

    std::string fixtures_dir = (argc > 1)
                                   ? std::string(argv[1])
                                   : (std::filesystem::current_path() / "fixtures").string();

    // 2) Launch step_basics, verify stop at main, stepping, deltas & provenance
    std::string step_exe = (std::filesystem::path(fixtures_dir) / "step_basics").string();
    std::printf("[TEST] Launching %s\n", step_exe.c_str());
    engine.launch(step_exe, {}, "", true);
    auto snap1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
    CHECK(snap1->state == scope::SessionState::Stopped, "step_basics stopped at main");
    CHECK(!snap1->active_file.empty(), "active_file populated");
    CHECK(!snap1->registers.empty(), "registers populated");
    CHECK(!snap1->disassembly.empty(), "disassembly populated");
    std::printf("  -> stopped at %s:%u (locals=%zu, regs=%zu, disasm=%zu)\n",
                snap1->active_file.c_str(), snap1->active_line,
                snap1->locals.size(), snap1->registers.size(), snap1->disassembly.size());

    // Step several lines to initialize frame, frame_ptr, null_ptr
    uint64_t cur_step = snap1->step_index;
    for (int k = 0; k < 5; ++k) {
        engine.step_over();
        auto s = wait_for_state(engine, scope::SessionState::Stopped, cur_step + 1);
        CHECK(s->state == scope::SessionState::Stopped, "step_over stopped");
        cur_step = s->step_index;
    }
    auto snap_stepped = engine.latest_snapshot();
    CHECK(engine.history_snapshots().size() >= 5, "step history ring buffer recorded");

    bool saw_stack_ptr = false;
    bool saw_null_ptr = false;
    for (const auto& v : snap_stepped->locals) {
        if (v.name == "frame_ptr" && v.provenance == scope::ProvenanceKind::Stack) saw_stack_ptr = true;
        if (v.name == "null_ptr" && v.provenance == scope::ProvenanceKind::Null) saw_null_ptr = true;
    }
    CHECK(saw_stack_ptr, "frame_ptr tagged as STACK");
    CHECK(saw_null_ptr, "null_ptr tagged as NULL");

    // Memory view with struct field overlays on &frame
    engine.configure_memory_view("frame_ptr", 256);
    engine.wait_idle_for_test();
    auto mem = engine.memory_snapshot();
    CHECK(mem.valid, "memory snapshot valid");
    CHECK(mem.fields.size() >= 4, "struct field overlays extracted for SensorFrame");
    std::printf("  -> memory struct overlay: %s (%zu fields)\n",
                mem.struct_type.c_str(), mem.fields.size());

    engine.terminate();
    wait_for_state(engine, scope::SessionState::Exited, 0, 2000);

    // 3) Test memory_watch: Hardware Watchpoint + Pointer Graph Cycle Detection + 2D Pixel Probe
    std::string mw_exe = (std::filesystem::path(fixtures_dir) / "memory_watch").string();
    std::printf("[TEST] Launching %s\n", mw_exe.c_str());
    engine.launch(mw_exe, {}, "", true);
    auto mw1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
    CHECK(mw1->state == scope::SessionState::Stopped, "memory_watch stopped at main");

    // Step until head & pixels are initialized (inside for loop at line 56)
    std::string mw_src = mw1->active_file;
    engine.toggle_breakpoint(mw_src, 56); // inside for loop
    engine.wait_idle_for_test();
    engine.continue_exec();
    auto mw_loop = wait_for_state(engine, scope::SessionState::Stopped, mw1->step_index + 1);
    CHECK(mw_loop->state == scope::SessionState::Stopped, "memory_watch hit loop breakpoint");

    // Pointer graph cycle detection on `head`
    engine.configure_pointer_graph("head", 16);
    engine.wait_idle_for_test();
    auto pg = engine.pointer_graph();
    CHECK(pg.nodes.size() == 4, "pointer graph found 4 ListNodes");
    CHECK(pg.has_cycle, "pointer graph detected intentional cycle");
    std::printf("  -> pointer graph: %zu nodes, cycle=%s\n",
                pg.nodes.size(), pg.has_cycle ? "YES" : "NO");

    // 2D pixel probe on `pixels`
    engine.configure_pixel_probe("pixels", 32, 32, 4);
    engine.wait_idle_for_test();
    auto pp = engine.pixel_probe();
    CHECK(pp.rgba_bytes.size() == 32 * 32 * 4, "pixel probe read 32x32 RGBA");

    // Remove loop breakpoint and arm hardware watchpoint on packet->gain
    engine.toggle_breakpoint(mw_src, 56);
    engine.add_variable_watchpoint("packet->gain", false, true);
    engine.wait_idle_for_test();
    auto mw_armed = engine.latest_snapshot();
    CHECK(!mw_armed->watchpoints.empty(), "hardware watchpoint armed");
    std::printf("  -> armed HW watchpoint #%d on packet->gain\n", mw_armed->watchpoints[0].id);

    // Continue and verify hardware watchpoint catches corrupt_gain_subtly!
    uint64_t before_wp = mw_armed->step_index;
    engine.continue_exec();
    auto mw_wp_hit = wait_for_state(engine, scope::SessionState::Stopped, before_wp + 1);
    CHECK(mw_wp_hit->state == scope::SessionState::Stopped, "HW watchpoint triggered stop");
    std::printf("  -> HW watchpoint caught write at %s:%u (%s)\n",
                mw_wp_hit->active_file.c_str(), mw_wp_hit->active_line,
                mw_wp_hit->stop_reason.c_str());

    engine.terminate();
    wait_for_state(engine, scope::SessionState::Exited, 0, 2000);

    // 3b) Test stl_containers: Struct-Table View on std::vector<Particle>
    std::string stl_exe = (std::filesystem::path(fixtures_dir) / "stl_containers").string();
    std::printf("[TEST] Launching %s\n", stl_exe.c_str());
    engine.launch(stl_exe, {}, "", true);
    auto stl1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
    CHECK(stl1->state == scope::SessionState::Stopped, "stl_containers stopped at main");
    engine.toggle_breakpoint(stl1->active_file, 45);
    engine.wait_idle_for_test();
    engine.continue_exec();
    auto stl_loop = wait_for_state(engine, scope::SessionState::Stopped, stl1->step_index + 1);
    CHECK(stl_loop->state == scope::SessionState::Stopped, "stl_containers hit loop breakpoint");
    engine.configure_struct_table("particles");
    engine.wait_idle_for_test();
    auto st = engine.struct_table();
    CHECK(st.rows.size() == 16, "struct table extracted 16 Particle rows");
    CHECK(st.columns.size() >= 6, "struct table extracted Particle columns");
    std::printf("  -> struct table: %s (%zu rows x %zu cols)\n",
                st.element_type.c_str(), st.rows.size(), st.columns.size());
    engine.terminate();
    wait_for_state(engine, scope::SessionState::Exited, 0, 2000);

    // 4) Test crash_autopsy: SIGSEGV / EXC_BAD_ACCESS detection
    std::string crash_exe = (std::filesystem::path(fixtures_dir) / "crash_autopsy").string();
    std::printf("[TEST] Launching %s\n", crash_exe.c_str());
    engine.launch(crash_exe, {}, "", false);
    auto cr = wait_for_state(engine, scope::SessionState::Fault, 1);
    CHECK(cr->state == scope::SessionState::Fault, "crash_autopsy detected Fault state");
    CHECK(cr->crash.is_crash, "crash autopsy info populated");
    std::printf("  -> crash autopsy: %s @ %s [%s] insn='%s'\n",
                cr->crash.signal_name.c_str(),
                scope::format_hex64_grouped(cr->crash.fault_address).c_str(),
                scope::provenance_label(cr->crash.fault_provenance),
                cr->crash.offending_instruction.c_str());

    // 5) Test HFT OrderBook fixture: Cache-line false sharing, Padding, Ring Buffer, OrderBook L2, SIMD & Symbol Search
    std::string hft_exe = (std::filesystem::path(fixtures_dir) / "hft_orderbook").string();
    std::printf("[TEST] Launching %s\n", hft_exe.c_str());
    engine.launch(hft_exe, {}, "", true);
    auto hft1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
    CHECK(hft1->state == scope::SessionState::Stopped, "hft_orderbook stopped at main");

    // Verify SIMD register raw bytes captured (ARM64 v0..v31 are 16B)
    bool saw_simd = false;
    for (const auto& r : hft1->registers) {
        if (r.raw_bytes.size() >= 16) {
            saw_simd = true;
            break;
        }
    }
    CHECK(saw_simd, "SIMD vector register raw bytes captured");

    // Verify symbol search resolves hft_orderbook
    engine.search_symbols("hft_orderbook");
    engine.wait_idle_for_test();
    auto syms = engine.symbol_results();
    CHECK(!syms.empty(), "symbol/file search found matches");
    std::printf("  -> symbol/file search: %zu matches\n", syms.size());

    // Break inside the tick loop after book update (line 151: std::printf)
    engine.toggle_breakpoint(hft1->active_file, 151);
    engine.wait_idle_for_test();
    engine.continue_exec();
    auto hft_tick0 = wait_for_state(engine, scope::SessionState::Stopped, hft1->step_index + 1);
    CHECK(hft_tick0->state == scope::SessionState::Stopped, "hft_orderbook hit tick 0");

    // Cache-line padding & false sharing on stats_ptr
    engine.configure_memory_view("stats_ptr", 256);
    engine.wait_idle_for_test();
    auto stats_mem = engine.memory_snapshot();
    CHECK(stats_mem.valid, "stats_ptr memory valid");
    CHECK(stats_mem.padding_bytes > 0, "detected struct padding holes in MatchingEngineStats");
    CHECK(!stats_mem.false_sharing_warnings.empty(), "detected 64B cache-line false sharing");
    std::printf("  -> cache-line analyzer: %s sizeof=%lluB pad=%lluB warn='%s'\n",
                stats_mem.struct_type.c_str(),
                static_cast<unsigned long long>(stats_mem.struct_size),
                static_cast<unsigned long long>(stats_mem.padding_bytes),
                stats_mem.false_sharing_warnings[0].c_str());

    // Ring Buffer Probe on SpscRing
    engine.configure_ring_probe("ring.slots", "ring.capacity", "ring.head", "ring.tail");
    engine.wait_idle_for_test();
    auto rp = engine.ring_probe();
    CHECK(rp.capacity == 16, "ring probe capacity == 16");
    CHECK(rp.occupied_count == 10, "ring probe occupied == 10");
    CHECK(rp.wrapped, "ring probe detected wrap-around");
    std::printf("  -> ring probe: %llu/%llu wrapped=%s\n",
                static_cast<unsigned long long>(rp.occupied_count),
                static_cast<unsigned long long>(rp.capacity),
                rp.wrapped ? "YES" : "NO");

    // Order Book L2 Probe (initially uncrossed -> continue 3 ticks -> crossed!)
    engine.configure_orderbook_probe("book.bids", "book.asks", 10000.0);
    engine.wait_idle_for_test();
    auto ob0 = engine.orderbook_probe();
    CHECK(ob0.bids.size() == 5 && ob0.asks.size() == 5, "orderbook extracted 5 bids & 5 asks");
    CHECK(!ob0.crossed, "orderbook initially uncrossed");
    std::printf("  -> orderbook tick 0: bid=%.4f ask=%.4f spread=%.4f crossed=NO\n",
                ob0.best_bid, ob0.best_ask, ob0.spread);

    uint64_t sidx = hft_tick0->step_index;
    for (int t = 0; t < 3; ++t) {
        engine.continue_exec();
        auto st = wait_for_state(engine, scope::SessionState::Stopped, sidx + 1);
        CHECK(st->state == scope::SessionState::Stopped, "hft tick stopped");
        sidx = st->step_index;
    }
    auto ob3 = engine.orderbook_probe();
    CHECK(ob3.crossed, "orderbook detected CROSSED BOOK on tick 3!");
    std::printf("  -> orderbook tick 3: bid=%.4f ask=%.4f spread=%.4f crossed=YES\n",
                ob3.best_bid, ob3.best_ask, ob3.spread);

    engine.terminate();
    std::printf("ALL SCOPE ENGINE INTEGRATION TESTS PASSED!\n");
    return 0;
}
