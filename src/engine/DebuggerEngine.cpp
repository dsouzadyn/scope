#include "DebuggerEngine.hpp"
#include "TypeFormat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace scope {

namespace {

std::atomic<int> g_lldb_refcount{0};
std::mutex g_lldb_init_mutex;

void acquire_lldb() {
    std::lock_guard<std::mutex> lock(g_lldb_init_mutex);
    if (g_lldb_refcount.fetch_add(1) == 0) {
        lldb::SBDebugger::Initialize();
    }
}

void release_lldb() {
    std::lock_guard<std::mutex> lock(g_lldb_init_mutex);
    if (g_lldb_refcount.fetch_sub(1) == 1) {
        lldb::SBDebugger::Terminate();
    }
}

std::string safe_str(const char* s) {
    return s ? std::string(s) : std::string();
}

std::string format_value_string(lldb::SBValue& val) {
    const char* summary = val.GetSummary();
    const char* value = val.GetValue();
    if (summary && *summary) {
        std::string s(summary);
        if (s.find("error:") != std::string::npos || s.find("invalid value") != std::string::npos) {
            return "<uninitialized>";
        }
        return s;
    }
    if (value && *value) {
        return std::string(value);
    }
    uint32_t n = val.GetNumChildren();
    if (n > 0) {
        return "{" + std::to_string(n) + " fields}";
    }
    return "<unavailable>";
}

std::string stop_reason_name(lldb::StopReason r) {
    switch (r) {
        case lldb::eStopReasonInvalid:      return "INVALID";
        case lldb::eStopReasonNone:         return "NONE";
        case lldb::eStopReasonTrace:        return "STEP";
        case lldb::eStopReasonBreakpoint:   return "BREAKPOINT";
        case lldb::eStopReasonWatchpoint:   return "WATCHPOINT";
        case lldb::eStopReasonSignal:       return "SIGNAL";
        case lldb::eStopReasonException:    return "EXCEPTION";
        case lldb::eStopReasonExec:         return "EXEC";
        case lldb::eStopReasonPlanComplete: return "STEP";
        case lldb::eStopReasonThreadExiting:return "EXITING";
        case lldb::eStopReasonInstrumentation: return "INSTRUMENT";
        default:                            return "STOPPED";
    }
}

bool is_crash_stop(lldb::StopReason r, int32_t signo, const std::string& desc) {
    if (r == lldb::eStopReasonException) return true;
    if (r == lldb::eStopReasonSignal) {
        if (signo == SIGSEGV || signo == SIGBUS || signo == SIGABRT ||
            signo == SIGILL || signo == SIGFPE) {
            return true;
        }
        if (desc.find("SIGSEGV") != std::string::npos ||
            desc.find("SIGBUS") != std::string::npos ||
            desc.find("SIGABRT") != std::string::npos ||
            desc.find("SIGILL") != std::string::npos ||
            desc.find("SIGFPE") != std::string::npos ||
            desc.find("EXC_BAD_ACCESS") != std::string::npos) {
            return true;
        }
    }
    return false;
}

uint64_t parse_hex_or_dec(const std::string& s, bool& ok) {
    ok = false;
    std::string trimmed = s;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) return 0;
    // Strip underscores for grouped hex input like 0x0000_0001_6fdf_f4a0
    std::string clean;
    for (char c : trimmed) {
        if (c != '_') clean.push_back(c);
    }
    char* end = nullptr;
    unsigned long long v = std::strtoull(clean.c_str(), &end, 0);
    if (end && *end == '\0') {
        ok = true;
        return static_cast<uint64_t>(v);
    }
    return 0;
}

} // namespace

DebuggerEngine::DebuggerEngine() {
    acquire_lldb();

    auto init_snap = std::make_shared<StepSnapshot>();
    init_snap->state = SessionState::Idle;
    latest_snapshot_ = init_snap;

    worker_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

DebuggerEngine::~DebuggerEngine() {
    worker_.request_stop();
    cmd_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    release_lldb();
}

void DebuggerEngine::enqueue(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        cmd_queue_.push_back(std::move(fn));
    }
    cmd_cv_.notify_one();
}

void DebuggerEngine::wait_idle_for_test() {
    for (int i = 0; i < 400; ++i) {
        {
            std::lock_guard<std::mutex> lock(cmd_mutex_);
            if (cmd_queue_.empty() && !cmd_busy_.load()) {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void DebuggerEngine::worker_loop(std::stop_token st) {
    debugger_ = lldb::SBDebugger::Create(false);
    debugger_.SetAsync(true);
    listener_ = debugger_.GetListener();

    append_console(ConsoleLineKind::System,
                   "SCOPE ENGINE READY // " + safe_str(lldb::SBDebugger::GetVersionString()));

    while (!st.stop_requested()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(cmd_mutex_);
            if (cmd_queue_.empty()) {
                cmd_cv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
                    return !cmd_queue_.empty() || st.stop_requested();
                });
            }
            if (st.stop_requested()) break;
            if (!cmd_queue_.empty()) {
                task = std::move(cmd_queue_.front());
                cmd_queue_.pop_front();
                cmd_busy_.store(true);
            }
        }

        if (task) {
            task();
            cmd_busy_.store(false);
        }

        poll_lldb_events();
        drain_process_io();
    }

    if (process_.IsValid()) {
        auto state = process_.GetState();
        if (state == lldb::eStateRunning || state == lldb::eStateStopped) {
            process_.Kill();
        }
    }
    if (debugger_.IsValid()) {
        lldb::SBDebugger::Destroy(debugger_);
    }
}

void DebuggerEngine::launch(const std::string& exe_path,
                            const std::vector<std::string>& args,
                            const std::string& cwd,
                            bool stop_at_main) {
    enqueue([this, exe_path, args, cwd, stop_at_main] {
        do_launch(exe_path, args, cwd, stop_at_main);
    });
}

void DebuggerEngine::attach(uint64_t pid) {
    enqueue([this, pid] { do_attach(pid); });
}

void DebuggerEngine::open_core(const std::string& exe_path, const std::string& core_path) {
    enqueue([this, exe_path, core_path] { do_open_core(exe_path, core_path); });
}

void DebuggerEngine::connect_remote(const std::string& exe_path, const std::string& remote_url) {
    enqueue([this, exe_path, remote_url] { do_connect_remote(exe_path, remote_url); });
}

void DebuggerEngine::restart() {
    enqueue([this] {
        if (!last_exe_.empty()) {
            do_launch(last_exe_, last_args_, last_cwd_, last_stop_at_main_);
        }
    });
}

void DebuggerEngine::terminate() {
    enqueue([this] {
        if (process_.IsValid()) {
            process_.Kill();
            append_console(ConsoleLineKind::System, "Target terminated.");
            publish_snapshot(SessionState::Exited, false);
        }
    });
}

void DebuggerEngine::continue_exec() {
    enqueue([this] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            lldb::SBError err = process_.Continue();
            if (err.Fail()) {
                append_console(ConsoleLineKind::Stderr, "Continue failed: " + safe_str(err.GetCString()));
            } else {
                publish_snapshot(SessionState::Running, false);
            }
        }
    });
}

void DebuggerEngine::pause_exec() {
    enqueue([this] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateRunning) {
            process_.Stop();
        }
    });
}

void DebuggerEngine::step_over() {
    enqueue([this] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            lldb::SBThread t = process_.GetSelectedThread();
            if (t.IsValid()) {
                t.StepOver();
                publish_snapshot(SessionState::Running, false);
            }
        }
    });
}

void DebuggerEngine::step_into() {
    enqueue([this] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            lldb::SBThread t = process_.GetSelectedThread();
            if (t.IsValid()) {
                t.StepInto();
                publish_snapshot(SessionState::Running, false);
            }
        }
    });
}

void DebuggerEngine::step_out() {
    enqueue([this] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            lldb::SBThread t = process_.GetSelectedThread();
            if (t.IsValid()) {
                t.StepOut();
                publish_snapshot(SessionState::Running, false);
            }
        }
    });
}

void DebuggerEngine::step_instruction() {
    enqueue([this] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            lldb::SBThread t = process_.GetSelectedThread();
            if (t.IsValid()) {
                t.StepInstruction(false);
                publish_snapshot(SessionState::Running, false);
            }
        }
    });
}

void DebuggerEngine::reverse_continue() {
    enqueue([this] {
        if (!process_.IsValid() || !is_remote_) {
            append_console(ConsoleLineKind::Stderr,
                           "Reverse execution requires a reverse-capable remote target (e.g. rr gdb-remote).");
            return;
        }
        lldb::SBCommandReturnObject ret;
        debugger_.GetCommandInterpreter().HandleCommand("process plugin packet send bc", ret);
        if (const char* out = ret.GetOutput(); out && *out) {
            append_console(ConsoleLineKind::System, "RR reverse-continue: " + std::string(out));
        }
        publish_snapshot(SessionState::Stopped, true);
    });
}

void DebuggerEngine::reverse_step_over() {
    enqueue([this] {
        if (!process_.IsValid() || !is_remote_) {
            append_console(ConsoleLineKind::Stderr,
                           "Reverse step requires a reverse-capable remote target (e.g. rr gdb-remote).");
            return;
        }
        lldb::SBCommandReturnObject ret;
        debugger_.GetCommandInterpreter().HandleCommand("process plugin packet send bs", ret);
        publish_snapshot(SessionState::Stopped, true);
    });
}

void DebuggerEngine::reverse_step_into() {
    reverse_step_over();
}

void DebuggerEngine::set_expand_all_threads(bool expand) {
    bool prev = expand_all_threads_.exchange(expand);
    if (expand && !prev) {
        enqueue([this] {
            if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
                publish_snapshot(latest_snapshot()->state, false);
            }
        });
    }
}

void DebuggerEngine::select_thread(uint64_t thread_id) {
    enqueue([this, thread_id] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            if (process_.SetSelectedThreadByID(thread_id)) {
                selected_frame_idx_ = 0;
                publish_snapshot(SessionState::Stopped, false);
            }
        }
    });
}

void DebuggerEngine::select_frame(uint32_t frame_idx) {
    enqueue([this, frame_idx] {
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            lldb::SBThread t = process_.GetSelectedThread();
            if (t.IsValid() && frame_idx < t.GetNumFrames()) {
                t.SetSelectedFrame(frame_idx);
                selected_frame_idx_ = frame_idx;
                publish_snapshot(SessionState::Stopped, false);
            }
        }
    });
}

