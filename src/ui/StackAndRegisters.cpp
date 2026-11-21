#include "UiContext.hpp"
#include "Theme.hpp"
#include "Widgets.hpp"
#include "../engine/TypeFormat.hpp"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace scope::ui {

namespace {

std::string format_simd_lanes(const std::vector<uint8_t>& bytes, int lane_mode) {
    if (bytes.empty()) return "";
    std::string out;
    char buf[48];
    const size_t n = bytes.size();
    if (lane_mode == 0) { // f32
        for (size_t i = 0; i + 4 <= n; i += 4) {
            float v = 0.0f;
            std::memcpy(&v, bytes.data() + i, 4);
            std::snprintf(buf, sizeof(buf), "%s%9.4g", (i > 0 ? " " : ""), v);
            out += buf;
        }
    } else if (lane_mode == 1) { // f64
        for (size_t i = 0; i + 8 <= n; i += 8) {
            double v = 0.0;
            std::memcpy(&v, bytes.data() + i, 8);
            std::snprintf(buf, sizeof(buf), "%s%13.6g", (i > 0 ? "  " : ""), v);
            out += buf;
        }
    } else if (lane_mode == 2) { // i32
        for (size_t i = 0; i + 4 <= n; i += 4) {
            int32_t v = 0;
            std::memcpy(&v, bytes.data() + i, 4);
            std::snprintf(buf, sizeof(buf), "%s%10d", (i > 0 ? " " : ""), v);
            out += buf;
        }
    } else if (lane_mode == 3) { // i64
        for (size_t i = 0; i + 8 <= n; i += 8) {
            int64_t v = 0;
            std::memcpy(&v, bytes.data() + i, 8);
            std::snprintf(buf, sizeof(buf), "%s%16lld", (i > 0 ? "  " : ""),
                          static_cast<long long>(v));
            out += buf;
        }
    } else { // u8 hex + ascii preview
        for (size_t i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof(buf), "%s%02x", (i > 0 ? " " : ""), bytes[i]);
            out += buf;
        }
        out += " |";
        for (size_t i = 0; i < n; ++i) {
            char c = (bytes[i] >= 32 && bytes[i] < 127) ? static_cast<char>(bytes[i]) : '.';
            out.push_back(c);
        }
        out.push_back('|');
    }
    return out;
}

} // namespace

