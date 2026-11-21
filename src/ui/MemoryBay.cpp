#include "UiContext.hpp"
#include "Theme.hpp"
#include "Widgets.hpp"
#include "../engine/TypeFormat.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace scope::ui {

namespace {

const StructFieldOverlay* find_field_for_offset(const std::vector<StructFieldOverlay>& fields,
                                                uint64_t offset) {
    for (const auto& f : fields) {
        if (offset >= f.offset && offset < f.offset + f.size) {
            return &f;
        }
    }
    return nullptr;
}

const PaddingRange* find_padding_for_offset(const std::vector<PaddingRange>& pads,
                                            uint64_t offset) {
    for (const auto& p : pads) {
        if (offset >= p.offset && offset < p.offset + p.size) {
            return &p;
        }
    }
    return nullptr;
}

uint16_t read_u16(const uint8_t* p, bool big_endian) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return big_endian ? __builtin_bswap16(v) : v;
}

uint32_t read_u32(const uint8_t* p, bool big_endian) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return big_endian ? __builtin_bswap32(v) : v;
}

uint64_t read_u64(const uint8_t* p, bool big_endian) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return big_endian ? __builtin_bswap64(v) : v;
}

std::string format_epoch_utc(uint64_t raw) {
    // Detect plausible epoch in nanoseconds (1e18..3e18) or microseconds (1e15..3e15)
    uint64_t sec = 0;
    uint64_t sub_ns = 0;
    bool is_us = false;
    if (raw >= 1000000000000000000ULL && raw <= 4000000000000000000ULL) {
        sec = raw / 1000000000ULL;
        sub_ns = raw % 1000000000ULL;
    } else if (raw >= 1000000000000000ULL && raw <= 4000000000000000ULL) {
        sec = raw / 1000000ULL;
        sub_ns = (raw % 1000000ULL) * 1000ULL;
        is_us = true;
    } else {
        return "";
    }
    std::time_t tt = static_cast<std::time_t>(sec);
    std::tm gmt{};
    gmtime_r(&tt, &gmt);
    char buf[64];
    if (is_us) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06llu UTC",
                      gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday,
                      gmt.tm_hour, gmt.tm_min, gmt.tm_sec,
                      static_cast<unsigned long long>(sub_ns / 1000ULL));
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%09llu UTC",
                      gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday,
                      gmt.tm_hour, gmt.tm_min, gmt.tm_sec,
                      static_cast<unsigned long long>(sub_ns));
    }
    return std::string(buf);
}

} // namespace