void DebuggerEngine::toggle_breakpoint(const std::string& file, uint32_t line) {
    enqueue([this, file, line] {
        std::string target_base = std::filesystem::path(file).filename().string();

        // Check existing pending list
        for (auto it = pending_bps_.begin(); it != pending_bps_.end(); ++it) {
            if (std::filesystem::path(it->file).filename().string() == target_base &&
                it->line == line) {
                pending_bps_.erase(it);
                // Remove matching LLDB breakpoint if target active
                if (target_.IsValid()) {
                    uint32_t n = target_.GetNumBreakpoints();
                    for (uint32_t i = 0; i < n; ++i) {
                        lldb::SBBreakpoint bp = target_.GetBreakpointAtIndex(i);
                        if (bp.GetNumLocations() > 0) {
                            lldb::SBLineEntry le = bp.GetLocationAtIndex(0).GetAddress().GetLineEntry();
                            std::string f = safe_str(le.GetFileSpec().GetFilename());
                            if (f == target_base && le.GetLine() == line) {
                                bp_log_messages_.erase(bp.GetID());
                                target_.BreakpointDelete(bp.GetID());
                                break;
                            }
                        }
                    }
                }
                publish_snapshot(latest_snapshot()->state, false);
                return;
            }
        }

        // Add new breakpoint
        PendingBreakpoint pb;
        pb.file = file;
        pb.line = line;
        pending_bps_.push_back(pb);

        if (target_.IsValid()) {
            lldb::SBBreakpoint bp = target_.BreakpointCreateByLocation(file.c_str(), line);
            if (!bp.IsValid() || bp.GetNumLocations() == 0) {
                target_.BreakpointDelete(bp.GetID());
                bp = target_.BreakpointCreateByLocation(target_base.c_str(), line);
            }
            if (bp.IsValid()) {
                append_console(ConsoleLineKind::System,
                               "BP #" + std::to_string(bp.GetID()) + " set at " +
                               target_base + ":" + std::to_string(line));
            }
        }
        publish_snapshot(latest_snapshot()->state, false);
    });
}

void DebuggerEngine::set_breakpoint_condition(int32_t bp_id, const std::string& condition) {
    enqueue([this, bp_id, condition] {
        if (target_.IsValid()) {
            lldb::SBBreakpoint bp = target_.FindBreakpointByID(bp_id);
            if (bp.IsValid()) {
                bp.SetCondition(condition.empty() ? nullptr : condition.c_str());
                // Sync to pending_bps_
                if (bp.GetNumLocations() > 0) {
                    lldb::SBLineEntry le = bp.GetLocationAtIndex(0).GetAddress().GetLineEntry();
                    std::string f = safe_str(le.GetFileSpec().GetFilename());
                    for (auto& pb : pending_bps_) {
                        if (std::filesystem::path(pb.file).filename().string() == f &&
                            pb.line == le.GetLine()) {
                            pb.condition = condition;
                        }
                    }
                }
                publish_snapshot(latest_snapshot()->state, false);
            }
        }
    });
}

void DebuggerEngine::set_breakpoint_log(int32_t bp_id, const std::string& log_msg) {
    enqueue([this, bp_id, log_msg] {
        if (target_.IsValid()) {
            lldb::SBBreakpoint bp = target_.FindBreakpointByID(bp_id);
            if (bp.IsValid()) {
                if (log_msg.empty()) {
                    bp_log_messages_.erase(bp_id);
                } else {
                    bp_log_messages_[bp_id] = log_msg;
                }
                bp.SetAutoContinue(false);
                if (bp.GetNumLocations() > 0) {
                    lldb::SBLineEntry le = bp.GetLocationAtIndex(0).GetAddress().GetLineEntry();
                    std::string f = safe_str(le.GetFileSpec().GetFilename());
                    for (auto& pb : pending_bps_) {
                        if (std::filesystem::path(pb.file).filename().string() == f &&
                            pb.line == le.GetLine()) {
                            pb.log_message = log_msg;
                        }
                    }
                }
                publish_snapshot(latest_snapshot()->state, false);
            }
        }
    });
}

void DebuggerEngine::remove_breakpoint(int32_t bp_id) {
    enqueue([this, bp_id] {
        if (target_.IsValid()) {
            lldb::SBBreakpoint bp = target_.FindBreakpointByID(bp_id);
            if (bp.IsValid() && bp.GetNumLocations() > 0) {
                lldb::SBLineEntry le = bp.GetLocationAtIndex(0).GetAddress().GetLineEntry();
                std::string f = safe_str(le.GetFileSpec().GetFilename());
                uint32_t l = le.GetLine();
                std::erase_if(pending_bps_, [&](const PendingBreakpoint& pb) {
                    return std::filesystem::path(pb.file).filename().string() == f && pb.line == l;
                });
            }
            bp_log_messages_.erase(bp_id);
            target_.BreakpointDelete(bp_id);
            publish_snapshot(latest_snapshot()->state, false);
        }
    });
}

void DebuggerEngine::add_watchpoint(uint64_t address,
                                    uint64_t byte_size,
                                    bool watch_read,
                                    bool watch_write,
                                    const std::string& label) {
    enqueue([this, address, byte_size, watch_read, watch_write, label] {
        if (!target_.IsValid() || !process_.IsValid()) return;
        uint64_t clamped_size = std::clamp<uint64_t>(byte_size == 0 ? 4 : byte_size, 1, 8);
        lldb::SBWatchpointOptions opts;
        opts.SetWatchpointTypeRead(watch_read);
        opts.SetWatchpointTypeWrite(watch_write ? lldb::eWatchpointWriteTypeOnModify
                                                : lldb::eWatchpointWriteTypeDisabled);
        lldb::SBError err;
        lldb::SBWatchpoint wp = target_.WatchpointCreateByAddress(address, clamped_size, opts, err);
        if (err.Fail() || !wp.IsValid()) {
            append_console(ConsoleLineKind::Stderr,
                           "Hardware watchpoint failed: " + safe_str(err.GetCString()));
        } else {
            wp_labels_[wp.GetID()] = label.empty() ? format_hex64_grouped(address) : label;
            append_console(ConsoleLineKind::System,
                           "HW WATCHPOINT #" + std::to_string(wp.GetID()) + " armed on " +
                           wp_labels_[wp.GetID()] + " (" + std::to_string(clamped_size) + "B)");
        }
        publish_snapshot(latest_snapshot()->state, false);
    });
}

void DebuggerEngine::add_variable_watchpoint(const std::string& expr,
                                             bool watch_read,
                                             bool watch_write) {
    enqueue([this, expr, watch_read, watch_write] {
        if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped) return;
        lldb::SBThread t = process_.GetSelectedThread();
        lldb::SBFrame f = t.GetSelectedFrame();
        if (!f.IsValid()) return;

        lldb::SBValue val = f.EvaluateExpression(expr.c_str());
        if (!val.IsValid() || val.GetError().Fail()) {
            val = f.GetValueForVariablePath(expr.c_str());
        }
        if (!val.IsValid() || val.GetError().Fail()) {
            append_console(ConsoleLineKind::Stderr,
                           "Cannot resolve watchpoint target: " + expr);
            return;
        }
        lldb::addr_t addr = val.GetLoadAddress();
        if (addr == LLDB_INVALID_ADDRESS || addr == 0) {
            append_console(ConsoleLineKind::Stderr,
                           "Expression has no memory address (register/rvalue): " + expr);
            return;
        }
        uint64_t size = std::clamp<uint64_t>(val.GetByteSize(), 1, 8);
        lldb::SBWatchpointOptions opts;
        opts.SetWatchpointTypeRead(watch_read);
        opts.SetWatchpointTypeWrite(watch_write ? lldb::eWatchpointWriteTypeOnModify
                                                : lldb::eWatchpointWriteTypeDisabled);
        lldb::SBError err;
        lldb::SBWatchpoint wp = target_.WatchpointCreateByAddress(addr, size, opts, err);
        if (err.Fail() || !wp.IsValid()) {
            append_console(ConsoleLineKind::Stderr,
                           "Watchpoint error on " + expr + ": " + safe_str(err.GetCString()));
        } else {
            wp_labels_[wp.GetID()] = expr;
            append_console(ConsoleLineKind::System,
                           "HW WATCHPOINT #" + std::to_string(wp.GetID()) + " armed on " +
                           expr + " @ " + format_hex64_grouped(addr) +
                           " (" + std::to_string(size) + "B)");
        }
        publish_snapshot(latest_snapshot()->state, false);
    });
}

void DebuggerEngine::set_watchpoint_condition(int32_t wp_id, const std::string& condition) {
    enqueue([this, wp_id, condition] {
        if (target_.IsValid()) {
            lldb::SBWatchpoint wp = target_.FindWatchpointByID(wp_id);
            if (wp.IsValid()) {
                wp.SetCondition(condition.empty() ? nullptr : condition.c_str());
                append_console(ConsoleLineKind::System,
                               "HW WATCHPOINT #" + std::to_string(wp_id) +
                               (condition.empty() ? " condition cleared" : (" condition: " + condition)));
                publish_snapshot(latest_snapshot()->state, false);
            }
        }
    });
}

void DebuggerEngine::remove_watchpoint(int32_t wp_id) {
    enqueue([this, wp_id] {
        if (target_.IsValid()) {
            wp_labels_.erase(wp_id);
            target_.DeleteWatchpoint(wp_id);
            publish_snapshot(latest_snapshot()->state, false);
        }
    });
}

void DebuggerEngine::add_watch_expr(const std::string& expr) {
    enqueue([this, expr] {
        if (expr.empty()) return;
        if (std::find(watch_exprs_.begin(), watch_exprs_.end(), expr) == watch_exprs_.end()) {
            watch_exprs_.push_back(expr);
            publish_snapshot(latest_snapshot()->state, false);
        }
    });
}

void DebuggerEngine::remove_watch_expr(const std::string& expr) {
    enqueue([this, expr] {
        std::erase(watch_exprs_, expr);
        publish_snapshot(latest_snapshot()->state, false);
    });
}

void DebuggerEngine::eval_hover(const std::string& expr) {
    enqueue([this, expr] {
        HoverEvalResult res;
        res.expr = expr;
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped && !expr.empty()) {
            lldb::SBFrame f = process_.GetSelectedThread().GetSelectedFrame();
            if (f.IsValid()) {
                lldb::SBValue v = f.GetValueForVariablePath(expr.c_str());
                if (!v.IsValid() || v.GetError().Fail()) {
                    v = f.EvaluateExpression(expr.c_str());
                }
                if (v.IsValid() && !v.GetError().Fail()) {
                    uint64_t sp = f.GetSP();
                    std::unordered_map<std::string, std::string> empty_prev;
                    res.root = extract_var(v, expr, 0, sp, empty_prev, nullptr, nullptr);
                    res.valid = true;
                } else {
                    res.error = safe_str(v.GetError().GetCString());
                }
            }
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        hover_result_ = std::move(res);
    });
}

