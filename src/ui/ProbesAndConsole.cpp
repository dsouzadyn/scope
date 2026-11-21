#include "UiContext.hpp"
#include "Theme.hpp"
#include "Widgets.hpp"
#include "../engine/TypeFormat.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

namespace scope::ui {

namespace {

void draw_waveform_probe(UiContext& ctx) {
    const auto& pal = Theme::palette();

    ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - 86.0f));
    bool enter = ImGui::InputTextWithHint("##WaveExpr", "Numeric vector/array (e.g. waveform)",
                                          ctx.wave_expr_buf, sizeof(ctx.wave_expr_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("CLIP", ImVec2(78.0f, 0.0f)) || enter) {
        if (ctx.wave_expr_buf[0] != '\0') {
            ctx.engine->configure_waveform_probe(ctx.wave_expr_buf);
        }
    }

    WaveformProbeData wp = ctx.engine->waveform_probe();
    if (!wp.error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "%s", wp.error.c_str());
        return;
    }
    if (wp.samples.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "Oscilloscope Waveform Probe: enter a numeric vector/array or right-click a variable.");
        return;
    }

    // Telemetry header
    ImGui::PushFont(Theme::fonts().micro);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.phosphor_green),
                       "CH1: %s   N=%zu   MIN=%.4g   MAX=%.4g   AVG=%.4g",
                       wp.expr.c_str(), wp.samples.size(), wp.min_val, wp.max_val, wp.avg_val);
    ImGui::PopFont();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 sz(std::max(200.0f, avail.x), std::max(120.0f, avail.y - 4.0f));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##OscCanvas", sz);
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), pal.bg_void);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), pal.border);

    // Tektronix graticule (10 x 6 divisions)
    const int gx = 10, gy = 6;
    for (int i = 1; i < gx; ++i) {
        float x = p.x + sz.x * (static_cast<float>(i) / gx);
        dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + sz.y),
                    (pal.border & 0x00FFFFFFu) | 0x60000000u, 1.0f);
    }
    for (int j = 1; j < gy; ++j) {
        float y = p.y + sz.y * (static_cast<float>(j) / gy);
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + sz.x, y),
                    (pal.border & 0x00FFFFFFu) | 0x60000000u, 1.0f);
    }

    float range = wp.max_val - wp.min_val;
    if (range < 1e-6f) range = 1.0f;
    float pad_y = 14.0f;
    float plot_h = std::max(20.0f, sz.y - pad_y * 2.0f);

    // Zero reference line if 0 is in range
    if (wp.min_val <= 0.0f && wp.max_val >= 0.0f) {
        float zy = p.y + pad_y + plot_h * (1.0f - (0.0f - wp.min_val) / range);
        dl->AddLine(ImVec2(p.x, zy), ImVec2(p.x + sz.x, zy), pal.border_strong, 1.0f);
    }

    size_t n = wp.samples.size();
    auto pt_for = [&](size_t idx) {
        float fx = (n > 1) ? static_cast<float>(idx) / static_cast<float>(n - 1) : 0.5f;
        float fy = (wp.samples[idx] - wp.min_val) / range;
        return ImVec2(p.x + 8.0f + fx * (sz.x - 16.0f),
                      p.y + pad_y + plot_h * (1.0f - fy));
    };

    // Subtle phosphor glow + crisp primary trace
    for (size_t i = 1; i < n; ++i) {
        ImVec2 a = pt_for(i - 1);
        ImVec2 b = pt_for(i);
        dl->AddLine(a, b, (pal.phosphor_green & 0x00FFFFFFu) | 0x38000000u, 4.5f);
        dl->AddLine(a, b, pal.phosphor_green, 1.8f);
    }
    if (n <= 96) {
        for (size_t i = 0; i < n; ++i) {
            dl->AddCircleFilled(pt_for(i), 2.2f, pal.phosphor_green);
        }
    }

    // Hover crosshair + exact sample readout
    if (hovered) {
        ImVec2 m = ImGui::GetMousePos();
        float frac = std::clamp((m.x - (p.x + 8.0f)) / std::max(1.0f, sz.x - 16.0f), 0.0f, 1.0f);
        size_t idx = static_cast<size_t>(std::round(frac * static_cast<float>(n - 1)));
        ImVec2 sp = pt_for(idx);
        dl->AddLine(ImVec2(sp.x, p.y), ImVec2(sp.x, p.y + sz.y), pal.signal_orange, 1.0f);
        dl->AddCircleFilled(sp, 4.0f, pal.signal_orange);
        ImGui::SetTooltip("[%zu] = %.6g", idx, wp.samples[idx]);
    }
}

