#include "../src/engine/SyntaxHighlighter.hpp"
#include "../src/engine/TypeFormat.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void check_impl(bool cond, const char* expr, const char* msg, int line) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::fprintf(stderr, "  [FAIL] line %d: %s (%s)\n", line, msg, expr);
    }
}

#define EXPECT(cond, msg) check_impl((cond), #cond, (msg), __LINE__)

void test_type_simplification() {
    std::printf("[UNIT] Type simplification\n");
    EXPECT(scope::simplify_cpp_type("") == "", "empty type");
    EXPECT(scope::simplify_cpp_type("int") == "int", "primitive int");
    EXPECT(scope::simplify_cpp_type("const char *") == "const char *", "const char pointer");

    EXPECT(scope::simplify_cpp_type(
               "std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >") ==
               "std::string",
           "libc++ std::string");
    EXPECT(scope::simplify_cpp_type(
               "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>") ==
               "std::string",
           "libstdc++ std::string");
    EXPECT(scope::simplify_cpp_type(
               "std::__1::basic_string_view<char, std::__1::char_traits<char> >") ==
               "std::string_view",
           "std::string_view");
    EXPECT(scope::simplify_cpp_type("std::__1::vector<Particle, std::__1::allocator<Particle> >") ==
               "std::vector<Particle>",
           "std::vector<Particle>");
    EXPECT(scope::simplify_cpp_type(
               "std::__1::vector<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >, std::__1::allocator<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > > >") ==
               "std::vector<std::string>",
           "nested std::vector<std::string>");
    EXPECT(scope::simplify_cpp_type(
               "std::__1::unordered_map<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >, int, std::__1::hash<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > >, std::__1::equal_to<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > >, std::__1::allocator<std::__1::pair<const std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >, int> > >") ==
               "std::unordered_map<std::string, int>",
           "std::unordered_map<std::string, int>");
    EXPECT(scope::simplify_cpp_type(
               "std::__1::unique_ptr<Particle, std::__1::default_delete<Particle> >") ==
               "std::unique_ptr<Particle>",
           "std::unique_ptr<Particle>");

    // Malformed / unbalanced templates must never hang or crash
    EXPECT(!scope::simplify_cpp_type("std::__1::vector<int, std::__1::allocator<int").empty(),
           "malformed template resilience");
    EXPECT(scope::simplify_function_name("") == "<unknown>", "empty function name fallback");
}

void test_runtime_frame_detection() {
    std::printf("[UNIT] Runtime frame classification\n");
    EXPECT(scope::is_runtime_frame("start", "dyld", ""), "dyld start");
    EXPECT(scope::is_runtime_frame("_pthread_start", "libsystem_pthread.dylib", ""), "pthread start");
    EXPECT(scope::is_runtime_frame("__assert_rtn", "libsystem_c.dylib", ""), "assert_rtn");
    EXPECT(scope::is_runtime_frame("std::__1::vector<int>::push_back", "app", "/usr/include/c++/v1/vector"),
           "stdlib header frame");
    EXPECT(scope::is_runtime_frame("abort", "libsystem_c.dylib", ""), "abort frame");

    EXPECT(!scope::is_runtime_frame("main", "hft_orderbook", "/Users/dev/hft_orderbook.cpp"),
           "user main frame");
    EXPECT(!scope::is_runtime_frame("OrderBook::match_order", "engine", "/Users/dev/book.cpp"),
           "user method frame");
}

void test_hex_formatting() {
    std::printf("[UNIT] Hex formatting & leading-zero offset\n");
    std::string h0 = scope::format_hex64_grouped(0);
    EXPECT(h0 == "0x0000_0000_0000_0000", "zero hex");
    EXPECT(scope::hex_significant_offset(h0) == h0.size() - 1, "zero keeps last digit bright");

    std::string h18 = scope::format_hex64_grouped(0x18);
    EXPECT(h18 == "0x0000_0000_0000_0018", "0x18 grouped hex");
    EXPECT(h18[scope::hex_significant_offset(h18)] == '1', "0x18 sig starts at 1");

    std::string hsp = scope::format_hex64_grouped(0x16fdff4a0ull);
    EXPECT(hsp == "0x0000_0001_6fdf_f4a0", "stack pointer grouped hex");
    EXPECT(hsp[scope::hex_significant_offset(hsp)] == '1', "stack pointer sig starts at 1");

    std::string hmax = scope::format_hex64_grouped(UINT64_MAX);
    EXPECT(hmax == "0xffff_ffff_ffff_ffff", "max u64 grouped hex");
    EXPECT(scope::hex_significant_offset(hmax) == 2, "max u64 sig starts right after 0x");
}

