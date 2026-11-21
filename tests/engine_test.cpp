#include "../src/engine/DebuggerEngine.hpp"
#include "../src/engine/TypeFormat.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {

std::shared_ptr<const scope::StepSnapshot> wait_for_state(
    scope::DebuggerEngine& eng,
    scope::SessionState want,
    uint64_t min_step = 0,
    int timeout_ms = 6000) {
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
    std::string fixtures_dir = (argc > 1)
                                   ? std::string(argv[1])
                                   : (std::filesystem::current_path() / "fixtures").string();

    scope::DebuggerEngine engine;

    // =========================================================================
    // 1) step_basics: Launch, stepping (over/into/out/asm), deltas, watches,
    //    hover eval, provenance, memory overlay + byte diff, waveform, REPL & restart
    // =========================================================================
    {
        std::string exe = (std::filesystem::path(fixtures_dir) / "step_basics").string();
        std::printf("[TEST 1/7] step_basics: stepping, deltas, watches, hover, memory diff, waveform, REPL\n");
        engine.launch(exe, {}, "", true);
        auto snap1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
        CHECK(snap1->state == scope::SessionState::Stopped, "step_basics stopped at main");
        CHECK(!snap1->active_file.empty(), "active_file populated");
        CHECK(!snap1->registers.empty(), "registers populated");
        CHECK(!snap1->disassembly.empty(), "disassembly populated");
        std::string src_file = snap1->active_file;

        // Add a watch expression before stepping
        engine.add_watch_expr("frame.seq");
        engine.wait_idle_for_test();

        // Step over 6 lines to initialize frame, frame_ptr, null_ptr, waveform, filtered, checksum
        uint64_t cur_step = snap1->step_index;
        for (int k = 0; k < 6; ++k) {
            engine.step_over();
            auto s = wait_for_state(engine, scope::SessionState::Stopped, cur_step + 1);
            CHECK(s->state == scope::SessionState::Stopped, "step_over stopped");
            cur_step = s->step_index;
        }
        auto snap_stepped = engine.latest_snapshot();
        CHECK(engine.history_snapshots().size() >= 6, "step history ring buffer recorded >=6 stops");

        // Verify pointer provenance
        bool saw_stack_ptr = false;
        bool saw_null_ptr = false;
        for (const auto& v : snap_stepped->locals) {
            if (v.name == "frame_ptr" && v.provenance == scope::ProvenanceKind::Stack) saw_stack_ptr = true;
            if (v.name == "null_ptr" && v.provenance == scope::ProvenanceKind::Null) saw_null_ptr = true;
        }
        CHECK(saw_stack_ptr, "frame_ptr tagged as STACK");
        CHECK(saw_null_ptr, "null_ptr tagged as NULL");

        // Verify Watch expression evaluated
        CHECK(snap_stepped->watches.size() == 1, "watch expression present");
        CHECK(snap_stepped->watches[0].value == "1", "frame.seq watch initially 1");

        // Verify Hover evaluation (valid & invalid)
        engine.eval_hover("frame");
        engine.wait_idle_for_test();
        auto hr = engine.hover_result();
        CHECK(hr.valid && hr.root.children.size() == 5, "hover eval returned SensorFrame with 5 fields");

        engine.eval_hover("totally_nonexistent_var_xyz");
        engine.wait_idle_for_test();
        auto hr_bad = engine.hover_result();
        CHECK(!hr_bad.valid, "invalid hover eval returns valid=false");

        // Verify Memory View + Struct Field Overlay on frame_ptr
        engine.configure_memory_view("frame_ptr", 256);
        engine.wait_idle_for_test();
        auto mem0 = engine.memory_snapshot();
        CHECK(mem0.valid, "memory snapshot valid");
        CHECK(mem0.fields.size() == 5, "SensorFrame has 5 struct field overlays");

        // Verify step_instruction advances PC on ARM64
        uint64_t pc_before = snap_stepped->active_pc;
        engine.step_instruction();
        auto snap_asm = wait_for_state(engine, scope::SessionState::Stopped, cur_step + 1);
        CHECK(snap_asm->active_pc != pc_before, "step_instruction advanced PC");
        cur_step = snap_asm->step_index;

        // Break inside loop at compute_filtered call (line 34) to test step_into + step_out
        engine.toggle_breakpoint(src_file, 34);
        engine.wait_idle_for_test();
        engine.continue_exec();
        auto snap_call = wait_for_state(engine, scope::SessionState::Stopped, cur_step + 1);
        CHECK(snap_call->active_line == 34, "hit breakpoint at compute_filtered call");
        cur_step = snap_call->step_index;

        engine.step_into();
        auto snap_in = wait_for_state(engine, scope::SessionState::Stopped, cur_step + 1);
        CHECK(snap_in->active_line < 20, "step_into entered compute_filtered");
        cur_step = snap_in->step_index;

        // Verify select_frame switches cleanly to caller frame #1 (main) and back to #0
        engine.select_frame(1);
        engine.wait_idle_for_test();
        CHECK(engine.latest_snapshot()->selected_frame_idx == 1, "selected caller frame #1");
        engine.select_frame(0);
        engine.wait_idle_for_test();
        CHECK(engine.latest_snapshot()->selected_frame_idx == 0, "returned to frame #0");

        engine.step_out();
        auto snap_out = wait_for_state(engine, scope::SessionState::Stopped, cur_step + 1);
        CHECK(snap_out->active_line >= 34, "step_out returned to main");
        cur_step = snap_out->step_index;

        // Remove line 34 BP, break at end of main (line 50) to verify waveform, stdout & memory diff
        engine.toggle_breakpoint(src_file, 34);
        engine.toggle_breakpoint(src_file, 50);
        engine.wait_idle_for_test();
        engine.continue_exec();
        auto snap_end = wait_for_state(engine, scope::SessionState::Stopped, cur_step + 1);
        CHECK(snap_end->active_line == 50, "reached end of loop in step_basics");

        // Verify memory bytes mutated across steps
        auto mem1 = engine.memory_snapshot();
        CHECK(mem1.valid && mem1.bytes != mem1.prev_bytes, "memory diff detected mutated SensorFrame bytes");

        // Verify Waveform Probe on 64-sample vector<float>
        engine.configure_waveform_probe("waveform", 128);
        engine.wait_idle_for_test();
        auto wp = engine.waveform_probe();
        CHECK(wp.samples.size() == 64, "waveform probe read all 64 float samples");
        CHECK(wp.min_val < wp.max_val && std::isfinite(wp.avg_val), "waveform stats finite and non-trivial");
        std::printf("  -> waveform: %zu samples, min=%.3f, max=%.3f, avg=%.3f\n",
                    wp.samples.size(), wp.min_val, wp.max_val, wp.avg_val);

        // Verify LLDB REPL command + stdout capture
        engine.run_lldb_command("frame info");
        engine.wait_idle_for_test();
        bool saw_stdout = false, saw_repl = false;
        for (const auto& l : engine.console_lines()) {
            if (l.kind == scope::ConsoleLineKind::Stdout && l.text.find("seq=") != std::string::npos) {
                saw_stdout = true;
            }
            if (l.kind == scope::ConsoleLineKind::LldbOut && l.text.find("frame #0") != std::string::npos) {
                saw_repl = true;
            }
        }
        CHECK(saw_stdout, "captured target stdout");
        CHECK(saw_repl, "executed LLDB REPL command");

        // Verify restart preserves user breakpoint (line 49)
        engine.restart();
        auto snap_restarted = wait_for_state(engine, scope::SessionState::Stopped, 1);
        CHECK(snap_restarted->state == scope::SessionState::Stopped, "restart stopped at main");
        CHECK(!snap_restarted->breakpoints.empty(), "breakpoints preserved across restart");

        engine.remove_watch_expr("frame.seq");
        engine.toggle_breakpoint(src_file, 50);
        engine.terminate();
        wait_for_state(engine, scope::SessionState::Exited, 0, 2000);
    }

    // =========================================================================
    // 2) memory_watch: Conditional BP, Expression-Interpolated Logpoint + Δt,
    //    Pointer Graph Cycle, 2D Pixel Probe, Hardware Watchpoint
    // =========================================================================
    {
        std::string exe = (std::filesystem::path(fixtures_dir) / "memory_watch").string();
        std::printf("[TEST 2/7] memory_watch: conditional BP, logpoint {expr}+dt, graph cycle, 2D pixel, HW watchpoint\n");
        engine.clear_console();
        engine.launch(exe, {}, "", true);
        auto mw1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
        CHECK(mw1->state == scope::SessionState::Stopped, "memory_watch stopped at main");
        std::string mw_src = mw1->active_file;

        // Set BP at line 56 with expression-interpolated logpoint on line 59 & conditional BP on line 56
        engine.toggle_breakpoint(mw_src, 56);
        engine.wait_idle_for_test();
        auto snap_bp = engine.latest_snapshot();
        CHECK(!snap_bp->breakpoints.empty(), "breakpoint added");
        int32_t bp_id = snap_bp->breakpoints.back().id;
        engine.set_breakpoint_condition(bp_id, "i == 2");
        engine.wait_idle_for_test();

        engine.continue_exec();
        auto mw_cond = wait_for_state(engine, scope::SessionState::Stopped, mw1->step_index + 1);
        CHECK(mw_cond->state == scope::SessionState::Stopped, "conditional breakpoint hit");
        bool i_is_2 = false;
        for (const auto& v : mw_cond->locals) {
            if (v.name == "i" && v.value == "2") i_is_2 = true;
        }
        CHECK(i_is_2, "conditional BP stopped specifically at i == 2");

        // Pointer graph cycle detection on `head`
        engine.configure_pointer_graph("head", 16);
        engine.wait_idle_for_test();
        auto pg = engine.pointer_graph();
        CHECK(pg.nodes.size() == 4, "pointer graph found 4 ListNodes");
        CHECK(pg.has_cycle, "pointer graph detected intentional cycle");

        // 2D pixel probe on `pixels` (verify procedural orange grid at pixel (0,0) == RGBA(255,87,34,255))
        engine.configure_pixel_probe("pixels", 32, 32, 4);
        engine.wait_idle_for_test();
        auto pp = engine.pixel_probe();
        CHECK(pp.rgba_bytes.size() == 32 * 32 * 4, "pixel probe read 32x32 RGBA");
        CHECK(pp.rgba_bytes[0] == 255 && pp.rgba_bytes[1] == 87 &&
                  pp.rgba_bytes[2] == 34 && pp.rgba_bytes[3] == 255,
              "pixel (0,0) matches procedural signal-orange grid");

        // Clear conditional BP, arm hardware watchpoint on packet->gain + test condition & raw add/remove
        engine.remove_breakpoint(bp_id);
        engine.add_variable_watchpoint("packet->gain", false, true);
        engine.wait_idle_for_test();
        auto mw_armed = engine.latest_snapshot();
        CHECK(mw_armed->watchpoints.size() == 1, "hardware watchpoint armed");
        int32_t wp_id = mw_armed->watchpoints[0].id;
        uint64_t wp_addr = mw_armed->watchpoints[0].address;

        // Test watchpoint condition set/clear & raw address watchpoint add/remove
        engine.set_watchpoint_condition(wp_id, "iter >= 0");
        engine.wait_idle_for_test();
        engine.set_watchpoint_condition(wp_id, "");
        engine.wait_idle_for_test();
        engine.add_watchpoint(wp_addr + 4, 4, false, true, "gain_neighbor");
        engine.wait_idle_for_test();
        auto mw_two = engine.latest_snapshot();
        if (mw_two->watchpoints.size() >= 2) {
            engine.remove_watchpoint(mw_two->watchpoints.back().id);
            engine.wait_idle_for_test();
        }

        uint64_t before_wp = engine.latest_snapshot()->step_index;
        engine.continue_exec();
        auto mw_wp_hit = wait_for_state(engine, scope::SessionState::Stopped, before_wp + 1);
        CHECK(mw_wp_hit->state == scope::SessionState::Stopped, "HW watchpoint triggered stop");
        CHECK(mw_wp_hit->active_line == 23 || mw_wp_hit->active_line == 24,
              "HW watchpoint caught write in corrupt_gain_subtly");
        std::printf("  -> HW watchpoint caught write at line %u (%s)\n",
                    mw_wp_hit->active_line, mw_wp_hit->stop_reason.c_str());

        engine.remove_watchpoint(wp_id);
        engine.wait_idle_for_test();
        engine.terminate();
        wait_for_state(engine, scope::SessionState::Exited, 0, 2000);

        // Also test expression-interpolated Logpoint + Δt non-stopping run
        engine.clear_console();
        engine.launch(exe, {}, "", true);
        auto lp1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
        engine.toggle_breakpoint(mw_src, 56);
        engine.wait_idle_for_test();
        int32_t lp_id = engine.latest_snapshot()->breakpoints.back().id;
        engine.set_breakpoint_log(lp_id, "loop i={i} ch={packet->channel}");
        engine.wait_idle_for_test();
        engine.continue_exec();
        auto lp_exit = wait_for_state(engine, scope::SessionState::Exited, 0, 5000);
        CHECK(lp_exit->state == scope::SessionState::Exited, "logpoint auto-continued to clean exit");
        int log_hits = 0;
        bool saw_dt = false;
        for (const auto& l : engine.console_lines()) {
            if (l.text.find("[LOGPOINT #") != std::string::npos &&
                l.text.find("loop i=") != std::string::npos) {
                ++log_hits;
                if (l.text.find("us]") != std::string::npos || l.text.find("ms]") != std::string::npos) {
                    saw_dt = true;
                }
            }
        }
        CHECK(log_hits == 6, "expression-interpolated logpoint fired 6 times without stopping");
        CHECK(saw_dt, "logpoint logged inter-hit delta timestamp");
        std::printf("  -> logpoint fired %d times with {expr} interpolation & delta-t\n", log_hits);
        engine.remove_breakpoint(lp_id);
    }

    // =========================================================================
    // 3) stl_containers: Simplified C++ types & Struct-Table View + row diffs
    // =========================================================================
    {
        std::string exe = (std::filesystem::path(fixtures_dir) / "stl_containers").string();
        std::printf("[TEST 3/7] stl_containers: STL display types & Struct-Table view diffs\n");
        engine.launch(exe, {}, "", true);
        auto stl1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
        std::string src = stl1->active_file;
        engine.toggle_breakpoint(src, 49);
        engine.wait_idle_for_test();
        engine.continue_exec();
        auto s0 = wait_for_state(engine, scope::SessionState::Stopped, stl1->step_index + 1);
        CHECK(s0->state == scope::SessionState::Stopped, "stl_containers hit loop");

        bool saw_clean_string = false;
        for (const auto& v : s0->locals) {
            if (v.name == "status" && v.display_type == "std::string") saw_clean_string = true;
        }
        CHECK(saw_clean_string, "status simplified to std::string");

        engine.configure_struct_table("particles");
        engine.wait_idle_for_test();
        auto st0 = engine.struct_table();
        CHECK(st0.rows.size() == 16 && st0.columns.size() >= 6, "struct table 16x7");

        // Advance one outer loop iteration and verify Struct-Table captures prev_rows diff
        engine.continue_exec();
        wait_for_state(engine, scope::SessionState::Stopped, s0->step_index + 1);
        auto st1 = engine.struct_table();
        CHECK(!st1.prev_rows.empty() && st1.rows[0] != st1.prev_rows[0],
              "struct table tracked particle position mutation across steps");

        engine.toggle_breakpoint(src, 49);
        engine.terminate();
        wait_for_state(engine, scope::SessionState::Exited, 0, 2000);
    }

    // =========================================================================
    // 4) multithread: Multiple live threads, lazy vs expanded unwinding & switching
    // =========================================================================
    {
        std::string exe = (std::filesystem::path(fixtures_dir) / "multithread").string();
        std::printf("[TEST 4/7] multithread: Thread Matrix, lazy unwinding & thread selection\n");
        engine.launch(exe, {}, "", true);
        auto mt1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
        std::string src = mt1->active_file;
        // Break inside main's tick loop (line 36: std::printf) when all 3 workers are running
        engine.toggle_breakpoint(src, 36);
        engine.wait_idle_for_test();
        engine.continue_exec();
        auto mt_run = wait_for_state(engine, scope::SessionState::Stopped, mt1->step_index + 1);
        CHECK(mt_run->threads.size() >= 4, "multithread has >=4 live threads");

        engine.set_expand_all_threads(true);
        engine.wait_idle_for_test();
        auto mt_exp = engine.latest_snapshot();
        CHECK(mt_exp->threads.size() >= 4, "expanded thread matrix populated");

        // Switch to a worker thread and verify no crash
        uint64_t other_tid = 0;
        for (const auto& t : mt_exp->threads) {
            if (!t.is_selected) {
                other_tid = t.id;
                break;
            }
        }
        CHECK(other_tid != 0, "found non-selected worker thread");
        engine.select_thread(other_tid);
        engine.wait_idle_for_test();
        CHECK(engine.latest_snapshot()->selected_thread_id == other_tid, "switched selected thread");
        std::printf("  -> verified %zu threads and thread switching\n", mt_exp->threads.size());

        engine.set_expand_all_threads(false);
        engine.toggle_breakpoint(src, 36);

        // Verify live pause_exec() interrupts a running multithreaded process cleanly
        uint64_t pre_pause_step = engine.latest_snapshot()->step_index;
        engine.continue_exec();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        engine.pause_exec();
        auto mt_paused = wait_for_state(engine, scope::SessionState::Stopped, pre_pause_step + 1, 3000);
        CHECK(mt_paused->state == scope::SessionState::Stopped ||
                  mt_paused->state == scope::SessionState::Exited,
              "pause_exec interrupted running target cleanly");

        engine.terminate();
        wait_for_state(engine, scope::SessionState::Exited, 0, 2000);
    }

    // =========================================================================
    // 5) crash_autopsy: Both EXC_BAD_ACCESS (null+0x18) AND SIGABRT (--assert)
    // =========================================================================
    {
        std::string exe = (std::filesystem::path(fixtures_dir) / "crash_autopsy").string();
        std::printf("[TEST 5/7] crash_autopsy: SIGSEGV/EXC_BAD_ACCESS & SIGABRT assert\n");

        engine.launch(exe, {}, "", false);
        auto cr1 = wait_for_state(engine, scope::SessionState::Fault, 1);
        CHECK(cr1->state == scope::SessionState::Fault && cr1->crash.is_crash, "segfault detected");
        CHECK(cr1->crash.fault_address == 0x18, "exact fault address 0x18 parsed");
        CHECK(cr1->crash.fault_provenance == scope::ProvenanceKind::Null, "fault provenance NULL");
        CHECK(!cr1->crash.offending_instruction.empty(), "offending instruction captured");
        engine.terminate();
        wait_for_state(engine, scope::SessionState::Exited, 0, 2000);

        engine.launch(exe, {"--assert"}, "", false);
        auto cr2 = wait_for_state(engine, scope::SessionState::Fault, 1);
        std::printf("  -> assert stop: state=%d reason='%s' sig='%s' user_frame=#%u\n",
                    static_cast<int>(cr2->state), cr2->stop_reason.c_str(),
                    cr2->crash.signal_name.c_str(), cr2->crash.user_frame_index);
        CHECK(cr2->state == scope::SessionState::Fault && cr2->crash.is_crash, "SIGABRT detected");
        CHECK(cr2->crash.signal_name == "SIGABRT", "signal_name == SIGABRT");
        CHECK(cr2->crash.user_frame_index > 0, "user_frame_index skips libc/abort frames to user code");
        engine.terminate();
        wait_for_state(engine, scope::SessionState::Exited, 0, 2000);
    }

    // =========================================================================
    // 6) hft_orderbook: Cache-line padding & false sharing, Big-Endian wire packet,
    //    SPSC Ring Buffer wrap-around, L2 Order Book crossed detection, SIMD, symbols
    // =========================================================================
    {
        std::string exe = (std::filesystem::path(fixtures_dir) / "hft_orderbook").string();
        std::printf("[TEST 6/7] hft_orderbook: cache-line false sharing, BE wire, ring, book L2, SIMD, symbols\n");
        engine.launch(exe, {}, "", true);
        auto hft1 = wait_for_state(engine, scope::SessionState::Stopped, 1);
        std::string src = hft1->active_file;

        bool saw_simd = false;
        for (const auto& r : hft1->registers) {
            if (r.raw_bytes.size() >= 16) {
                saw_simd = true;
                break;
            }
        }
        CHECK(saw_simd, "SIMD 16B register raw bytes captured");

        engine.search_symbols("hft_orderbook");
        engine.wait_idle_for_test();
        CHECK(!engine.symbol_results().empty(), "symbol/file search returned results");

        engine.toggle_breakpoint(src, 151);
        engine.wait_idle_for_test();
        engine.continue_exec();
        auto tick0 = wait_for_state(engine, scope::SessionState::Stopped, hft1->step_index + 1);

        // Cache-line padding & false sharing on stats_ptr
        engine.configure_memory_view("stats_ptr", 256);
        engine.wait_idle_for_test();
        auto sm = engine.memory_snapshot();
        CHECK(sm.valid && sm.struct_size == 128 && sm.padding_bytes == 99,
              "MatchingEngineStats sizeof=128B pad=99B");
        CHECK(!sm.false_sharing_warnings.empty(), "CL#0 false-sharing warning emitted");

        // Big-Endian WireOrderMsg bytes check (msg_type_be == 0x00 0x41, symbol == "AAPL    ")
        engine.configure_memory_view("wire_ptr", 64);
        engine.wait_idle_for_test();
        auto wm = engine.memory_snapshot();
        CHECK(wm.valid && wm.bytes.size() >= 39, "wire_ptr memory read");
        CHECK(wm.bytes[0] == 0x00 && wm.bytes[1] == 0x41, "Big-Endian msg_type 0x0041 verified");
        CHECK(std::memcmp(wm.bytes.data() + 31, "AAPL    ", 8) == 0, "wire symbol 'AAPL    ' verified");

        // SPSC Ring Buffer Probe
        engine.configure_ring_probe("ring.slots", "ring.capacity", "ring.head", "ring.tail");
        engine.wait_idle_for_test();
        auto rp = engine.ring_probe();
        CHECK(rp.capacity == 16 && rp.occupied_count == 10 && rp.wrapped,
              "SPSC ring probe 10/16 wrapped");

        // L2 Order Book Probe (uncrossed on tick 0 -> crossed on tick 3!)
        engine.configure_orderbook_probe("book.bids", "book.asks", 10000.0);
        engine.wait_idle_for_test();
        auto ob0 = engine.orderbook_probe();
        CHECK(ob0.bids.size() == 5 && ob0.asks.size() == 5 && !ob0.crossed,
              "orderbook tick 0 uncrossed");

        uint64_t sidx = tick0->step_index;
        for (int t = 0; t < 3; ++t) {
            engine.continue_exec();
            auto st = wait_for_state(engine, scope::SessionState::Stopped, sidx + 1);
            CHECK(st->state == scope::SessionState::Stopped, "hft tick stopped");
            sidx = st->step_index;
        }
        auto ob3 = engine.orderbook_probe();
        CHECK(ob3.crossed && ob3.spread < 0.0, "orderbook tick 3 CROSSED BOOK detected");

        engine.toggle_breakpoint(src, 151);
        engine.terminate();
        wait_for_state(engine, scope::SessionState::Exited, 0, 2000);
    }

    // =========================================================================
    // 7) Negative / Error Resilience Tests (never crash or deadlock on bad input)
    // =========================================================================
    {
        std::printf("[TEST 7/7] Negative & error resilience tests\n");
        engine.launch("/nonexistent/binary/does_not_exist", {}, "", true);
        engine.wait_idle_for_test();
        CHECK(engine.latest_snapshot()->state == scope::SessionState::Idle,
              "non-existent binary stays Idle cleanly");

        engine.open_core("/nonexistent/binary", "/nonexistent/core");
        engine.wait_idle_for_test();

        engine.connect_remote("", "connect://127.0.0.1:1");
        engine.wait_idle_for_test();

        engine.attach(999999999ULL);
        engine.wait_idle_for_test();

        engine.reverse_continue();
        engine.reverse_step_over();
        engine.reverse_step_into();
        engine.wait_idle_for_test();

        engine.configure_memory_view("0x0", 256);
        engine.wait_idle_for_test();
        CHECK(!engine.memory_snapshot().valid, "null memory read reports invalid cleanly");
    }

    std::printf("ALL 7 SCOPE ENGINE INTEGRATION TEST SUITES PASSED!\n");
    return 0;
}