void draw_stack_thread_bay(UiContext& ctx) {
    const auto& pal = Theme::palette();
    const auto& snap = *ctx.snap;

    static int s_tab = 0; // 0=STACK, 1=THREADS, 2=BP/WP
    static bool s_fold_runtime = true;

    // Lazy background thread unwinding: only unwind all threads when Thread Matrix tab is open!
    ctx.engine->set_expand_all_threads(s_tab == 1);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("04 STACK & THREADS", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    char meta[48];
    std::snprintf(meta, sizeof(meta), "%zuT %zuB %zuW",
                  snap.threads.size(), snap.breakpoints.size(), snap.watchpoints.size());
    begin_bay_toolbar(meta, 30.0f);
    if (segmented_button("STACK", s_tab == 0, pal.signal_orange)) s_tab = 0;
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("TH", s_tab == 1, pal.signal_orange)) s_tab = 1;
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("BP/WP", s_tab == 2, pal.osc_amber)) s_tab = 2;

    if (s_tab == 0) {
        ImGui::SameLine(0.0f, 6.0f);
        if (segmented_button("FOLD", s_fold_runtime, pal.cobalt)) {
            s_fold_runtime = !s_fold_runtime;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(s_fold_runtime ? "Runtime/stdlib frames folded (click to expand)"
                                             : "Showing all frames (click to fold runtime)");
        }
    }
    end_bay_toolbar();

    ImGui::BeginChild("##StackBody", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    if (s_tab == 0) {
        const ThreadInfo* active_th = nullptr;
        for (const auto& t : snap.threads) {
            if (t.id == snap.selected_thread_id) {
                active_th = &t;
                break;
            }
        }
        if (!active_th || active_th->frames.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                               "No active stack frames.");
        } else {
            size_t i = 0;
            while (i < active_th->frames.size()) {
                const auto& fr = active_th->frames[i];
                if (s_fold_runtime && fr.is_runtime) {
                    size_t j = i;
                    while (j < active_th->frames.size() && active_th->frames[j].is_runtime) {
                        ++j;
                    }
                    size_t hidden = j - i;
                    ImGui::PushFont(Theme::fonts().micro);
                    char flbl[64];
                    std::snprintf(flbl, sizeof(flbl), "  + %zu runtime / stdlib frames", hidden);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_secondary));
                    if (ImGui::Selectable(flbl)) {
                        s_fold_runtime = false;
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    i = j;
                    continue;
                }

                bool is_sel = (fr.index == snap.selected_frame_idx);
                std::string fname = fr.file.empty()
                                        ? fr.module
                                        : std::filesystem::path(fr.file).filename().string();
                char row_lbl[320];
                std::snprintf(row_lbl, sizeof(row_lbl), "#%02u%s %s (%s:%u)##fr%u",
                              fr.index, fr.is_inlined ? " [INL]" : "",
                              fr.display_function.c_str(), fname.c_str(),
                              fr.line, fr.index);

                ImU32 fg = is_sel ? pal.signal_orange
                                  : (fr.is_inlined ? pal.cobalt
                                                   : (fr.is_runtime ? pal.text_dim : pal.text_primary));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(fg));
                if (ImGui::Selectable(row_lbl, is_sel)) {
                    ctx.engine->select_frame(fr.index);
                    if (!fr.file.empty()) {
                        ctx.jump_to_source(fr.file, fr.line);
                    }
                }
                ImGui::PopStyleColor();
                ++i;
            }
        }
    } else if (s_tab == 1) {
        // Thread Matrix
        if (snap.threads.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "No active threads.");
        } else {
            ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
            if (ImGui::BeginTable("##ThreadMatrix", 4, tf)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 34.0f);
                ImGui::TableSetupColumn("THREAD", ImGuiTableColumnFlags_WidthStretch, 0.30f);
                ImGui::TableSetupColumn("STOP REASON", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("TOP USER FRAME", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableHeadersRow();

                for (const auto& t : snap.threads) {
                    ImGui::TableNextRow();
                    if (t.is_selected) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               (pal.signal_orange & 0x00FFFFFFu) | 0x22000000u);
                    }
                    ImGui::TableSetColumnIndex(0);
                    char idbuf[32];
                    std::snprintf(idbuf, sizeof(idbuf), "T%u##th%llu",
                                  t.index_id, static_cast<unsigned long long>(t.id));
                    if (ImGui::Selectable(idbuf, t.is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        ctx.engine->select_thread(t.id);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(t.name.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImU32 rcol = (t.stop_reason == "BREAKPOINT" || t.stop_reason == "WATCHPOINT")
                                     ? pal.osc_amber
                                     : (t.stop_reason == "SIGNAL" || t.stop_reason == "EXCEPTION"
                                            ? pal.fault_crimson
                                            : pal.text_secondary);
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rcol), "%s", t.stop_reason.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(t.top_user_function.c_str());
                }
                ImGui::EndTable();
            }
        }
    } else {
        // Breakpoints & Hardware Watchpoints (with Watchpoint Conditions!)
        static int32_t s_edit_wp_id = -1;
        static char s_wp_cond_buf[256] = "";

        ImGui::PushFont(Theme::fonts().micro);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.osc_amber),
                           "HARDWARE WATCHPOINTS (%zu / 4)", snap.watchpoints.size());
        ImGui::PopFont();
        ImGui::Separator();
        if (snap.watchpoints.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                               "None armed. Right-click any variable or memory byte -> Arm HW Watchpoint.");
        } else {
            for (const auto& wp : snap.watchpoints) {
                ImGui::PushID(wp.id + 10000);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.osc_amber),
                                   "WP#%d", wp.id);
                ImGui::SameLine();
                ImGui::Text("%s (%lluB, h=%u)", wp.label.c_str(),
                            static_cast<unsigned long long>(wp.byte_size), wp.hit_count);
                if (!wp.condition.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.cobalt),
                                       "[if %s]", wp.condition.c_str());
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("COND")) {
                    s_edit_wp_id = wp.id;
                    std::snprintf(s_wp_cond_buf, sizeof(s_wp_cond_buf), "%s", wp.condition.c_str());
                    ImGui::OpenPopup("##WpCondPop");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("DEL")) {
                    ctx.engine->remove_watchpoint(wp.id);
                }
                if (ImGui::BeginPopup("##WpCondPop")) {
                    ImGui::SetNextItemWidth(240.0f);
                    bool enter = ImGui::InputTextWithHint("##WpCond", "Condition (e.g. bid >= ask)",
                                                          s_wp_cond_buf, sizeof(s_wp_cond_buf),
                                                          ImGuiInputTextFlags_EnterReturnsTrue);
                    if (ImGui::Button("SET") || enter) {
                        ctx.engine->set_watchpoint_condition(s_edit_wp_id, s_wp_cond_buf);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        ImGui::PushFont(Theme::fonts().micro);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.fault_crimson),
                           "BREAKPOINTS (%zu)", snap.breakpoints.size());
        ImGui::PopFont();
        ImGui::Separator();
        if (snap.breakpoints.empty()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                               "Click the source gutter to toggle line breakpoints.");
        } else {
            for (const auto& bp : snap.breakpoints) {
                ImGui::PushID(bp.id);
                std::string fn = std::filesystem::path(bp.file).filename().string();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.fault_crimson),
                                   "BP#%d", bp.id);
                ImGui::SameLine();
                if (ImGui::Selectable((fn + ":" + std::to_string(bp.line)).c_str(),
                                      false, 0, ImVec2(140.0f, 0.0f))) {
                    ctx.jump_to_source(bp.file, bp.line);
                }
                if (!bp.condition.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.osc_amber),
                                       "[if %s]", bp.condition.c_str());
                }
                if (!bp.log_message.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.cobalt),
                                       "[log]");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("DEL")) {
                    ctx.engine->remove_breakpoint(bp.id);
                }
                ImGui::PopID();
            }
        }
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

