#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace scope {

enum class SyntaxTokenKind : uint8_t {
    Plain,
    Keyword,
    Type,
    Function,
    Field,
    Number,
    String,
    Comment,
    Preprocessor,
    Operator,
    Namespace,
};

struct SyntaxSpan {
    uint32_t start_col = 0;
    uint32_t end_col = 0;
    SyntaxTokenKind kind = SyntaxTokenKind::Plain;
};

struct HighlightedFile {
    std::string path;
    std::vector<std::string> lines;
    std::vector<std::vector<SyntaxSpan>> line_spans;
    bool loaded = false;
};

class SyntaxHighlighter {
public:
    SyntaxHighlighter();
    ~SyntaxHighlighter();

    SyntaxHighlighter(const SyntaxHighlighter&) = delete;
    SyntaxHighlighter& operator=(const SyntaxHighlighter&) = delete;

    // Loads and highlights a file from disk (cached by path).
    const HighlightedFile& get_file(const std::string& path);
    void invalidate(const std::string& path);

private:
    HighlightedFile parse_file(const std::string& path);
    std::unordered_map<std::string, HighlightedFile> cache_;
};

} // namespace scope
