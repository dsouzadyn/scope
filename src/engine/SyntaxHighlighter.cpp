#include "SyntaxHighlighter.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>

extern "C" const TSLanguage* tree_sitter_cpp();

namespace scope {

namespace {

bool is_cpp_keyword(std::string_view s) {
    static constexpr std::string_view kKeywords[] = {
        "alignas", "alignof", "auto", "bool", "break", "case", "catch", "char",
        "char8_t", "char16_t", "char32_t", "class", "co_await", "co_return",
        "co_yield", "concept", "const", "consteval", "constexpr", "constinit",
        "const_cast", "continue", "decltype", "default", "delete", "do",
        "double", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "float", "for", "friend", "goto", "if", "inline",
        "int", "long", "mutable", "namespace", "new", "noexcept", "nullptr",
        "operator", "override", "final", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "short",
        "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try",
        "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while",
    };
    for (auto kw : kKeywords) {
        if (s == kw) return true;
    }
    return false;
}

SyntaxTokenKind classify_leaf(TSNode node, const std::string& src) {
    const char* type_c = ts_node_type(node);
    std::string_view t(type_c ? type_c : "");
    TSNode parent = ts_node_parent(node);
    std::string_view pt = ts_node_is_null(parent) ? "" : std::string_view(ts_node_type(parent));

    if (t == "comment") return SyntaxTokenKind::Comment;
    if (t == "string_literal" || t == "string_content" || t == "char_literal" ||
        t == "system_lib_string" || t == "raw_string_literal" || t == "escape_sequence" ||
        t == "\"") {
        if (pt == "string_literal" || pt == "char_literal" || t == "system_lib_string" ||
            t == "string_literal" || t == "raw_string_literal" || t == "char_literal") {
            return SyntaxTokenKind::String;
        }
    }
    if (t == "number_literal") return SyntaxTokenKind::Number;
    if (t == "true" || t == "false" || t == "nullptr") return SyntaxTokenKind::Number;

    if (t == "primitive_type" || t == "type_identifier" || t == "sized_type_specifier" ||
        t == "auto") {
        return SyntaxTokenKind::Type;
    }

    if (t == "namespace_identifier") return SyntaxTokenKind::Namespace;
    if (t == "field_identifier") return SyntaxTokenKind::Field;

    if (t == "preproc_directive" || t == "#include" || t == "#define" || t == "#ifdef" ||
        t == "#ifndef" || t == "#endif" || t == "#pragma" || t == "#if" || t == "#else" ||
        t == "#elif") {
        return SyntaxTokenKind::Preprocessor;
    }

    if (t == "identifier") {
        if (pt == "function_declarator" || pt == "call_expression") {
            return SyntaxTokenKind::Function;
        }
        if (pt == "qualified_identifier") {
            TSNode gp = ts_node_parent(parent);
            std::string_view gpt = ts_node_is_null(gp) ? "" : std::string_view(ts_node_type(gp));
            if (gpt == "call_expression" || gpt == "function_declarator") {
                return SyntaxTokenKind::Function;
            }
        }
        if (pt == "preproc_def" || pt == "preproc_function_def") {
            return SyntaxTokenKind::Preprocessor;
        }
    }

    uint32_t s = ts_node_start_byte(node);
    uint32_t e = ts_node_end_byte(node);
    if (e > s && e <= src.size()) {
        std::string_view text(src.data() + s, e - s);
        if (is_cpp_keyword(text)) {
            if (text == "int" || text == "float" || text == "double" || text == "void" ||
                text == "bool" || text == "char" || text == "unsigned" || text == "signed" ||
                text == "long" || text == "short" || text == "auto") {
                return SyntaxTokenKind::Type;
            }
            return SyntaxTokenKind::Keyword;
        }
        if (!text.empty() &&
            (text[0] == '+' || text[0] == '-' || text[0] == '*' || text[0] == '/' ||
             text[0] == '=' || text[0] == '!' || text[0] == '<' || text[0] == '>' ||
             text[0] == '&' || text[0] == '|' || text[0] == '^' || text[0] == '%' ||
             text[0] == '~' || text[0] == ':' || text[0] == '?' || text[0] == '.')) {
            return SyntaxTokenKind::Operator;
        }
    }

    return SyntaxTokenKind::Plain;
}

void collect_spans(TSNode node,
                   const std::string& src,
                   std::vector<std::vector<SyntaxSpan>>& line_spans) {
    uint32_t child_count = ts_node_child_count(node);
    std::string_view t(ts_node_type(node));

    // Treat comments and string literals atomically so multiline spans color cleanly.
    bool atomic = (child_count == 0) ||
                  (t == "comment") ||
                  (t == "string_literal") ||
                  (t == "char_literal") ||
                  (t == "system_lib_string") ||
                  (t == "raw_string_literal");

    if (atomic) {
        SyntaxTokenKind kind = classify_leaf(node, src);
        if (kind == SyntaxTokenKind::Plain) return;

        TSPoint sp = ts_node_start_point(node);
        TSPoint ep = ts_node_end_point(node);
        for (uint32_t row = sp.row; row <= ep.row && row < line_spans.size(); ++row) {
            uint32_t col_start = (row == sp.row) ? sp.column : 0;
            uint32_t col_end = (row == ep.row) ? ep.column : 100000u;
            if (col_end > col_start) {
                line_spans[row].push_back(SyntaxSpan{col_start, col_end, kind});
            }
        }
        return;
    }

    for (uint32_t i = 0; i < child_count; ++i) {
        collect_spans(ts_node_child(node, i), src, line_spans);
    }
}

} // namespace

SyntaxHighlighter::SyntaxHighlighter() = default;
SyntaxHighlighter::~SyntaxHighlighter() = default;

void SyntaxHighlighter::invalidate(const std::string& path) {
    cache_.erase(path);
}

const HighlightedFile& SyntaxHighlighter::get_file(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) {
        return it->second;
    }
    auto [inserted, _] = cache_.emplace(path, parse_file(path));
    return inserted->second;
}

HighlightedFile SyntaxHighlighter::parse_file(const std::string& path) {
    HighlightedFile hf;
    hf.path = path;

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return hf;
    }

    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();

    // Split lines preserving line numbers 1:1
    size_t start = 0;
    while (start <= content.size()) {
        size_t nl = content.find('\n', start);
        if (nl == std::string::npos) {
            hf.lines.push_back(content.substr(start));
            break;
        }
        std::string line = content.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        hf.lines.push_back(std::move(line));
        start = nl + 1;
    }

    hf.line_spans.resize(hf.lines.size());

    TSParser* parser = ts_parser_new();
    if (parser && ts_parser_set_language(parser, tree_sitter_cpp())) {
        TSTree* tree = ts_parser_parse_string(parser, nullptr, content.data(),
                                              static_cast<uint32_t>(content.size()));
        if (tree) {
            collect_spans(ts_tree_root_node(tree), content, hf.line_spans);
            ts_tree_delete(tree);
        }
    }
    if (parser) {
        ts_parser_delete(parser);
    }

    // Clamp & sort spans per line
    for (size_t i = 0; i < hf.lines.size(); ++i) {
        auto& spans = hf.line_spans[i];
        uint32_t len = static_cast<uint32_t>(hf.lines[i].size());
        for (auto& sp : spans) {
            sp.start_col = std::min(sp.start_col, len);
            sp.end_col = std::min(sp.end_col, len);
        }
        std::sort(spans.begin(), spans.end(), [](const SyntaxSpan& a, const SyntaxSpan& b) {
            return a.start_col < b.start_col;
        });
    }

    hf.loaded = true;
    return hf;
}

} // namespace scope
