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

class DebuggerEngine {
public:
    DebuggerEngine();
    ~DebuggerEngine();

    DebuggerEngine(const DebuggerEngine&) = delete;
    DebuggerEngine& operator=(const DebuggerEngine&) = delete;

    // Lifecycle & execution commands (thread-safe, async)
    void launch(const std::string& exe_path,
                const std::vector<std::string>& args = {},
                const std::string& cwd = "",
                bool stop_at_main = true);
    void attach(uint64_t pid);
    void open_core(const std::string& exe_path, const std::string& core_path);
    void connect_remote(const std::string& exe_path, const std::string& remote_url);
    void restart();
    void terminate();

    void continue_exec();
    void pause_exec();
    void step_over();
    void step_into();
    void step_out();
    void step_instruction();
    void reverse_continue();
    void reverse_step_over();
    void reverse_step_into();

    void select_thread(uint64_t thread_id);
    void select_frame(uint32_t frame_idx);
    void set_expand_all_threads(bool expand);

    // Breakpoints & hardware watchpoints
    void toggle_breakpoint(const std::string& file, uint32_t line);
    void set_breakpoint_condition(int32_t bp_id, const std::string& condition);
    void set_breakpoint_log(int32_t bp_id, const std::string& log_msg);
    void remove_breakpoint(int32_t bp_id);

    void add_watchpoint(uint64_t address,
                        uint64_t byte_size,
                        bool watch_read,
                        bool watch_write,
                        const std::string& label);
    void add_variable_watchpoint(const std::string& expr, bool watch_read, bool watch_write);
    void set_watchpoint_condition(int32_t wp_id, const std::string& condition);
    void remove_watchpoint(int32_t wp_id);

    // Watches, symbols & evaluation
    void add_watch_expr(const std::string& expr);
    void remove_watch_expr(const std::string& expr);
    void eval_hover(const std::string& expr);
    void search_symbols(const std::string& query);
    void run_lldb_command(const std::string& cmd);

    // Memory & Probes (auto-refreshed on every stop once configured)
    void configure_memory_view(const std::string& addr_or_expr, uint64_t size = 512);
    void configure_struct_table(const std::string& expr);
    void configure_waveform_probe(const std::string& expr, uint32_t max_samples = 512);
    void configure_pixel_probe(const std::string& expr, int width, int height, int channels = 4);
    void configure_pointer_graph(const std::string& expr, uint32_t max_nodes = 48);
    void configure_ring_probe(const std::string& buf_expr,
                              const std::string& cap_expr,
                              const std::string& head_expr,
                              const std::string& tail_expr);
    void configure_orderbook_probe(const std::string& bids_expr,
                                   const std::string& asks_expr,
                                   double price_scale = 10000.0);

    // Thread-safe read accessors for the 120 FPS UI thread
    std::shared_ptr<const StepSnapshot> latest_snapshot() const;
    std::vector<std::shared_ptr<const StepSnapshot>> history_snapshots() const;
    MemorySnapshot memory_snapshot() const;
    StructTableData struct_table() const;
    WaveformProbeData waveform_probe() const;
    PixelProbeData pixel_probe() const;
    PointerGraphData pointer_graph() const;
    RingBufferProbeData ring_probe() const;
    OrderBookProbeData orderbook_probe() const;
    std::vector<SymbolMatch> symbol_results() const;
    HoverEvalResult hover_result() const;
    std::vector<ConsoleLine> console_lines() const;
    void clear_console();

    // Synchronous barrier for integration tests
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
    void do_launch(const std::string& exe_path,
                   const std::vector<std::string>& args,
                   const std::string& cwd,
                   bool stop_at_main);
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
    std::string interpolate_logpoint_msg(int32_t bp_id, const std::string& tmpl, lldb::SBFrame frame);

    ProvenanceKind classify_address_lazy(uint64_t addr, uint64_t sp);

    VarNode extract_var(lldb::SBValue val,
                        const std::string& expr_path,
                        int depth,
                        uint64_t sp,
                        const std::unordered_map<std::string, std::string>& prev_values,
                        std::vector<StepDeltaEntry>* deltas,
                        const char* delta_category);

    std::vector<DisasmInstruction> extract_disassembly(lldb::SBFrame frame);
    CrashInfo extract_crash_info(lldb::SBThread thread,
                                 const std::vector<FrameInfo>& frames,
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
