#include "UiContext.hpp"
#include "Theme.hpp"
#include "Widgets.hpp"
#include "../engine/TypeFormat.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

namespace scope::ui {

namespace {

ImU32 token_color(SyntaxTokenKind k, const Palette& pal) {
    switch (k) {
        case SyntaxTokenKind::Keyword:      return pal.syn_keyword;
        case SyntaxTokenKind::Type:         return pal.syn_type;
        case SyntaxTokenKind::Function:     return pal.syn_function;
        case SyntaxTokenKind::Field:        return pal.syn_field;
        case SyntaxTokenKind::Number:       return pal.syn_number;
        case SyntaxTokenKind::String:       return pal.syn_string;
        case SyntaxTokenKind::Comment:      return pal.syn_comment;
        case SyntaxTokenKind::Preprocessor: return pal.syn_preproc;
        case SyntaxTokenKind::Operator:     return pal.syn_operator;
        case SyntaxTokenKind::Namespace:    return pal.syn_namespace;
        default:                            return pal.text_primary;
    }
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string extract_word_at(const std::string& line, int col) {
    if (col < 0 || col >= static_cast<int>(line.size())) return "";
    if (!is_ident_char(line[col])) return "";
    int l = col;
    int r = col;
    while (l > 0 && is_ident_char(line[l - 1])) --l;
    while (r + 1 < static_cast<int>(line.size()) && is_ident_char(line[r + 1])) ++r;
    return line.substr(l, r - l + 1);
}

void draw_highlighted_line(ImDrawList* dl,
                           ImVec2 pos,
                           const std::string& text,
                           const std::vector<SyntaxSpan>& spans,
                           const Palette& pal) {
    if (text.empty()) return;
    float x = pos.x;
    uint32_t cursor = 0;
    const uint32_t len = static_cast<uint32_t>(text.size());

    auto emit = [&](uint32_t a, uint32_t b, ImU32 col) {
        if (b <= a || a >= len) return;
        b = std::min(b, len);
        const char* p0 = text.data() + a;
        const char* p1 = text.data() + b;
        dl->AddText(ImVec2(x, pos.y), col, p0, p1);
        x += ImGui::CalcTextSize(p0, p1).x;
    };

    for (const auto& sp : spans) {
        if (sp.start_col > cursor) {
            emit(cursor, sp.start_col, pal.text_primary);
        }
        if (sp.end_col > cursor) {
            uint32_t s = std::max(cursor, sp.start_col);
            emit(s, sp.end_col, token_color(sp.kind, pal));
            cursor = sp.end_col;
        }
    }
    if (cursor < len) {
        emit(cursor, len, pal.text_primary);
    }
}

void draw_pure_assembly(UiContext& ctx, const StepSnapshot& snap) {
    const auto& pal = Theme::palette();
    if (snap.disassembly.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "  No disassembly available (launch and stop target to inspect instructions).");
        return;
    }

    // Map address -> index for branch target margin arcs
    std::unordered_map<uint64_t, int> addr_to_idx;
    for (int i = 0; i < static_cast<int>(snap.disassembly.size()); ++i) {
        addr_to_idx[snap.disassembly[i].address] = i;
    }

    const float row_h = ImGui::GetTextLineHeight() + 6.0f;
    const float arc_margin = 34.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##AsmCanvas", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail_w = std::max(ImGui::GetContentRegionAvail().x, 680.0f);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(snap.disassembly.size()), row_h);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& di = snap.disassembly[i];
            ImVec2 rp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(avail_w, row_h));
            float y = rp.y;
            bool is_pc = (di.address == snap.active_pc);
            bool is_synced_line = (!is_pc && ctx.highlighted_source_line > 0 &&
                                   di.line == ctx.highlighted_source_line);

