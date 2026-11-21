#include "UiContext.hpp"
#include "Theme.hpp"
#include "Widgets.hpp"
#include "../engine/TypeFormat.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace scope::ui {

float draw_transport_and_timeline(UiContext& ctx, float viewport_w, float start_y) {
    const auto& pal = Theme::palette();
    const auto& snap = *ctx.snap;

    bool has_crash = snap.crash.is_crash;
    bool has_history = !ctx.history.empty();

    float bar_h = 40.0f;
    float timeline_h = has_history ? 34.0f : 0.0f;
    float crash_h = has_crash ? 34.0f : 0.0f;
    float total_h = bar_h + timeline_h + crash_h;

    ImGui::SetNextWindowPos(ImVec2(0.0f, start_y));
    ImGui::SetNextWindowSize(ImVec2(viewport_w, total_h));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.bg_bezel));
    ImGui::Begin("##TransportDeck", nullptr, flags);

    // 1) Brand Instrument Stamp (all top-deck widgets aligned to bar_h=40 centerline)
    ImGui::SetCursorPos(ImVec2(10.0f, 11.0f));
    ImGui::PushFont(Theme::fonts().display);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange), "SCOPE");
    ImGui::PopFont();
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::PushFont(Theme::fonts().micro);
    ImGui::SetCursorPosY(14.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "//01");
    ImGui::PopFont();

    ImGui::SameLine(0.0f, 14.0f);
    ImGui::SetCursorPosY(9.0f);

    // 2) Target Quick-Launch Combo
    std::string target_label = snap.target_path.empty()
                                   ? "SELECT TARGET..."
                                   : std::filesystem::path(snap.target_path).filename().string();
    ImGui::SetNextItemWidth(175.0f);
    if (ImGui::BeginCombo("##TargetQuick", target_label.c_str())) {
        ImGui::PushFont(Theme::fonts().micro);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim), "TEST FIXTURES");
        ImGui::PopFont();
        ImGui::Separator();
        for (const auto& fpath : ctx.discovered_fixtures) {
            std::string fname = std::filesystem::path(fpath).filename().string();
            bool selected = (fpath == snap.target_path);
            if (ImGui::Selectable(fname.c_str(), selected)) {
                ctx.history_cursor = -1;
                ctx.engine->launch(fpath, {}, "", true);
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("Launch Custom Binary...")) {
            ctx.open_launch_modal = true;
        }
        if (ImGui::Selectable("Attach to Running PID...")) {
            ctx.open_attach_modal = true;
        }
        if (ImGui::Selectable("Open Core Dump (Post-Mortem)...")) {
            ctx.open_core_modal = true;
        }
        if (ImGui::Selectable("Connect Remote (lldb-server / rr)...")) {
            ctx.open_remote_modal = true;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0.0f, 10.0f);

    // 3) Hardware Execution Controls (26px tall -> y=7)
    bool is_stopped = (snap.state == SessionState::Stopped || snap.state == SessionState::Fault);
    bool is_running = (snap.state == SessionState::Running);
    bool has_target = !snap.target_path.empty();

    ImGui::SetCursorPosY(7.0f);
    if (is_running) {
        if (transport_button("##Pause", IconKind::Pause, "Pause (F6)", true, pal.osc_amber)) {
            ctx.engine->pause_exec();
        }
    } else {
        if (transport_button("##Continue", IconKind::Play, "Continue (F5)", is_stopped, pal.phosphor_green)) {
            ctx.history_cursor = -1;
            ctx.engine->continue_exec();
        }
    }
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::SetCursorPosY(7.0f);
    if (transport_button("##StepOver", IconKind::StepOver, "Step Over (F10)", is_stopped, pal.signal_orange)) {
        ctx.history_cursor = -1;
        ctx.engine->step_over();
    }
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::SetCursorPosY(7.0f);
    if (transport_button("##StepInto", IconKind::StepInto, "Step Into (F11)", is_stopped, pal.cobalt)) {
        ctx.history_cursor = -1;
        ctx.engine->step_into();
    }
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::SetCursorPosY(7.0f);
    if (transport_button("##StepOut", IconKind::StepOut, "Step Out (Shift+F11)", is_stopped, pal.cobalt)) {
        ctx.history_cursor = -1;
        ctx.engine->step_out();
    }
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::SetCursorPosY(7.0f);
    if (transport_button("##StepAsm", IconKind::StepAsm, "Step Instruction (Cmd+Shift+F10)", is_stopped, pal.text_primary)) {
        ctx.history_cursor = -1;
        ctx.engine->step_instruction();
    }
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::SetCursorPosY(7.0f);
    if (transport_button("##Restart", IconKind::Restart, "Restart Target (Cmd+R)", has_target, pal.text_primary)) {
        ctx.history_cursor = -1;
        ctx.engine->restart();
    }
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::SetCursorPosY(7.0f);
    if (transport_button("##Stop", IconKind::Stop, "Terminate Target", is_stopped || is_running, pal.fault_crimson)) {
        ctx.history_cursor = -1;
        ctx.engine->terminate();
    }

    // Optional Reverse-Execution buttons when attached to rr / gdb-remote
    if (snap.is_remote && is_stopped) {
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::SetCursorPosY(9.0f);
        if (segmented_button("<< REV", false, pal.purple_hint)) {
            ctx.history_cursor = -1;
            ctx.engine->reverse_continue();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reverse Continue (rr gdb-remote)");
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::SetCursorPosY(9.0f);
        if (segmented_button("< RSTEP", false, pal.purple_hint)) {
            ctx.history_cursor = -1;
            ctx.engine->reverse_step_over();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reverse Step (rr gdb-remote)");
    }

    // 4) Live Status LED + Location Telemetry (24px tall -> y=8)
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::SetCursorPosY(8.0f);
    char extra[112] = "";
    const char* mode_tag = snap.is_core_dump ? "[CORE] " : (snap.is_remote ? "[REM] " : "");
    if (is_stopped && !snap.active_file.empty()) {
        std::string fname = std::filesystem::path(snap.active_file).filename().string();
        std::snprintf(extra, sizeof(extra), "%s%s:%u", mode_tag, fname.c_str(), snap.active_line);
    } else if (snap.state == SessionState::Exited) {
        std::snprintf(extra, sizeof(extra), "CODE %d", snap.exit_code);
    } else if (snap.pid != 0) {
        std::snprintf(extra, sizeof(extra), "%sPID %llu", mode_tag, static_cast<unsigned long long>(snap.pid));
    }
    draw_status_led(snap.state, extra);

    // 5) Right-aligned Instrument Gauges: Watchpoint Slots + Cmd+K + Theme Toggle
    float right_group_w = 278.0f;
    float cur_x = ImGui::GetCursorPosX();
    if (viewport_w - right_group_w > cur_x + 12.0f) {
        ImGui::SameLine(viewport_w - right_group_w);
    } else {
        ImGui::SameLine(0.0f, 12.0f);
    }
    ImGui::SetCursorPosY(8.0f);
    draw_watchpoint_gauge(static_cast<int>(snap.watchpoints.size()), 4);

    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetCursorPosY(9.0f);
    if (segmented_button("CMD+K", ctx.open_command_palette, pal.cobalt)) {
        ctx.open_command_palette = true;
    }

    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetCursorPosY(9.0f);
    bool is_lab = (Theme::current_mode() == ThemeMode::AnodizedLab);
    if (segmented_button("LAB", is_lab, pal.signal_orange)) {
        Theme::apply(ThemeMode::AnodizedLab);
    }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetCursorPosY(9.0f);
    if (segmented_button("SCHEM", !is_lab, pal.cobalt)) {
        Theme::apply(ThemeMode::TechnicalSchematic);
    }

    // 6) Step-Delta Oscilloscope Timeline Scrubber
    if (has_history) {
        ImGui::SetCursorPos(ImVec2(0.0f, bar_h));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + viewport_w, p.y + timeline_h), pal.bg_void);
        dl->AddLine(p, ImVec2(p.x + viewport_w, p.y), pal.border, 1.0f);
        dl->AddLine(ImVec2(p.x, p.y + timeline_h - 1.0f),
                    ImVec2(p.x + viewport_w, p.y + timeline_h - 1.0f), pal.border, 1.0f);

        ImGui::PushFont(Theme::fonts().micro);
        dl->AddText(ImVec2(p.x + 10.0f, p.y + 10.0f), pal.text_secondary, "STEP DELTA");

        // Timeline track region
        float track_x = p.x + 92.0f;
        float right_reserve = ctx.is_scrubbing_history() ? 220.0f : 155.0f;
        float track_w = std::max(120.0f, viewport_w - 102.0f - right_reserve);
        float track_y = p.y + 4.0f;
        float track_h = timeline_h - 8.0f;

        dl->AddRectFilled(ImVec2(track_x, track_y), ImVec2(track_x + track_w, track_y + track_h),
                          pal.bg_chassis);
        dl->AddRect(ImVec2(track_x, track_y), ImVec2(track_x + track_w, track_y + track_h), pal.border);

        int count = static_cast<int>(ctx.history.size());
        int active_idx = (ctx.history_cursor >= 0 && ctx.history_cursor < count)
                             ? ctx.history_cursor
                             : (count - 1);

        int max_visible = std::max(1, static_cast<int>(track_w / 14.0f));
        int start_idx = std::max(0, count - max_visible);
        int visible_n = count - start_idx;
        float cell_w = std::min(22.0f, track_w / static_cast<float>(std::max(1, visible_n)));

        ImVec2 mouse = ImGui::GetMousePos();
        int hovered_i = -1;

        for (int i = start_idx; i < count; ++i) {
            const auto& hs = *ctx.history[i];
            float cx = track_x + static_cast<float>(i - start_idx) * cell_w + 2.0f;
            float cw = std::max(4.0f, cell_w - 3.0f);
            bool is_active = (i == active_idx);

            int muts = static_cast<int>(hs.deltas.size());
            float bar_frac = std::min(1.0f, 0.22f + static_cast<float>(muts) * 0.14f);
            float bh = std::max(4.0f, (track_h - 4.0f) * bar_frac);

            ImU32 col = pal.border_strong;
            if (hs.state == SessionState::Fault) col = pal.fault_crimson;
            else if (is_active) col = pal.signal_orange;
            else if (muts > 0) col = pal.osc_amber;
            else col = pal.cobalt;

            ImVec2 rmin(cx, track_y + track_h - 2.0f - bh);
            ImVec2 rmax(cx + cw, track_y + track_h - 2.0f);
            dl->AddRectFilled(rmin, rmax, col);

            if (is_active) {
                dl->AddRect(ImVec2(cx - 1.0f, track_y + 1.0f),
                            ImVec2(cx + cw + 1.0f, track_y + track_h - 1.0f),
                            pal.signal_orange, 0.0f, 0, 1.5f);
            }

            if (mouse.x >= cx - 1.0f && mouse.x <= cx + cw + 1.0f &&
                mouse.y >= track_y && mouse.y <= track_y + track_h) {
                hovered_i = i;
            }
        }

        if (hovered_i >= 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ctx.history_cursor = (hovered_i == count - 1) ? -1 : hovered_i;
                const auto& sel = *ctx.history[hovered_i];
                ctx.jump_to_source(sel.active_file, sel.active_line);
            }
            const auto& hs = *ctx.history[hovered_i];
            ImGui::BeginTooltip();
            std::string fn = std::filesystem::path(hs.active_file).filename().string();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange),
                               "STOP #%llu  %s:%u",
                               static_cast<unsigned long long>(hs.step_index),
                               fn.c_str(), hs.active_line);
            if (hs.deltas.empty()) {
                ImGui::TextDisabled("No tracked variable/register changes");
            } else {
                int shown = 0;
                for (const auto& d : hs.deltas) {
                    if (shown++ >= 6) {
                        ImGui::TextDisabled("+%zu more changes...", hs.deltas.size() - 6);
                        break;
                    }
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.osc_amber),
                                       "[%s] %s: %s -> %s",
                                       d.category.c_str(), d.name.c_str(),
                                       d.old_val.c_str(), d.new_val.c_str());
                }
            }
            ImGui::EndTooltip();
        }

        // Right-side summary or "RETURN TO LIVE" button
        ImGui::SetCursorPos(ImVec2(track_x + track_w + 10.0f, bar_h + 7.0f));
        if (ctx.is_scrubbing_history()) {
            char hlbl[64];
            std::snprintf(hlbl, sizeof(hlbl), "HISTORY #%llu  [LIVE]",
                          static_cast<unsigned long long>(snap.step_index));
            if (segmented_button(hlbl, true, pal.osc_amber)) {
                ctx.history_cursor = -1;
            }
        } else {
            char slbl[64];
            std::snprintf(slbl, sizeof(slbl), "STOP #%llu  %zu DELTAS",
                          static_cast<unsigned long long>(snap.step_index),
                          snap.deltas.size());
            ImGui::SetCursorPosY(bar_h + 10.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(
                                   snap.deltas.empty() ? pal.text_dim : pal.osc_amber),
                               "%s", slbl);
        }
        ImGui::PopFont();
    }

    // 7) Crash Autopsy Banner
    if (has_crash) {
        float cy = bar_h + timeline_h;
        ImGui::SetCursorPos(ImVec2(0.0f, cy));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + viewport_w, p.y + crash_h),
                          IM_COL32(120, 20, 24, 210));
        dl->AddLine(ImVec2(p.x, p.y + crash_h - 1.0f),
                    ImVec2(p.x + viewport_w, p.y + crash_h - 1.0f), pal.fault_crimson, 2.0f);

        ImGui::SetCursorPos(ImVec2(12.0f, cy + 8.0f));
        ImGui::PushFont(Theme::fonts().bold);
        ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.92f, 1.0f),
                           "CRASH AUTOPSY // %s", snap.crash.signal_name.c_str());
        ImGui::PopFont();

        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.85f, 1.0f),
                           "ADDR %s [%s]  INSN: %s",
                           format_hex64_grouped(snap.crash.fault_address).c_str(),
                           provenance_label(snap.crash.fault_provenance),
                           snap.crash.offending_instruction.c_str());

        ImGui::SameLine(viewport_w - 175.0f);
        ImGui::SetCursorPosY(cy + 7.0f);
        if (segmented_button("JUMP TO USER FRAME", true, pal.signal_orange)) {
            ctx.engine->select_frame(snap.crash.user_frame_index);
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return total_h;
}

} // namespace scope::ui
