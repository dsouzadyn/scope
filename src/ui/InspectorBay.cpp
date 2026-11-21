#include "UiContext.hpp"
#include "Theme.hpp"
#include "Widgets.hpp"
#include "../engine/TypeFormat.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace scope::ui {

namespace {

void draw_var_context_menu(UiContext& ctx, const VarNode& node, bool is_watch_list) {
    if (!ImGui::BeginPopupContextItem()) return;

    const auto& pal = Theme::palette();
    ImGui::PushFont(Theme::fonts().micro);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange),
                       "%s  (%s)", node.expr_path.c_str(), node.display_type.c_str());
    ImGui::PopFont();
    ImGui::Separator();

    if (ImGui::MenuItem("Arm Hardware Watchpoint (Break on Write)")) {
        ctx.engine->add_variable_watchpoint(node.expr_path, false, true);
    }
    if (ImGui::MenuItem("Arm Hardware Watchpoint (Read + Write)")) {
        ctx.engine->add_variable_watchpoint(node.expr_path, true, true);
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Inspect in Memory Bay (Struct Overlay)")) {
        ctx.inspect_in_memory(node.expr_path, 512);
    }
    if (node.is_pointer && node.pointer_target != 0) {
        if (ImGui::MenuItem("Chase Pointer Target in Memory Bay")) {
            ctx.inspect_in_memory(format_hex64_grouped(node.pointer_target), 512);
        }
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Open in Struct-Table View")) {
        ctx.open_in_struct_table(node.expr_path);
    }
    if (ImGui::MenuItem("Clip to Waveform Probe")) {
        ctx.pin_waveform(node.expr_path);
    }
    if (ImGui::MenuItem("Clip to 2D Pixel Probe (32x32 RGBA)")) {
        ctx.pin_pixel_probe(node.expr_path, 32, 32, 4);
    }
    if (ImGui::MenuItem("Clip to Pointer Graph Probe")) {
        ctx.pin_pointer_graph(node.expr_path);
    }
    ImGui::Separator();

    if (!is_watch_list) {
        if (ImGui::MenuItem("Pin to Watch")) {
            ctx.engine->add_watch_expr(node.expr_path);
        }
    } else {
        if (ImGui::MenuItem("Remove Watch")) {
            ctx.engine->remove_watch_expr(node.name);
        }
    }
    if (ImGui::MenuItem("Copy Value")) {
        ImGui::SetClipboardText(node.value.c_str());
    }
    if (node.address != 0 && ImGui::MenuItem("Copy Address")) {
        ImGui::SetClipboardText(format_hex64_grouped(node.address).c_str());
    }
    ImGui::EndPopup();
}

