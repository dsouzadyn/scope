#include "Widgets.hpp"
#include "Theme.hpp"
#include "../engine/TypeFormat.hpp"

#include <cmath>
#include <cstdio>

namespace scope::ui {

void draw_icon(ImDrawList* dl, IconKind kind, ImVec2 c, float r, ImU32 col) {
    switch (kind) {
        case IconKind::Play: {
            ImVec2 p1(c.x - r * 0.55f, c.y - r * 0.78f);
            ImVec2 p2(c.x - r * 0.55f, c.y + r * 0.78f);
            ImVec2 p3(c.x + r * 0.85f, c.y);
            dl->AddTriangleFilled(p1, p2, p3, col);
            break;
        }
        case IconKind::Pause: {
            float w = r * 0.34f;
            float h = r * 0.75f;
            dl->AddRectFilled(ImVec2(c.x - r * 0.6f, c.y - h),
                              ImVec2(c.x - r * 0.6f + w, c.y + h), col);
            dl->AddRectFilled(ImVec2(c.x + r * 0.6f - w, c.y - h),
                              ImVec2(c.x + r * 0.6f, c.y + h), col);
            break;
        }
        case IconKind::Stop: {
            float s = r * 0.65f;
            dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col);
            break;
        }
        case IconKind::Restart: {
            dl->PathArcTo(c, r * 0.7f, 0.3f, 5.2f, 16);
            dl->PathStroke(col, 0, 1.8f);
            ImVec2 tip(c.x + r * 0.72f, c.y - r * 0.32f);
            dl->AddTriangleFilled(tip,
                                  ImVec2(tip.x - r * 0.45f, tip.y - r * 0.15f),
                                  ImVec2(tip.x - r * 0.12f, tip.y + r * 0.42f), col);
            break;
        }
        case IconKind::StepOver: {
            // Dot below + curved jump arrow over
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.58f), r * 0.24f, col);
            dl->PathArcTo(ImVec2(c.x, c.y + r * 0.25f), r * 0.72f, 3.35f, 5.95f, 12);
            dl->PathStroke(col, 0, 1.7f);
            ImVec2 tip(c.x + r * 0.76f, c.y + r * 0.05f);
            dl->AddTriangleFilled(tip,
                                  ImVec2(tip.x - r * 0.42f, tip.y - r * 0.10f),
                                  ImVec2(tip.x - r * 0.05f, tip.y - r * 0.45f), col);
            break;
        }
        case IconKind::StepInto: {
            // Downward arrow onto dot
            dl->AddLine(ImVec2(c.x, c.y - r * 0.85f), ImVec2(c.x, c.y + r * 0.22f), col, 1.8f);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + r * 0.40f),
                                  ImVec2(c.x - r * 0.45f, c.y - r * 0.05f),
                                  ImVec2(c.x + r * 0.45f, c.y - r * 0.05f), col);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.72f), r * 0.22f, col);
            break;
        }
        case IconKind::StepOut: {
            // Upward arrow from dot
            dl->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x, c.y - r * 0.72f), col, 1.8f);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 0.90f),
                                  ImVec2(c.x - r * 0.45f, c.y - r * 0.42f),
                                  ImVec2(c.x + r * 0.45f, c.y - r * 0.42f), col);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.72f), r * 0.22f, col);
            break;
        }
        case IconKind::StepAsm: {
            // Twin horizontal chevrons ">>" for instruction step
            float s = r * 0.55f;
            for (int k = 0; k < 2; ++k) {
                float ox = (k == 0) ? -r * 0.32f : r * 0.26f;
                dl->AddLine(ImVec2(c.x + ox - s * 0.4f, c.y - s),
                            ImVec2(c.x + ox + s * 0.4f, c.y), col, 1.7f);
                dl->AddLine(ImVec2(c.x + ox + s * 0.4f, c.y),
                            ImVec2(c.x + ox - s * 0.4f, c.y + s), col, 1.7f);
            }
            break;
        }
        case IconKind::Watchpoint: {
            ImVec2 pts[4] = {
                ImVec2(c.x, c.y - r * 0.8f),
                ImVec2(c.x + r * 0.8f, c.y),
                ImVec2(c.x, c.y + r * 0.8f),
                ImVec2(c.x - r * 0.8f, c.y),
            };
            dl->AddConvexPolyFilled(pts, 4, col);
            break;
        }
        case IconKind::Probe: {
            // Mini sine wave
            ImVec2 prev(c.x - r, c.y);
            for (int i = 1; i <= 8; ++i) {
                float t = static_cast<float>(i) / 8.0f;
                ImVec2 cur(c.x - r + t * 2.0f * r, c.y - std::sin(t * 6.28318f) * r * 0.7f);
                dl->AddLine(prev, cur, col, 1.6f);
                prev = cur;
            }
            break;
        }
        case IconKind::Table: {
            float s = r * 0.75f;
            dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 0.0f, 0, 1.4f);
            dl->AddLine(ImVec2(c.x - s, c.y - s * 0.2f), ImVec2(c.x + s, c.y - s * 0.2f), col, 1.2f);
            dl->AddLine(ImVec2(c.x - s * 0.1f, c.y - s), ImVec2(c.x - s * 0.1f, c.y + s), col, 1.2f);
            break;
        }
        case IconKind::Memory: {
            float s = r * 0.75f;
            dl->AddRect(ImVec2(c.x - s, c.y - s * 0.65f), ImVec2(c.x + s, c.y + s * 0.65f), col, 0.0f, 0, 1.5f);
            dl->AddLine(ImVec2(c.x - s * 0.4f, c.y), ImVec2(c.x + s * 0.4f, c.y), col, 1.5f);
            break;
        }
    }
}