void DebuggerEngine::search_symbols(const std::string& query) {
    enqueue([this, query] {
        std::vector<SymbolMatch> results;
        if (target_.IsValid() && !query.empty()) {
            std::string lo_q = query;
            std::transform(lo_q.begin(), lo_q.end(), lo_q.begin(), ::tolower);

            // Search functions by regex/substring
            lldb::SBSymbolContextList syms =
                target_.FindFunctions(query.c_str(), lldb::eFunctionNameTypeAuto);
            uint32_t n = std::min<uint32_t>(syms.GetSize(), 64);
            for (uint32_t i = 0; i < n; ++i) {
                lldb::SBSymbolContext sc = syms.GetContextAtIndex(i);
                SymbolMatch m;
                lldb::SBFunction fn = sc.GetFunction();
                if (fn.IsValid()) {
                    m.name = simplify_function_name(safe_str(fn.GetDisplayName()));
                    lldb::SBAddress addr = fn.GetStartAddress();
                    m.address = addr.GetLoadAddress(target_);
                    lldb::SBLineEntry le = addr.GetLineEntry();
                    if (le.IsValid()) {
                        char pbuf[1024];
                        if (le.GetFileSpec().GetPath(pbuf, sizeof(pbuf)) > 0) m.file = pbuf;
                        m.line = le.GetLine();
                    }
                } else if (sc.GetSymbol().IsValid()) {
                    m.name = simplify_function_name(safe_str(sc.GetSymbol().GetDisplayName()));
                    m.address = sc.GetSymbol().GetStartAddress().GetLoadAddress(target_);
                }
                if (!m.name.empty()) results.push_back(std::move(m));
            }

            // Also scan compile units for source filenames matching query
            uint32_t nmods = target_.GetNumModules();
            for (uint32_t mi = 0; mi < nmods && results.size() < 80; ++mi) {
                lldb::SBModule mod = target_.GetModuleAtIndex(mi);
                uint32_t ncu = mod.GetNumCompileUnits();
                for (uint32_t ci = 0; ci < ncu && results.size() < 80; ++ci) {
                    lldb::SBCompileUnit cu = mod.GetCompileUnitAtIndex(ci);
                    char pbuf[1024];
                    if (cu.GetFileSpec().GetPath(pbuf, sizeof(pbuf)) > 0) {
                        std::string path(pbuf);
                        std::string lo_p = path;
                        std::transform(lo_p.begin(), lo_p.end(), lo_p.begin(), ::tolower);
                        if (lo_p.find(lo_q) != std::string::npos) {
                            results.push_back(SymbolMatch{
                                "[FILE] " + std::filesystem::path(path).filename().string(),
                                path,
                                1,
                                0,
                            });
                        }
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        symbol_results_ = std::move(results);
    });
}

void DebuggerEngine::run_lldb_command(const std::string& cmd) {
    enqueue([this, cmd] {
        if (cmd.empty()) return;
        append_console(ConsoleLineKind::LldbCmd, "(lldb) " + cmd);
        lldb::SBCommandReturnObject ret;
        debugger_.GetCommandInterpreter().HandleCommand(cmd.c_str(), ret);
        if (const char* out = ret.GetOutput(); out && *out) {
            append_console(ConsoleLineKind::LldbOut, out);
        }
        if (const char* err = ret.GetError(); err && *err) {
            append_console(ConsoleLineKind::Stderr, err);
        }
        if (process_.IsValid() && process_.GetState() == lldb::eStateStopped) {
            publish_snapshot(SessionState::Stopped, false);
        }
    });
}

void DebuggerEngine::configure_memory_view(const std::string& addr_or_expr, uint64_t size) {
    enqueue([this, addr_or_expr, size] {
        mem_expr_ = addr_or_expr;
        mem_size_ = std::clamp<uint64_t>(size, 64, 16384);
        prev_mem_bytes_.clear();
        prev_mem_base_ = 0;
        refresh_memory_locked_worker();
    });
}

void DebuggerEngine::configure_struct_table(const std::string& expr) {
    enqueue([this, expr] {
        table_expr_ = expr;
        prev_table_rows_.clear();
        refresh_struct_table_worker();
    });
}

void DebuggerEngine::configure_waveform_probe(const std::string& expr, uint32_t max_samples) {
    enqueue([this, expr, max_samples] {
        waveform_expr_ = expr;
        waveform_max_ = std::clamp<uint32_t>(max_samples, 8, 4096);
        refresh_waveform_worker();
    });
}

void DebuggerEngine::configure_pixel_probe(const std::string& expr, int width, int height, int channels) {
    enqueue([this, expr, width, height, channels] {
        pixel_expr_ = expr;
        pixel_w_ = std::clamp(width, 1, 1024);
        pixel_h_ = std::clamp(height, 1, 1024);
        pixel_ch_ = (channels == 1 || channels == 3 || channels == 4) ? channels : 4;
        refresh_pixel_probe_worker();
    });
}

void DebuggerEngine::configure_pointer_graph(const std::string& expr, uint32_t max_nodes) {
    enqueue([this, expr, max_nodes] {
        graph_expr_ = expr;
        graph_max_nodes_ = std::clamp<uint32_t>(max_nodes, 2, 128);
        refresh_pointer_graph_worker();
    });
}

void DebuggerEngine::configure_ring_probe(const std::string& buf_expr,
                                          const std::string& cap_expr,
                                          const std::string& head_expr,
                                          const std::string& tail_expr) {
    enqueue([this, buf_expr, cap_expr, head_expr, tail_expr] {
        ring_buf_expr_ = buf_expr;
        ring_cap_expr_ = cap_expr;
        ring_head_expr_ = head_expr;
        ring_tail_expr_ = tail_expr;
        refresh_ring_probe_worker();
    });
}

void DebuggerEngine::configure_orderbook_probe(const std::string& bids_expr,
                                               const std::string& asks_expr,
                                               double price_scale) {
    enqueue([this, bids_expr, asks_expr, price_scale] {
        book_bids_expr_ = bids_expr;
        book_asks_expr_ = asks_expr;
        book_price_scale_ = (price_scale > 0.0) ? price_scale : 1.0;
        refresh_orderbook_probe_worker();
    });
}

void DebuggerEngine::do_launch(const std::string& exe_path,
                               const std::vector<std::string>& args,
                               const std::string& cwd,
                               bool stop_at_main) {
    if (process_.IsValid()) {
        auto st = process_.GetState();
        if (st == lldb::eStateRunning || st == lldb::eStateStopped) {
            process_.Kill();
        }
    }

    last_exe_ = exe_path;
    last_args_ = args;
    last_cwd_ = cwd;
    last_stop_at_main_ = stop_at_main;
    is_core_dump_ = false;
    is_remote_ = false;
    selected_frame_idx_ = 0;
    step_counter_ = 0;
    bp_last_hit_.clear();
    page_prov_cache_.clear();
    prev_local_values_.clear();
    prev_watch_values_.clear();
    prev_reg_values_.clear();
    prev_mem_bytes_.clear();
    prev_table_rows_.clear();
    wp_labels_.clear();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        history_.clear();
    }

    if (target_.IsValid()) {
        debugger_.DeleteTarget(target_);
    }

    lldb::SBError err;
    target_ = debugger_.CreateTarget(exe_path.c_str(), nullptr, nullptr, true, err);
    if (!target_.IsValid() || err.Fail()) {
        append_console(ConsoleLineKind::Stderr,
                       "Failed to create target '" + exe_path + "': " + safe_str(err.GetCString()));
        publish_snapshot(SessionState::Idle, false);
        return;
    }

    apply_pending_breakpoints();

    if (stop_at_main) {
        lldb::SBBreakpoint main_bp = target_.BreakpointCreateByName("main");
        if (main_bp.IsValid()) {
            main_bp.SetOneShot(true);
        }
    }

    std::vector<const char*> c_args;
    c_args.reserve(args.size() + 1);
    for (const auto& a : args) c_args.push_back(a.c_str());
    c_args.push_back(nullptr);

    lldb::SBLaunchInfo launch_info(c_args.data());
    if (!cwd.empty()) {
        launch_info.SetWorkingDirectory(cwd.c_str());
    }
    launch_info.SetLaunchFlags(lldb::eLaunchFlagNone);

    process_ = target_.Launch(launch_info, err);
    if (!process_.IsValid() || err.Fail()) {
        append_console(ConsoleLineKind::Stderr,
                       "Launch failed: " + safe_str(err.GetCString()));
        publish_snapshot(SessionState::Idle, false);
        return;
    }

    append_console(ConsoleLineKind::System,
                   "Launched " + exe_path + " (PID " + std::to_string(process_.GetProcessID()) + ")");
    publish_snapshot(SessionState::Running, false);
}

void DebuggerEngine::do_attach(uint64_t pid) {
    if (process_.IsValid()) {
        auto st = process_.GetState();
        if (st == lldb::eStateRunning || st == lldb::eStateStopped) {
            process_.Detach();
        }
    }

    lldb::SBError err;
    target_ = debugger_.CreateTarget("");
    lldb::SBAttachInfo attach_info(static_cast<lldb::pid_t>(pid));
    process_ = target_.Attach(attach_info, err);
    if (!process_.IsValid() || err.Fail()) {
        append_console(ConsoleLineKind::Stderr,
                       "Attach to PID " + std::to_string(pid) + " failed: " + safe_str(err.GetCString()));
        return;
    }
    is_core_dump_ = false;
    is_remote_ = false;
    append_console(ConsoleLineKind::System, "Attached to PID " + std::to_string(pid));
    publish_snapshot(SessionState::Stopped, true);
}

void DebuggerEngine::do_open_core(const std::string& exe_path, const std::string& core_path) {
    if (process_.IsValid()) {
        auto st = process_.GetState();
        if (st == lldb::eStateRunning || st == lldb::eStateStopped) {
            process_.Kill();
        }
    }
    if (target_.IsValid()) {
        debugger_.DeleteTarget(target_);
    }

    last_exe_ = exe_path;
    is_core_dump_ = true;
    is_remote_ = false;
    selected_frame_idx_ = 0;
    step_counter_ = 0;
    page_prov_cache_.clear();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        history_.clear();
    }

    lldb::SBError err;
    target_ = debugger_.CreateTarget(exe_path.c_str(), nullptr, nullptr, true, err);
    if (!target_.IsValid() || err.Fail()) {
        append_console(ConsoleLineKind::Stderr,
                       "Core target creation failed: " + safe_str(err.GetCString()));
        return;
    }

    process_ = target_.LoadCore(core_path.c_str(), err);
    if (!process_.IsValid() || err.Fail()) {
        append_console(ConsoleLineKind::Stderr,
                       "LoadCore failed ('" + core_path + "'): " + safe_str(err.GetCString()));
        return;
    }

    append_console(ConsoleLineKind::System,
                   "Loaded CORE DUMP " + core_path + " for " + exe_path +
                   " (" + std::to_string(process_.GetNumThreads()) + " threads)");
    publish_snapshot(SessionState::Stopped, true);
}

void DebuggerEngine::do_connect_remote(const std::string& exe_path, const std::string& remote_url) {
    if (process_.IsValid()) {
        auto st = process_.GetState();
        if (st == lldb::eStateRunning || st == lldb::eStateStopped) {
            process_.Detach();
        }
    }
    if (target_.IsValid()) {
        debugger_.DeleteTarget(target_);
    }

    last_exe_ = exe_path;
    is_core_dump_ = false;
    is_remote_ = true;
    selected_frame_idx_ = 0;
    step_counter_ = 0;
    page_prov_cache_.clear();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        history_.clear();
    }

    lldb::SBError err;
    target_ = debugger_.CreateTarget(exe_path.empty() ? "" : exe_path.c_str(),
                                     nullptr, nullptr, true, err);
    if (!target_.IsValid() || err.Fail()) {
        append_console(ConsoleLineKind::Stderr,
                       "Remote target creation failed: " + safe_str(err.GetCString()));
        return;
    }

    apply_pending_breakpoints();
    process_ = target_.ConnectRemote(listener_, remote_url.c_str(), "gdb-remote", err);
    if (!process_.IsValid() || err.Fail()) {
        append_console(ConsoleLineKind::Stderr,
                       "ConnectRemote failed ('" + remote_url + "'): " + safe_str(err.GetCString()));
        is_remote_ = false;
        return;
    }

    append_console(ConsoleLineKind::System,
                   "Connected remote gdb-remote/rr at " + remote_url +
                   " (PID " + std::to_string(process_.GetProcessID()) + ")");
    publish_snapshot(SessionState::Stopped, true);
}

void DebuggerEngine::apply_pending_breakpoints() {
    if (!target_.IsValid()) return;
    bp_log_messages_.clear();
    for (const auto& pb : pending_bps_) {
        std::string base = std::filesystem::path(pb.file).filename().string();
        lldb::SBBreakpoint bp = target_.BreakpointCreateByLocation(pb.file.c_str(), pb.line);
        if (!bp.IsValid() || bp.GetNumLocations() == 0) {
            target_.BreakpointDelete(bp.GetID());
            bp = target_.BreakpointCreateByLocation(base.c_str(), pb.line);
        }
        if (bp.IsValid()) {
            bp.SetEnabled(pb.enabled);
            if (!pb.condition.empty()) bp.SetCondition(pb.condition.c_str());
            if (!pb.log_message.empty()) {
                bp_log_messages_[bp.GetID()] = pb.log_message;
            }
        }
    }
}

void DebuggerEngine::poll_lldb_events() {
    if (!listener_.IsValid()) return;
    lldb::SBEvent event;
    while (listener_.WaitForEvent(0, event)) {
        if (!event.IsValid()) break;

        if (lldb::SBProcess::EventIsProcessEvent(event)) {
            uint32_t event_type = event.GetType();
            if (event_type & lldb::SBProcess::eBroadcastBitStateChanged) {
                lldb::StateType st = lldb::SBProcess::GetStateFromEvent(event);
                if (st == lldb::eStateStopped) {
                    if (lldb::SBProcess::GetRestartedFromEvent(event)) {
                        continue;
                    }
                    drain_process_io();

                    // Select the first thread that has a non-none stop reason
                    uint32_t num_threads = process_.GetNumThreads();
                    for (uint32_t i = 0; i < num_threads; ++i) {
                        lldb::SBThread th = process_.GetThreadAtIndex(i);
                        auto r = th.GetStopReason();
                        if (r != lldb::eStopReasonNone && r != lldb::eStopReasonInvalid) {
                            process_.SetSelectedThread(th);
                            break;
                        }
                    }

                    // Check if stopped at a logpoint -> safely evaluate {expr} + Δt while stopped, then auto-continue!
                    lldb::SBThread t = process_.GetSelectedThread();
                    if (t.IsValid() && t.GetStopReason() == lldb::eStopReasonBreakpoint) {
                        int32_t bp_id = static_cast<int32_t>(t.GetStopReasonDataAtIndex(0));
                        auto it = bp_log_messages_.find(bp_id);
                        if (it != bp_log_messages_.end()) {
                            std::string msg = interpolate_logpoint_msg(
                                bp_id, it->second, t.GetFrameAtIndex(0));
                            append_console(ConsoleLineKind::System, msg);
                            process_.Continue();
                            continue;
                        }
                    }

                    selected_frame_idx_ = 0;
                    publish_snapshot(SessionState::Stopped, true);
                } else if (st == lldb::eStateRunning) {
                    publish_snapshot(SessionState::Running, false);
                } else if (st == lldb::eStateExited) {
                    drain_process_io();
                    int code = process_.GetExitStatus();
                    append_console(ConsoleLineKind::System,
                                   "Process exited with code " + std::to_string(code));
                    publish_snapshot(SessionState::Exited, false);
                } else if (st == lldb::eStateCrashed) {
                    drain_process_io();
                    publish_snapshot(SessionState::Fault, true);
                }
            }
            if (event_type & (lldb::SBProcess::eBroadcastBitSTDOUT |
                              lldb::SBProcess::eBroadcastBitSTDERR)) {
                drain_process_io();
            }
        }
    }
}

void DebuggerEngine::drain_process_io() {
    if (!process_.IsValid()) return;
    char buf[4096];
    size_t n = 0;
    while ((n = process_.GetSTDOUT(buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        append_console(ConsoleLineKind::Stdout, std::string(buf, n));
    }
    while ((n = process_.GetSTDERR(buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        append_console(ConsoleLineKind::Stderr, std::string(buf, n));
    }
}

std::string DebuggerEngine::interpolate_logpoint_msg(int32_t bp_id,
                                                     const std::string& tmpl,
                                                     lldb::SBFrame frame) {
    auto now = std::chrono::steady_clock::now();
    std::string dt_str;
    if (auto it = bp_last_hit_.find(bp_id); it != bp_last_hit_.end()) {
        double us = std::chrono::duration<double, std::micro>(now - it->second).count();
        char dbuf[48];
        if (us < 1000.0) std::snprintf(dbuf, sizeof(dbuf), " +%.1fus", us);
        else std::snprintf(dbuf, sizeof(dbuf), " +%.2fms", us / 1000.0);
        dt_str = dbuf;
    }
    bp_last_hit_[bp_id] = now;

    std::string rendered;
    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i + 1);
            if (close != std::string::npos && close > i + 1) {
                std::string expr = tmpl.substr(i + 1, close - i - 1);
                if (frame.IsValid()) {
                    lldb::SBValue v = frame.GetValueForVariablePath(expr.c_str());
                    if (!v.IsValid() || v.GetError().Fail()) {
                        v = frame.EvaluateExpression(expr.c_str());
                    }
                    if (v.IsValid() && !v.GetError().Fail()) {
                        rendered += format_value_string(v);
                    } else {
                        rendered += "<err:" + expr + ">";
                    }
                }
                i = close + 1;
                continue;
            }
        }
        rendered.push_back(tmpl[i++]);
    }
    return "[LOGPOINT #" + std::to_string(bp_id) + dt_str + "] " + rendered;
}

ProvenanceKind DebuggerEngine::classify_address_lazy(uint64_t addr, uint64_t sp) {
    if (addr == 0 || addr < 0x1000) return ProvenanceKind::Null;

    if (sp != 0) {
        uint64_t diff = (addr > sp) ? (addr - sp) : (sp - addr);
        if (diff < 8ull * 1024ull * 1024ull) {
            return ProvenanceKind::Stack;
        }
    }

    uint64_t page = addr & ~0xFFFull;
    if (auto it = page_prov_cache_.find(page); it != page_prov_cache_.end()) {
        return it->second;
    }

    ProvenanceKind kind = ProvenanceKind::Unknown;
    if (process_.IsValid()) {
        lldb::SBMemoryRegionInfo info;
        lldb::SBError err = process_.GetMemoryRegionInfo(addr, info);
        if (err.Success()) {
            bool r = info.IsReadable();
            bool w = info.IsWritable();
            bool x = info.IsExecutable();
            std::string name = safe_str(info.GetName());
            if (!r && !w && !x) {
                kind = ProvenanceKind::Unmapped;
            } else if (x) {
                kind = ProvenanceKind::Text;
            } else if (r && !w) {
                kind = ProvenanceKind::Rodata;
            } else if (name.find("stack") != std::string::npos ||
                       name.find("STACK") != std::string::npos) {
                kind = ProvenanceKind::Stack;
            } else if (name.find("MALLOC") != std::string::npos ||
                       name.find("heap") != std::string::npos ||
                       name.empty()) {
                kind = ProvenanceKind::Heap;
            } else {
                kind = ProvenanceKind::Data;
            }
        }
    }
    page_prov_cache_[page] = kind;
    return kind;
}

VarNode DebuggerEngine::extract_var(lldb::SBValue val,
                                    const std::string& expr_path,
                                    int depth,
                                    uint64_t sp,
                                    const std::unordered_map<std::string, std::string>& prev_values,
                                    std::vector<StepDeltaEntry>* deltas,
                                    const char* delta_category) {
    VarNode node;
    node.name = safe_str(val.GetName());
    node.expr_path = expr_path.empty() ? node.name : expr_path;
    node.raw_type = safe_str(val.GetTypeName());
    node.display_type = simplify_cpp_type(node.raw_type);
    node.value = format_value_string(val);
    node.byte_size = val.GetByteSize();
    node.is_synthetic = val.IsSynthetic();

    lldb::addr_t load_addr = val.GetLoadAddress();
    if (load_addr != LLDB_INVALID_ADDRESS) {
        node.address = load_addr;
    }

    lldb::SBType ty = val.GetType();
    if (ty.IsValid() && (ty.IsPointerType() || ty.IsReferenceType())) {
        node.is_pointer = true;
        node.pointer_target = val.GetValueAsUnsigned(0);
        node.provenance = classify_address_lazy(node.pointer_target, sp);
    }

    auto prev_it = prev_values.find(node.expr_path);
    if (prev_it != prev_values.end() && prev_it->second != node.value) {
        node.changed = true;
        node.prev_value = prev_it->second;
        if (deltas && delta_category && depth <= 1) {
            deltas->push_back(StepDeltaEntry{
                delta_category,
                node.expr_path,
                prev_it->second,
                node.value,
            });
        }
    }

    uint32_t num_children = val.GetNumChildren(64);
    node.num_children = num_children;

    if (depth < 2 && num_children > 0) {
        // Avoid dereferencing null or unmapped pointers
        bool skip_children = node.is_pointer &&
                             (node.provenance == ProvenanceKind::Null ||
                              node.provenance == ProvenanceKind::Unmapped);
        if (!skip_children) {
            uint32_t limit = std::min<uint32_t>(num_children, 24);
            node.children.reserve(limit);
            for (uint32_t i = 0; i < limit; ++i) {
                lldb::SBValue child = val.GetChildAtIndex(i);
                if (!child.IsValid()) continue;
                std::string cname = safe_str(child.GetName());
                std::string cpath;
                if (!cname.empty() && cname[0] == '[') {
                    cpath = node.expr_path + cname;
                } else if (node.is_pointer) {
                    cpath = "(" + node.expr_path + ")->" + cname;
                } else {
                    cpath = node.expr_path + "." + cname;
                }
                node.children.push_back(extract_var(child, cpath, depth + 1, sp,
                                                    prev_values, deltas, delta_category));
            }
        }
    }
    return node;
}

std::vector<DisasmInstruction> DebuggerEngine::extract_disassembly(lldb::SBFrame frame) {
    std::vector<DisasmInstruction> out;
    if (!frame.IsValid() || !target_.IsValid()) return out;

    lldb::SBInstructionList insts;
    lldb::SBFunction fn = frame.GetFunction();
    if (fn.IsValid()) {
        insts = fn.GetInstructions(target_);
    }
    if (!insts.IsValid() || insts.GetSize() == 0) {
        insts = target_.ReadInstructions(frame.GetPCAddress(), 48);
    }
    if (!insts.IsValid()) return out;

    uint32_t count = std::min<uint32_t>(insts.GetSize(), 256);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        lldb::SBInstruction inst = insts.GetInstructionAtIndex(i);
        if (!inst.IsValid()) continue;
        DisasmInstruction di;
        lldb::SBAddress addr = inst.GetAddress();
        di.address = addr.GetLoadAddress(target_);
        di.mnemonic = safe_str(inst.GetMnemonic(target_));
        di.operands = safe_str(inst.GetOperands(target_));
        di.comment = safe_str(inst.GetComment(target_));
        di.is_branch = inst.DoesBranch();
        if (di.is_branch) {
            bool ok = false;
            di.branch_target = parse_hex_or_dec(di.operands, ok);
            if (!ok) {
                // Handle operands like "b.eq 0x100003f20" or "cbz w8, 0x100003f20"
                auto pos = di.operands.find("0x");
                if (pos != std::string::npos) {
                    di.branch_target = parse_hex_or_dec(di.operands.substr(pos), ok);
                }
            }
        }
        lldb::SBLineEntry le = addr.GetLineEntry();
        if (le.IsValid()) {
            char path_buf[1024];
            if (le.GetFileSpec().GetPath(path_buf, sizeof(path_buf)) > 0) {
                di.file = path_buf;
            }
            di.line = le.GetLine();
        }
        out.push_back(std::move(di));
    }
    return out;
}

CrashInfo DebuggerEngine::extract_crash_info(lldb::SBThread thread,
                                             const std::vector<FrameInfo>& frames,
                                             uint64_t sp) {
    CrashInfo ci;
    if (!thread.IsValid()) return ci;

    char desc_buf[512];
    thread.GetStopDescription(desc_buf, sizeof(desc_buf));
    std::string desc = desc_buf;
    lldb::StopReason reason = thread.GetStopReason();
    int32_t signo = (reason == lldb::eStopReasonSignal && thread.GetStopReasonDataCount() > 0)
                        ? static_cast<int32_t>(thread.GetStopReasonDataAtIndex(0))
                        : 0;

    if (!is_crash_stop(reason, signo, desc)) return ci;

    ci.is_crash = true;
    ci.summary = desc;
    if (reason == lldb::eStopReasonSignal) {
        ci.signal_number = signo;
        if (signo == SIGSEGV || desc.find("SIGSEGV") != std::string::npos) ci.signal_name = "SIGSEGV";
        else if (signo == SIGBUS || desc.find("SIGBUS") != std::string::npos) ci.signal_name = "SIGBUS";
        else if (signo == SIGABRT || desc.find("SIGABRT") != std::string::npos) ci.signal_name = "SIGABRT";
        else if (signo == SIGILL || desc.find("SIGILL") != std::string::npos) ci.signal_name = "SIGILL";
        else if (signo == SIGFPE || desc.find("SIGFPE") != std::string::npos) ci.signal_name = "SIGFPE";
        else ci.signal_name = "SIGNAL " + std::to_string(ci.signal_number);
    } else {
        ci.signal_name = "EXC_BAD_ACCESS";
        if (desc.find("EXC_BAD_INSTRUCTION") != std::string::npos) ci.signal_name = "EXC_BAD_INSTRUCTION";
        else if (desc.find("EXC_ARITHMETIC") != std::string::npos) ci.signal_name = "EXC_ARITHMETIC";
        auto pos = desc.find("address=");
        if (pos != std::string::npos) {
            size_t start = pos + 8;
            size_t end = start;
            while (end < desc.size() &&
                   (std::isxdigit(static_cast<unsigned char>(desc[end])) ||
                    desc[end] == 'x' || desc[end] == 'X')) {
                ++end;
            }
            bool ok = false;
            ci.fault_address = parse_hex_or_dec(desc.substr(start, end - start), ok);
        } else if (thread.GetStopReasonDataCount() >= 2) {
            ci.fault_address = thread.GetStopReasonDataAtIndex(1);
        }
    }

    ci.fault_provenance = classify_address_lazy(ci.fault_address, sp);

    // Offending instruction
    lldb::SBFrame top = thread.GetFrameAtIndex(0);
    if (top.IsValid() && target_.IsValid()) {
        lldb::SBInstructionList il = target_.ReadInstructions(top.GetPCAddress(), 1);
        if (il.IsValid() && il.GetSize() > 0) {
            lldb::SBInstruction inst = il.GetInstructionAtIndex(0);
            ci.offending_instruction = safe_str(inst.GetMnemonic(target_)) + " " +
                                       safe_str(inst.GetOperands(target_));
        }
    }

    // Nearest user frame
    ci.user_frame_index = 0;
    for (const auto& f : frames) {
        if (!f.is_runtime && !f.file.empty()) {
            ci.user_frame_index = f.index;
            break;
        }
    }
    return ci;
}

void DebuggerEngine::publish_snapshot(SessionState forced_state, bool record_history) {
    auto snap = std::make_shared<StepSnapshot>();
    snap->target_path = last_exe_;
    snap->state = forced_state;
    snap->is_core_dump = is_core_dump_;
    snap->is_remote = is_remote_;

    if (process_.IsValid()) {
        snap->pid = process_.GetProcessID();
        if (forced_state == SessionState::Exited) {
            snap->exit_code = process_.GetExitStatus();
        }
    }

    // Breakpoints
    if (target_.IsValid()) {
        uint32_t nb = target_.GetNumBreakpoints();
        for (uint32_t i = 0; i < nb; ++i) {
            lldb::SBBreakpoint bp = target_.GetBreakpointAtIndex(i);
            if (!bp.IsValid() || bp.IsOneShot()) continue;
            BreakpointInfo bi;
            bi.id = bp.GetID();
            bi.enabled = bp.IsEnabled();
            bi.hit_count = bp.GetHitCount();
            bi.condition = safe_str(bp.GetCondition());
            if (auto it = bp_log_messages_.find(bi.id); it != bp_log_messages_.end()) {
                bi.log_message = it->second;
            }
            if (bp.GetNumLocations() > 0) {
                bi.resolved = bp.GetNumResolvedLocations() > 0;
                lldb::SBLineEntry le = bp.GetLocationAtIndex(0).GetAddress().GetLineEntry();
                if (le.IsValid()) {
                    char path_buf[1024];
                    if (le.GetFileSpec().GetPath(path_buf, sizeof(path_buf)) > 0) {
                        bi.file = path_buf;
                    }
                    bi.line = le.GetLine();
                }
            }
            snap->breakpoints.push_back(std::move(bi));
        }

        // Hardware watchpoints
        uint32_t nw = target_.GetNumWatchpoints();
        for (uint32_t i = 0; i < nw; ++i) {
            lldb::SBWatchpoint wp = target_.GetWatchpointAtIndex(i);
            if (!wp.IsValid()) continue;
            WatchpointInfo wi;
            wi.id = wp.GetID();
            wi.address = wp.GetWatchAddress();
            wi.byte_size = wp.GetWatchSize();
            wi.hit_count = wp.GetHitCount();
            wi.enabled = wp.IsEnabled();
            wi.condition = safe_str(wp.GetCondition());
            wi.watch_write = true;
            if (auto it = wp_labels_.find(wi.id); it != wp_labels_.end()) {
                wi.label = it->second;
            } else {
                wi.label = format_hex64_grouped(wi.address);
            }
            snap->watchpoints.push_back(std::move(wi));
        }
    } else {
        int32_t fake_id = 1;
        for (const auto& pb : pending_bps_) {
            BreakpointInfo bi;
            bi.id = fake_id++;
            bi.file = pb.file;
            bi.line = pb.line;
            bi.condition = pb.condition;
            bi.log_message = pb.log_message;
            bi.enabled = pb.enabled;
            snap->breakpoints.push_back(std::move(bi));
        }
    }

    if (process_.IsValid() &&
        (forced_state == SessionState::Stopped || forced_state == SessionState::Fault)) {
        if (record_history) {
            ++step_counter_;
        }
        snap->step_index = step_counter_;
        // Reset per-stop lazy provenance page cache (O(unique pointer pages) instead of scanning all 4096 VM regions!)
        page_prov_cache_.clear();

        lldb::SBThread sel_thread = process_.GetSelectedThread();
        if (!sel_thread.IsValid() && process_.GetNumThreads() > 0) {
            sel_thread = process_.GetThreadAtIndex(0);
            process_.SetSelectedThread(sel_thread);
        }

        uint64_t active_sp = 0;
        if (sel_thread.IsValid()) {
            snap->selected_thread_id = sel_thread.GetThreadID();
            lldb::SBFrame top = sel_thread.GetFrameAtIndex(0);
            if (top.IsValid()) active_sp = top.GetSP();
        }

        // Extract threads & frames (lazy background thread unwinding for 50+ thread HFT binaries!)
        const bool full_all_threads = expand_all_threads_.load();
        uint32_t nt = process_.GetNumThreads();
        for (uint32_t ti = 0; ti < nt; ++ti) {
            lldb::SBThread th = process_.GetThreadAtIndex(ti);
            if (!th.IsValid()) continue;
            ThreadInfo tinfo;
            tinfo.id = th.GetThreadID();
            tinfo.index_id = th.GetIndexID();
            tinfo.name = safe_str(th.GetName());
            if (tinfo.name.empty()) {
                tinfo.name = "Thread #" + std::to_string(tinfo.index_id);
            }
            tinfo.stop_reason = stop_reason_name(th.GetStopReason());
            char desc_buf[256];
            th.GetStopDescription(desc_buf, sizeof(desc_buf));
            tinfo.stop_description = desc_buf;
            tinfo.is_selected = (tinfo.id == snap->selected_thread_id);
            tinfo.is_stopped = true;

            uint32_t max_frames = (tinfo.is_selected || full_all_threads) ? 64u : 4u;
            uint32_t nf = std::min<uint32_t>(th.GetNumFrames(), max_frames);
            for (uint32_t fi = 0; fi < nf; ++fi) {
                lldb::SBFrame fr = th.GetFrameAtIndex(fi);
                if (!fr.IsValid()) continue;
                FrameInfo finfo;
                finfo.index = fi;
                finfo.function = safe_str(fr.GetDisplayFunctionName());
                if (finfo.function.empty()) finfo.function = safe_str(fr.GetFunctionName());
                finfo.display_function = simplify_function_name(finfo.function);
                finfo.pc = fr.GetPC();
                finfo.is_inlined = fr.IsInlined();
                lldb::SBModule mod = fr.GetModule();
                if (mod.IsValid()) {
                    finfo.module = safe_str(mod.GetFileSpec().GetFilename());
                }
                lldb::SBLineEntry le = fr.GetLineEntry();
                if (le.IsValid()) {
                    char path_buf[1024];
                    if (le.GetFileSpec().GetPath(path_buf, sizeof(path_buf)) > 0) {
                        finfo.file = path_buf;
                    }
                    finfo.line = le.GetLine();
                }
                finfo.is_runtime = is_runtime_frame(finfo.function, finfo.module, finfo.file);
                if (tinfo.top_user_function.empty() && !finfo.is_runtime) {
                    tinfo.top_user_function = finfo.display_function;
                }
                tinfo.frames.push_back(std::move(finfo));
            }
            if (tinfo.top_user_function.empty() && !tinfo.frames.empty()) {
                tinfo.top_user_function = tinfo.frames[0].display_function;
            }
            snap->threads.push_back(std::move(tinfo));
        }

        // Selected thread + frame inspection
        if (sel_thread.IsValid()) {
            const ThreadInfo* sel_tinfo = nullptr;
            for (const auto& t : snap->threads) {
                if (t.id == snap->selected_thread_id) {
                    sel_tinfo = &t;
                    break;
                }
            }
            if (sel_tinfo) {
                snap->stop_reason = sel_tinfo->stop_description.empty()
                                        ? sel_tinfo->stop_reason
                                        : sel_tinfo->stop_description;
                snap->crash = extract_crash_info(sel_thread, sel_tinfo->frames, active_sp);
                if (snap->crash.is_crash) {
                    snap->state = SessionState::Fault;
                }
            }

            if (selected_frame_idx_ >= sel_thread.GetNumFrames()) {
                selected_frame_idx_ = 0;
            }
            snap->selected_frame_idx = selected_frame_idx_;

            lldb::SBFrame frame = sel_thread.GetFrameAtIndex(selected_frame_idx_);
            if (frame.IsValid()) {
                snap->active_pc = frame.GetPC();
                lldb::SBLineEntry le = frame.GetLineEntry();
                if (le.IsValid()) {
                    char path_buf[1024];
                    if (le.GetFileSpec().GetPath(path_buf, sizeof(path_buf)) > 0) {
                        snap->active_file = path_buf;
                    }
                    snap->active_line = le.GetLine();
                }

                // Locals & arguments
                std::unordered_map<std::string, std::string> next_locals;
                lldb::SBValueList vars = frame.GetVariables(true, true, false, true);
                uint32_t nv = std::min<uint32_t>(vars.GetSize(), 128);
                for (uint32_t i = 0; i < nv; ++i) {
                    lldb::SBValue v = vars.GetValueAtIndex(i);
                    if (!v.IsValid()) continue;
                    std::string vname = safe_str(v.GetName());
                    VarNode vn = extract_var(v, vname, 0, active_sp,
                                             prev_local_values_,
                                             record_history ? &snap->deltas : nullptr,
                                             "VAR");
                    next_locals[vn.expr_path] = vn.value;
                    for (const auto& c : vn.children) {
                        next_locals[c.expr_path] = c.value;
                    }
                    snap->locals.push_back(std::move(vn));
                }
                if (record_history) {
                    prev_local_values_ = std::move(next_locals);
                }

                // Watch expressions
                std::unordered_map<std::string, std::string> next_watches;
                for (const auto& expr : watch_exprs_) {
                    lldb::SBValue v = frame.GetValueForVariablePath(expr.c_str());
                    if (!v.IsValid() || v.GetError().Fail()) {
                        v = frame.EvaluateExpression(expr.c_str());
                    }
                    if (v.IsValid() && !v.GetError().Fail()) {
                        VarNode vn = extract_var(v, expr, 0, active_sp,
                                                 prev_watch_values_,
                                                 record_history ? &snap->deltas : nullptr,
                                                 "WATCH");
                        vn.name = expr;
                        next_watches[expr] = vn.value;
                        snap->watches.push_back(std::move(vn));
                    } else {
                        VarNode err_node;
                        err_node.name = expr;
                        err_node.expr_path = expr;
                        err_node.value = "<eval error>";
                        snap->watches.push_back(std::move(err_node));
                    }
                }
                if (record_history) {
                    prev_watch_values_ = std::move(next_watches);
                }

                // CPU Registers (including SIMD raw bytes for NEON/XMM/YMM/ZMM lane decoding!)
                std::unordered_map<std::string, uint64_t> next_regs;
                lldb::SBValueList reg_sets = frame.GetRegisters();
                uint32_t nsets = reg_sets.GetSize();
                for (uint32_t si = 0; si < nsets; ++si) {
                    lldb::SBValue set = reg_sets.GetValueAtIndex(si);
                    std::string group_name = safe_str(set.GetName());
                    uint32_t nr = set.GetNumChildren();
                    for (uint32_t ri = 0; ri < nr; ++ri) {
                        lldb::SBValue reg = set.GetChildAtIndex(ri);
                        if (!reg.IsValid()) continue;
                        RegisterInfo rinfo;
                        rinfo.name = safe_str(reg.GetName());
                        rinfo.value_hex = safe_str(reg.GetValue());
                        rinfo.value_u64 = reg.GetValueAsUnsigned(0);
                        rinfo.byte_size = static_cast<uint32_t>(reg.GetByteSize());
                        rinfo.group = group_name;

                        if (rinfo.byte_size == 16 || rinfo.byte_size == 32 || rinfo.byte_size == 64) {
                            lldb::SBData data = reg.GetData();
                            if (data.IsValid() && data.GetByteSize() >= rinfo.byte_size) {
                                rinfo.raw_bytes.resize(rinfo.byte_size);
                                lldb::SBError derr;
                                data.ReadRawData(derr, 0, rinfo.raw_bytes.data(), rinfo.byte_size);
                            }
                        }

                        if (auto it = prev_reg_values_.find(rinfo.name);
                            it != prev_reg_values_.end() && it->second != rinfo.value_u64) {
                            rinfo.changed = true;
                            rinfo.prev_value_u64 = it->second;
                            if (record_history && group_name.find("General") != std::string::npos &&
                                rinfo.name != "pc") {
                                snap->deltas.push_back(StepDeltaEntry{
                                    "REG",
                                    rinfo.name,
                                    format_hex64_grouped(it->second),
                                    format_hex64_grouped(rinfo.value_u64),
                                });
                            }
                        }
                        next_regs[rinfo.name] = rinfo.value_u64;
                        snap->registers.push_back(std::move(rinfo));
                    }
                }
                if (record_history) {
                    prev_reg_values_ = std::move(next_regs);
                }

                // Disassembly
                snap->disassembly = extract_disassembly(frame);
            }
        }
    }

    // Auto-refresh active memory & data probes BEFORE publishing snapshot so UI sees atomic consistency
    if (process_.IsValid() &&
        (snap->state == SessionState::Stopped || snap->state == SessionState::Fault)) {
        if (!mem_expr_.empty()) refresh_memory_locked_worker();
        if (!table_expr_.empty()) refresh_struct_table_worker();
        if (!waveform_expr_.empty()) refresh_waveform_worker();
        if (!pixel_expr_.empty()) refresh_pixel_probe_worker();
        if (!graph_expr_.empty()) refresh_pointer_graph_worker();
        if (!ring_buf_expr_.empty()) refresh_ring_probe_worker();
        if (!book_bids_expr_.empty() || !book_asks_expr_.empty()) refresh_orderbook_probe_worker();
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        latest_snapshot_ = snap;
        if (record_history) {
            history_.push_back(snap);
            if (history_.size() > 256) {
                history_.erase(history_.begin());
            }
        }
    }
}

void DebuggerEngine::refresh_memory_locked_worker() {
    MemorySnapshot ms;
    ms.label = mem_expr_;
    if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped) {
        ms.error = "Target not stopped";
        std::lock_guard<std::mutex> lock(state_mutex_);
        memory_snapshot_ = std::move(ms);
        return;
    }

    uint64_t addr = 0;
    bool parsed_num = false;
    addr = parse_hex_or_dec(mem_expr_, parsed_num);

    lldb::SBType struct_type;
    if (!parsed_num) {
        lldb::SBFrame f = process_.GetSelectedThread().GetFrameAtIndex(selected_frame_idx_);
        if (f.IsValid()) {
            lldb::SBValue v = f.GetValueForVariablePath(mem_expr_.c_str());
            if (!v.IsValid() || v.GetError().Fail()) {
                v = f.EvaluateExpression(mem_expr_.c_str());
            }
            if (v.IsValid() && !v.GetError().Fail()) {
                lldb::SBType ty = v.GetType();
                if (ty.IsValid() && ty.IsPointerType()) {
                    addr = v.GetValueAsUnsigned(0);
                    struct_type = ty.GetPointeeType().GetCanonicalType();
                } else {
                    addr = v.GetLoadAddress();
                    if (addr == LLDB_INVALID_ADDRESS) addr = v.GetValueAsUnsigned(0);
                    struct_type = ty.GetCanonicalType();
                }
            } else {
                ms.error = "Could not evaluate: " + mem_expr_;
            }
        }
    }

    if (addr == 0 || addr == LLDB_INVALID_ADDRESS) {
        if (ms.error.empty()) ms.error = "Null or invalid address";
        std::lock_guard<std::mutex> lock(state_mutex_);
        memory_snapshot_ = std::move(ms);
        return;
    }

    ms.base_address = addr;
    ms.bytes.resize(mem_size_);
    lldb::SBError err;
    size_t got = process_.ReadMemory(addr, ms.bytes.data(), mem_size_, err);
    if (got == 0 || err.Fail()) {
        ms.bytes.clear();
        ms.error = "ReadMemory failed: " + safe_str(err.GetCString());
    } else {
        ms.bytes.resize(got);
        ms.valid = true;
        ms.prev_base_address = prev_mem_base_;
        ms.prev_bytes = prev_mem_bytes_;
        prev_mem_base_ = addr;
        prev_mem_bytes_ = ms.bytes;

        // Extract struct field overlays, padding holes, and 64B cache-line false-sharing warnings!
        if (struct_type.IsValid() && struct_type.GetNumberOfFields() > 0) {
            ms.struct_type = simplify_cpp_type(safe_str(struct_type.GetName()));
            ms.struct_size = struct_type.GetByteSize();
            uint32_t nf = struct_type.GetNumberOfFields();
            for (uint32_t i = 0; i < nf && i < 64; ++i) {
                lldb::SBTypeMember m = struct_type.GetFieldAtIndex(i);
                if (!m.IsValid()) continue;
                StructFieldOverlay sfo;
                sfo.name = safe_str(m.GetName());
                std::string raw_ft = safe_str(m.GetType().GetName());
                sfo.type_name = simplify_cpp_type(raw_ft);
                sfo.offset = m.GetOffsetInBytes();
                sfo.size = m.GetType().GetByteSize();
                sfo.color_index = i;
                sfo.is_atomic = (raw_ft.find("atomic") != std::string::npos);
                ms.fields.push_back(std::move(sfo));
            }

            // Compute padding holes between fields and trailing padding
            std::vector<const StructFieldOverlay*> sorted;
            for (const auto& f : ms.fields) sorted.push_back(&f);
            std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
                return a->offset < b->offset;
            });
            uint64_t cursor = 0;
            for (const auto* f : sorted) {
                if (f->offset > cursor) {
                    uint64_t pad = f->offset - cursor;
                    ms.padding_ranges.push_back(PaddingRange{cursor, pad});
                    ms.padding_bytes += pad;
                }
                cursor = std::max(cursor, f->offset + f->size);
            }
            if (ms.struct_size > cursor) {
                uint64_t tail_pad = ms.struct_size - cursor;
                ms.padding_ranges.push_back(PaddingRange{cursor, tail_pad});
                ms.padding_bytes += tail_pad;
            }

            // Detect 64-byte cache-line false sharing (2+ fields on same CL with >=1 atomic)
            std::unordered_map<uint64_t, std::vector<const StructFieldOverlay*>> by_cl;
            for (const auto* f : sorted) {
                if (f->size == 0) continue;
                uint64_t cl0 = f->offset / 64;
                uint64_t cl1 = (f->offset + f->size - 1) / 64;
                for (uint64_t cl = cl0; cl <= cl1; ++cl) {
                    by_cl[cl].push_back(f);
                }
            }
            for (const auto& [cl, list] : by_cl) {
                int atomic_cnt = 0;
                for (const auto* f : list) {
                    if (f->is_atomic) ++atomic_cnt;
                }
                if (atomic_cnt >= 1 && list.size() >= 2) {
                    std::string warn = "CL#" + std::to_string(cl) + " shares ";
                    for (size_t k = 0; k < list.size(); ++k) {
                        if (k > 0) warn += " + ";
                        warn += list[k]->name;
                        if (list[k]->is_atomic) warn += "[atomic]";
                    }
                    ms.false_sharing_warnings.push_back(std::move(warn));
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    memory_snapshot_ = std::move(ms);
}

void DebuggerEngine::refresh_struct_table_worker() {
    StructTableData td;
    td.expr = table_expr_;
    if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped || table_expr_.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        struct_table_ = std::move(td);
        return;
    }

    lldb::SBFrame f = process_.GetSelectedThread().GetFrameAtIndex(selected_frame_idx_);
    if (!f.IsValid()) return;

    lldb::SBValue v = f.GetValueForVariablePath(table_expr_.c_str());
    if (!v.IsValid() || v.GetError().Fail()) {
        v = f.EvaluateExpression(table_expr_.c_str());
    }
    if (!v.IsValid() || v.GetError().Fail()) {
        td.error = "Cannot evaluate: " + table_expr_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        struct_table_ = std::move(td);
        return;
    }

    uint32_t count = v.GetNumChildren(256);
    td.total_count = count;
    if (count == 0) {
        td.error = "Container is empty or has no indexable children";
        std::lock_guard<std::mutex> lock(state_mutex_);
        struct_table_ = std::move(td);
        return;
    }

    lldb::SBValue first = v.GetChildAtIndex(0);
    td.element_type = simplify_cpp_type(safe_str(first.GetTypeName()));
    uint32_t nfields = first.GetNumChildren(32);

    td.columns.push_back("#");
    if (nfields > 0) {
        for (uint32_t c = 0; c < nfields; ++c) {
            td.columns.push_back(safe_str(first.GetChildAtIndex(c).GetName()));
        }
    } else {
        td.columns.push_back("value");
    }

    uint32_t limit = std::min<uint32_t>(count, 256);
    for (uint32_t r = 0; r < limit; ++r) {
        lldb::SBValue elem = v.GetChildAtIndex(r);
        std::vector<std::string> row;
        row.push_back(std::to_string(r));
        if (nfields > 0) {
            for (uint32_t c = 0; c < nfields; ++c) {
                lldb::SBValue fld = elem.GetChildAtIndex(c);
                row.push_back(format_value_string(fld));
            }
        } else {
            row.push_back(format_value_string(elem));
        }
        td.rows.push_back(std::move(row));
    }

    td.prev_rows = prev_table_rows_;
    prev_table_rows_ = td.rows;

    std::lock_guard<std::mutex> lock(state_mutex_);
    struct_table_ = std::move(td);
}

void DebuggerEngine::refresh_waveform_worker() {
    WaveformProbeData wp;
    wp.expr = waveform_expr_;
    if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped || waveform_expr_.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        waveform_probe_ = std::move(wp);
        return;
    }

    lldb::SBFrame f = process_.GetSelectedThread().GetFrameAtIndex(selected_frame_idx_);
    if (!f.IsValid()) return;

    lldb::SBValue v = f.GetValueForVariablePath(waveform_expr_.c_str());
    if (!v.IsValid() || v.GetError().Fail()) {
        v = f.EvaluateExpression(waveform_expr_.c_str());
    }
    if (!v.IsValid() || v.GetError().Fail()) {
        wp.error = "Cannot evaluate: " + waveform_expr_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        waveform_probe_ = std::move(wp);
        return;
    }

    uint32_t n = std::min<uint32_t>(v.GetNumChildren(waveform_max_), waveform_max_);
    if (n == 0) {
        wp.error = "No samples in " + waveform_expr_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        waveform_probe_ = std::move(wp);
        return;
    }

    wp.samples.reserve(n);
    // Fast path: contiguous float array via direct ReadMemory
    lldb::SBValue elem0 = v.GetChildAtIndex(0);
    lldb::addr_t base = elem0.GetLoadAddress();
    std::string etype = safe_str(elem0.GetTypeName());
    if (base != LLDB_INVALID_ADDRESS && etype == "float") {
        wp.samples.resize(n);
        lldb::SBError err;
        size_t got = process_.ReadMemory(base, wp.samples.data(), n * sizeof(float), err);
        wp.samples.resize(got / sizeof(float));
    } else if (base != LLDB_INVALID_ADDRESS && etype == "double") {
        std::vector<double> tmp(n);
        lldb::SBError err;
        size_t got = process_.ReadMemory(base, tmp.data(), n * sizeof(double), err);
        size_t cnt = got / sizeof(double);
        for (size_t i = 0; i < cnt; ++i) wp.samples.push_back(static_cast<float>(tmp[i]));
    } else {
        for (uint32_t i = 0; i < n; ++i) {
            lldb::SBValue c = v.GetChildAtIndex(i);
            const char* vs = c.GetValue();
            if (vs) {
                wp.samples.push_back(std::strtof(vs, nullptr));
            }
        }
    }

    if (!wp.samples.empty()) {
        auto [mn, mx] = std::minmax_element(wp.samples.begin(), wp.samples.end());
        wp.min_val = *mn;
        wp.max_val = *mx;
        double sum = std::accumulate(wp.samples.begin(), wp.samples.end(), 0.0);
        wp.avg_val = static_cast<float>(sum / static_cast<double>(wp.samples.size()));
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    waveform_probe_ = std::move(wp);
}

void DebuggerEngine::refresh_pixel_probe_worker() {
    PixelProbeData pp;
    pp.expr = pixel_expr_;
    pp.width = pixel_w_;
    pp.height = pixel_h_;
    pp.channels = pixel_ch_;

    if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped || pixel_expr_.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pixel_probe_ = std::move(pp);
        return;
    }

    bool ok_num = false;
    uint64_t addr = parse_hex_or_dec(pixel_expr_, ok_num);
    if (!ok_num) {
        lldb::SBFrame f = process_.GetSelectedThread().GetFrameAtIndex(selected_frame_idx_);
        if (f.IsValid()) {
            lldb::SBValue v = f.GetValueForVariablePath(pixel_expr_.c_str());
            if (!v.IsValid() || v.GetError().Fail()) {
                v = f.EvaluateExpression(pixel_expr_.c_str());
            }
            if (v.IsValid() && !v.GetError().Fail()) {
                if (v.GetType().IsPointerType()) {
                    addr = v.GetValueAsUnsigned(0);
                } else if (v.GetNumChildren() > 0) {
                    addr = v.GetChildAtIndex(0).GetLoadAddress();
                } else {
                    addr = v.GetLoadAddress();
                }
            }
        }
    }

    if (addr == 0 || addr == LLDB_INVALID_ADDRESS) {
        pp.error = "Invalid pixel buffer address: " + pixel_expr_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        pixel_probe_ = std::move(pp);
        return;
    }

    pp.address = addr;
    size_t raw_size = static_cast<size_t>(pp.width) * static_cast<size_t>(pp.height) *
                      static_cast<size_t>(pp.channels);
    std::vector<uint8_t> raw(raw_size, 0);
    lldb::SBError err;
    size_t got = process_.ReadMemory(addr, raw.data(), raw_size, err);
    if (got == 0 || err.Fail()) {
        pp.error = "ReadMemory failed: " + safe_str(err.GetCString());
    } else {
        size_t npix = static_cast<size_t>(pp.width) * static_cast<size_t>(pp.height);
        pp.rgba_bytes.resize(npix * 4, 255);
        for (size_t i = 0; i < npix; ++i) {
            if (pp.channels == 4 && i * 4 + 3 < got) {
                pp.rgba_bytes[i * 4 + 0] = raw[i * 4 + 0];
                pp.rgba_bytes[i * 4 + 1] = raw[i * 4 + 1];
                pp.rgba_bytes[i * 4 + 2] = raw[i * 4 + 2];
                pp.rgba_bytes[i * 4 + 3] = raw[i * 4 + 3];
            } else if (pp.channels == 3 && i * 3 + 2 < got) {
                pp.rgba_bytes[i * 4 + 0] = raw[i * 3 + 0];
                pp.rgba_bytes[i * 4 + 1] = raw[i * 3 + 1];
                pp.rgba_bytes[i * 4 + 2] = raw[i * 3 + 2];
                pp.rgba_bytes[i * 4 + 3] = 255;
            } else if (pp.channels == 1 && i < got) {
                uint8_t g = raw[i];
                pp.rgba_bytes[i * 4 + 0] = g;
                pp.rgba_bytes[i * 4 + 1] = g;
                pp.rgba_bytes[i * 4 + 2] = g;
                pp.rgba_bytes[i * 4 + 3] = 255;
            }
        }
        pp.version = ++pixel_version_;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    pixel_probe_ = std::move(pp);
}

void DebuggerEngine::refresh_pointer_graph_worker() {
    PointerGraphData pg;
    pg.root_expr = graph_expr_;
    if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped || graph_expr_.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pointer_graph_ = std::move(pg);
        return;
    }

    lldb::SBFrame f = process_.GetSelectedThread().GetFrameAtIndex(selected_frame_idx_);
    if (!f.IsValid()) return;

    lldb::SBValue root = f.GetValueForVariablePath(graph_expr_.c_str());
    if (!root.IsValid() || root.GetError().Fail()) {
        root = f.EvaluateExpression(graph_expr_.c_str());
    }
    if (!root.IsValid() || root.GetError().Fail()) {
        pg.error = "Cannot evaluate: " + graph_expr_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        pointer_graph_ = std::move(pg);
        return;
    }

    lldb::SBType root_type = root.GetType();
    if (!root_type.IsPointerType()) {
        root = root.AddressOf();
        root_type = root.GetType();
    }

    std::unordered_set<uint64_t> visited;
    std::queue<lldb::SBValue> q;
    uint64_t root_addr = root.GetValueAsUnsigned(0);
    if (root_addr == 0) {
        pg.error = "Root pointer is NULL";
        std::lock_guard<std::mutex> lock(state_mutex_);
        pointer_graph_ = std::move(pg);
        return;
    }

    visited.insert(root_addr);
    q.push(root);

    while (!q.empty() && pg.nodes.size() < graph_max_nodes_) {
        lldb::SBValue cur_ptr = q.front();
        q.pop();

        uint64_t addr = cur_ptr.GetValueAsUnsigned(0);
        lldb::SBValue deref = cur_ptr.Dereference();
        if (!deref.IsValid()) continue;

        GraphNodeInfo gnode;
        gnode.address = addr;
        gnode.label = format_hex64_grouped(addr);

        std::ostringstream summary;
        uint32_t nf = deref.GetNumChildren(16);
        bool first_field = true;
        for (uint32_t i = 0; i < nf; ++i) {
            lldb::SBValue fld = deref.GetChildAtIndex(i);
            if (!fld.IsValid()) continue;
            std::string fname = safe_str(fld.GetName());
            lldb::SBType fty = fld.GetType();
            if (fty.IsPointerType() &&
                fty.GetPointeeType().GetCanonicalType().GetName() &&
                root_type.GetPointeeType().GetCanonicalType().GetName() &&
                std::strcmp(fty.GetPointeeType().GetCanonicalType().GetName(),
                            root_type.GetPointeeType().GetCanonicalType().GetName()) == 0) {
                uint64_t next_addr = fld.GetValueAsUnsigned(0);
                if (next_addr != 0) {
                    gnode.outgoing.emplace_back(fname, next_addr);
                    if (visited.count(next_addr)) {
                        pg.has_cycle = true;
                        pg.cycle_target = next_addr;
                    } else if (visited.size() < graph_max_nodes_) {
                        visited.insert(next_addr);
                        q.push(fld);
                    }
                }
            } else {
                if (!first_field) summary << "  ";
                summary << fname << "=" << format_value_string(fld);
                first_field = false;
            }
        }
        gnode.summary = summary.str();
        pg.nodes.push_back(std::move(gnode));
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    pointer_graph_ = std::move(pg);
}

void DebuggerEngine::append_console(ConsoleLineKind kind, const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lock(state_mutex_);
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            console_lines_.push_back(ConsoleLine{kind, text.substr(start)});
            break;
        }
        console_lines_.push_back(ConsoleLine{kind, text.substr(start, nl - start)});
        start = nl + 1;
    }
    if (console_lines_.size() > 2000) {
        console_lines_.erase(console_lines_.begin(), console_lines_.begin() + 500);
    }
}