void draw_pixel_probe(UiContext& ctx) {
    const auto& pal = Theme::palette();
    static float s_zoom = 5.0f;
    static uint64_t s_uploaded_version = 0;
    static ImTextureID s_tex = 0;

    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 270.0f));
    bool enter = ImGui::InputTextWithHint("##PixExpr", "Buffer ptr/vec (e.g. pixels, framebuffer)",
                                          ctx.pixel_expr_buf, sizeof(ctx.pixel_expr_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(48.0f);
    ImGui::InputInt("##PixW", &ctx.pixel_w, 0);
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextDisabled("x");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(48.0f);
    ImGui::InputInt("##PixH", &ctx.pixel_h, 0);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("CAPTURE", ImVec2(68.0f, 0.0f)) || enter) {
        if (ctx.pixel_expr_buf[0] != '\0') {
            ctx.engine->configure_pixel_probe(ctx.pixel_expr_buf, ctx.pixel_w, ctx.pixel_h, ctx.pixel_ch);
        }
    }
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(62.0f);
    ImGui::SliderFloat("##Zoom", &s_zoom, 1.0f, 14.0f, "%.0fx");

    PixelProbeData pp = ctx.engine->pixel_probe();
    if (!pp.error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "%s", pp.error.c_str());
        return;
    }
    if (pp.rgba_bytes.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "2D Pixel Buffer Probe: enter a raw RGBA/Gray buffer pointer (e.g. 'pixels' in memory_watch).");
        return;
    }

    if (ctx.textures && pp.version != s_uploaded_version) {
        s_tex = ctx.textures->upload_rgba(pp.width, pp.height, pp.rgba_bytes.data());
        s_uploaded_version = pp.version;
    }

    ImGui::BeginChild("##PixelCanvas", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (s_tex) {
        ImVec2 img_sz(static_cast<float>(pp.width) * s_zoom, static_cast<float>(pp.height) * s_zoom);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Image(s_tex, img_sz);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(p, ImVec2(p.x + img_sz.x, p.y + img_sz.y), pal.border_strong);

        if (ImGui::IsItemHovered()) {
            ImVec2 m = ImGui::GetMousePos();
            int px = std::clamp(static_cast<int>((m.x - p.x) / s_zoom), 0, pp.width - 1);
            int py = std::clamp(static_cast<int>((m.y - p.y) / s_zoom), 0, pp.height - 1);
            size_t idx = static_cast<size_t>((py * pp.width + px) * 4);
            if (idx + 3 < pp.rgba_bytes.size()) {
                uint8_t r = pp.rgba_bytes[idx + 0];
                uint8_t g = pp.rgba_bytes[idx + 1];
                uint8_t b = pp.rgba_bytes[idx + 2];
                uint8_t a = pp.rgba_bytes[idx + 3];
                dl->AddRect(ImVec2(p.x + px * s_zoom, p.y + py * s_zoom),
                            ImVec2(p.x + (px + 1) * s_zoom, p.y + (py + 1) * s_zoom),
                            pal.signal_orange, 0.0f, 0, 1.5f);
                ImGui::SetTooltip("(%d, %d)  RGBA=(%u, %u, %u, %u)  #%02X%02X%02X",
                                  px, py, r, g, b, a, r, g, b);
            }
        }
    }
    ImGui::EndChild();
}

