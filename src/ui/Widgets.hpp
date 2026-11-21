#pragma once

#include "../engine/Types.hpp"

#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace scope::ui {

enum class IconKind {
    Play,
    Pause,
    StepOver,
    StepInto,
    StepOut,
    StepAsm,
    Restart,
    Stop,
    Watchpoint,
    Probe,
    Table,
    Memory,
};

// Draws a razor-sharp vector icon centered inside [pos, pos+size].
void draw_icon(ImDrawList* dl, IconKind kind, ImVec2 center, float radius, ImU32 col);

// Hardware-styled rectilinear transport button with icon + optional label/hotkey tooltip.
bool transport_button(const char* id,
                      IconKind icon,
                      const char* tooltip,
                      bool enabled,
                      ImU32 accent_col = 0,
                      ImVec2 size = ImVec2(32.0f, 26.0f));

// Segmented toggle pill (e.g. "SRC | MIX | ASM" or "LAB | SCHEM").
bool segmented_button(const char* label, bool active, ImU32 active_accent = 0);

// Hardware LED status badge ("● STOPPED", "● RUNNING", "▲ FAULT").
void draw_status_led(SessionState state, const char* extra_text = nullptr);

// Hardware watchpoint slot gauge: WP [■][■][□][□]
void draw_watchpoint_gauge(int active_count, int max_slots = 4);

// Renders nibble-grouped 64-bit hex with dimmed leading zeros. Returns true if clicked.
bool hex_address_link(const char* id, uint64_t addr, bool clickable = true);

// Draws a compact 1px bordered provenance tag ([STACK], [HEAP], [NULL], [UNMAPPED!]).
void draw_provenance_badge(ProvenanceKind kind);

// Draws a single unified 30px Bay Action Strip background with optional right-aligned telemetry.
// Returns the screen-space Y center for placing left-side segmented controls.
void begin_bay_toolbar(const char* right_meta = nullptr, float height = 30.0f);
void end_bay_toolbar();

} // namespace scope::ui