std::shared_ptr<const StepSnapshot> DebuggerEngine::latest_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return latest_snapshot_;
}

std::vector<std::shared_ptr<const StepSnapshot>> DebuggerEngine::history_snapshots() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return history_;
}

MemorySnapshot DebuggerEngine::memory_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return memory_snapshot_;
}

StructTableData DebuggerEngine::struct_table() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return struct_table_;
}

WaveformProbeData DebuggerEngine::waveform_probe() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return waveform_probe_;
}

PixelProbeData DebuggerEngine::pixel_probe() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return pixel_probe_;
}

void DebuggerEngine::refresh_ring_probe_worker() {
    RingBufferProbeData rp;
    rp.buf_expr = ring_buf_expr_;
    rp.head_expr = ring_head_expr_;
    rp.tail_expr = ring_tail_expr_;

    if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped || ring_buf_expr_.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ring_probe_ = std::move(rp);
        return;
    }

    lldb::SBFrame f = process_.GetSelectedThread().GetFrameAtIndex(selected_frame_idx_);
    if (!f.IsValid()) return;

    auto eval_u64 = [&](const std::string& expr, bool& ok) -> uint64_t {
        ok = false;
        if (expr.empty()) return 0;
        uint64_t literal = parse_hex_or_dec(expr, ok);
        if (ok) return literal;
        lldb::SBValue v = f.GetValueForVariablePath(expr.c_str());
        if (!v.IsValid() || v.GetError().Fail()) {
            v = f.EvaluateExpression(expr.c_str());
        }
        if (!v.IsValid() || v.GetError().Fail()) return 0;
        // Handle std::atomic<T> seamlessly by descending into first numeric child if needed
        uint64_t val = v.GetValueAsUnsigned(UINT64_MAX);
        if (val == UINT64_MAX && v.GetNumChildren() > 0) {
            lldb::SBValue c = v.GetChildAtIndex(0);
            while (c.IsValid() && c.GetValueAsUnsigned(UINT64_MAX) == UINT64_MAX && c.GetNumChildren() > 0) {
                c = c.GetChildAtIndex(0);
            }
            if (c.IsValid()) val = c.GetValueAsUnsigned(UINT64_MAX);
        }
        if (val != UINT64_MAX) {
            ok = true;
            return val;
        }
        return 0;
    };

    lldb::SBValue buf = f.GetValueForVariablePath(ring_buf_expr_.c_str());
    if (!buf.IsValid() || buf.GetError().Fail()) {
        buf = f.EvaluateExpression(ring_buf_expr_.c_str());
    }
    if (!buf.IsValid() || buf.GetError().Fail()) {
        rp.error = "Cannot evaluate ring buffer: " + ring_buf_expr_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        ring_probe_ = std::move(rp);
        return;
    }

    bool ok_cap = false, ok_h = false, ok_t = false;
    uint64_t cap = eval_u64(ring_cap_expr_, ok_cap);
    if (!ok_cap || cap == 0) cap = buf.GetNumChildren(256);
    uint64_t head = eval_u64(ring_head_expr_, ok_h);
    uint64_t tail = eval_u64(ring_tail_expr_, ok_t);

    if (cap == 0) {
        rp.error = "Ring capacity resolved to 0";
        std::lock_guard<std::mutex> lock(state_mutex_);
        ring_probe_ = std::move(rp);
        return;
    }

    rp.capacity = cap;
    rp.head = head;
    rp.tail = tail;
    uint64_t h_mod = head % cap;
    uint64_t t_mod = tail % cap;
    if (tail >= head) {
        rp.occupied_count = std::min<uint64_t>(tail - head, cap);
    } else {
        rp.occupied_count = (cap - h_mod + t_mod) % cap;
    }
    rp.wrapped = (rp.occupied_count > 0) && (t_mod <= h_mod);

    uint64_t show_n = std::min<uint64_t>(cap, 64);
    rp.slots.reserve(show_n);
    for (uint64_t i = 0; i < show_n; ++i) {
        RingSlotInfo si;
        si.slot_index = i;
        si.is_head = (i == h_mod);
        si.is_tail = (i == t_mod);
        if (rp.occupied_count == cap) {
            si.occupied = true;
        } else if (rp.occupied_count == 0) {
            si.occupied = false;
        } else if (h_mod < t_mod) {
            si.occupied = (i >= h_mod && i < t_mod);
        } else {
            si.occupied = (i >= h_mod || i < t_mod);
        }

        lldb::SBValue elem = buf.GetChildAtIndex(static_cast<uint32_t>(i));
        if (elem.IsValid()) {
            uint32_t nf = elem.GetNumChildren(6);
            if (nf > 0) {
                std::string s;
                for (uint32_t k = 0; k < nf; ++k) {
                    lldb::SBValue fld = elem.GetChildAtIndex(k);
                    if (!fld.IsValid()) continue;
                    if (!s.empty()) s += "  ";
                    s += safe_str(fld.GetName()) + "=" + format_value_string(fld);
                }
                si.summary = std::move(s);
            } else {
                si.summary = format_value_string(elem);
            }
        }
        rp.slots.push_back(std::move(si));
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    ring_probe_ = std::move(rp);
}