void draw_pointer_graph_probe(UiContext& ctx) {
    const auto& pal = Theme::palette();

    ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - 86.0f));
    bool enter = ImGui::InputTextWithHint("##GraphExpr", "Root pointer (e.g. head)",
                                          ctx.graph_expr_buf, sizeof(ctx.graph_expr_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("TRACE", ImVec2(78.0f, 0.0f)) || enter) {
        if (ctx.graph_expr_buf[0] != '\0') {
            ctx.engine->configure_pointer_graph(ctx.graph_expr_buf);
        }
    }

    PointerGraphData pg = ctx.engine->pointer_graph();
    if (!pg.error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "%s", pg.error.c_str());
        return;
    }
    if (pg.nodes.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "Pointer Graph Probe: trace linked lists & trees with automatic cycle detection (e.g. 'head' in memory_watch).");
        return;
    }

    ImGui::PushFont(Theme::fonts().micro);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.cobalt),
                       "NODES: %zu", pg.nodes.size());
    if (pg.has_cycle) {
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.fault_crimson),
                           "CYCLE DETECTED -> %s", format_hex64_grouped(pg.cycle_target).c_str());
    }
    ImGui::PopFont();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 sz(std::max(240.0f, avail.x), std::max(130.0f, avail.y - 4.0f));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##GraphCanvas", sz);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), pal.bg_void);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), pal.border);

    const float node_w = 158.0f;
    const float node_h = 52.0f;
    const float gap_x = 38.0f;
    const float base_y = p.y + sz.y * 0.42f - node_h * 0.5f;

    std::unordered_map<uint64_t, int> idx_by_addr;
    for (int i = 0; i < static_cast<int>(pg.nodes.size()); ++i) {
        idx_by_addr[pg.nodes[i].address] = i;
    }

    auto box_for = [&](int i) {
        float x = p.x + 18.0f + static_cast<float>(i) * (node_w + gap_x);
        return ImVec2(x, base_y);
    };

    // Edges
    for (int i = 0; i < static_cast<int>(pg.nodes.size()); ++i) {
        ImVec2 src = box_for(i);
        for (const auto& [fname, target_addr] : pg.nodes[i].outgoing) {
            auto it = idx_by_addr.find(target_addr);
            if (it == idx_by_addr.end()) continue;
            int j = it->second;
            ImVec2 dst = box_for(j);
            if (j > i) {
                ImVec2 a(src.x + node_w, src.y + node_h * 0.5f);
                ImVec2 b(dst.x, dst.y + node_h * 0.5f);
                dl->AddLine(a, b, pal.cobalt, 1.8f);
                dl->AddTriangleFilled(b, ImVec2(b.x - 7.0f, b.y - 4.0f),
                                      ImVec2(b.x - 7.0f, b.y + 4.0f), pal.cobalt);
            } else {
                // Back-edge (cycle!) drawn underneath in Fault Crimson
                ImVec2 a(src.x + node_w * 0.5f, src.y + node_h);
                ImVec2 b(dst.x + node_w * 0.5f, dst.y + node_h);
                float arc_y = src.y + node_h + 28.0f;
                dl->AddLine(a, ImVec2(a.x, arc_y), pal.fault_crimson, 2.0f);
                dl->AddLine(ImVec2(a.x, arc_y), ImVec2(b.x, arc_y), pal.fault_crimson, 2.0f);
                dl->AddLine(ImVec2(b.x, arc_y), b, pal.fault_crimson, 2.0f);
                dl->AddTriangleFilled(b, ImVec2(b.x - 4.0f, b.y + 7.0f),
                                      ImVec2(b.x + 4.0f, b.y + 7.0f), pal.fault_crimson);
            }
        }
    }

    // Nodes
    for (int i = 0; i < static_cast<int>(pg.nodes.size()); ++i) {
        const auto& nd = pg.nodes[i];
        ImVec2 bp = box_for(i);
        bool is_cycle_tgt = pg.has_cycle && (nd.address == pg.cycle_target);
        ImU32 accent = (i == 0) ? pal.signal_orange
                                : (is_cycle_tgt ? pal.fault_crimson : pal.cobalt);

        dl->AddRectFilled(bp, ImVec2(bp.x + node_w, bp.y + node_h), pal.bg_chassis, 1.0f);
        dl->AddRect(bp, ImVec2(bp.x + node_w, bp.y + node_h), accent, 1.0f);
        dl->AddRectFilled(bp, ImVec2(bp.x + node_w, bp.y + 3.0f), accent);

        ImGui::PushFont(Theme::fonts().micro);
        std::string short_addr = nd.label.size() > 12 ? (".." + nd.label.substr(nd.label.size() - 10)) : nd.label;
        dl->AddText(ImVec2(bp.x + 7.0f, bp.y + 8.0f), accent, short_addr.c_str());
        dl->AddText(ImVec2(bp.x + 7.0f, bp.y + 26.0f), pal.text_primary, nd.summary.c_str());
        ImGui::PopFont();
    }
}

