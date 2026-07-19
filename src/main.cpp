// Nxrth — entry point. Dear ImGui + GLFW + DirectX 11, borderless fixed-size
// window (700x650) with a custom title bar (lock / always-on-top / minimize /
// close). All Nxrth UI lives in ui/app_ui.*, styling + fonts in ui/theme.*.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"

#include "automation/config_store.h"
#include "bot/bot_manager.h"
#include "core/constants.h"
#include "core/logger.h"
#include "mcp/app_bridge.h"
#include "proxy/proxy_pool.h"
#include "recovery/recovery_controller.h"
#include "ui/app_ui.h"
#include "ui/dx11_renderer.h"
#include "ui/theme.h"
#include "ui/window_host.h"

namespace {

constexpr int kWinW = 700;
constexpr int kWinH = 500;

// GLFW-backed window controller the custom title bar drives.
class GlfwHost : public nxrth::ui::IWindowHost {
public:
    explicit GlfwHost(GLFWwindow* w) : w_(w) {}
    bool is_locked() const override { return locked_; }
    void set_locked(bool v) override { locked_ = v; }
    bool is_always_on_top() const override { return floating_; }
    void set_always_on_top(bool v) override {
        floating_ = v;
        glfwSetWindowAttrib(w_, GLFW_FLOATING, v ? GLFW_TRUE : GLFW_FALSE);
    }
    void minimize() override { glfwIconifyWindow(w_); }
    void request_close() override { glfwSetWindowShouldClose(w_, GLFW_TRUE); }
    void begin_drag() override {
        if (locked_) return;
        POINT p{};
        ::GetCursorPos(&p);
        grab_cursor_x_ = p.x;
        grab_cursor_y_ = p.y;
        glfwGetWindowPos(w_, &grab_win_x_, &grab_win_y_);
    }
    void drag_update() override {
        if (locked_) return;
        POINT p{};
        ::GetCursorPos(&p);  // absolute screen coords — independent of the window
        glfwSetWindowPos(w_, grab_win_x_ + (p.x - grab_cursor_x_),
                         grab_win_y_ + (p.y - grab_cursor_y_));
    }

private:
    GLFWwindow* w_;
    bool locked_ = false;
    bool floating_ = false;
    long grab_cursor_x_ = 0, grab_cursor_y_ = 0;  // screen cursor at grab
    int grab_win_x_ = 0, grab_win_y_ = 0;         // window pos at grab
};

// In-process event sink: bots push console lines here; forward to the shared
// Logger the Console panel reads. Everything else is mirrored into BotState.
class UiEventSink : public nxrth::bot::EventSink {
public:
    void bot_added(std::uint32_t bot_id, const std::string& username) override {
        nxrth::log("[Manager] bot #" + std::to_string(bot_id) + " added" +
                    (username.empty() ? "" : " (" + username + ")"));
    }
    void bot_removed(std::uint32_t bot_id) override {
        nxrth::log("[Manager] bot #" + std::to_string(bot_id) + " removed");
    }
    void console(std::uint32_t bot_id, const std::string& message) override {
        nxrth::Logger::Instance().Log(message, static_cast<int>(bot_id));
    }
};

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void show_startup_error(const std::string& message) {
    MessageBoxA(nullptr, message.c_str(), "Nxrth - startup error", MB_OK | MB_ICONERROR);
}

std::filesystem::path application_root() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::filesystem::current_path();
    buffer.resize(length);
    const auto executable_dir = std::filesystem::path(buffer).parent_path();
    auto candidate = executable_dir;
    for (int depth = 0; depth < 5 && !candidate.empty(); ++depth) {
        std::error_code ec;
        if (std::filesystem::exists(candidate / "CMakeLists.txt", ec)) return candidate;
        candidate = candidate.parent_path();
    }
    candidate = executable_dir;
    for (int depth = 0; depth < 5 && !candidate.empty(); ++depth) {
        std::error_code ec;
        if (std::filesystem::exists(candidate / "data", ec)) return candidate;
        candidate = candidate.parent_path();
    }
    return executable_dir;
}

}  // namespace

