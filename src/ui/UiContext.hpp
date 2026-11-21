#pragma once

#include "../engine/DebuggerEngine.hpp"
#include "../engine/SyntaxHighlighter.hpp"
#include "../engine/Types.hpp"
#include "TextureBridge.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scope::ui {

enum class CodeViewMode {
    Source,
    Mixed,
    Assembly,
};

enum class InspectorTab {
    Locals,
    Watch,
    StructTable,
};

enum class ProbeTab {
    Waveform,
    Pixel2D,
    PointerGraph,
    RingBuffer,
    OrderBook,
};

struct UiContext {
    DebuggerEngine* engine = nullptr;
    SyntaxHighlighter* highlighter = nullptr;
    TextureBridge* textures = nullptr;

    // Currently displayed snapshot (live or historical scrubbed snapshot)
    std::shared_ptr<const StepSnapshot> snap;
    std::vector<std::shared_ptr<const StepSnapshot>> history;
    int history_cursor = -1; // -1 == LIVE

    // Discovered fixture binaries for instant 1-click launching
    std::vector<std::string> discovered_fixtures;

    // Cross-bay focus & navigation state
    std::string viewed_file;
    uint32_t scroll_to_line = 0;
    uint32_t highlighted_source_line = 0;
    uint64_t last_seen_step = 0;
    CodeViewMode code_mode = CodeViewMode::Source;

    InspectorTab inspector_tab = InspectorTab::Locals;
    ProbeTab probe_tab = ProbeTab::Waveform;

    // Focus triggers (ImGui::SetWindowFocus on next frame)
    bool focus_memory_bay = false;
    bool focus_inspector_bay = false;
    bool focus_probes_bay = false;
    bool open_launch_modal = false;
    bool open_attach_modal = false;
    bool open_core_modal = false;
    bool open_remote_modal = false;
    bool open_command_palette = false;

    // Synced input buffers for cross-bay jumps
    char mem_expr_buf[256] = "";
    char table_expr_buf[256] = "";
    char wave_expr_buf[256] = "";
    char pixel_expr_buf[256] = "";
    int pixel_w = 32;
    int pixel_h = 32;
    int pixel_ch = 4;
    char graph_expr_buf[256] = "";
    char ring_buf_buf[128] = "ring.slots";
    char ring_cap_buf[64] = "ring.capacity";
    char ring_head_buf[64] = "ring.head";
    char ring_tail_buf[64] = "ring.tail";
    char book_bids_buf[128] = "book.bids";
    char book_asks_buf[128] = "book.asks";
    int book_scale_idx = 1; // 0=1, 1=1e4, 2=1e6, 3=1e8

    bool is_scrubbing_history() const {
        return history_cursor >= 0 &&
               history_cursor < static_cast<int>(history.size()) - 1;
    }

    void jump_to_source(const std::string& file, uint32_t line) {
        if (!file.empty()) {
            viewed_file = file;
            scroll_to_line = line;
            highlighted_source_line = line;
        }
    }

    void inspect_in_memory(const std::string& expr_or_addr, uint64_t size = 512) {
        std::snprintf(mem_expr_buf, sizeof(mem_expr_buf), "%s", expr_or_addr.c_str());
        engine->configure_memory_view(expr_or_addr, size);
        focus_memory_bay = true;
    }

    void open_in_struct_table(const std::string& expr) {
        std::snprintf(table_expr_buf, sizeof(table_expr_buf), "%s", expr.c_str());
        engine->configure_struct_table(expr);
        inspector_tab = InspectorTab::StructTable;
        focus_inspector_bay = true;
    }

    void pin_waveform(const std::string& expr) {
        std::snprintf(wave_expr_buf, sizeof(wave_expr_buf), "%s", expr.c_str());
        engine->configure_waveform_probe(expr);
        probe_tab = ProbeTab::Waveform;
        focus_probes_bay = true;
    }

    void pin_pixel_probe(const std::string& expr, int w, int h, int ch = 4) {
        std::snprintf(pixel_expr_buf, sizeof(pixel_expr_buf), "%s", expr.c_str());
        pixel_w = w;
        pixel_h = h;
        pixel_ch = ch;
        engine->configure_pixel_probe(expr, w, h, ch);
        probe_tab = ProbeTab::Pixel2D;
        focus_probes_bay = true;
    }

    void pin_pointer_graph(const std::string& expr) {
        std::snprintf(graph_expr_buf, sizeof(graph_expr_buf), "%s", expr.c_str());
        engine->configure_pointer_graph(expr);
        probe_tab = ProbeTab::PointerGraph;
        focus_probes_bay = true;
    }

    void pin_ring_buffer(const std::string& buf,
                         const std::string& cap,
                         const std::string& head,
                         const std::string& tail) {
        std::snprintf(ring_buf_buf, sizeof(ring_buf_buf), "%s", buf.c_str());
        std::snprintf(ring_cap_buf, sizeof(ring_cap_buf), "%s", cap.c_str());
        std::snprintf(ring_head_buf, sizeof(ring_head_buf), "%s", head.c_str());
        std::snprintf(ring_tail_buf, sizeof(ring_tail_buf), "%s", tail.c_str());
        engine->configure_ring_probe(buf, cap, head, tail);
        probe_tab = ProbeTab::RingBuffer;
        focus_probes_bay = true;
    }

    void pin_orderbook(const std::string& bids, const std::string& asks, double scale = 10000.0) {
        std::snprintf(book_bids_buf, sizeof(book_bids_buf), "%s", bids.c_str());
        std::snprintf(book_asks_buf, sizeof(book_asks_buf), "%s", asks.c_str());
        engine->configure_orderbook_probe(bids, asks, scale);
        probe_tab = ProbeTab::OrderBook;
        focus_probes_bay = true;
    }
};

// Bay render functions
float draw_transport_and_timeline(UiContext& ctx, float viewport_w, float start_y);
void draw_code_canvas_bay(UiContext& ctx);
void draw_inspector_bay(UiContext& ctx);
void draw_memory_bay(UiContext& ctx);
void draw_stack_thread_bay(UiContext& ctx);
void draw_registers_bay(UiContext& ctx);
void draw_probes_bay(UiContext& ctx);
void draw_console_bay(UiContext& ctx);
void draw_modals_and_palette(UiContext& ctx);

} // namespace scope::ui