bool transport_button(const char* id,
                      IconKind icon,
                      const char* tooltip,
                      bool enabled,
                      ImU32 accent_col,
                      ImVec2 size) {
    const auto& pal = Theme::palette();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = false;

    if (!enabled) ImGui::BeginDisabled();
    clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    if (!enabled) ImGui::EndDisabled();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = active ? pal.bg_selected : (hovered ? pal.bg_hover : pal.bg_bezel);
    ImU32 border = hovered && enabled ? (accent_col ? accent_col : pal.border_strong) : pal.border;
    ImU32 icon_col = !enabled ? pal.text_dim
                              : (accent_col ? accent_col : pal.text_primary);

    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 1.0f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), border, 1.0f);

    if (hovered && enabled && accent_col) {
        dl->AddRectFilled(ImVec2(p.x + 1, p.y + size.y - 2),
                          ImVec2(p.x + size.x - 1, p.y + size.y - 1), accent_col);
    }

    draw_icon(dl, icon, ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f), 6.2f, icon_col);

    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool segmented_button(const char* label, bool active, ImU32 active_accent) {
    const auto& pal = Theme::palette();
    ImU32 accent = active_accent ? active_accent : pal.signal_orange;

    ImGui::PushFont(Theme::fonts().micro);
    ImVec2 txt_sz = ImGui::CalcTextSize(label);
    ImVec2 sz(txt_sz.x + 14.0f, 22.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();

    bool clicked = ImGui::InvisibleButton(label, sz);
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = active ? pal.bg_selected : (hovered ? pal.bg_hover : pal.bg_void);
    ImU32 border = active ? accent : pal.border;
    ImU32 fg = active ? pal.text_primary : pal.text_secondary;

    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, 1.0f);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), border, 1.0f);
    if (active) {
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + 2.0f), accent);
    }
    dl->AddText(ImVec2(p.x + 7.0f, p.y + (sz.y - txt_sz.y) * 0.5f), fg, label);
    ImGui::PopFont();
    return clicked;
}

void draw_status_led(SessionState state, const char* extra_text) {
    const auto& pal = Theme::palette();
    const char* label = "IDLE";
    ImU32 led_col = pal.text_dim;
    switch (state) {
        case SessionState::Idle:    label = "IDLE";    led_col = pal.text_dim; break;
        case SessionState::Running: label = "RUNNING"; led_col = pal.phosphor_green; break;
        case SessionState::Stopped: label = "STOPPED"; led_col = pal.signal_orange; break;
        case SessionState::Fault:   label = "FAULT";   led_col = pal.fault_crimson; break;
        case SessionState::Exited:  label = "EXITED";  led_col = pal.text_secondary; break;
    }

    ImGui::PushFont(Theme::fonts().micro);
    char buf[128];
    if (extra_text && *extra_text) {
        std::snprintf(buf, sizeof(buf), "%s  %s", label, extra_text);
    } else {
        std::snprintf(buf, sizeof(buf), "%s", label);
    }
    ImVec2 txt_sz = ImGui::CalcTextSize(buf);
    ImVec2 sz(txt_sz.x + 24.0f, 24.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(sz);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), pal.bg_void, 1.0f);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y),
                (state == SessionState::Fault) ? pal.fault_crimson : pal.border, 1.0f);

    ImVec2 led_c(p.x + 10.0f, p.y + sz.y * 0.5f);
    if (state == SessionState::Running || state == SessionState::Stopped || state == SessionState::Fault) {
        dl->AddCircleFilled(led_c, 6.0f, (led_col & 0x00FFFFFFu) | 0x38000000u);
    }
    dl->AddCircleFilled(led_c, 3.2f, led_col);
    dl->AddText(ImVec2(p.x + 19.0f, p.y + (sz.y - txt_sz.y) * 0.5f), pal.text_primary, buf);
    ImGui::PopFont();
}