            if (is_pc) {
                dl->AddRectFilled(ImVec2(rp.x, y), ImVec2(rp.x + avail_w, y + row_h),
                                  (pal.signal_orange & 0x00FFFFFFu) | 0x26000000u);
                dl->AddRectFilled(ImVec2(rp.x, y), ImVec2(rp.x + 3.0f, y + row_h),
                                  pal.signal_orange);
            } else if (is_synced_line) {
                dl->AddRectFilled(ImVec2(rp.x, y), ImVec2(rp.x + avail_w, y + row_h),
                                  (pal.cobalt & 0x00FFFFFFu) | 0x1E000000u);
                dl->AddRectFilled(ImVec2(rp.x, y), ImVec2(rp.x + 3.0f, y + row_h),
                                  pal.cobalt);
            } else if (i % 2 == 1) {
                dl->AddRectFilled(ImVec2(rp.x, y), ImVec2(rp.x + avail_w, y + row_h),
                                  IM_COL32(255, 255, 255, 4));
            }

            // Branch arc hint
            if (di.is_branch && di.branch_target != 0) {
                auto it = addr_to_idx.find(di.branch_target);
                if (it != addr_to_idx.end()) {
                    float ty = y + static_cast<float>(it->second - i) * row_h + row_h * 0.5f;
                    float sy = y + row_h * 0.5f;
                    float lane_x = rp.x + 10.0f + static_cast<float>((i % 4) * 5);
                    ImU32 acol = (it->second < i) ? pal.osc_amber : pal.cobalt;
                    dl->AddLine(ImVec2(rp.x + arc_margin - 4.0f, sy), ImVec2(lane_x, sy), acol, 1.2f);
                    dl->AddLine(ImVec2(lane_x, sy), ImVec2(lane_x, ty), acol, 1.2f);
                    dl->AddLine(ImVec2(lane_x, ty), ImVec2(rp.x + arc_margin - 4.0f, ty), acol, 1.2f);
                }
            }

            // Address
            std::string addr_str = format_hex64_grouped(di.address);
            size_t sig = hex_significant_offset(addr_str);
            float x = rp.x + arc_margin + 6.0f;
            float ty = y + 3.0f;
            dl->AddText(ImVec2(x, ty), pal.text_dim, addr_str.c_str(), addr_str.c_str() + sig);
            x += ImGui::CalcTextSize(addr_str.c_str(), addr_str.c_str() + sig).x;
            dl->AddText(ImVec2(x, ty), is_pc ? pal.signal_orange : pal.cobalt, addr_str.c_str() + sig);

            // Mnemonic
            float mn_x = rp.x + arc_margin + 205.0f;
            ImU32 mn_col = di.is_branch ? pal.syn_keyword : pal.syn_function;
            dl->AddText(ImVec2(mn_x, ty), mn_col, di.mnemonic.c_str());

            // Operands
            float op_x = mn_x + 88.0f;
            dl->AddText(ImVec2(op_x, ty), pal.text_primary, di.operands.c_str());

