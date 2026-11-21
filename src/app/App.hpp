#pragma once

#include "../engine/DebuggerEngine.hpp"
#include "../engine/SyntaxHighlighter.hpp"
#include "../ui/TextureBridge.hpp"
#include "../ui/UiContext.hpp"

#include <string>
#include <vector>

namespace scope {

/// Top-level SCOPE application controller executed on the main Metal/ImGui thread.
///
/// Owns the `DebuggerEngine` worker, `SyntaxHighlighter` cache, and shared
/// `ui::UiContext`. On the first frame, constructs the default 7-bay dockspace
/// layout via ImGui `DockBuilder` and processes global debugging shortcuts.
class App {
  public:
    /// Initializes the workbench, discovers compiled sample fixtures relative to
    /// `app_dir`, and optionally launches `initial_target` stopping at `main`.
    App(TextureBridge* textures, const std::string& app_dir, const std::string& initial_target);
    ~App();

    /// Renders one complete ImGui frame (transport deck, timeline, dockspace, all 7 bays & modals).
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