void draw_ring_buffer_probe(UiContext& ctx) {
    const auto& pal = Theme::palette();

    ImGui::SetNextItemWidth(135.0f);
    bool e1 = ImGui::InputTextWithHint("##RingBuf", "buf (ring.slots)", ctx.ring_buf_buf,
                                       sizeof(ctx.ring_buf_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(95.0f);
    bool e2 = ImGui::InputTextWithHint("##RingCap", "cap", ctx.ring_cap_buf,
                                       sizeof(ctx.ring_cap_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(95.0f);
    bool e3 = ImGui::InputTextWithHint("##RingHead", "head", ctx.ring_head_buf,
                                       sizeof(ctx.ring_head_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(95.0f);
    bool e4 = ImGui::InputTextWithHint("##RingTail", "tail", ctx.ring_tail_buf,
                                       sizeof(ctx.ring_tail_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("ATTACH", ImVec2(64.0f, 0.0f)) || e1 || e2 || e3 || e4) {
        ctx.engine->configure_ring_probe(ctx.ring_buf_buf, ctx.ring_cap_buf,
                                         ctx.ring_head_buf, ctx.ring_tail_buf);
    }

    RingBufferProbeData rp = ctx.engine->ring_probe();
    if (!rp.error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "%s", rp.error.c_str());
        return;
    }
    if (rp.capacity == 0 || rp.slots.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "SPSC/MPSC Ring Buffer Probe: enter buffer, capacity, head & tail expressions (try hft_orderbook).");
        return;
    }

    double fill_pct = (rp.capacity > 0)
                          ? (100.0 * static_cast<double>(rp.occupied_count) / static_cast<double>(rp.capacity))
                          : 0.0;
    ImGui::PushFont(Theme::fonts().micro);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.phosphor_green),
                       "OCCUPIED %llu / %llu (%.0f%%)   HEAD=%llu [%llu]   TAIL=%llu [%llu]%s",
                       static_cast<unsigned long long>(rp.occupied_count),
                       static_cast<unsigned long long>(rp.capacity),
                       fill_pct,
                       static_cast<unsigned long long>(rp.head),
                       static_cast<unsigned long long>(rp.head % rp.capacity),
                       static_cast<unsigned long long>(rp.tail),
                       static_cast<unsigned long long>(rp.tail % rp.capacity),
                       rp.wrapped ? "   [WRAPPED]" : "");
    ImGui::PopFont();

    // Linear slot occupancy visual strip
    float strip_h = 34.0f;
    float avail_w = std::max(240.0f, ImGui::GetContentRegionAvail().x);
    ImVec2 sp = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(avail_w, strip_h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(sp, ImVec2(sp.x + avail_w, sp.y + strip_h), pal.bg_void);
    dl->AddRect(sp, ImVec2(sp.x + avail_w, sp.y + strip_h), pal.border);

    int n = static_cast<int>(rp.slots.size());
    float cell_w = (avail_w - 8.0f) / static_cast<float>(std::max(1, n));
    static int s_selected_slot = -1;
    ImVec2 mouse = ImGui::GetMousePos();

    for (int i = 0; i < n; ++i) {
        const auto& sl = rp.slots[i];
        float x0 = sp.x + 4.0f + static_cast<float>(i) * cell_w;
        float x1 = x0 + std::max(4.0f, cell_w - 2.0f);
        ImVec2 rmin(x0, sp.y + 4.0f);
        ImVec2 rmax(x1, sp.y + strip_h - 4.0f);

        ImU32 fill = sl.occupied ? ((pal.phosphor_green & 0x00FFFFFFu) | 0x55000000u) : pal.bg_chassis;
        ImU32 border = sl.occupied ? pal.phosphor_green : pal.border;
        if (sl.is_head) border = pal.cobalt;
        if (sl.is_tail) border = pal.signal_orange;
        if (i == s_selected_slot) fill = (pal.signal_orange & 0x00FFFFFFu) | 0x45000000u;

        dl->AddRectFilled(rmin, rmax, fill, 1.0f);
        dl->AddRect(rmin, rmax, border, 1.0f, 0, (sl.is_head || sl.is_tail) ? 2.0f : 1.0f);

        if (cell_w >= 24.0f) {
            ImGui::PushFont(Theme::fonts().micro);
            char lbl[16];
            if (sl.is_head && sl.is_tail) std::snprintf(lbl, sizeof(lbl), "HT");
            else if (sl.is_head) std::snprintf(lbl, sizeof(lbl), "H%d", i);
            else if (sl.is_tail) std::snprintf(lbl, sizeof(lbl), "T%d", i);
            else std::snprintf(lbl, sizeof(lbl), "%d", i);
            dl->AddText(ImVec2(x0 + 3.0f, sp.y + 10.0f),
                        sl.occupied ? pal.text_primary : pal.text_dim, lbl);
            ImGui::PopFont();
        }

        if (mouse.x >= rmin.x && mouse.x <= rmax.x && mouse.y >= rmin.y && mouse.y <= rmax.y) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                s_selected_slot = i;
            }
            ImGui::SetTooltip("Slot [%d] (%s%s%s)\n%s",
                              i,
                              sl.occupied ? "LIVE" : "FREE",
                              sl.is_head ? " HEAD" : "",
                              sl.is_tail ? " TAIL" : "",
                              sl.summary.c_str());
        }
    }

    // Slot payload table
    ImGui::Spacing();
    ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##RingSlots", 3, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("SLOT", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("STATE", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn("PAYLOAD", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; ++i) {
            const auto& sl = rp.slots[i];
            ImGui::TableNextRow();
            if (i == s_selected_slot) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       (pal.signal_orange & 0x00FFFFFFu) | 0x26000000u);
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("[%02d]%s%s", i, sl.is_head ? " H" : "", sl.is_tail ? " T" : "");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(
                                   sl.occupied ? pal.phosphor_green : pal.text_dim),
                               "%s", sl.occupied ? "QUEUED" : "CONSUMED");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(
                                   sl.occupied ? pal.text_primary : pal.text_dim),
                               "%s", sl.summary.c_str());
        }
        ImGui::EndTable();
    }
}