            // Comment / Source line hint
            if (!di.comment.empty()) {
                dl->AddText(ImVec2(op_x + 250.0f, ty), pal.syn_comment, ("; " + di.comment).c_str());
            } else if (di.line > 0) {
                char lbuf[48];
                std::snprintf(lbuf, sizeof(lbuf), "; L%u", di.line);
                dl->AddText(ImVec2(op_x + 250.0f, ty), pal.text_dim, lbuf);
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace

void draw_code_canvas_bay(UiContext& ctx) {
    const auto& pal = Theme::palette();
    const auto& snap = *ctx.snap;

    // Auto-follow active file when step advances
    if (snap.step_index != ctx.last_seen_step && !snap.active_file.empty()) {
        ctx.last_seen_step = snap.step_index;
        if (!ctx.is_scrubbing_history()) {
            ctx.viewed_file = snap.active_file;
            ctx.scroll_to_line = snap.active_line;
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("01 SOURCE", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    std::string file_meta = ctx.viewed_file.empty()
                                ? "NO FILE LOADED"
                                : std::filesystem::path(ctx.viewed_file).filename().string();
    if (snap.active_pc != 0) {
        file_meta += "  PC " + format_hex64_grouped(snap.active_pc);
    }

    // Single unified Instrument Bay Toolbar (no duplicate header!)
    begin_bay_toolbar(file_meta.c_str(), 30.0f);
    if (segmented_button("SRC", ctx.code_mode == CodeViewMode::Source, pal.signal_orange)) {
        ctx.code_mode = CodeViewMode::Source;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("MIXED", ctx.code_mode == CodeViewMode::Mixed, pal.signal_orange)) {
        ctx.code_mode = CodeViewMode::Mixed;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (segmented_button("ASM", ctx.code_mode == CodeViewMode::Assembly, pal.signal_orange)) {
        ctx.code_mode = CodeViewMode::Assembly;
    }
    end_bay_toolbar();

    if (ctx.code_mode == CodeViewMode::Assembly) {
        draw_pure_assembly(ctx, snap);
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    if (ctx.viewed_file.empty()) {
        ImGui::SetCursorPos(ImVec2(24.0f, 96.0f));
        ImGui::PushFont(Theme::fonts().display);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_secondary),
                           "READY ON BENCH // SELECT A TARGET TO BEGIN");
        ImGui::PopFont();
        ImGui::SetCursorPosX(24.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "Use the target selector in the top deck or press Cmd+K to launch a C++ fixture.");
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    const HighlightedFile& hf = ctx.highlighter->get_file(ctx.viewed_file);
    if (!hf.loaded) {
        ImGui::SetCursorPos(ImVec2(16.0f, 70.0f));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.fault_crimson),
                           "Unable to read source file: %s", ctx.viewed_file.c_str());
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // Index breakpoints for this file
    std::string cur_base = std::filesystem::path(ctx.viewed_file).filename().string();
    std::unordered_map<uint32_t, const BreakpointInfo*> bp_by_line;
    for (const auto& bp : snap.breakpoints) {
        if (std::filesystem::path(bp.file).filename().string() == cur_base) {
            bp_by_line[bp.line] = &bp;
        }
    }

    // Index disassembly by line for MIXED mode
    std::unordered_map<uint32_t, std::vector<const DisasmInstruction*>> asm_by_line;
    if (ctx.code_mode == CodeViewMode::Mixed) {
        for (const auto& di : snap.disassembly) {
            if (di.line > 0 && std::filesystem::path(di.file).filename().string() == cur_base) {
                asm_by_line[di.line].push_back(&di);
            }
        }
    }

    // Map local variable names -> compact inline ghost annotation (skip uninitialized/error values)
    std::unordered_map<std::string, std::pair<std::string, bool>> local_vals;
    for (const auto& v : snap.locals) {
        if (v.name.empty() || v.value.empty()) continue;
        if (v.value == "<uninitialized>" || v.value == "<unavailable>" ||
            v.value.find("error:") != std::string::npos) {
            continue;
        }
        local_vals[v.name] = {v.value, v.changed};
    }

    const float line_h = ImGui::GetTextLineHeight() + 6.0f;
    const float gutter_w = 66.0f;
    const float char_w = ImGui::CalcTextSize("M").x;
    bool same_as_active = (std::filesystem::path(snap.active_file).filename().string() == cur_base);

    // State for right-click breakpoint editor & hover eval
    static int32_t s_edit_bp_id = -1;
    static char s_bp_cond_buf[256] = "";
    static char s_bp_log_buf[256] = "";
    static std::string s_last_hover_word;
    static double s_hover_since = 0.0;

    // Zero vertical item spacing so gutter, PC highlight, and divider line are 100% seamless!
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##SourceCanvas", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    // Scroll to active line when requested
    if (ctx.scroll_to_line > 0 && ctx.scroll_to_line <= hf.lines.size()) {
        float target_y = std::max(0.0f, (static_cast<float>(ctx.scroll_to_line) - 8.0f) * line_h);
        ImGui::SetScrollY(target_y);
        ctx.scroll_to_line = 0;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 win_pos = ImGui::GetWindowPos();
    float avail_w = std::max(ImGui::GetContentRegionAvail().x, 960.0f);
    ImVec2 mouse = ImGui::GetMousePos();
    bool canvas_hovered = ImGui::IsWindowHovered();

    std::string hovered_ident;

    if (ctx.code_mode == CodeViewMode::Source) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(hf.lines.size()), line_h);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                uint32_t line_no = static_cast<uint32_t>(i + 1);
                ImVec2 row_pos = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(avail_w, line_h));

                bool is_exec = same_as_active && (snap.active_line == line_no) &&
                               (snap.state == SessionState::Stopped || snap.state == SessionState::Fault);
                bool is_top_frame = (snap.selected_frame_idx == 0);

                // Gutter background
                dl->AddRectFilled(row_pos, ImVec2(row_pos.x + gutter_w, row_pos.y + line_h), pal.bg_chassis);
                dl->AddLine(ImVec2(row_pos.x + gutter_w, row_pos.y),
                            ImVec2(row_pos.x + gutter_w, row_pos.y + line_h), pal.border, 1.0f);

                // Active execution line highlight
                if (is_exec) {
                    ImU32 hl = is_top_frame ? pal.signal_orange : pal.cobalt;
                    dl->AddRectFilled(ImVec2(row_pos.x + gutter_w + 1.0f, row_pos.y),
                                      ImVec2(row_pos.x + avail_w, row_pos.y + line_h),
                                      (hl & 0x00FFFFFFu) | 0x24000000u);
                    dl->AddRectFilled(ImVec2(row_pos.x, row_pos.y),
                                      ImVec2(row_pos.x + 3.0f, row_pos.y + line_h), hl);
                    // Crisp PC arrow in gutter
                    float cy = row_pos.y + line_h * 0.5f;
                    dl->AddTriangleFilled(ImVec2(row_pos.x + gutter_w - 8.0f, cy - 4.0f),
                                          ImVec2(row_pos.x + gutter_w - 8.0f, cy + 4.0f),
                                          ImVec2(row_pos.x + gutter_w - 2.0f, cy), hl);
                }

                // Breakpoint glyph
                auto bpit = bp_by_line.find(line_no);
                if (bpit != bp_by_line.end()) {
                    const BreakpointInfo* bp = bpit->second;
                    ImVec2 bc(row_pos.x + 14.0f, row_pos.y + line_h * 0.5f);
                    if (!bp->log_message.empty()) {
                        dl->AddCircle(bc, 4.8f, pal.cobalt, 16, 1.8f);
                    } else if (!bp->condition.empty()) {
                        draw_icon(dl, IconKind::Watchpoint, bc, 5.5f, pal.osc_amber);
                    } else {
                        dl->AddCircleFilled(bc, 4.8f, pal.fault_crimson);
                    }
                }

                // Line number
                char lbuf[16];
                std::snprintf(lbuf, sizeof(lbuf), "%4u", line_no);
                dl->AddText(ImVec2(row_pos.x + 22.0f, row_pos.y + 2.5f),
                            is_exec ? pal.text_primary : pal.text_dim, lbuf);

                // Gutter click (toggle BP) / right-click (edit condition/logpoint)
                bool gutter_hit = canvas_hovered &&
                                  mouse.x >= row_pos.x && mouse.x <= row_pos.x + gutter_w &&
                                  mouse.y >= row_pos.y && mouse.y < row_pos.y + line_h;
                if (gutter_hit) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        ctx.engine->toggle_breakpoint(ctx.viewed_file, line_no);
                    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && bpit != bp_by_line.end()) {
                        s_edit_bp_id = bpit->second->id;
                        std::snprintf(s_bp_cond_buf, sizeof(s_bp_cond_buf), "%s",
                                      bpit->second->condition.c_str());
                        std::snprintf(s_bp_log_buf, sizeof(s_bp_log_buf), "%s",
                                      bpit->second->log_message.c_str());
                        ImGui::OpenPopup("##EditBreakpointPopup");
                    }
                }

                // Source line with Tree-sitter syntax spans
                ImVec2 text_pos(row_pos.x + gutter_w + 12.0f, row_pos.y + 2.5f);
                draw_highlighted_line(dl, text_pos, hf.lines[i], hf.line_spans[i], pal);

                // Inline ghost variable values ONLY on already-executed lines above the current PC
                if (same_as_active && !local_vals.empty() &&
                    line_no < snap.active_line && (snap.active_line - line_no) <= 12) {
                    std::string ghost;
                    bool any_changed = false;
                    // Find locals mentioned on this line
                    for (const auto& [vname, vpair] : local_vals) {
                        if (vname.size() < 2) continue;
                        size_t pos = hf.lines[i].find(vname);
                        if (pos != std::string::npos) {
                            bool left_ok = (pos == 0) || !is_ident_char(hf.lines[i][pos - 1]);
                            bool right_ok = (pos + vname.size() >= hf.lines[i].size()) ||
                                            !is_ident_char(hf.lines[i][pos + vname.size()]);
                            if (left_ok && right_ok) {
                                if (!ghost.empty()) ghost += "  ";
                                std::string val_short = vpair.first;
                                if (val_short.size() > 26) val_short = val_short.substr(0, 26) + "..";
                                ghost += vname + "=" + val_short;
                                any_changed = any_changed || vpair.second;
                                if (ghost.size() > 60) break;
                            }
                        }
                    }
                    if (!ghost.empty()) {
                        float line_w = ImGui::CalcTextSize(hf.lines[i].c_str()).x;
                        float gx = text_pos.x + line_w + 24.0f;
                        ImU32 gcol = any_changed ? pal.osc_amber : pal.text_secondary;
                        ImGui::PushFont(Theme::fonts().micro);
                        ImVec2 gsz = ImGui::CalcTextSize(ghost.c_str());
                        float gy = row_pos.y + (line_h - gsz.y) * 0.5f;
                        dl->AddRectFilled(ImVec2(gx - 6.0f, row_pos.y + 2.0f),
                                          ImVec2(gx + gsz.x + 6.0f, row_pos.y + line_h - 2.0f),
                                          (gcol & 0x00FFFFFFu) | 0x1A000000u, 1.0f);
                        dl->AddRect(ImVec2(gx - 6.0f, row_pos.y + 2.0f),
                                    ImVec2(gx + gsz.x + 6.0f, row_pos.y + line_h - 2.0f),
                                    (gcol & 0x00FFFFFFu) | 0x45000000u, 1.0f);
                        dl->AddText(ImVec2(gx, gy), gcol, ghost.c_str());
                        ImGui::PopFont();
                    }
                }

                // Click source code row to sync-highlight scattered -O2/-O3 instructions in ASM mode
                if (canvas_hovered &&
                    mouse.x >= text_pos.x && mouse.y >= row_pos.y && mouse.y < row_pos.y + line_h) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        ctx.highlighted_source_line = line_no;
                    }
                    int col = static_cast<int>((mouse.x - text_pos.x) / std::max(1.0f, char_w));
                    hovered_ident = extract_word_at(hf.lines[i], col);
                }
            }
        }
    } else {
        // MIXED MODE: Source lines interleaved with corresponding ARM64/x86_64 instructions
        for (int i = 0; i < static_cast<int>(hf.lines.size()); ++i) {
            uint32_t line_no = static_cast<uint32_t>(i + 1);
            ImVec2 row_pos = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(avail_w, line_h));

            bool is_exec = same_as_active && (snap.active_line == line_no) &&
                           (snap.state == SessionState::Stopped || snap.state == SessionState::Fault);
            dl->AddRectFilled(row_pos, ImVec2(row_pos.x + gutter_w, row_pos.y + line_h), pal.bg_chassis);
            dl->AddLine(ImVec2(row_pos.x + gutter_w, row_pos.y),
                        ImVec2(row_pos.x + gutter_w, row_pos.y + line_h), pal.border, 1.0f);
            if (is_exec) {
                dl->AddRectFilled(ImVec2(row_pos.x + gutter_w + 1.0f, row_pos.y),
                                  ImVec2(row_pos.x + avail_w, row_pos.y + line_h),
                                  (pal.signal_orange & 0x00FFFFFFu) | 0x1C000000u);
            }
            char lbuf[16];
            std::snprintf(lbuf, sizeof(lbuf), "%4u", line_no);
            dl->AddText(ImVec2(row_pos.x + 22.0f, row_pos.y + 2.5f), pal.text_dim, lbuf);
            draw_highlighted_line(dl, ImVec2(row_pos.x + gutter_w + 12.0f, row_pos.y + 2.5f),
                                  hf.lines[i], hf.line_spans[i], pal);

            // Interleaved instructions for this source line
            auto ait = asm_by_line.find(line_no);
            if (ait != asm_by_line.end()) {
                ImGui::PushFont(Theme::fonts().micro);
                float asm_h = ImGui::GetTextLineHeight() + 3.0f;
                for (const DisasmInstruction* di : ait->second) {
                    ImVec2 ap = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(avail_w, asm_h));
                    bool is_pc = (di->address == snap.active_pc);
                    dl->AddRectFilled(ImVec2(ap.x + gutter_w + 1.0f, ap.y),
                                      ImVec2(ap.x + avail_w, ap.y + asm_h),
                                      is_pc ? ((pal.signal_orange & 0x00FFFFFFu) | 0x30000000u)
                                            : pal.bg_chassis);
                    if (is_pc) {
                        dl->AddRectFilled(ImVec2(ap.x + gutter_w + 1.0f, ap.y),
                                          ImVec2(ap.x + gutter_w + 4.0f, ap.y + asm_h),
                                          pal.signal_orange);
                    }
                    float ax = ap.x + gutter_w + 22.0f;
                    std::string addr_s = format_hex64_grouped(di->address);
                    dl->AddText(ImVec2(ax, ap.y + 1.5f), is_pc ? pal.signal_orange : pal.text_dim,
                                addr_s.c_str());
                    dl->AddText(ImVec2(ax + 150.0f, ap.y + 1.5f), pal.syn_function,
                                di->mnemonic.c_str());
                    dl->AddText(ImVec2(ax + 220.0f, ap.y + 1.5f), pal.text_secondary,
                                di->operands.c_str());
                }
                ImGui::PopFont();
            }
        }
    }

    // Hover Expression Popover (debounced 220ms)
    if (!hovered_ident.empty() &&
        (snap.state == SessionState::Stopped || snap.state == SessionState::Fault)) {
        double now = ImGui::GetTime();
        if (hovered_ident != s_last_hover_word) {
            s_last_hover_word = hovered_ident;
            s_hover_since = now;
        } else if (now - s_hover_since > 0.22) {
            HoverEvalResult hr = ctx.engine->hover_result();
            if (hr.expr != hovered_ident) {
                ctx.engine->eval_hover(hovered_ident);
            } else if (hr.valid) {
                ImGui::BeginTooltip();
                ImGui::PushFont(Theme::fonts().bold);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange),
                                   "%s", hr.root.name.c_str());
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.syn_type),
                                   "%s", hr.root.display_type.c_str());
                if (hr.root.is_pointer && hr.root.provenance != ProvenanceKind::None) {
                    ImGui::SameLine();
                    draw_provenance_badge(hr.root.provenance);
                }
                ImGui::Separator();
                ImGui::TextUnformatted(hr.root.value.c_str());
                for (size_t k = 0; k < std::min<size_t>(hr.root.children.size(), 8); ++k) {
                    const auto& c = hr.root.children[k];
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_secondary),
                                       "  .%s = %s", c.name.c_str(), c.value.c_str());
                }
                ImGui::EndTooltip();
            }
        }
    } else {
        s_last_hover_word.clear();
    }

    // Breakpoint Condition / Logpoint Popup
    if (ImGui::BeginPopup("##EditBreakpointPopup")) {
        ImGui::PushFont(Theme::fonts().micro);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange),
                           "BREAKPOINT #%d CONFIG", s_edit_bp_id);
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##BpCond", "Condition (e.g. i == 16)", s_bp_cond_buf, sizeof(s_bp_cond_buf));
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##BpLog", "Logpoint message (non-stopping)", s_bp_log_buf, sizeof(s_bp_log_buf));
        if (ImGui::Button("APPLY")) {
            ctx.engine->set_breakpoint_condition(s_edit_bp_id, s_bp_cond_buf);
            ctx.engine->set_breakpoint_log(s_edit_bp_id, s_bp_log_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("DELETE")) {
            ctx.engine->remove_breakpoint(s_edit_bp_id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    (void)win_pos;
    ImGui::EndChild();
    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
}

} // namespace scope::ui