void draw_memory_bay(UiContext& ctx) {
    const auto& pal = Theme::palette();

    if (ctx.focus_memory_bay) {
        ImGui::SetNextWindowFocus();
        ctx.focus_memory_bay = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("03 MEMORY", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    MemorySnapshot ms = ctx.engine->memory_snapshot();
    std::string meta = ms.valid
                           ? (format_hex64_grouped(ms.base_address) + "  " +
                              std::to_string(ms.bytes.size()) + "B")
                           : "DISCONNECTED";
    if (!ms.struct_type.empty()) {
        uint64_t cls = (ms.struct_size + 63) / 64;
        meta = ms.struct_type + " (" + std::to_string(ms.struct_size) + "B, pad " +
               std::to_string(ms.padding_bytes) + "B, " + std::to_string(cls) + " CL)";
    }

    // Address / expression bar inside unified toolbar
    static int s_size_idx = 1; // 0=256, 1=512, 2=1024, 3=4096
    static constexpr uint64_t kSizes[] = {256, 512, 1024, 4096};
    static const char* kSizeLabels[] = {"256B", "512B", "1KB", "4KB"};
    static int s_selected_offset = 0;
    static bool s_big_endian = false;
    static bool s_show_cache_lines = true;
    static int s_fixed_scale_idx = 1; // 0=OFF, 1=1e-4, 2=1e-6, 3=1e-8
    static constexpr double kFixedScales[] = {1.0, 1e4, 1e6, 1e8};
    static const char* kFixedLabels[] = {"FX:OFF", "1e-4", "1e-6", "1e-8"};

    begin_bay_toolbar(meta.c_str(), 32.0f);
    ImGui::SetNextItemWidth(220.0f);
    bool enter = ImGui::InputTextWithHint("##MemAddr", "Address or expr (wire_ptr, stats_ptr)",
                                          ctx.mem_expr_buf, sizeof(ctx.mem_expr_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::Combo("##MemSize", &s_size_idx, kSizeLabels, 4)) {
        if (ctx.mem_expr_buf[0] != '\0') {
            ctx.engine->configure_memory_view(ctx.mem_expr_buf, kSizes[s_size_idx]);
        }
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("READ", ImVec2(50.0f, 0.0f)) || enter) {
        if (ctx.mem_expr_buf[0] != '\0') {
            ctx.engine->configure_memory_view(ctx.mem_expr_buf, kSizes[s_size_idx]);
            s_selected_offset = 0;
        }
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (segmented_button(s_big_endian ? "BE" : "LE", s_big_endian, pal.cobalt)) {
        s_big_endian = !s_big_endian;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle Little-Endian / Big-Endian (Network Byte Order for ITCH/OUCH/SBE)");
    }
    ImGui::SameLine(0.0f, 3.0f);
    if (segmented_button("64B CL", s_show_cache_lines, pal.signal_orange)) {
        s_show_cache_lines = !s_show_cache_lines;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show 64-byte CPU cache-line dividers & padding holes");
    }
    ImGui::SameLine(0.0f, 3.0f);
    if (segmented_button(kFixedLabels[s_fixed_scale_idx], s_fixed_scale_idx > 0, pal.phosphor_green)) {
        s_fixed_scale_idx = (s_fixed_scale_idx + 1) % 4;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fixed-point price divisor for int64/int32 decoder (click to cycle)");
    }
    end_bay_toolbar();

    // False-Sharing Warning Strip (HFT Cache-Line Analyzer!)
    if (ms.valid && !ms.false_sharing_warnings.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float avail_w = ImGui::GetContentRegionAvail().x;
        float wh = 22.0f;
        ImGui::Dummy(ImVec2(avail_w, wh));
        dl->AddRectFilled(p, ImVec2(p.x + avail_w, p.y + wh),
                          (pal.osc_amber & 0x00FFFFFFu) | 0x28000000u);
        dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + wh), pal.osc_amber);
        ImGui::PushFont(Theme::fonts().micro);
        std::string wtxt = "FALSE SHARING RISK // " + ms.false_sharing_warnings[0];
        if (ms.false_sharing_warnings.size() > 1) {
            wtxt += " (+" + std::to_string(ms.false_sharing_warnings.size() - 1) + " more)";
        }
        dl->AddText(ImVec2(p.x + 9.0f, p.y + 4.0f), pal.osc_amber, wtxt.c_str());
        ImGui::PopFont();
    }

    // Struct field overlay legend
    if (ms.valid && !ms.fields.empty()) {
        ImGui::SetCursorPos(ImVec2(8.0f, ImGui::GetCursorPosY() + 4.0f));
        ImGui::PushFont(Theme::fonts().micro);
        for (size_t i = 0; i < std::min<size_t>(ms.fields.size(), 10); ++i) {
            const auto& f = ms.fields[i];
            ImU32 fcol = pal.field_colors[f.color_index % 6];
            if (i > 0) ImGui::SameLine(0.0f, 6.0f);
            ImVec2 p = ImGui::GetCursorScreenPos();
            char fbuf[112];
            std::snprintf(fbuf, sizeof(fbuf), "%s%s +%llu (%lluB)",
                          f.name.c_str(), f.is_atomic ? "*" : "",
                          static_cast<unsigned long long>(f.offset),
                          static_cast<unsigned long long>(f.size));
            ImVec2 sz = ImGui::CalcTextSize(fbuf);
            ImGui::Dummy(ImVec2(sz.x + 12.0f, sz.y + 4.0f));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + sz.x + 12.0f, p.y + sz.y + 4.0f),
                              (fcol & 0x00FFFFFFu) | 0x22000000u, 1.0f);
            dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + sz.y + 4.0f), fcol);
            dl->AddText(ImVec2(p.x + 7.0f, p.y + 2.0f), pal.text_primary, fbuf);
        }
        if (ms.padding_bytes > 0) {
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.fault_crimson),
                               "PAD: %lluB", static_cast<unsigned long long>(ms.padding_bytes));
        }
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 3.0f));
        ImGui::Separator();
    }

    if (!ms.valid) {
        ImGui::SetCursorPos(ImVec2(14.0f, ImGui::GetCursorPosY() + 10.0f));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.text_dim),
                           "%s", ms.error.empty()
                                     ? "Enter an address/variable above or click any pointer in Inspector to inspect memory."
                                     : ms.error.c_str());
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // Reserve footer for Live HFT Multi-Type / Wire-Protocol Decoder
    const float footer_h = 62.0f;
    float grid_h = std::max(80.0f, ImGui::GetContentRegionAvail().y - footer_h);

    const int cols = 16;
    const int num_rows = (static_cast<int>(ms.bytes.size()) + cols - 1) / cols;
    const float row_h = ImGui::GetTextLineHeight() + 6.0f;
    const float byte_w = ImGui::CalcTextSize("FF ").x + 2.0f;
    const float ascii_char_w = ImGui::CalcTextSize("M").x;
    const float addr_col_w = 216.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##HexGrid", ImVec2(0.0f, grid_h), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetMousePos();
    bool hovered_win = ImGui::IsWindowHovered();

    ImGuiListClipper clipper;
    clipper.Begin(num_rows, row_h);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            ImVec2 rp = ImGui::GetCursorScreenPos();
            float total_w = addr_col_w + cols * byte_w + 26.0f + cols * ascii_char_w + 20.0f;
            ImGui::Dummy(ImVec2(total_w, row_h));

            int row_byte_off = r * cols;
            bool is_cl_start = s_show_cache_lines && ((row_byte_off % 64) == 0);
            if (is_cl_start) {
                dl->AddLine(ImVec2(rp.x, rp.y), ImVec2(rp.x + total_w, rp.y),
                            (pal.signal_orange & 0x00FFFFFFu) | 0x88000000u, 1.0f);
            }

            uint64_t row_addr = ms.base_address + static_cast<uint64_t>(row_byte_off);
            std::string addr_s = format_hex64_grouped(row_addr);
            size_t sig = hex_significant_offset(addr_s);
            float ty = rp.y + 3.0f;

            // Optional CL# badge on cache-line start rows
            if (s_show_cache_lines) {
                ImGui::PushFont(Theme::fonts().micro);
                if (is_cl_start) {
                    char clbuf[16];
                    std::snprintf(clbuf, sizeof(clbuf), "CL%d", row_byte_off / 64);
                    dl->AddText(ImVec2(rp.x + 6.0f, ty + 1.0f), pal.signal_orange, clbuf);
                }
                ImGui::PopFont();
            }

            // Address column
            float ax = rp.x + (s_show_cache_lines ? 34.0f : 8.0f);
            dl->AddText(ImVec2(ax, ty), pal.text_dim, addr_s.c_str(), addr_s.c_str() + sig);
            ax += ImGui::CalcTextSize(addr_s.c_str(), addr_s.c_str() + sig).x;
            dl->AddText(ImVec2(ax, ty), pal.cobalt, addr_s.c_str() + sig);

            float hex_x0 = rp.x + addr_col_w;
            float asc_x0 = hex_x0 + cols * byte_w + 18.0f;

            for (int c = 0; c < cols; ++c) {
                int off = row_byte_off + c;
                if (off >= static_cast<int>(ms.bytes.size())) break;
                uint8_t b = ms.bytes[off];

                float bx = hex_x0 + c * byte_w + (c >= 8 ? 6.0f : 0.0f);
                float cx = asc_x0 + c * ascii_char_w;

                bool changed = false;
                if (ms.prev_base_address == ms.base_address &&
                    off < static_cast<int>(ms.prev_bytes.size()) &&
                    ms.prev_bytes[off] != b) {
                    changed = true;
                }

                const StructFieldOverlay* fld = find_field_for_offset(ms.fields, static_cast<uint64_t>(off));
                const PaddingRange* pad = s_show_cache_lines
                                              ? find_padding_for_offset(ms.padding_ranges, static_cast<uint64_t>(off))
                                              : nullptr;
                bool selected = (off == s_selected_offset);

                if (selected) {
                    dl->AddRectFilled(ImVec2(bx - 2.0f, rp.y + 1.0f),
                                      ImVec2(bx + byte_w - 2.0f, rp.y + row_h - 1.0f),
                                      (pal.signal_orange & 0x00FFFFFFu) | 0x44000000u);
                    dl->AddRect(ImVec2(bx - 2.0f, rp.y + 1.0f),
                                ImVec2(bx + byte_w - 2.0f, rp.y + row_h - 1.0f),
                                pal.signal_orange);
                } else if (changed) {
                    dl->AddRectFilled(ImVec2(bx - 2.0f, rp.y + 1.0f),
                                      ImVec2(bx + byte_w - 2.0f, rp.y + row_h - 1.0f),
                                      (pal.osc_amber & 0x00FFFFFFu) | 0x35000000u);
                } else if (pad) {
                    // Struct padding hole indicator!
                    dl->AddRectFilled(ImVec2(bx - 1.0f, rp.y + 2.0f),
                                      ImVec2(bx + byte_w - 2.0f, rp.y + row_h - 2.0f),
                                      (pal.fault_crimson & 0x00FFFFFFu) | 0x1E000000u);
                } else if (fld) {
                    ImU32 fcol = pal.field_colors[fld->color_index % 6];
                    dl->AddRectFilled(ImVec2(bx - 1.0f, rp.y + row_h - 3.0f),
                                      ImVec2(bx + byte_w - 2.0f, rp.y + row_h - 1.0f),
                                      fcol);
                }

                char hbuf[4];
                std::snprintf(hbuf, sizeof(hbuf), "%02x", b);
                ImU32 fg = changed ? pal.osc_amber
                                   : (pad ? pal.fault_crimson
                                          : (b == 0 ? pal.text_dim : pal.text_primary));
                dl->AddText(ImVec2(bx, ty), fg, hbuf);

                char ch = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
                char abuf[2] = {ch, '\0'};
                dl->AddText(ImVec2(cx, ty),
                            selected ? pal.signal_orange : (b >= 32 && b < 127 ? pal.text_secondary : pal.text_dim),
                            abuf);

                if (hovered_win &&
                    mouse.x >= bx - 2.0f && mouse.x < bx + byte_w - 2.0f &&
                    mouse.y >= rp.y && mouse.y < rp.y + row_h) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        s_selected_offset = off;
                    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        s_selected_offset = off;
                        ImGui::OpenPopup("##MemByteContext");
                    }
                    if (fld) {
                        ImGui::SetTooltip("Field: %s (%s)%s\nOffset +%llu [%lluB]  CL#%llu+%llu",
                                          fld->name.c_str(), fld->type_name.c_str(),
                                          fld->is_atomic ? " [ATOMIC]" : "",
                                          static_cast<unsigned long long>(fld->offset),
                                          static_cast<unsigned long long>(fld->size),
                                          static_cast<unsigned long long>(off / 64),
                                          static_cast<unsigned long long>(off % 64));
                    } else if (pad) {
                        ImGui::SetTooltip("STRUCT PADDING HOLE: +%llu (%llu wasted bytes)\nCL#%llu+%llu",
                                          static_cast<unsigned long long>(pad->offset),
                                          static_cast<unsigned long long>(pad->size),
                                          static_cast<unsigned long long>(off / 64),
                                          static_cast<unsigned long long>(off % 64));
                    }
                }
            }
        }
    }

    if (ImGui::BeginPopup("##MemByteContext")) {
        uint64_t sel_addr = ms.base_address + static_cast<uint64_t>(s_selected_offset);
        ImGui::PushFont(Theme::fonts().micro);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange),
                           "ADDR %s  (CL#%d+%d)", format_hex64_grouped(sel_addr).c_str(),
                           s_selected_offset / 64, s_selected_offset % 64);
        ImGui::PopFont();
        ImGui::Separator();
        if (ImGui::MenuItem("Arm HW Watchpoint (4B Write)")) {
            ctx.engine->add_watchpoint(sel_addr, 4, false, true, format_hex64_grouped(sel_addr));
        }
        if (ImGui::MenuItem("Arm HW Watchpoint (8B Write)")) {
            ctx.engine->add_watchpoint(sel_addr, 8, false, true, format_hex64_grouped(sel_addr));
        }
        if (s_selected_offset + 8 <= static_cast<int>(ms.bytes.size())) {
            uint64_t ptr_val = read_u64(ms.bytes.data() + s_selected_offset, false);
            if (ptr_val > 0x1000 && ImGui::MenuItem("Chase 64-bit Pointer Here")) {
                ctx.inspect_in_memory(format_hex64_grouped(ptr_val), kSizes[s_size_idx]);
            }
        }
        if (ImGui::MenuItem("Copy Selected Address")) {
            ImGui::SetClipboardText(format_hex64_grouped(sel_addr).c_str());
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(); // ItemSpacing

    // Live HFT Multi-Type / Wire-Protocol Decoder Strip
    ImGui::Separator();
    ImGui::BeginChild("##MemDecoder", ImVec2(0.0f, 0.0f), false);
    if (s_selected_offset >= 0 && s_selected_offset < static_cast<int>(ms.bytes.size())) {
        const uint8_t* ptr = ms.bytes.data() + s_selected_offset;
        size_t rem = ms.bytes.size() - static_cast<size_t>(s_selected_offset);

        ImGui::SetCursorPos(ImVec2(8.0f, 5.0f));
        ImGui::PushFont(Theme::fonts().micro);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.signal_orange),
                           "+%d [CL%d+%d] %s",
                           s_selected_offset, s_selected_offset / 64, s_selected_offset % 64,
                           s_big_endian ? "BE" : "LE");
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::Text("u8=%u", ptr[0]);
        if (rem >= 2) {
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::Text("u16=%u", read_u16(ptr, s_big_endian));
        }
        if (rem >= 4) {
            uint32_t u32 = read_u32(ptr, s_big_endian);
            int32_t i32 = static_cast<int32_t>(u32);
            float f32 = 0.0f;
            std::memcpy(&f32, &u32, 4);
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::Text("i32=%d  u32=0x%08x  f32=%.5g", i32, u32, f32);
        }

        // Row 2: 64-bit, Fixed-Point Price, Epoch ns/us UTC Timestamp & Pointer Chase
        ImGui::SetCursorPos(ImVec2(8.0f, 26.0f));
        if (rem >= 8) {
            uint64_t u64 = read_u64(ptr, s_big_endian);
            int64_t i64 = static_cast<int64_t>(u64);
            double f64 = 0.0;
            std::memcpy(&f64, &u64, 8);
            ImGui::Text("i64=%lld  f64=%.6g", static_cast<long long>(i64), f64);

            if (s_fixed_scale_idx > 0) {
                double px = static_cast<double>(i64) / kFixedScales[s_fixed_scale_idx];
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.phosphor_green),
                                   "PX(%s)=%.6f", kFixedLabels[s_fixed_scale_idx], px);
            }

            std::string utc = format_epoch_utc(u64);
            if (!utc.empty()) {
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(pal.cobalt), "TS=%s", utc.c_str());
            } else if (!s_big_endian && u64 > 0x1000) {
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::TextDisabled("ptr=");
                ImGui::SameLine(0.0f, 2.0f);
                if (hex_address_link("##ChaseDecoderPtr", u64, true)) {
                    ctx.inspect_in_memory(format_hex64_grouped(u64), kSizes[s_size_idx]);
                }
            }
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace scope::ui