void draw_registers_bay(UiContext& ctx) {
    const auto& pal = Theme::palette();
    const auto& snap = *ctx.snap;

    static int s_reg_mode = 0; // 0=GENERAL, 1=SIMD, 2=ALL
    static int s_simd_lane = 0; // 0=f32, 1=f64, 2=i32, 3=i64, 4=u8

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("05 REGISTERS", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // CPSR / RFLAGS bit-flag LED bar if present
    const RegisterInfo* flags_reg = nullptr;
    for (const auto& r : snap.registers) {
        if (r.name == "cpsr" || r.name == "rflags" || r.name == "eflags") {
            flags_reg = &r;
            break;
        }
    }

    char meta[48];
    std::snprintf(meta, sizeof(meta), "%zu REGS", snap.registers.size());
    begin_bay_toolbar(meta, 30.0f);
    if (segmented_button("GEN", s_reg_mode == 0, pal.signal_orange)) s_reg_mode = 0;
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("SIMD", s_reg_mode == 1, pal.phosphor_green)) s_reg_mode = 1;
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("ALL", s_reg_mode == 2, pal.cobalt)) s_reg_mode = 2;

    if (s_reg_mode == 1) {
        ImGui::SameLine(0.0f, 10.0f);
        static const char* kLaneLabels[5] = {"F32", "F64", "I32", "I64", "U8"};
        for (int k = 0; k < 5; ++k) {
            if (k > 0) ImGui::SameLine(0.0f, 2.0f);
            if (segmented_button(kLaneLabels[k], s_simd_lane == k, pal.cobalt)) {
                s_simd_lane = k;
            }
        }
    } else if (flags_reg) {
        ImGui::SameLine(0.0f, 10.0f);
        if (flags_reg->name == "cpsr") {
            const char* names[4] = {"N", "Z", "C", "V"};
            int bits[4] = {31, 30, 29, 28};
            for (int k = 0; k < 4; ++k) {
                bool set = ((flags_reg->value_u64 >> bits[k]) & 1ull) != 0;
                if (k > 0) ImGui::SameLine(0.0f, 2.0f);
                segmented_button(names[k], set, pal.phosphor_green);
            }
        } else {
            const char* names[4] = {"CF", "ZF", "SF", "OF"};
            int bits[4] = {0, 6, 7, 11};
            for (int k = 0; k < 4; ++k) {
                bool set = ((flags_reg->value_u64 >> bits[k]) & 1ull) != 0;
                if (k > 0) ImGui::SameLine(0.0f, 2.0f);
                segmented_button(names[k], set, pal.phosphor_green);
            }
        }
    }
    end_bay_toolbar();

    ImGui::BeginChild("##RegList", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (snap.registers.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "No register data (stop target to inspect CPU state).");
    } else {
        ImGuiTableFlags tf = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##RegsTable", 2, tf)) {
            ImGui::TableSetupColumn("REG", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn(s_reg_mode == 1 ? "SIMD LANES" : "HEX VALUE",
                                    ImGuiTableColumnFlags_WidthStretch);
            for (const auto& r : snap.registers) {
                if (s_reg_mode == 0 && r.group.find("General") == std::string::npos) continue;
                if (s_reg_mode == 1 && r.raw_bytes.empty()) continue;

                ImGui::TableNextRow();
                if (r.changed) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           (pal.osc_amber & 0x00FFFFFFu) | 0x24000000u);
                }
                ImGui::TableSetColumnIndex(0);
                ImU32 ncol = (r.name == "pc") ? pal.signal_orange
                                              : (r.changed ? pal.osc_amber : pal.text_secondary);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ncol), "%s", r.name.c_str());

                ImGui::TableSetColumnIndex(1);
                if (s_reg_mode == 1 && !r.raw_bytes.empty()) {
                    std::string lanes = format_simd_lanes(r.raw_bytes, s_simd_lane);
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(
                                           r.changed ? pal.osc_amber : pal.phosphor_green),
                                       "%s", lanes.c_str());
                } else if (r.byte_size <= 8) {
                    ImGui::PushID(r.name.c_str());
                    if (hex_address_link("##regaddr", r.value_u64, r.value_u64 > 0x1000)) {
                        ctx.inspect_in_memory(format_hex64_grouped(r.value_u64), 256);
                    }
                    ImGui::PopID();
                } else {
                    ImGui::TextUnformatted(r.value_hex.c_str());
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace scope::ui