void draw_orderbook_probe(UiContext& ctx) {
    const auto& pal = Theme::palette();
    static constexpr double kScales[4] = {1.0, 1e4, 1e6, 1e8};
    static const char* kScaleNames[4] = {"1", "1e-4", "1e-6", "1e-8"};

    ImGui::SetNextItemWidth(135.0f);
    bool e1 = ImGui::InputTextWithHint("##BookBids", "bids (book.bids)", ctx.book_bids_buf,
                                       sizeof(ctx.book_bids_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(135.0f);
    bool e2 = ImGui::InputTextWithHint("##BookAsks", "asks (book.asks)", ctx.book_asks_buf,
                                       sizeof(ctx.book_asks_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(62.0f);
    bool sc = ImGui::Combo("##BookScale", &ctx.book_scale_idx, kScaleNames, 4);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("PROBE", ImVec2(60.0f, 0.0f)) || e1 || e2 || sc) {
        ctx.engine->configure_orderbook_probe(ctx.book_bids_buf, ctx.book_asks_buf,
                                              kScales[ctx.book_scale_idx]);
    }

    OrderBookProbeData ob = ctx.engine->orderbook_probe();
    if (!ob.error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "%s", ob.error.c_str());
        return;
    }
    if (ob.bids.empty() && ob.asks.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "L2 Order Book Probe: inspect bids & asks depth ladder with crossed-book detection (try hft_orderbook).");
        return;
    }

    // Spread / Mid / Crossed-Book Alert Strip
    ImGui::PushFont(Theme::fonts().micro);
    if (ob.crossed) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.fault_crimson),
                           "!! CROSSED BOOK DETECTED !!  BID=%.4f >= ASK=%.4f  SPREAD=%.4f",
                           ob.best_bid, ob.best_ask, ob.spread);
    } else {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.cobalt),
                           "BID=%.4f   ASK=%.4f   SPREAD=%.4f   MID=%.4f",
                           ob.best_bid, ob.best_ask, ob.spread, ob.mid);
    }
    ImGui::PopFont();

    uint64_t max_qty = 1;
    for (const auto& l : ob.bids) max_qty = std::max(max_qty, l.qty);
    for (const auto& l : ob.asks) max_qty = std::max(max_qty, l.qty);

    ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##BookLadder", 4, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("SIDE", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("PRICE", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("QTY", ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupColumn("DEPTH", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto draw_lvl = [&](const char* side, const OrderBookLevel& lvl, ImU32 col, bool highlight_top) {
            ImGui::TableNextRow();
            if (highlight_top) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       (col & 0x00FFFFFFu) | 0x26000000u);
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", side);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.4f", lvl.price);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(lvl.qty));
            ImGui::TableSetColumnIndex(3);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float cw = std::max(40.0f, ImGui::GetContentRegionAvail().x);
            float frac = static_cast<float>(lvl.qty) / static_cast<float>(max_qty);
            ImGui::Dummy(ImVec2(cw, 14.0f));
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(cp.x, cp.y + 2.0f),
                ImVec2(cp.x + cw * frac, cp.y + 13.0f),
                (col & 0x00FFFFFFu) | 0x88000000u, 1.0f);
        };

        // Asks highest -> lowest (best ask closest to spread)
        for (int i = static_cast<int>(ob.asks.size()) - 1; i >= 0; --i) {
            draw_lvl("ASK", ob.asks[i], pal.fault_crimson, i == 0 && ob.crossed);
        }
        // Spread divider row
        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                               ob.crossed ? ((pal.fault_crimson & 0x00FFFFFFu) | 0x45000000u)
                                          : pal.bg_bezel);
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(
                               ob.crossed ? pal.fault_crimson : pal.signal_orange),
                           "%s", ob.crossed ? "XBOOK" : "SPRD");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.4f", ob.spread);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("MID");
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.4f", ob.mid);

        // Bids highest -> lowest (best bid closest to spread)
        for (size_t i = 0; i < ob.bids.size(); ++i) {
            draw_lvl("BID", ob.bids[i], pal.phosphor_green, i == 0 && ob.crossed);
        }
        ImGui::EndTable();
    }
}

} // namespace

