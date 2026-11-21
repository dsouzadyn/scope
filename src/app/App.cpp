#include "App.hpp"
#include "../ui/Theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>

namespace scope {

App::App(TextureBridge* textures,
         const std::string& app_dir,
         const std::string& initial_target) {
    ctx_.engine = &engine_;
    ctx_.highlighter = &highlighter_;
    ctx_.textures = textures;

    discover_fixtures(app_dir);

    if (!initial_target.empty() && std::filesystem::exists(initial_target)) {
        engine_.launch(initial_target, {}, "", true);
    }
}

App::~App() = default;

void App::discover_fixtures(const std::string& app_dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> search_dirs = {
        fs::path(app_dir) / "fixtures",
        fs::path(app_dir).parent_path() / "fixtures",
        fs::current_path() / "build" / "fixtures",
        fs::current_path() / "fixtures",
    };

    for (const auto& dir : search_dirs) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            auto perms = entry.status().permissions();
            bool exec = (perms & fs::perms::owner_exec) != fs::perms::none;
            auto ext = entry.path().extension().string();
            if (exec && ext.empty()) {
                std::string p = fs::weakly_canonical(entry.path()).string();
                if (std::find(ctx_.discovered_fixtures.begin(),
                              ctx_.discovered_fixtures.end(), p) == ctx_.discovered_fixtures.end()) {
                    ctx_.discovered_fixtures.push_back(p);
                }
            }
        }
        if (!ctx_.discovered_fixtures.empty()) break;
    }
    std::sort(ctx_.discovered_fixtures.begin(), ctx_.discovered_fixtures.end());
}

void App::handle_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;

    bool super_or_ctrl = io.KeySuper || io.KeyCtrl;

    if (super_or_ctrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        ctx_.open_command_palette = true;
    }
    if (super_or_ctrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        ctx_.history_cursor = -1;
        engine_.restart();
    }

    auto snap = ctx_.snap;
    if (!snap) return;
    bool stopped = (snap->state == SessionState::Stopped || snap->state == SessionState::Fault);
    bool running = (snap->state == SessionState::Running);

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && stopped) {
        ctx_.history_cursor = -1;
        engine_.continue_exec();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false) && running) {
        engine_.pause_exec();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false) && stopped) {
        ctx_.history_cursor = -1;
        if (super_or_ctrl && io.KeyShift) {
            engine_.step_instruction();
        } else {
            engine_.step_over();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && stopped) {
        ctx_.history_cursor = -1;
        if (io.KeyShift) {
            engine_.step_out();
        } else {
            engine_.step_into();
        }
    }
}

void App::build_default_dock_layout(unsigned int dockspace_id) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

    ImGuiID dock_main = dockspace_id;
    ImGuiID dock_left = 0;
    ImGuiID dock_right = 0;
    ImGuiID dock_bottom = 0;
    ImGuiID dock_right_bottom = 0;

    // Classic Precision Instrument Workbench:
    // Left (19%): 04 STACK & THREADS
    // Right (29%): Top = 02 INSPECTOR, Bottom = 05 REGISTERS
    // Center: Top (66%) = 01 SOURCE, Bottom (34%) = 03 MEMORY + 06 PROBES + 07 CONSOLE
    ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.19f, &dock_left, &dock_main);
    ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.34f, &dock_right, &dock_main);
    ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.34f, &dock_bottom, &dock_main);
    ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.36f, &dock_right_bottom, &dock_right);

    ImGui::DockBuilderDockWindow("01 SOURCE", dock_main);
    ImGui::DockBuilderDockWindow("04 STACK & THREADS", dock_left);
    ImGui::DockBuilderDockWindow("02 INSPECTOR", dock_right);
    ImGui::DockBuilderDockWindow("05 REGISTERS", dock_right_bottom);
    ImGui::DockBuilderDockWindow("03 MEMORY", dock_bottom);
    ImGui::DockBuilderDockWindow("06 PROBES", dock_bottom);
    ImGui::DockBuilderDockWindow("07 CONSOLE", dock_bottom);

    ImGui::DockBuilderFinish(dockspace_id);
}

void App::render_frame() {
    ctx_.history = engine_.history_snapshots();
    if (ctx_.history_cursor >= 0 &&
        ctx_.history_cursor < static_cast<int>(ctx_.history.size())) {
        ctx_.snap = ctx_.history[ctx_.history_cursor];
    } else {
        ctx_.history_cursor = -1;
        ctx_.snap = engine_.latest_snapshot();
    }

    handle_shortcuts();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top_h = ui::draw_transport_and_timeline(ctx_, vp->WorkSize.x, vp->WorkPos.y);

    // Main Dockspace below the Transport + Step-Delta Timeline Deck
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + top_h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - top_h));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                  ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##ScopeDockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("##ScopeDockspace");
    if (!layout_initialized_) {
        layout_initialized_ = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            build_default_dock_layout(dockspace_id);
        }
    }
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    // Render all 7 Precision Instrument Bays
    ui::draw_code_canvas_bay(ctx_);
    ui::draw_inspector_bay(ctx_);
    ui::draw_memory_bay(ctx_);
    ui::draw_stack_thread_bay(ctx_);
    ui::draw_registers_bay(ctx_);
    ui::draw_probes_bay(ctx_);
    ui::draw_console_bay(ctx_);
    ui::draw_modals_and_palette(ctx_);
}

} // namespace scope