void draw_watchpoint_gauge(int active_count, int max_slots) {
    const auto& pal = Theme::palette();
    ImGui::PushFont(Theme::fonts().micro);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = 26.0f + static_cast<float>(max_slots) * 11.0f + 8.0f;
    float h = 24.0f;
    ImGui::Dummy(ImVec2(w, h));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pal.bg_void, 1.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), pal.border, 1.0f);
    ImVec2 wp_sz = ImGui::CalcTextSize("WP");
    dl->AddText(ImVec2(p.x + 6.0f, p.y + (h - wp_sz.y) * 0.5f), pal.text_secondary, "WP");

    for (int i = 0; i < max_slots; ++i) {
        float x = p.x + 27.0f + static_cast<float>(i) * 11.0f;
        float y = p.y + 7.5f;
        if (i < active_count) {
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 8.0f, y + 9.0f), pal.osc_amber);
        } else {
            dl->AddRect(ImVec2(x, y), ImVec2(x + 8.0f, y + 9.0f), pal.border_strong);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hardware Watchpoint Slots: %d / %d active", active_count, max_slots);
    }
    ImGui::PopFont();
}

bool hex_address_link(const char* id, uint64_t addr, bool clickable) {
    const auto& pal = Theme::palette();
    std::string hex = format_hex64_grouped(addr);
    size_t sig = hex_significant_offset(hex);

    ImVec2 txt_sz = ImGui::CalcTextSize(hex.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = false;
    bool hovered = false;

    if (clickable) {
        clicked = ImGui::InvisibleButton(id, ImVec2(txt_sz.x + 2.0f, txt_sz.y + 2.0f));
        hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else {
        ImGui::Dummy(txt_sz);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string prefix = hex.substr(0, sig);
    std::string suffix = hex.substr(sig);

    float x = p.x;
    if (!prefix.empty()) {
        dl->AddText(ImVec2(x, p.y), pal.text_dim, prefix.c_str());
        x += ImGui::CalcTextSize(prefix.c_str()).x;
    }
    ImU32 sig_col = clickable ? (hovered ? pal.signal_orange : pal.cobalt) : pal.text_primary;
    dl->AddText(ImVec2(x, p.y), sig_col, suffix.c_str());

    if (hovered) {
        dl->AddLine(ImVec2(p.x, p.y + txt_sz.y), ImVec2(p.x + txt_sz.x, p.y + txt_sz.y),
                    pal.signal_orange, 1.0f);
    }
    return clicked;
}

void draw_provenance_badge(ProvenanceKind kind) {
    const char* lbl = provenance_label(kind);
    if (!lbl || !*lbl) return;
    const auto& pal = Theme::palette();

    ImU32 col = pal.text_secondary;
    switch (kind) {
        case ProvenanceKind::Stack:    col = pal.cobalt; break;
        case ProvenanceKind::Heap:     col = pal.phosphor_green; break;
        case ProvenanceKind::Text:     col = pal.purple_hint; break;
        case ProvenanceKind::Rodata:   col = pal.text_secondary; break;
        case ProvenanceKind::Data:     col = pal.text_secondary; break;
        case ProvenanceKind::Null:     col = pal.osc_amber; break;
        case ProvenanceKind::Unmapped: col = pal.fault_crimson; break;
        default: break;
    }

    ImGui::PushFont(Theme::fonts().micro);
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    float badge_h = ts.y + 2.0f;
    ImVec2 sz(ts.x + 8.0f, badge_h);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // Center badge nicely against standard code line height
    float y_off = 1.0f;
    ImGui::Dummy(ImVec2(sz.x, badge_h + 1.0f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bmin(p.x, p.y + y_off);
    ImVec2 bmax(p.x + sz.x, p.y + y_off + badge_h);
    dl->AddRectFilled(bmin, bmax, (col & 0x00FFFFFFu) | 0x22000000u, 1.0f);
    dl->AddRect(bmin, bmax, (col & 0x00FFFFFFu) | 0x99000000u, 1.0f);
    dl->AddText(ImVec2(bmin.x + 4.0f, bmin.y + 1.0f), col, lbl);
    ImGui::PopFont();
}

namespace {
float g_toolbar_start_y = 0.0f;
float g_toolbar_h = 30.0f;
} // namespace

void begin_bay_toolbar(const char* right_meta, float height) {
    const auto& pal = Theme::palette();
    g_toolbar_h = height;
    g_toolbar_start_y = ImGui::GetCursorPosY();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + height), pal.bg_bezel);
    dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + height), pal.signal_orange);
    dl->AddLine(ImVec2(p.x, p.y + height - 1.0f),
                ImVec2(p.x + avail.x, p.y + height - 1.0f), pal.border, 1.0f);

    if (right_meta && *right_meta) {
        ImGui::PushFont(Theme::fonts().micro);
        ImVec2 mw = ImGui::CalcTextSize(right_meta);
        dl->AddText(ImVec2(p.x + avail.x - mw.x - 10.0f, p.y + (height - mw.y) * 0.5f),
                    pal.text_secondary, right_meta);
        ImGui::PopFont();
    }

    // Place cursor cleanly centered for 22px controls on the left
    ImGui::SetCursorPos(ImVec2(10.0f, g_toolbar_start_y + (height - 22.0f) * 0.5f));
}

void end_bay_toolbar() {
    ImGui::SetCursorPos(ImVec2(0.0f, g_toolbar_start_y + g_toolbar_h));
}

} // namespace scope::ui
