#include "Theme.hpp"

#include <misc/freetype/imgui_freetype.h>

#include <filesystem>

namespace scope {

namespace {

ThemeMode g_mode = ThemeMode::AnodizedLab;
Palette g_palette{};
FontSet g_fonts{};

ImVec4 u32_to_v4(ImU32 c) {
    return ImGui::ColorConvertU32ToFloat4(c);
}

Palette make_lab_palette() {
    Palette p{};
    p.bg_void       = IM_COL32(11, 12, 14, 255);    // #0B0C0E
    p.bg_chassis    = IM_COL32(19, 21, 24, 255);    // #131518
    p.bg_bezel      = IM_COL32(28, 31, 36, 255);    // #1C1F24
    p.bg_hover      = IM_COL32(38, 42, 49, 255);    // #262A31
    p.bg_selected   = IM_COL32(46, 52, 62, 255);    // #2E343E
    p.border        = IM_COL32(44, 48, 56, 255);    // #2C3038
    p.border_strong = IM_COL32(72, 78, 90, 255);    // #484E5A

    p.text_primary   = IM_COL32(236, 234, 228, 255);// #ECEAE4 warm ivory
    p.text_secondary = IM_COL32(154, 161, 173, 255);// #9AA1AD
    p.text_dim       = IM_COL32(92, 99, 112, 255);  // #5C6370

    p.signal_orange  = IM_COL32(255, 87, 34, 255);  // #FF5722
    p.phosphor_green = IM_COL32(52, 211, 153, 255); // #34D399
    p.osc_amber      = IM_COL32(245, 176, 46, 255); // #F5B02E
    p.cobalt         = IM_COL32(76, 185, 248, 255); // #4CB9F8
    p.fault_crimson  = IM_COL32(239, 68, 68, 255);  // #EF4444
    p.purple_hint    = IM_COL32(192, 152, 240, 255);// #C098F0

    // Crisp technical syntax palette (high legibility on matte carbon)
    p.syn_keyword   = IM_COL32(255, 126, 84, 255);  // warm coral-orange
    p.syn_type      = IM_COL32(106, 200, 248, 255); // precision cyan-cobalt
    p.syn_function  = IM_COL32(246, 224, 158, 255); // warm tungsten gold
    p.syn_field     = IM_COL32(198, 212, 228, 255); // cool silver
    p.syn_number    = IM_COL32(116, 226, 176, 255); // phosphor mint
    p.syn_string    = IM_COL32(168, 214, 122, 255); // oscilloscope olive-green
    p.syn_comment   = IM_COL32(102, 110, 124, 255); // muted graphite
    p.syn_preproc   = IM_COL32(198, 156, 232, 255); // muted violet
    p.syn_operator  = IM_COL32(162, 170, 184, 255); // steel
    p.syn_namespace = IM_COL32(138, 184, 218, 255); // slate blue

    p.field_colors[0] = IM_COL32(255, 87, 34, 220);
    p.field_colors[1] = IM_COL32(76, 185, 248, 220);
    p.field_colors[2] = IM_COL32(52, 211, 153, 220);
    p.field_colors[3] = IM_COL32(245, 176, 46, 220);
    p.field_colors[4] = IM_COL32(192, 152, 240, 220);
    p.field_colors[5] = IM_COL32(244, 114, 182, 220);
    return p;
}

Palette make_schematic_palette() {
    Palette p{};
    p.bg_void       = IM_COL32(248, 245, 238, 255); // #F8F5EE warm vellum
    p.bg_chassis    = IM_COL32(241, 237, 227, 255); // #F1EDE3 drafting paper
    p.bg_bezel      = IM_COL32(230, 225, 213, 255); // #E6E1D5
    p.bg_hover      = IM_COL32(220, 214, 200, 255);
    p.bg_selected   = IM_COL32(204, 216, 230, 255);
    p.border        = IM_COL32(194, 188, 174, 255);
    p.border_strong = IM_COL32(136, 130, 118, 255);

    p.text_primary   = IM_COL32(24, 25, 29, 255);   // carbon ink
    p.text_secondary = IM_COL32(82, 86, 96, 255);
    p.text_dim       = IM_COL32(138, 136, 128, 255);

    p.signal_orange  = IM_COL32(217, 62, 22, 255);  // vermilion drafting ink
    p.phosphor_green = IM_COL32(21, 128, 61, 255);
    p.osc_amber      = IM_COL32(180, 102, 8, 255);
    p.cobalt         = IM_COL32(3, 105, 161, 255);
    p.fault_crimson  = IM_COL32(196, 30, 38, 255);
    p.purple_hint    = IM_COL32(114, 62, 168, 255);

    p.syn_keyword   = IM_COL32(192, 52, 24, 255);
    p.syn_type      = IM_COL32(3, 105, 161, 255);
    p.syn_function  = IM_COL32(126, 78, 8, 255);
    p.syn_field     = IM_COL32(42, 52, 68, 255);
    p.syn_number    = IM_COL32(21, 128, 61, 255);
    p.syn_string    = IM_COL32(62, 116, 36, 255);
    p.syn_comment   = IM_COL32(132, 130, 120, 255);
    p.syn_preproc   = IM_COL32(114, 62, 168, 255);
    p.syn_operator  = IM_COL32(78, 84, 96, 255);
    p.syn_namespace = IM_COL32(24, 88, 132, 255);

    p.field_colors[0] = IM_COL32(217, 62, 22, 220);
    p.field_colors[1] = IM_COL32(3, 105, 161, 220);
    p.field_colors[2] = IM_COL32(21, 128, 61, 220);
    p.field_colors[3] = IM_COL32(180, 102, 8, 220);
    p.field_colors[4] = IM_COL32(114, 62, 168, 220);
    p.field_colors[5] = IM_COL32(184, 42, 112, 220);
    return p;
}

std::string find_font_path(const std::initializer_list<const char*>& candidates) {
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) {
            return p;
        }
    }
    return "";
}

} // namespace

