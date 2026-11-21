#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scope {

/// High-level lifecycle state of the active debugging session.
enum class SessionState {
    Idle,    ///< No live target process.
    Running, ///< Target process is actively executing.
    Stopped, ///< Target paused at a breakpoint, step, watchpoint, or user interrupt.
    Fault,   ///< Target stopped on a fatal signal or hardware exception (Crash Autopsy active).
    Exited,  ///< Target process terminated normally or was killed.
};

/// Memory region classification attached to pointer values and fault addresses.
enum class ProvenanceKind {
    None,     ///< Not a pointer or address.
    Stack,    ///< Within active thread stack window or stack VM region.
    Heap,     ///< Read-write anonymous / MALLOC heap region.
    Text,     ///< Executable code segment.
    Rodata,   ///< Read-only data / string literal segment.
    Data,     ///< Named read-write data segment.
    Null,     ///< Null or low-page offset (< 0x1000).
    Unmapped, ///< Guard page or unmapped virtual address.
    Unknown,  ///< Region could not be resolved.
};

struct MemoryRegion {
    uint64_t start = 0;
    uint64_t end = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    std::string name;
    ProvenanceKind kind = ProvenanceKind::Unknown;
};

struct VarNode {
    std::string name;
    std::string expr_path;
    std::string raw_type;
    std::string display_type;
    std::string value;
    std::string prev_value;
    bool changed = false;
    uint64_t address = 0;
    uint64_t byte_size = 0;
    bool is_pointer = false;
    uint64_t pointer_target = 0;
    ProvenanceKind provenance = ProvenanceKind::None;
    uint32_t num_children = 0;
    bool is_synthetic = false;
    std::vector<VarNode> children;
};

struct FrameInfo {
    uint32_t index = 0;
    std::string function;
    std::string display_function;
    std::string file;
    uint32_t line = 0;
    uint64_t pc = 0;
    std::string module;
    bool is_runtime = false;
    bool is_inlined = false;
};

struct ThreadInfo {
    uint64_t id = 0;
    uint32_t index_id = 0;
    std::string name;
    std::string stop_reason;
    std::string stop_description;
    bool is_selected = false;
    bool is_stopped = false;
    std::string top_user_function;
    std::vector<FrameInfo> frames;
};

struct RegisterInfo {
    std::string name;
    std::string value_hex;
    uint64_t value_u64 = 0;
    uint64_t prev_value_u64 = 0;
    bool changed = false;
    uint32_t byte_size = 8;
    std::string group;
    std::vector<uint8_t> raw_bytes; // populated for 16/32/64B SIMD registers
};

struct BreakpointInfo {
    int32_t id = 0;
    std::string file;
    uint32_t line = 0;
    std::string condition;
    std::string log_message;
    uint32_t hit_count = 0;
    bool enabled = true;
    bool resolved = false;
};

struct WatchpointInfo {
    int32_t id = 0;
    uint64_t address = 0;
    uint64_t byte_size = 0;
    std::string label;
    std::string condition;
    bool watch_read = false;
    bool watch_write = true;
    uint32_t hit_count = 0;
    bool enabled = true;
};

struct DisasmInstruction {
    uint64_t address = 0;
    std::string mnemonic;
    std::string operands;
    std::string comment;
    std::string file;
    uint32_t line = 0;
    uint64_t branch_target = 0;
    bool is_branch = false;
};

struct StructFieldOverlay {
    std::string name;
    std::string type_name;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t color_index = 0;
    bool is_atomic = false;
};