void DebuggerEngine::refresh_orderbook_probe_worker() {
    OrderBookProbeData ob;
    ob.bids_expr = book_bids_expr_;
    ob.asks_expr = book_asks_expr_;
    ob.price_scale = book_price_scale_;

    if (!process_.IsValid() || process_.GetState() != lldb::eStateStopped ||
        (book_bids_expr_.empty() && book_asks_expr_.empty())) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        orderbook_probe_ = std::move(ob);
        return;
    }

    lldb::SBFrame f = process_.GetSelectedThread().GetFrameAtIndex(selected_frame_idx_);
    if (!f.IsValid()) return;

    auto extract_side = [&](const std::string& expr, std::vector<OrderBookLevel>& out) {
        if (expr.empty()) return;
        lldb::SBValue v = f.GetValueForVariablePath(expr.c_str());
        if (!v.IsValid() || v.GetError().Fail()) {
            v = f.EvaluateExpression(expr.c_str());
        }
        if (!v.IsValid() || v.GetError().Fail()) {
            ob.error = "Cannot evaluate: " + expr;
            return;
        }
        uint32_t n = std::min<uint32_t>(v.GetNumChildren(32), 32);
        for (uint32_t i = 0; i < n; ++i) {
            lldb::SBValue elem = v.GetChildAtIndex(i);
            if (!elem.IsValid()) continue;
            OrderBookLevel lvl{};
            uint32_t nf = elem.GetNumChildren(8);
            for (uint32_t k = 0; k < nf; ++k) {
                lldb::SBValue fld = elem.GetChildAtIndex(k);
                std::string fn = safe_str(fld.GetName());
                if (fn == "price" || fn == "px" || fn == "price_e4" || fn == "price_ticks") {
                    const char* vs = fld.GetValue();
                    if (vs) {
                        if (std::strchr(vs, '.') != nullptr) {
                            lvl.price = std::strtod(vs, nullptr);
                            lvl.raw_price = static_cast<int64_t>(lvl.price * ob.price_scale);
                        } else {
                            lvl.raw_price = fld.GetValueAsSigned(0);
                            lvl.price = static_cast<double>(lvl.raw_price) / ob.price_scale;
                        }
                    }
                } else if (fn == "qty" || fn == "size" || fn == "quantity" || fn == "volume") {
                    lvl.qty = fld.GetValueAsUnsigned(0);
                } else if (fn == "orders" || fn == "count" || fn == "num_orders") {
                    lvl.orders = static_cast<uint32_t>(fld.GetValueAsUnsigned(0));
                }
            }
            out.push_back(lvl);
        }
    };

    extract_side(book_bids_expr_, ob.bids);
    extract_side(book_asks_expr_, ob.asks);

    if (!ob.bids.empty() && !ob.asks.empty()) {
        ob.best_bid = ob.bids[0].price;
        ob.best_ask = ob.asks[0].price;
        ob.spread = ob.best_ask - ob.best_bid;
        ob.mid = (ob.best_bid + ob.best_ask) * 0.5;
        ob.crossed = (ob.best_bid >= ob.best_ask);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    orderbook_probe_ = std::move(ob);
}

PointerGraphData DebuggerEngine::pointer_graph() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return pointer_graph_;
}

RingBufferProbeData DebuggerEngine::ring_probe() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return ring_probe_;
}

OrderBookProbeData DebuggerEngine::orderbook_probe() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return orderbook_probe_;
}

std::vector<SymbolMatch> DebuggerEngine::symbol_results() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return symbol_results_;
}

HoverEvalResult DebuggerEngine::hover_result() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return hover_result_;
}

std::vector<ConsoleLine> DebuggerEngine::console_lines() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return console_lines_;
}

void DebuggerEngine::clear_console() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    console_lines_.clear();
}

} // namespace scope