int main(int argc, char** argv) {
    // Every persistence path is relative to the application root. Resolve it
    // from the executable so shortcuts and supervisor restarts cannot silently
    // select a different data/proxy/scripts directory.
    try {
        std::filesystem::current_path(application_root());
    } catch (...) {
    }

    std::optional<std::filesystem::path> recovery_plan;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--recover-plan") {
            recovery_plan = std::filesystem::path(argv[++i]);
            break;
        }
    }
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        show_startup_error("Nxrth window system failed to initialize (GLFW).\n"
                           "Make sure the VDS desktop session is open.");
        return 1;
    }

    // GLFW owns the Win32 window and input only. DirectX owns rendering, so a
    // VDS never needs an OpenGL context or the VMware OpenGL helper.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* window = glfwCreateWindow(kWinW, kWinH, "Nxrth", nullptr, nullptr);
    if (!window) {
        show_startup_error("Nxrth window could not be created.\n"
                           "An active Windows desktop session on the VDS is required.");
        glfwTerminate();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // fixed single-window layout — no imgui.ini

    nxrth::ui::ApplyTheme();
    nxrth::ui::LoadFonts();  // Tahoma + FontAwesome merge

    if (!ImGui_ImplGlfw_InitForOther(window, true)) {
        show_startup_error("ImGui GLFW backend failed to initialize.");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    nxrth::ui::Dx11Renderer renderer;
    std::string renderer_error;
    if (!renderer.initialize(window, renderer_error)) {
        show_startup_error(renderer_error);
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    nxrth::log(std::string("Nxrth ") + std::string(nxrth::constants::APP_VERSION) +
                " started — GT " + std::string(nxrth::constants::GAME_VER) + " / proto " +
                std::to_string(nxrth::constants::PROTOCOL) + ".");
    nxrth::log(std::string("[UI] DirectX 11 renderer: ") +
                (renderer.using_warp() ? "WARP software" : "hardware"));

    GlfwHost host(window);
    auto sink = std::make_shared<UiEventSink>();
    nxrth::bot::BotManager manager(sink);
    nxrth::proxy::ProxyPool proxy_pool = nxrth::proxy::ProxyPool::load_default();

    // Restore the saved automation config (geiger hunt/depot/pickup worlds, module
    // enables, etc.) BEFORE the UI reads it on the first frame.
    {
        nxrth::bot::AutomationConfig cfg;
        if (nxrth::automation::load_automation_config(cfg)) {
            // Never auto-arm AutoGeiger on startup: a freshly-loaded config must not
            // make every (including later-added) bot warp on spawn. Force an EMPTY
            // geiger scope (= nobody, see AutomationConfig::is_on_for) so the user
            // arms specific bots each session from the Executor. Worlds/params/enable
            // still persist and load; only the per-bot arming is reset.
            cfg.module_bot_ids["geiger"] = {};
            manager.fleet()->set_config(std::move(cfg));
        }
    }

    if (recovery_plan) {
        const auto restored = nxrth::recovery::RecoveryController::restore_from_plan(
            *recovery_plan, manager, proxy_pool);
        if (restored.fleet) {
            nxrth::log("[Recovery] restored " +
                        std::to_string(restored.fleet->spawned_count) + " of " +
                        std::to_string(restored.fleet->record_count) +
                        " bot(s) from the protected local checkpoint.");
        }
        if (restored.script) {
            nxrth::log(restored.script->ok ? "[Recovery] saved Lua script completed."
                                             : "[Recovery] saved Lua script failed.");
        }
        if (!restored.ok) {
            // Recovery errors can originate in user-authored Lua; keep their
            // contents out of the shared log's AI-visible surface.
            nxrth::log(
                "[Recovery] startup restore was partial or failed; no secret-bearing details logged.");
        }
    }

    nxrth::ui::AppUi app(manager, proxy_pool, host);
    nxrth::mcp::AppMcpBridgeServer mcp_bridge(manager, proxy_pool);
    nxrth::recovery::RecoveryRuntime recovery_runtime(manager, proxy_pool);
    nxrth::log("[MCP] Desktop shared-fleet bridge enabled.");

    // UI-layout screenshot aid: NXRTH_TESTBOT=1 spawns one offline bot so the
    // detail pane can be inspected without live credentials.
    if (std::getenv("NXRTH_TESTBOT"))
        manager.spawn("merhaba", "testpass", std::nullopt, std::nullopt,
                      nxrth::bot::ProxyPolicy::Direct);

    using UiClock = std::chrono::steady_clock;
    auto next_ui_frame = UiClock::now();
    while (!glfwWindowShouldClose(window)) {
        const auto now = UiClock::now();
        const bool minimized = glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE;
        int fps = app.target_fps();  // Settings-adjustable UI cap (default 30)
        if (fps < 5) fps = 5;
        if (fps > 240) fps = 240;
        const auto frame_interval = minimized ? std::chrono::milliseconds(200)
                                              : std::chrono::milliseconds(1000 / fps);
        if (now < next_ui_frame) {
            const std::chrono::duration<double> remaining = next_ui_frame - now;
            glfwWaitEventsTimeout(remaining.count());
            continue;
        }
        next_ui_frame = now + frame_interval;
        glfwPollEvents();

        // Execute AI requests on the same thread that owns BotManager/ProxyPool.
        // Pipe I/O stays on the bridge worker and never blocks rendering.
        mcp_bridge.pump();
        recovery_runtime.tick();

        renderer.new_frame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.Draw();

        ImGui::Render();
        constexpr float clear_color[4] = {0.06f, 0.06f, 0.08f, 1.0f};
        renderer.render(ImGui::GetDrawData(), clear_color);
    }

    mcp_bridge.stop();

    renderer.shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
