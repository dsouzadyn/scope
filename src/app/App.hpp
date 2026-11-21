#pragma once

#include "../engine/DebuggerEngine.hpp"
#include "../engine/SyntaxHighlighter.hpp"
#include "../ui/TextureBridge.hpp"
#include "../ui/UiContext.hpp"

#include <string>
#include <vector>

namespace scope {

class App {
public:
    App(TextureBridge* textures, const std::string& app_dir, const std::string& initial_target);
    ~App();

    void render_frame();

private:
    void discover_fixtures(const std::string& app_dir);
    void handle_shortcuts();
    void build_default_dock_layout(unsigned int dockspace_id);

    DebuggerEngine engine_;
    SyntaxHighlighter highlighter_;
    ui::UiContext ctx_;
    bool layout_initialized_ = false;
};

} // namespace scope