void draw_probes_bay(UiContext& ctx) {
    const auto& pal = Theme::palette();

    if (ctx.focus_probes_bay) {
        ImGui::SetNextWindowFocus();
        ctx.focus_probes_bay = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("06 PROBES", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    begin_bay_toolbar("LIVE DATA VISUALIZERS", 30.0f);
    if (segmented_button("WAVE", ctx.probe_tab == ProbeTab::Waveform, pal.phosphor_green)) {
        ctx.probe_tab = ProbeTab::Waveform;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("PIXEL", ctx.probe_tab == ProbeTab::Pixel2D, pal.signal_orange)) {
        ctx.probe_tab = ProbeTab::Pixel2D;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("GRAPH", ctx.probe_tab == ProbeTab::PointerGraph, pal.cobalt)) {
        ctx.probe_tab = ProbeTab::PointerGraph;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("RING", ctx.probe_tab == ProbeTab::RingBuffer, pal.phosphor_green)) {
        ctx.probe_tab = ProbeTab::RingBuffer;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("BOOK L2", ctx.probe_tab == ProbeTab::OrderBook, pal.osc_amber)) {
        ctx.probe_tab = ProbeTab::OrderBook;
    }
    end_bay_toolbar();

    ImGui::BeginChild("##ProbesBody", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (ctx.probe_tab) {
        case ProbeTab::Waveform:     draw_waveform_probe(ctx); break;
        case ProbeTab::Pixel2D:      draw_pixel_probe(ctx); break;
        case ProbeTab::PointerGraph: draw_pointer_graph_probe(ctx); break;
        case ProbeTab::RingBuffer:   draw_ring_buffer_probe(ctx); break;
        case ProbeTab::OrderBook:    draw_orderbook_probe(ctx); break;
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

void draw_console_bay(UiContext& ctx) {
    const auto& pal = Theme::palette();
    static int s_filter = 0; // 0=ALL, 1=TARGET I/O, 2=LLDB
    static char s_cmd_buf[512] = "";
    static std::vector<std::string> s_cmd_hist;
    static int s_hist_idx = -1;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("07 CONSOLE", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    begin_bay_toolbar("TARGET I/O + LLDB REPL", 30.0f);
    if (segmented_button("ALL", s_filter == 0, pal.signal_orange)) s_filter = 0;
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("TARGET I/O", s_filter == 1, pal.phosphor_green)) s_filter = 1;
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("LLDB REPL", s_filter == 2, pal.cobalt)) s_filter = 2;
    ImGui::SameLine(0.0f, 10.0f);
    if (segmented_button("CLEAR", false, pal.text_secondary)) {
        ctx.engine->clear_console();
    }
    end_bay_toolbar();

    float input_h = 38.0f;
    ImGui::BeginChild("##ConsoleScroll", ImVec2(0.0f, -input_h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);

    auto lines = ctx.engine->console_lines();
    for (const auto& l : lines) {
        if (s_filter == 1 && l.kind != ConsoleLineKind::Stdout && l.kind != ConsoleLineKind::Stderr) continue;
        if (s_filter == 2 && l.kind != ConsoleLineKind::LldbCmd && l.kind != ConsoleLineKind::LldbOut &&
            l.kind != ConsoleLineKind::System) continue;

        ImU32 col = pal.text_primary;
        switch (l.kind) {
            case ConsoleLineKind::Stdout:  col = pal.phosphor_green; break;
            case ConsoleLineKind::Stderr:  col = pal.fault_crimson; break;
            case ConsoleLineKind::LldbCmd: col = pal.signal_orange; break;
            case ConsoleLineKind::LldbOut: col = pal.text_primary; break;
            case ConsoleLineKind::System:  col = pal.cobalt; break;
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", l.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::SetCursorPos(ImVec2(10.0f, ImGui::GetCursorPosY() + 5.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange), "(lldb)");
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 10.0f);

    auto hist_cb = [](ImGuiInputTextCallbackData* data) -> int {
        if (s_cmd_hist.empty()) return 0;
        if (data->EventKey == ImGuiKey_UpArrow) {
            if (s_hist_idx < 0) s_hist_idx = static_cast<int>(s_cmd_hist.size()) - 1;
            else if (s_hist_idx > 0) --s_hist_idx;
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, s_cmd_hist[s_hist_idx].c_str());
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (s_hist_idx >= 0 && s_hist_idx + 1 < static_cast<int>(s_cmd_hist.size())) {
                ++s_hist_idx;
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, s_cmd_hist[s_hist_idx].c_str());
            } else {
                s_hist_idx = -1;
                data->DeleteChars(0, data->BufTextLen);
            }
        }
        return 0;
    };

    if (ImGui::InputTextWithHint("##LldbRepl", "Enter LLDB command (e.g. bt, frame var, p expr)...",
                                 s_cmd_buf, sizeof(s_cmd_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                 hist_cb)) {
        if (s_cmd_buf[0] != '\0') {
            s_cmd_hist.emplace_back(s_cmd_buf);
            s_hist_idx = -1;
            ctx.engine->run_lldb_command(s_cmd_buf);
            s_cmd_buf[0] = '\0';
        }
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void draw_modals_and_palette(UiContext& ctx) {
    const auto& pal = Theme::palette();

    // 1) Custom Launch Modal
    if (ctx.open_launch_modal) {
        ImGui::OpenPopup("LAUNCH BINARY");
        ctx.open_launch_modal = false;
    }
    if (ImGui::BeginPopupModal("LAUNCH BINARY", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char exe_buf[512] = "";
        static char args_buf[256] = "";
        static bool stop_main = true;
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputTextWithHint("##ExePath", "/path/to/cpp_binary", exe_buf, sizeof(exe_buf));
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputTextWithHint("##ExeArgs", "Optional CLI args (space separated)", args_buf, sizeof(args_buf));
        ImGui::Checkbox("Break at main()", &stop_main);
        ImGui::Separator();
        if (ImGui::Button("LAUNCH", ImVec2(110, 0))) {
            std::vector<std::string> args;
            std::string a(args_buf);
            size_t pos = 0;
            while (pos < a.size()) {
                while (pos < a.size() && a[pos] == ' ') ++pos;
                size_t next = a.find(' ', pos);
                if (pos < a.size()) {
                    args.push_back(a.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
                }
                if (next == std::string::npos) break;
                pos = next + 1;
            }
            ctx.history_cursor = -1;
            ctx.engine->launch(exe_buf, args, "", stop_main);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", ImVec2(90, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 2) Attach PID Modal
    if (ctx.open_attach_modal) {
        ImGui::OpenPopup("ATTACH TO PID");
        ctx.open_attach_modal = false;
    }
    if (ImGui::BeginPopupModal("ATTACH TO PID", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static int pid_val = 0;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("Process ID", &pid_val);
        if (ImGui::Button("ATTACH", ImVec2(100, 0)) && pid_val > 0) {
            ctx.history_cursor = -1;
            ctx.engine->attach(static_cast<uint64_t>(pid_val));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", ImVec2(90, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 3) Open Core Dump Modal (Post-Mortem HFT Crash Analysis)
    if (ctx.open_core_modal) {
        ImGui::OpenPopup("OPEN CORE DUMP");
        ctx.open_core_modal = false;
    }
    if (ImGui::BeginPopupModal("OPEN CORE DUMP", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char core_exe_buf[512] = "";
        static char core_file_buf[512] = "";
        ImGui::SetNextItemWidth(440.0f);
        ImGui::InputTextWithHint("##CoreExe", "/path/to/binary (with DWARF debug info)",
                                 core_exe_buf, sizeof(core_exe_buf));
        ImGui::SetNextItemWidth(440.0f);
        ImGui::InputTextWithHint("##CorePath", "/cores/core.12345",
                                 core_file_buf, sizeof(core_file_buf));
        ImGui::Separator();
        if (ImGui::Button("LOAD CORE", ImVec2(110, 0)) && core_exe_buf[0] && core_file_buf[0]) {
            ctx.history_cursor = -1;
            ctx.engine->open_core(core_exe_buf, core_file_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", ImVec2(90, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 4) Connect Remote Modal (lldb-server / Linux / rr gdb-remote)
    if (ctx.open_remote_modal) {
        ImGui::OpenPopup("CONNECT REMOTE");
        ctx.open_remote_modal = false;
    }
    if (ImGui::BeginPopupModal("CONNECT REMOTE", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char rem_exe_buf[512] = "";
        static char rem_url_buf[256] = "connect://127.0.0.1:1234";
        ImGui::SetNextItemWidth(440.0f);
        ImGui::InputTextWithHint("##RemExe", "Optional local symbol binary path",
                                 rem_exe_buf, sizeof(rem_exe_buf));
        ImGui::SetNextItemWidth(440.0f);
        ImGui::InputTextWithHint("##RemUrl", "connect://host:port (lldb-server or rr replay -s)",
                                 rem_url_buf, sizeof(rem_url_buf));
        ImGui::Separator();
        if (ImGui::Button("CONNECT", ImVec2(110, 0)) && rem_url_buf[0]) {
            ctx.history_cursor = -1;
            ctx.engine->connect_remote(rem_exe_buf, rem_url_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL", ImVec2(90, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 5) Cmd+K Quick Instrument Palette + Live Symbol / Source File Jump
    if (ctx.open_command_palette) {
        ImGui::OpenPopup("##CmdPalette");
        ctx.open_command_palette = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 390.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##CmdPalette")) {
        static char filter_buf[128] = "";
        static std::string s_last_sym_query;
        ImGui::PushFont(Theme::fonts().micro);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange),
                           "SCOPE COMMAND & SYMBOL PALETTE");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) {
            filter_buf[0] = '\0';
            s_last_sym_query.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##PalFilter", "Search action, fixture, C++ symbol, or source file...",
                                 filter_buf, sizeof(filter_buf));

        // Live target symbol/file lookup when query >= 2 chars
        std::string cur_q(filter_buf);
        if (cur_q.size() >= 2 && cur_q != s_last_sym_query) {
            s_last_sym_query = cur_q;
            ctx.engine->search_symbols(cur_q);
        }

        ImGui::Separator();

        auto matches = [&](const std::string& label) {
            if (filter_buf[0] == '\0') return true;
            std::string lo_l = label, lo_f = filter_buf;
            std::transform(lo_l.begin(), lo_l.end(), lo_l.begin(), ::tolower);
            std::transform(lo_f.begin(), lo_f.end(), lo_f.begin(), ::tolower);
            return lo_l.find(lo_f) != std::string::npos;
        };

        auto item = [&](const char* label, auto&& fn) {
            if (matches(label) && ImGui::Selectable(label)) {
                fn();
                ImGui::CloseCurrentPopup();
            }
        };

        // Live Symbol & File Jump Results
        if (cur_q.size() >= 2) {
            auto syms = ctx.engine->symbol_results();
            if (!syms.empty()) {
                ImGui::PushFont(Theme::fonts().micro);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.cobalt), "SYMBOLS & FILES");
                ImGui::PopFont();
                for (size_t i = 0; i < std::min<size_t>(syms.size(), 10); ++i) {
                    const auto& sm = syms[i];
                    std::string fn = sm.file.empty()
                                         ? ""
                                         : ("  (" + std::filesystem::path(sm.file).filename().string() +
                                            ":" + std::to_string(sm.line) + ")");
                    std::string lbl = sm.name + fn + "##sym" + std::to_string(i);
                    if (ImGui::Selectable(lbl.c_str())) {
                        if (!sm.file.empty()) {
                            ctx.jump_to_source(sm.file, sm.line);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::Separator();
            }
        }

        for (const auto& fpath : ctx.discovered_fixtures) {
            std::string fname = std::filesystem::path(fpath).filename().string();
            std::string lbl = "Launch Fixture: " + fname;
            item(lbl.c_str(), [&] {
                ctx.history_cursor = -1;
                ctx.engine->launch(fpath, {}, "", true);
            });
        }
        ImGui::Separator();
        item("Action: Launch Custom Binary...", [&] { ctx.open_launch_modal = true; });
        item("Action: Attach to Running PID...", [&] { ctx.open_attach_modal = true; });
        item("Action: Open Core Dump (Post-Mortem)...", [&] { ctx.open_core_modal = true; });
        item("Action: Connect Remote (lldb-server / rr)...", [&] { ctx.open_remote_modal = true; });
        item("Action: Switch Source Mode -> Pure Source", [&] { ctx.code_mode = CodeViewMode::Source; });
        item("Action: Switch Source Mode -> Mixed Source + Assembly", [&] { ctx.code_mode = CodeViewMode::Mixed; });
        item("Action: Switch Source Mode -> Pure Assembly", [&] { ctx.code_mode = CodeViewMode::Assembly; });
        item("Action: Toggle Theme (Anodized Lab / Technical Schematic)", [&] {
            Theme::apply(Theme::current_mode() == ThemeMode::AnodizedLab
                             ? ThemeMode::TechnicalSchematic
                             : ThemeMode::AnodizedLab);
        });
        ImGui::Separator();
        item("HFT Preset: Memory Cache-Line + False Sharing -> stats_ptr", [&] { ctx.inspect_in_memory("stats_ptr", 256); });
        item("HFT Preset: Wire Packet (BE / Price / ns) -> wire_ptr", [&] { ctx.inspect_in_memory("wire_ptr", 128); });
        item("HFT Preset: Ring Buffer Probe -> ring", [&] { ctx.pin_ring_buffer("ring.slots", "ring.capacity", "ring.head", "ring.tail"); });
        item("HFT Preset: Order Book L2 Probe -> book", [&] { ctx.pin_orderbook("book.bids", "book.asks", 10000.0); });
        item("Preset Probe: Waveform -> waveform", [&] { ctx.pin_waveform("waveform"); });
        item("Preset Probe: Struct Table -> particles", [&] { ctx.open_in_struct_table("particles"); });
        item("Preset Probe: 2D Pixel -> pixels (32x32 RGBA)", [&] { ctx.pin_pixel_probe("pixels", 32, 32, 4); });
        item("Preset Probe: Pointer Graph -> head", [&] { ctx.pin_pointer_graph("head"); });

        ImGui::EndPopup();
    }
}

} // namespace scope::ui