void test_provenance_classification() {
    std::printf("[UNIT] Address provenance classification\n");
    std::vector<scope::MemoryRegion> regions = {
        {0x100000000ull, 0x100010000ull, true, false, true, "app.__TEXT", scope::ProvenanceKind::Text},
        {0x100010000ull, 0x100020000ull, true, false, false, "app.__RODATA", scope::ProvenanceKind::Rodata},
        {0x600000000000ull, 0x600010000000ull, true, true, false, "MALLOC_SMALL", scope::ProvenanceKind::Heap},
        {0x700000000000ull, 0x700000004000ull, false, false, false, "GUARD", scope::ProvenanceKind::Unmapped},
    };
    const uint64_t sp = 0x16fdff000ull;

    EXPECT(scope::classify_address(0, regions, sp) == scope::ProvenanceKind::Null, "null 0x0");
    EXPECT(scope::classify_address(0x18, regions, sp) == scope::ProvenanceKind::Null, "null+offset 0x18");
    EXPECT(scope::classify_address(sp + 0x200, regions, sp) == scope::ProvenanceKind::Stack, "near SP -> STACK");
    EXPECT(scope::classify_address(0x100004000ull, regions, sp) == scope::ProvenanceKind::Text, "TEXT region");
    EXPECT(scope::classify_address(0x100014000ull, regions, sp) == scope::ProvenanceKind::Rodata, "RODATA region");
    EXPECT(scope::classify_address(0x600000001234ull, regions, sp) == scope::ProvenanceKind::Heap, "HEAP region");
    EXPECT(scope::classify_address(0x700000001000ull, regions, sp) == scope::ProvenanceKind::Unmapped, "GUARD -> UNMAPPED");
    EXPECT(scope::classify_address(0xdeadbeef1234ull, regions, sp) == scope::ProvenanceKind::Unmapped, "outside all -> UNMAPPED");
    EXPECT(scope::classify_address(0xdeadbeef1234ull, {}, 0) == scope::ProvenanceKind::Unknown, "empty regions -> UNKNOWN");

    EXPECT(std::string(scope::provenance_label(scope::ProvenanceKind::Stack)) == "STACK", "STACK label");
    EXPECT(std::string(scope::provenance_label(scope::ProvenanceKind::Unmapped)) == "UNMAPPED!", "UNMAPPED label");
}

void test_syntax_highlighter(const std::string& fixtures_src_dir) {
    std::printf("[UNIT] Tree-sitter C++20 syntax highlighter\n");
    scope::SyntaxHighlighter hl;

    // Missing file must fail gracefully
    const auto& missing = hl.get_file("/nonexistent/path/does_not_exist.cpp");
    EXPECT(!missing.loaded, "missing file not loaded");
    EXPECT(missing.lines.empty(), "missing file has 0 lines");

    std::string sample = (std::filesystem::path(fixtures_src_dir) / "hft_orderbook.cpp").string();
    const auto& hf = hl.get_file(sample);
    EXPECT(hf.loaded, "hft_orderbook.cpp loaded");
    EXPECT(hf.lines.size() > 100, "hft_orderbook.cpp has >100 lines");
    EXPECT(hf.lines.size() == hf.line_spans.size(), "1:1 lines and span vectors");

    bool saw_kw = false, saw_type = false, saw_str = false, saw_comment = false, saw_preproc = false;
    bool all_in_bounds = true;
    bool all_sorted = true;

    for (size_t i = 0; i < hf.lines.size(); ++i) {
        uint32_t len = static_cast<uint32_t>(hf.lines[i].size());
        uint32_t prev_start = 0;
        for (size_t j = 0; j < hf.line_spans[i].size(); ++j) {
            const auto& sp = hf.line_spans[i][j];
            if (sp.start_col > sp.end_col || sp.end_col > len) all_in_bounds = false;
            if (j > 0 && sp.start_col < prev_start) all_sorted = false;
            prev_start = sp.start_col;
            if (sp.kind == scope::SyntaxTokenKind::Keyword) saw_kw = true;
            if (sp.kind == scope::SyntaxTokenKind::Type) saw_type = true;
            if (sp.kind == scope::SyntaxTokenKind::String) saw_str = true;
            if (sp.kind == scope::SyntaxTokenKind::Comment) saw_comment = true;
            if (sp.kind == scope::SyntaxTokenKind::Preprocessor) saw_preproc = true;
        }
    }

    EXPECT(all_in_bounds, "all syntax spans strictly within line bounds");
    EXPECT(all_sorted, "all syntax spans sorted by start_col");
    EXPECT(saw_kw, "detected C++ keywords");
    EXPECT(saw_type, "detected C++ types");
    EXPECT(saw_str, "detected string literals");
    EXPECT(saw_comment, "detected comments");
    EXPECT(saw_preproc, "detected preprocessor directives");

    // Invalidate & re-parse
    size_t orig_lines = hf.lines.size();
    hl.invalidate(sample);
    const auto& hf2 = hl.get_file(sample);
    EXPECT(hf2.loaded && hf2.lines.size() == orig_lines, "invalidate + reload preserves file");
}

} // namespace

int main(int argc, char** argv) {
    std::string fixtures_src = (argc > 1)
                                   ? std::string(argv[1])
                                   : (std::filesystem::current_path() / "fixtures").string();

    test_type_simplification();
    test_runtime_frame_detection();
    test_hex_formatting();
    test_provenance_classification();
    test_syntax_highlighter(fixtures_src);

    std::printf("UNIT TESTS COMPLETE: %d passed, %d failed\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
