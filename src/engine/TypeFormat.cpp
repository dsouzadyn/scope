#include "TypeFormat.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace scope {

namespace {

void replace_all(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

void strip_default_allocator(std::string& s) {
    // Strip ", std::allocator<...>" patterns inside templates.
    const std::string needle = ", std::allocator<";
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        int depth = 1;
        size_t i = pos + needle.size();
        while (i < s.size() && depth > 0) {
            if (s[i] == '<') ++depth;
            else if (s[i] == '>') --depth;
            ++i;
        }
        if (depth == 0) {
            s.erase(pos, i - pos);
        } else {
            break;
        }
    }
}

void strip_default_hash_and_eq(std::string& s) {
    // Simplify unordered_map<K, V, std::hash<K>, std::equal_to<K>>
    const std::string needle = ", std::hash<";
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        int depth = 0;
        size_t i = pos;
        while (i < s.size()) {
            if (s[i] == '<') ++depth;
            else if (s[i] == '>') {
                if (depth == 0) break;
                --depth;
            }
            ++i;
        }
        s.erase(pos, i - pos);
    }
}

} // namespace

std::string simplify_cpp_type(std::string_view raw_type) {
    if (raw_type.empty()) return "";
    std::string out(raw_type);

    replace_all(out, "std::__1::", "std::");
    replace_all(out, "std::__cxx11::", "std::");
    replace_all(out,
                "std::basic_string<char, std::char_traits<char>, std::allocator<char> >",
                "std::string");
    replace_all(out,
                "std::basic_string<char, std::char_traits<char>, std::allocator<char>>",
                "std::string");
    replace_all(out,
                "std::basic_string_view<char, std::char_traits<char> >",
                "std::string_view");
    replace_all(out,
                "std::basic_string_view<char, std::char_traits<char>>",
                "std::string_view");

    strip_default_allocator(out);
    strip_default_hash_and_eq(out);
    replace_all(out, " >", ">");

    // Strip ", std::default_delete<...>" cleanly for std::unique_ptr<T>
    const std::string dd_needle = ", std::default_delete<";
    size_t pos = 0;
    while ((pos = out.find(dd_needle, pos)) != std::string::npos) {
        int depth = 1;
        size_t i = pos + dd_needle.size();
        while (i < out.size() && depth > 0) {
            if (out[i] == '<') ++depth;
            else if (out[i] == '>') --depth;
            ++i;
        }
        if (depth == 0) {
            out.erase(pos, i - pos);
        } else {
            break;
        }
    }
    return out;
}

std::string simplify_function_name(std::string_view raw_fn) {
    if (raw_fn.empty()) return "<unknown>";
    std::string out = simplify_cpp_type(raw_fn);
    return out;
}

bool is_runtime_frame(std::string_view function, std::string_view module, std::string_view file) {
    auto contains = [](std::string_view hay, std::string_view needle) {
        return hay.find(needle) != std::string_view::npos;
    };

    if (contains(module, "dyld") ||
        contains(module, "libsystem") ||
        contains(module, "libc++") ||
        contains(module, "libobjc") ||
        contains(module, "CoreFoundation")) {
        return true;
    }

    if (function == "start" ||
        contains(function, "_pthread_") ||
        contains(function, "std::__1::") ||
        contains(function, "__cxxabiv1") ||
        contains(function, "__assert_rtn") ||
        contains(function, "abort")) {
        return true;
    }

    if (contains(file, "/include/c++/") || contains(file, "/usr/include/")) {
        return true;
    }

    return false;
}

std::string format_hex64_grouped(uint64_t addr) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%04x_%04x_%04x_%04x",
                  static_cast<unsigned>((addr >> 48) & 0xFFFFu),
                  static_cast<unsigned>((addr >> 32) & 0xFFFFu),
                  static_cast<unsigned>((addr >> 16) & 0xFFFFu),
                  static_cast<unsigned>(addr & 0xFFFFu));
    return std::string(buf);
}

size_t hex_significant_offset(std::string_view grouped_hex) {
    // Skip "0x", find first non-'0' non-'_' character.
    size_t i = (grouped_hex.size() >= 2 && grouped_hex[0] == '0' && grouped_hex[1] == 'x') ? 2 : 0;
    while (i < grouped_hex.size() && (grouped_hex[i] == '0' || grouped_hex[i] == '_')) {
        ++i;
    }
    if (i == grouped_hex.size() && grouped_hex.size() > 0) {
        return grouped_hex.size() - 1; // keep final '0' bright for 0x0000_..._0000
    }
    return i;
}

const char* provenance_label(ProvenanceKind kind) {
    switch (kind) {
        case ProvenanceKind::None:     return "";
        case ProvenanceKind::Stack:    return "STACK";
        case ProvenanceKind::Heap:     return "HEAP";
        case ProvenanceKind::Text:     return "TEXT";
        case ProvenanceKind::Rodata:   return "RODATA";
        case ProvenanceKind::Data:     return "DATA";
        case ProvenanceKind::Null:     return "NULL";
        case ProvenanceKind::Unmapped: return "UNMAPPED!";
        case ProvenanceKind::Unknown:  return "MEM";
    }
    return "";
}

ProvenanceKind classify_address(uint64_t addr,
                                const std::vector<MemoryRegion>& regions,
                                uint64_t stack_pointer) {
    if (addr == 0 || addr < 0x1000) {
        return ProvenanceKind::Null;
    }

    // Heuristic: within 8 MB of active thread SP -> STACK
    if (stack_pointer != 0) {
        uint64_t diff = (addr > stack_pointer) ? (addr - stack_pointer) : (stack_pointer - addr);
        if (diff < 8ull * 1024ull * 1024ull) {
            return ProvenanceKind::Stack;
        }
    }

    for (const auto& r : regions) {
        if (addr >= r.start && addr < r.end) {
            if (!r.readable && !r.writable && !r.executable) {
                return ProvenanceKind::Unmapped;
            }
            if (r.kind != ProvenanceKind::Unknown) {
                return r.kind;
            }
            if (r.executable) return ProvenanceKind::Text;
            if (r.readable && !r.writable) return ProvenanceKind::Rodata;
            if (r.readable && r.writable) {
                if (r.name.find("stack") != std::string::npos ||
                    r.name.find("STACK") != std::string::npos) {
                    return ProvenanceKind::Stack;
                }
                if (r.name.find("MALLOC") != std::string::npos ||
                    r.name.find("heap") != std::string::npos ||
                    r.name.empty()) {
                    return ProvenanceKind::Heap;
                }
                return ProvenanceKind::Data;
            }
            return ProvenanceKind::Unknown;
        }
    }

    return regions.empty() ? ProvenanceKind::Unknown : ProvenanceKind::Unmapped;
}

} // namespace scope
