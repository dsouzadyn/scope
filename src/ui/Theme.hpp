#pragma once

#include <imgui.h>

namespace scope {

enum class ThemeMode {
    AnodizedLab,       // Default: matte carbon, signal orange, phosphor green
    TechnicalSchematic // Warm drafting paper light mode
};

struct Palette {
    ImU32 bg_void;       // Recessed well
    ImU32 bg_chassis;    // Main panel background
    ImU32 bg_bezel;      // Elevated header / toolbar
    ImU32 bg_hover;      // Subtle row / button hover
    ImU32 bg_selected;   // Active selection fill
    ImU32 border;        // 1px precision hairline
    ImU32 border_strong; // Active bezel hairline

    ImU32 text_primary;
    ImU32 text_secondary;
    ImU32 text_dim;

    ImU32 signal_orange;  // PC, active stop, active bay LED
    ImU32 phosphor_green; // Running state, waveform trace, heap
    ImU32 osc_amber;      // Mutated values/bytes/regs
    ImU32 cobalt;         // Pointers, addresses, stack
    ImU32 fault_crimson;  // Breakpoints, crashes, unmapped
    ImU32 purple_hint;    // Preprocessor / macro accent (subtle, drafting violet)

    // Syntax colors
    ImU32 syn_keyword;
    ImU32 syn_type;
    ImU32 syn_function;
    ImU32 syn_field;
    ImU32 syn_number;
    ImU32 syn_string;
    ImU32 syn_comment;
    ImU32 syn_preproc;
    ImU32 syn_operator;
    ImU32 syn_namespace;

    // Struct field overlay colors (6 distinct muted technical tones)
    ImU32 field_colors[6];
};

struct FontSet {
    ImFont* code = nullptr;    // 13.5px regular mono
    ImFont* bold = nullptr;    // 13.5px semibold mono
    ImFont* micro = nullptr;   // 11.0px uppercase telemetry labels
    ImFont* display = nullptr; // 16.0px PC / status readouts
};

/// Global visual theme manager for the Precision Bench Instrument UI.
///
/// Loads FreeType-hinted JetBrains Mono / SF Mono font tiers and applies sharp
/// rectilinear ImGui styling (Anodized Lab dark or Technical Schematic light).
class Theme {
  public:
    /// Loads and builds the FreeType font atlas (`code`, `bold`, `micro`, `display`).
    static void load_fonts(float dpi_scale);
    /// Applies palette colors and rectilinear instrument metrics to `ImGui::GetStyle()`.
    static void apply(ThemeMode mode);
    /// Returns the currently active `ThemeMode`.
    static ThemeMode current_mode();
    /// Returns the active color palette.
    static const Palette& palette();
    /// Returns the loaded monospace font tier set.
    static const FontSet& fonts();
};

} // namespace scope