void draw_var_row(UiContext& ctx, const VarNode& node, int depth, bool is_watch_list) {
    const auto& pal = Theme::palette();
    ImGui::TableNextRow();

    if (node.changed) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                               (pal.osc_amber & 0x00FFFFFFu) | 0x22000000u);
    }

    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(node.expr_path.c_str());

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding;
    if (node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", node.name.c_str());
    ImGui::PopStyleVar();
    draw_var_context_menu(ctx, node, is_watch_list);

    // Column 1: Value + Provenance + Delta arrow
    ImGui::TableSetColumnIndex(1);
    if (node.is_pointer && node.provenance != ProvenanceKind::None) {
        draw_provenance_badge(node.provenance);
        ImGui::SameLine(0.0f, 6.0f);
    }
    bool is_string_summary = (!node.value.empty() && node.value.front() == '"');
    bool is_uninit = (node.value == "<uninitialized>" || node.value == "<unavailable>");

    if (is_uninit) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "%s", node.value.c_str());
    } else if (node.changed) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.osc_amber), "%s", node.value.c_str());
        if (ImGui::IsItemHovered()) {
            if (!node.prev_value.empty()) {
                ImGui::SetTooltip("Changed: %s  ->  %s", node.prev_value.c_str(), node.value.c_str());
            } else if (node.is_pointer && node.pointer_target != 0) {
                ImGui::SetTooltip("Address: %s", format_hex64_grouped(node.pointer_target).c_str());
            }
        }
    } else if (node.is_pointer && is_string_summary) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.syn_string), "%s", node.value.c_str());
        if (ImGui::IsItemHovered() && node.pointer_target != 0) {
            ImGui::SetTooltip("Address: %s\nRight-click to inspect in Memory Bay",
                              format_hex64_grouped(node.pointer_target).c_str());
        }
    } else if (node.is_pointer && node.pointer_target != 0 &&
               node.provenance != ProvenanceKind::Null &&
               node.provenance != ProvenanceKind::Unmapped) {
        if (hex_address_link("##ptr", node.pointer_target, true)) {
            ctx.inspect_in_memory(node.expr_path, 512);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to inspect %s in Memory Bay\nAddress: %s",
                              node.expr_path.c_str(),
                              format_hex64_grouped(node.pointer_target).c_str());
        }
    } else {
        ImGui::TextUnformatted(node.value.c_str());
    }

    // Column 2: Clean C++ Type (raw ABI type on hover)
    ImGui::TableSetColumnIndex(2);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.syn_type), "%s", node.display_type.c_str());
    if (ImGui::IsItemHovered() && node.raw_type != node.display_type) {
        ImGui::SetTooltip("Raw ABI type:\n%s", node.raw_type.c_str());
    }

    if (open && !node.children.empty()) {
        for (const auto& child : node.children) {
            draw_var_row(ctx, child, depth + 1, false);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_struct_table_tab(UiContext& ctx) {
    const auto& pal = Theme::palette();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 86.0f);
    bool submit = ImGui::InputTextWithHint("##TableExpr",
                                           "Container expr (e.g. particles, waveform)",
                                           ctx.table_expr_buf, sizeof(ctx.table_expr_buf),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("INSPECT", ImVec2(78.0f, 0.0f)) || submit) {
        ctx.engine->configure_struct_table(ctx.table_expr_buf);
    }

    StructTableData td = ctx.engine->struct_table();
    if (!td.error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "%s", td.error.c_str());
        return;
    }
    if (td.columns.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "Enter a vector/array expression or right-click any container -> Open in Struct-Table View.");
        return;
    }

    ImGui::PushFont(Theme::fonts().micro);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_secondary),
                       "%s  //  %s  [%llu rows]",
                       td.expr.c_str(), td.element_type.c_str(),
                       static_cast<unsigned long long>(td.total_count));
    ImGui::PopFont();

    ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##StructTable", static_cast<int>(td.columns.size()), tflags)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        for (const auto& col : td.columns) {
            ImGui::TableSetupColumn(col.c_str());
        }
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(td.rows.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                ImGui::TableNextRow();
                const auto& row = td.rows[r];
                bool has_prev = (r < static_cast<int>(td.prev_rows.size()));
                for (int c = 0; c < static_cast<int>(row.size()); ++c) {
                    ImGui::TableSetColumnIndex(c);
                    bool changed = has_prev &&
                                   c < static_cast<int>(td.prev_rows[r].size()) &&
                                   td.prev_rows[r][c] != row[c];
                    if (changed) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                               (pal.osc_amber & 0x00FFFFFFu) | 0x30000000u);
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.osc_amber),
                                           "%s", row[c].c_str());
                    } else if (c == 0) {
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                                           "%s", row[c].c_str());
                    } else {
                        ImGui::TextUnformatted(row[c].c_str());
                    }
                }
            }
        }
        ImGui::EndTable();
    }
}

} // namespace

void draw_inspector_bay(UiContext& ctx) {
    const auto& pal = Theme::palette();
    const auto& snap = *ctx.snap;

    if (ctx.focus_inspector_bay) {
        ImGui::SetNextWindowFocus();
        ctx.focus_inspector_bay = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("02 INSPECTOR", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    char meta[64];
    std::snprintf(meta, sizeof(meta), "%zu LOCALS  %zu WATCH",
                  snap.locals.size(), snap.watches.size());
    begin_bay_toolbar(meta, 30.0f);
    if (segmented_button("LOCALS", ctx.inspector_tab == InspectorTab::Locals, pal.signal_orange)) {
        ctx.inspector_tab = InspectorTab::Locals;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("WATCH", ctx.inspector_tab == InspectorTab::Watch, pal.signal_orange)) {
        ctx.inspector_tab = InspectorTab::Watch;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("TABLE", ctx.inspector_tab == InspectorTab::StructTable, pal.cobalt)) {
        ctx.inspector_tab = InspectorTab::StructTable;
    }
    end_bay_toolbar();

    ImGui::BeginChild("##InspectorBody", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (ctx.inspector_tab == InspectorTab::StructTable) {
        draw_struct_table_tab(ctx);
    } else {
        if (ctx.inspector_tab == InspectorTab::Watch) {
            static char watch_buf[256] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 66.0f);
            bool enter = ImGui::InputTextWithHint("##AddWatch", "Add C++ watch expression...",
                                                  watch_buf, sizeof(watch_buf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0.0f, 4.0f);
            if ((ImGui::Button("ADD", ImVec2(58.0f, 0.0f)) || enter) && watch_buf[0] != '\0') {
                ctx.engine->add_watch_expr(watch_buf);
                watch_buf[0] = '\0';
            }
            ImGui::Spacing();
        }

        const auto& list = (ctx.inspector_tab == InspectorTab::Locals) ? snap.locals : snap.watches;
        if (list.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                               ctx.inspector_tab == InspectorTab::Locals
                                   ? "No locals in current frame. Right-click variables for Watchpoints & Probes."
                                   : "No watches pinned. Add an expression above or right-click any local.");
        } else {
            ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##VarTable", 3, tflags)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch, 0.27f);
                ImGui::TableSetupColumn("VALUE", ImGuiTableColumnFlags_WidthStretch, 0.47f);
                ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthStretch, 0.26f);
                ImGui::TableHeadersRow();

                for (const auto& node : list) {
                    draw_var_row(ctx, node, 0, ctx.inspector_tab == InspectorTab::Watch);
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace scope::ui