struct PaddingRange {
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct MemorySnapshot {
    uint64_t base_address = 0;
    std::vector<uint8_t> bytes;
    uint64_t prev_base_address = 0;
    std::vector<uint8_t> prev_bytes;
    std::string label;
    std::string struct_type;
    uint64_t struct_size = 0;
    uint64_t padding_bytes = 0;
    std::vector<StructFieldOverlay> fields;
    std::vector<PaddingRange> padding_ranges;
    std::vector<std::string> false_sharing_warnings;
    bool valid = false;
    std::string error;
};

struct CrashInfo {
    bool is_crash = false;
    std::string signal_name;
    int32_t signal_number = 0;
    uint64_t fault_address = 0;
    ProvenanceKind fault_provenance = ProvenanceKind::None;
    std::string summary;
    std::string offending_instruction;
    uint32_t user_frame_index = 0;
};

struct StepDeltaEntry {
    std::string category; // "VAR", "WATCH", "REG"
    std::string name;
    std::string old_val;
    std::string new_val;
};

struct StepSnapshot {
    uint64_t step_index = 0;
    SessionState state = SessionState::Idle;
    std::string target_path;
    uint64_t pid = 0;
    int32_t exit_code = 0;
    bool is_core_dump = false;
    bool is_remote = false;
    uint64_t selected_thread_id = 0;
    uint32_t selected_frame_idx = 0;
    std::string active_file;
    uint32_t active_line = 0;
    uint64_t active_pc = 0;
    std::string stop_reason;

    std::vector<ThreadInfo> threads;
    std::vector<VarNode> locals;
    std::vector<VarNode> watches;
    std::vector<RegisterInfo> registers;
    std::vector<MemoryRegion> regions;
    std::vector<BreakpointInfo> breakpoints;
    std::vector<WatchpointInfo> watchpoints;
    std::vector<DisasmInstruction> disassembly;
    CrashInfo crash;
    std::vector<StepDeltaEntry> deltas;
};

struct SymbolMatch {
    std::string name;
    std::string file;
    uint32_t line = 0;
    uint64_t address = 0;
};

struct RingSlotInfo {
    uint64_t slot_index = 0;
    bool occupied = false;
    bool is_head = false;
    bool is_tail = false;
    std::string summary;
};

struct RingBufferProbeData {
    std::string buf_expr;
    std::string head_expr;
    std::string tail_expr;
    uint64_t capacity = 0;
    uint64_t head = 0;
    uint64_t tail = 0;
    uint64_t occupied_count = 0;
    bool wrapped = false;
    std::vector<RingSlotInfo> slots;
    std::string error;
};

struct OrderBookLevel {
    double price = 0.0;
    int64_t raw_price = 0;
    uint64_t qty = 0;
    uint32_t orders = 0;
};

struct OrderBookProbeData {
    std::string bids_expr;
    std::string asks_expr;
    double price_scale = 10000.0;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double spread = 0.0;
    double mid = 0.0;
    bool crossed = false;
    std::string error;
};

struct StructTableData {
    std::string expr;
    std::string element_type;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::vector<std::string>> prev_rows;
    uint64_t total_count = 0;
    std::string error;
};

struct WaveformProbeData {
    std::string expr;
    std::vector<float> samples;
    float min_val = 0.0f;
    float max_val = 0.0f;
    float avg_val = 0.0f;
    std::string error;
};

struct PixelProbeData {
    std::string expr;
    uint64_t address = 0;
    int width = 32;
    int height = 32;
    int channels = 4; // 4=RGBA, 1=Gray
    std::vector<uint8_t> rgba_bytes;
    uint64_t version = 0;
    std::string error;
};

struct GraphNodeInfo {
    uint64_t address = 0;
    std::string label;
    std::string summary;
    std::vector<std::pair<std::string, uint64_t>> outgoing; // field_name -> target_addr
};

struct PointerGraphData {
    std::string root_expr;
    std::vector<GraphNodeInfo> nodes;
    bool has_cycle = false;
    uint64_t cycle_target = 0;
    std::string error;
};

struct HoverEvalResult {
    std::string expr;
    bool valid = false;
    VarNode root;
    std::string error;
};

enum class ConsoleLineKind {
    Stdout,
    Stderr,
    LldbCmd,
    LldbOut,
    System,
};

struct ConsoleLine {
    ConsoleLineKind kind = ConsoleLineKind::System;
    std::string text;
};

} // namespace scope
