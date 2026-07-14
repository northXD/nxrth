// Adonai — entry point. Dear ImGui + GLFW + OpenGL 3, borderless fixed-size
// window (700x650) with a custom title bar (lock / always-on-top / minimize /
// close). All Adonai UI lives in ui/app_ui.*, styling + fonts in ui/theme.*.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "automation/config_store.h"
#include "bot/bot_manager.h"
#include "core/constants.h"
#include "core/logger.h"
#include "mcp/app_bridge.h"
#include "proxy/proxy_pool.h"
#include "ui/app_ui.h"
#include "ui/theme.h"
#include "ui/window_host.h"

namespace {

constexpr int kWinW = 700;
constexpr int kWinH = 500;

// GLFW-backed window controller the custom title bar drives.
class GlfwHost : public adonai::ui::IWindowHost {
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
    void drag_by(float dx, float dy) override {
        if (locked_ || (dx == 0.0f && dy == 0.0f)) return;
        int x = 0, y = 0;
        glfwGetWindowPos(w_, &x, &y);
        glfwSetWindowPos(w_, x + static_cast<int>(dx), y + static_cast<int>(dy));
    }

private:
    GLFWwindow* w_;
    bool locked_ = false;
    bool floating_ = false;
};

// In-process event sink: bots push console lines here; forward to the shared
// Logger the Console panel reads. Everything else is mirrored into BotState.
class UiEventSink : public adonai::bot::EventSink {
public:
    void bot_added(std::uint32_t bot_id, const std::string& username) override {
        adonai::log("[Manager] bot #" + std::to_string(bot_id) + " added" +
                    (username.empty() ? "" : " (" + username + ")"));
    }
    void bot_removed(std::uint32_t bot_id) override {
        adonai::log("[Manager] bot #" + std::to_string(bot_id) + " removed");
    }
    void console(std::uint32_t bot_id, const std::string& message) override {
        adonai::Logger::Instance().Log(message, static_cast<int>(bot_id));
    }
};

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

}  // namespace

int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    // Borderless (custom title bar), fixed size, no OS resize.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(kWinW, kWinH, "Adonai", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // fixed single-window layout — no imgui.ini

    adonai::ui::ApplyTheme();
    adonai::ui::LoadFonts();  // Tahoma + FontAwesome merge

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    adonai::log(std::string("Adonai ") + std::string(adonai::constants::APP_VERSION) +
                " started — GT " + std::string(adonai::constants::GAME_VER) + " / proto " +
                std::to_string(adonai::constants::PROTOCOL) + ".");

    GlfwHost host(window);
    auto sink = std::make_shared<UiEventSink>();
    adonai::bot::BotManager manager(sink);
    adonai::proxy::ProxyPool proxy_pool = adonai::proxy::ProxyPool::load_default();

    // Restore the saved automation config (geiger hunt/depot/pickup worlds, module
    // enables, etc.) BEFORE the UI reads it on the first frame.
    {
        adonai::bot::AutomationConfig cfg;
        if (adonai::automation::load_automation_config(cfg)) {
            // Never auto-arm AutoGeiger on startup: a freshly-loaded config must not
            // make every (incl. newly-spawned) bot warp on spawn. Force an EMPTY
            // geiger scope (= nobody, see AutomationConfig::is_on_for) so the user
            // arms specific bots each session from the Executor. Worlds/params/enable
            // still persist and load; only the per-bot arming is reset.
            cfg.module_bot_ids["geiger"] = {};
            manager.fleet()->set_config(std::move(cfg));
        }
    }

    adonai::ui::AppUi app(manager, proxy_pool, host);
    adonai::mcp::AppMcpBridgeServer mcp_bridge(manager, proxy_pool);
    adonai::log("[MCP] Desktop shared-fleet bridge enabled.");

    // UI-layout screenshot aid: ADONAI_TESTBOT=1 spawns one offline bot so the
    // detail pane can be inspected without live credentials.
    if (std::getenv("ADONAI_TESTBOT"))
        manager.spawn("merhaba", "testpass", std::nullopt, std::nullopt);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Execute AI requests on the same thread that owns BotManager/ProxyPool.
        // Pipe I/O stays on the bridge worker and never blocks rendering.
        mcp_bridge.pump();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.Draw();

        ImGui::Render();
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    mcp_bridge.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
