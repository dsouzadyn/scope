#pragma once

#include "Types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <lldb/API/LLDB.h>

namespace scope {

/// Asynchronous, thread-safe liblldb (SB API) debugging engine.
///
/// All LLDB operations execute on an internal worker `std::jthread` so the 120 FPS
/// UI thread never blocks. Public control methods enqueue work asynchronously; read
/// accessors return immutable snapshots protected by an internal mutex.
///
/// On every target stop, active memory views and data probes are refreshed before
/// publishing `latest_snapshot()` so the UI observes an atomically consistent frame.
class DebuggerEngine {
  public:
    /// Initializes liblldb (ref-counted) and starts the background worker thread.
    DebuggerEngine();
    /// Stops the worker thread, terminates any active target, and releases liblldb.
    ~DebuggerEngine();

    DebuggerEngine(const DebuggerEngine&) = delete;
    DebuggerEngine& operator=(const DebuggerEngine&) = delete;

    /// Launches a local executable with optional CLI arguments and working directory.
    /// When `stop_at_main` is true, arms a one-shot breakpoint at `main`.
    void launch(const std::string& exe_path, const std::vector<std::string>& args = {},
                const std::string& cwd = "", bool stop_at_main = true);
    /// Attaches to an already-running local process by PID.
    void attach(uint64_t pid);
    /// Loads a post-mortem core dump paired with its debug-symbol executable via
    /// `SBTarget::LoadCore`.
    void open_core(const std::string& exe_path, const std::string& core_path);
    /// Connects to a remote `gdb-remote` stub (e.g., `lldb-server` or `rr replay -s`).
    void connect_remote(const std::string& exe_path, const std::string& remote_url);
    /// Re-launches the most recently launched target, preserving user breakpoints.
    void restart();
    /// Kills the active target process.
    void terminate();

    /// Resumes target execution.
    void continue_exec();
    /// Asynchronously interrupts a running target process.
    void pause_exec();
    /// Steps over one source line on the selected thread.
    void step_over();
    /// Steps into the call on the current source line.
    void step_into();
    /// Steps out of the current stack frame.
    void step_out();
    /// Executes a single machine instruction on the selected thread.
    void step_instruction();
    /// Issues a reverse-continue packet (`bc`) on reverse-capable remote stubs (`rr`).
    void reverse_continue();
    /// Issues a reverse-step packet (`bs`) on reverse-capable remote stubs (`rr`).
    void reverse_step_over();
    /// Issues a reverse-step-into packet on reverse-capable remote stubs (`rr`).
    void reverse_step_into();

    /// Selects the active thread by LLDB thread ID and refreshes the snapshot.
    void select_thread(uint64_t thread_id);
    /// Selects a stack frame index on the active thread and refreshes locals/disassembly.
    void select_frame(uint32_t frame_idx);
    /// Controls lazy unwinding of background threads (64 frames when true, 4 frames when false).
    void set_expand_all_threads(bool expand);

    /// Toggles a line breakpoint at `file:line` (persists across restarts).
    void toggle_breakpoint(const std::string& file, uint32_t line);
    /// Sets or clears (when empty) a C++ boolean condition on breakpoint `bp_id`.
    void set_breakpoint_condition(int32_t bp_id, const std::string& condition);
    /// Converts breakpoint `bp_id` into a non-stopping logpoint with `{expr}` interpolation and
    /// `+Δt`.
    void set_breakpoint_log(int32_t bp_id, const std::string& log_msg);
    /// Permanently deletes breakpoint `bp_id`.
    void remove_breakpoint(int32_t bp_id);

    /// Arms a hardware data watchpoint on `[address, address + byte_size)` (clamped to 1..8B).
    void add_watchpoint(uint64_t address, uint64_t byte_size, bool watch_read, bool watch_write,
                        const std::string& label);
    /// Resolves `expr` in the selected frame and arms a hardware watchpoint on its load address.
    void add_variable_watchpoint(const std::string& expr, bool watch_read, bool watch_write);
    /// Sets or clears an LLDB condition expression on hardware watchpoint `wp_id`.
    void set_watchpoint_condition(int32_t wp_id, const std::string& condition);
    /// Deletes hardware watchpoint `wp_id`.
    void remove_watchpoint(int32_t wp_id);