void Theme::load_fonts(float /*dpi_scale*/) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    std::string regular_path = find_font_path({
        "/Library/Fonts/JetBrainsMono-Regular.ttf",
        "/Library/Fonts/JetBrainsMonoNL-Regular.ttf",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
    });
    std::string bold_path = find_font_path({
        "/Library/Fonts/JetBrainsMono-SemiBold.ttf",
        "/Library/Fonts/JetBrainsMono-Bold.ttf",
        "/System/Library/Fonts/SFNSMono.ttf",
    });

    ImFontConfig cfg;
    cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;

    if (!regular_path.empty()) {
        g_fonts.code = io.Fonts->AddFontFromFileTTF(regular_path.c_str(), 14.0f, &cfg);
        g_fonts.micro = io.Fonts->AddFontFromFileTTF(regular_path.c_str(), 11.5f, &cfg);
    } else {
        g_fonts.code = io.Fonts->AddFontDefault();
        g_fonts.micro = g_fonts.code;
    }

    if (!bold_path.empty()) {
        g_fonts.bold = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), 14.0f, &cfg);
        g_fonts.display = io.Fonts->AddFontFromFileTTF(bold_path.c_str(), 16.0f, &cfg);
    } else {
        g_fonts.bold = g_fonts.code;
        g_fonts.display = g_fonts.code;
    }

    io.FontDefault = g_fonts.code;
}

