#pragma once

#include "Types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scope {

/// Condenses verbose libc++/libstdc++ ABI template noise into readable C++ types
/// (e.g., `std::__1::basic_string<...>` -> `std::string`, strips default
/// allocators/hashers/deleters).
std::string simplify_cpp_type(std::string_view raw_type);

/// Condenses verbose function signatures for compact stack frames (returns `"<unknown>"` if empty).
std::string simplify_function_name(std::string_view raw_fn);

/// Returns true for stdlib, libc++, dyld, libsystem, and pthread frames used by smart stack
/// folding.
bool is_runtime_frame(std::string_view function, std::string_view module, std::string_view file);

/// Formats a 64-bit address with nibble separators (e.g., `"0x0000_0001_6fdf_f4a0"`).
std::string format_hex64_grouped(uint64_t addr);

/// Returns byte index of the first significant hex digit after `"0x"` for leading-zero dimming.
size_t hex_significant_offset(std::string_view grouped_hex);

/// Returns compact badge label (`"STACK"`, `"HEAP"`, `"TEXT"`, `"NULL"`, `"UNMAPPED!"`, etc.).
const char* provenance_label(ProvenanceKind kind);

/// Classifies `addr` against VM regions and active thread stack pointer `stack_pointer`.
ProvenanceKind classify_address(uint64_t addr, const std::vector<MemoryRegion>& regions,
                                uint64_t stack_pointer);

} // namespace scope