    /// Pins a C++ expression to the Watch inspector (re-evaluated on every stop).
    void add_watch_expr(const std::string& expr);
    /// Removes a pinned Watch expression.
    void remove_watch_expr(const std::string& expr);
    /// Evaluates a single identifier/expression for the source hover popover.
    void eval_hover(const std::string& expr);
    /// Searches target functions and compile-unit filenames matching `query`.
    void search_symbols(const std::string& query);
    /// Executes a raw LLDB command via `SBCommandInterpreter` and logs output to console.
    void run_lldb_command(const std::string& cmd);

    /// Configures live memory inspection (hex dump, byte diffs, `SBType`
    /// field/padding/false-sharing analysis).
    void configure_memory_view(const std::string& addr_or_expr, uint64_t size = 512);
    /// Configures the Struct-Table view on a container or array expression.
    void configure_struct_table(const std::string& expr);
    /// Configures the Waveform oscilloscope probe on a numeric vector/array expression.
    void configure_waveform_probe(const std::string& expr, uint32_t max_samples = 512);
    /// Configures the 2D Pixel Buffer probe (`channels` = 1, 3, or 4) for Metal texture upload.
    void configure_pixel_probe(const std::string& expr, int width, int height, int channels = 4);
    /// Configures the Pointer Graph probe starting from root pointer `expr` (with cycle detection).
    void configure_pointer_graph(const std::string& expr, uint32_t max_nodes = 48);
    /// Configures the HFT SPSC/MPSC Ring Buffer probe (`buf`, `capacity`, `head`, `tail`).
    void configure_ring_probe(const std::string& buf_expr, const std::string& cap_expr,
                              const std::string& head_expr, const std::string& tail_expr);
    /// Configures the HFT L2 Order Book probe with fixed-point `price_scale` and crossed-book
    /// detection.
    void configure_orderbook_probe(const std::string& bids_expr, const std::string& asks_expr,
                                   double price_scale = 10000.0);

    /// Returns the latest immutable session snapshot.
    std::shared_ptr<const StepSnapshot> latest_snapshot() const;
    /// Returns all recorded stop snapshots (ring buffer up to 256 stops) for Step-Delta scrubbing.
    std::vector<std::shared_ptr<const StepSnapshot>> history_snapshots() const;
    /// Returns the latest memory inspection snapshot (bytes, diffs, struct overlays, padding,
    /// warnings).
    MemorySnapshot memory_snapshot() const;
    /// Returns the latest Struct-Table container projection.
    StructTableData struct_table() const;
    /// Returns the latest Waveform probe samples and min/max/avg statistics.
    WaveformProbeData waveform_probe() const;
    /// Returns the latest normalized RGBA pixel buffer for Metal texture upload.
    PixelProbeData pixel_probe() const;
    /// Returns the latest traversed pointer graph and cycle diagnostics.
    PointerGraphData pointer_graph() const;
    /// Returns the latest SPSC/MPSC ring buffer occupancy snapshot.
    RingBufferProbeData ring_probe() const;
    /// Returns the latest L2 Order Book depth snapshot and crossed-book flag.
    OrderBookProbeData orderbook_probe() const;
    /// Returns the latest `search_symbols()` matches.
    std::vector<SymbolMatch> symbol_results() const;
    /// Returns the latest debounced hover evaluation result.
    HoverEvalResult hover_result() const;
    /// Returns buffered target stdout/stderr and LLDB REPL console lines.
    std::vector<ConsoleLine> console_lines() const;
    /// Clears all buffered console lines.
    void clear_console();

    /// Blocks until the worker command queue drains (intended for headless integration tests).
    void wait_idle_for_test();

  private:
    struct PendingBreakpoint {
        std::string file;
        uint32_t line = 0;
        std::string condition;
        std::string log_message;
        bool enabled = true;
    };

    void worker_loop(std::stop_token st);
    void enqueue(std::function<void()> fn);
    void poll_lldb_events();
    void drain_process_io();