void Theme::apply(ThemeMode mode) {
    g_mode = mode;
    g_palette = (mode == ThemeMode::AnodizedLab) ? make_lab_palette() : make_schematic_palette();

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding     = ImVec2(8.0f, 6.0f);
    st.FramePadding      = ImVec2(6.0f, 3.0f);
    st.CellPadding       = ImVec2(6.0f, 3.0f);
    st.ItemSpacing       = ImVec2(6.0f, 4.0f);
    st.ItemInnerSpacing  = ImVec2(4.0f, 4.0f);
    st.IndentSpacing     = 14.0f;
    st.ScrollbarSize     = 10.0f;
    st.GrabMinSize       = 8.0f;

    // Precision rectilinear instrument geometry — zero rounded SaaS cards
    st.WindowBorderSize  = 1.0f;
    st.ChildBorderSize   = 1.0f;
    st.PopupBorderSize   = 1.0f;
    st.FrameBorderSize   = 1.0f;
    st.TabBorderSize     = 0.0f;
    st.TabBarBorderSize  = 1.0f;
    st.TabBarOverlineSize = 2.0f; // Hardware LED accent strip on active tab!

    st.WindowRounding    = 0.0f;
    st.ChildRounding     = 0.0f;
    st.FrameRounding     = 1.0f;
    st.PopupRounding     = 1.0f;
    st.ScrollbarRounding = 0.0f;
    st.GrabRounding      = 0.0f;
    st.TabRounding       = 0.0f;
    st.DockingSeparatorSize = 2.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text]                 = u32_to_v4(g_palette.text_primary);
    c[ImGuiCol_TextDisabled]         = u32_to_v4(g_palette.text_dim);
    c[ImGuiCol_WindowBg]             = u32_to_v4(g_palette.bg_chassis);
    c[ImGuiCol_ChildBg]              = u32_to_v4(g_palette.bg_void);
    c[ImGuiCol_PopupBg]              = u32_to_v4(g_palette.bg_bezel);
    c[ImGuiCol_Border]               = u32_to_v4(g_palette.border);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]              = u32_to_v4(g_palette.bg_void);
    c[ImGuiCol_FrameBgHovered]       = u32_to_v4(g_palette.bg_hover);
    c[ImGuiCol_FrameBgActive]        = u32_to_v4(g_palette.bg_selected);

    c[ImGuiCol_TitleBg]              = u32_to_v4(g_palette.bg_bezel);
    c[ImGuiCol_TitleBgActive]        = u32_to_v4(g_palette.bg_bezel);
    c[ImGuiCol_TitleBgCollapsed]     = u32_to_v4(g_palette.bg_void);
    c[ImGuiCol_MenuBarBg]            = u32_to_v4(g_palette.bg_bezel);

    c[ImGuiCol_ScrollbarBg]          = u32_to_v4(g_palette.bg_void);
    c[ImGuiCol_ScrollbarGrab]        = u32_to_v4(g_palette.border_strong);
    c[ImGuiCol_ScrollbarGrabHovered] = u32_to_v4(g_palette.text_dim);
    c[ImGuiCol_ScrollbarGrabActive]  = u32_to_v4(g_palette.signal_orange);

    c[ImGuiCol_CheckMark]            = u32_to_v4(g_palette.signal_orange);
    c[ImGuiCol_SliderGrab]           = u32_to_v4(g_palette.signal_orange);
    c[ImGuiCol_SliderGrabActive]     = u32_to_v4(g_palette.signal_orange);

    c[ImGuiCol_Button]               = u32_to_v4(g_palette.bg_bezel);
    c[ImGuiCol_ButtonHovered]        = u32_to_v4(g_palette.bg_hover);
    c[ImGuiCol_ButtonActive]         = u32_to_v4(g_palette.bg_selected);

    c[ImGuiCol_Header]               = u32_to_v4(g_palette.bg_bezel);
    c[ImGuiCol_HeaderHovered]        = u32_to_v4(g_palette.bg_hover);
    c[ImGuiCol_HeaderActive]         = u32_to_v4(g_palette.bg_selected);

    c[ImGuiCol_Separator]            = u32_to_v4(g_palette.border);
    c[ImGuiCol_SeparatorHovered]     = u32_to_v4(g_palette.signal_orange);
    c[ImGuiCol_SeparatorActive]      = u32_to_v4(g_palette.signal_orange);

    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = u32_to_v4(g_palette.signal_orange);
    c[ImGuiCol_ResizeGripActive]     = u32_to_v4(g_palette.signal_orange);

    c[ImGuiCol_Tab]                  = u32_to_v4(g_palette.bg_void);
    c[ImGuiCol_TabHovered]           = u32_to_v4(g_palette.bg_hover);
    c[ImGuiCol_TabSelected]          = u32_to_v4(g_palette.bg_chassis);
    c[ImGuiCol_TabSelectedOverline]  = u32_to_v4(g_palette.signal_orange);
    c[ImGuiCol_TabDimmed]            = u32_to_v4(g_palette.bg_void);
    c[ImGuiCol_TabDimmedSelected]    = u32_to_v4(g_palette.bg_chassis);
    c[ImGuiCol_TabDimmedSelectedOverline] = u32_to_v4(g_palette.border_strong);

    c[ImGuiCol_DockingPreview]       = ImVec4(1.0f, 0.34f, 0.13f, 0.35f);
    c[ImGuiCol_DockingEmptyBg]       = u32_to_v4(g_palette.bg_void);

    c[ImGuiCol_TableHeaderBg]        = u32_to_v4(g_palette.bg_bezel);
    c[ImGuiCol_TableBorderStrong]    = u32_to_v4(g_palette.border);
    c[ImGuiCol_TableBorderLight]     = u32_to_v4(g_palette.border);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = (mode == ThemeMode::AnodizedLab)
                                           ? ImVec4(1.0f, 1.0f, 1.0f, 0.02f)
                                           : ImVec4(0.0f, 0.0f, 0.0f, 0.025f);

    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.30f, 0.72f, 0.97f, 0.25f);
}

ThemeMode Theme::current_mode() { return g_mode; }
const Palette& Theme::palette() { return g_palette; }
const FontSet& Theme::fonts() { return g_fonts; }

} // namespace scope