    // Internal worker-thread operations (always called on worker_)
    void do_launch(const std::string& exe_path, const std::vector<std::string>& args,
                   const std::string& cwd, bool stop_at_main);
    void do_attach(uint64_t pid);
    void do_open_core(const std::string& exe_path, const std::string& core_path);
    void do_connect_remote(const std::string& exe_path, const std::string& remote_url);
    void apply_pending_breakpoints();
    void publish_snapshot(SessionState forced_state, bool record_history);
    void refresh_memory_locked_worker();
    void refresh_struct_table_worker();
    void refresh_waveform_worker();
    void refresh_pixel_probe_worker();
    void refresh_pointer_graph_worker();
    void refresh_ring_probe_worker();
    void refresh_orderbook_probe_worker();
    std::string interpolate_logpoint_msg(int32_t bp_id, const std::string& tmpl,
                                         lldb::SBFrame frame);

    ProvenanceKind classify_address_lazy(uint64_t addr, uint64_t sp);

    VarNode extract_var(lldb::SBValue val, const std::string& expr_path, int depth, uint64_t sp,
                        const std::unordered_map<std::string, std::string>& prev_values,
                        std::vector<StepDeltaEntry>* deltas, const char* delta_category);

    std::vector<DisasmInstruction> extract_disassembly(lldb::SBFrame frame);
    CrashInfo extract_crash_info(lldb::SBThread thread, const std::vector<FrameInfo>& frames,
                                 uint64_t sp);
    void append_console(ConsoleLineKind kind, const std::string& text);

    lldb::SBDebugger debugger_;
    lldb::SBTarget target_;
    lldb::SBProcess process_;
    lldb::SBListener listener_;

    std::string last_exe_;
    std::vector<std::string> last_args_;
    std::string last_cwd_;
    bool last_stop_at_main_ = true;
    bool is_core_dump_ = false;
    bool is_remote_ = false;
    std::atomic<bool> expand_all_threads_{false};
    uint32_t selected_frame_idx_ = 0;
    uint64_t step_counter_ = 0;

    std::vector<PendingBreakpoint> pending_bps_;
    std::unordered_map<int32_t, std::string> bp_log_messages_;
    std::unordered_map<int32_t, std::chrono::steady_clock::time_point> bp_last_hit_;
    std::unordered_map<int32_t, std::string> wp_labels_;
    std::vector<std::string> watch_exprs_;

    // Per-stop lazy page provenance cache (page_addr -> ProvenanceKind)
    std::unordered_map<uint64_t, ProvenanceKind> page_prov_cache_;

    // Active inspection configurations (owned by worker thread)
    std::string mem_expr_;
    uint64_t mem_size_ = 512;
    std::string table_expr_;
    std::string waveform_expr_;
    uint32_t waveform_max_ = 512;
    std::string pixel_expr_;
    int pixel_w_ = 32;
    int pixel_h_ = 32;
    int pixel_ch_ = 4;
    uint64_t pixel_version_ = 0;
    std::string graph_expr_;
    uint32_t graph_max_nodes_ = 48;
    std::string ring_buf_expr_;
    std::string ring_cap_expr_;
    std::string ring_head_expr_;
    std::string ring_tail_expr_;
    std::string book_bids_expr_;
    std::string book_asks_expr_;
    double book_price_scale_ = 10000.0;

    // Previous values for step-delta diffing
    std::unordered_map<std::string, std::string> prev_local_values_;
    std::unordered_map<std::string, std::string> prev_watch_values_;
    std::unordered_map<std::string, uint64_t> prev_reg_values_;
    uint64_t prev_mem_base_ = 0;
    std::vector<uint8_t> prev_mem_bytes_;
    std::vector<std::vector<std::string>> prev_table_rows_;

    // Command queue
    mutable std::mutex cmd_mutex_;
    std::condition_variable cmd_cv_;
    std::deque<std::function<void()>> cmd_queue_;
    std::atomic<bool> cmd_busy_{false};

    // Published immutable UI state
    mutable std::mutex state_mutex_;
    std::shared_ptr<const StepSnapshot> latest_snapshot_;
    std::vector<std::shared_ptr<const StepSnapshot>> history_;
    MemorySnapshot memory_snapshot_;
    StructTableData struct_table_;
    WaveformProbeData waveform_probe_;
    PixelProbeData pixel_probe_;
    PointerGraphData pointer_graph_;
    RingBufferProbeData ring_probe_;
    OrderBookProbeData orderbook_probe_;
    std::vector<SymbolMatch> symbol_results_;
    HoverEvalResult hover_result_;
    std::vector<ConsoleLine> console_lines_;

    std::jthread worker_;
};

} // namespace scope
