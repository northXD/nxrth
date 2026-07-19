#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lua/lua_engine.h"
#include "recovery/fleet_store.h"

namespace nxrth::bot {
struct AutomationConfig;
class BotManager;
}
namespace nxrth::proxy {
class ProxyPool;
}

namespace nxrth::recovery {

enum class RestoreMode { SnapshotOnly, ScriptOnly, SnapshotThenScript };

const char* to_string(RestoreMode mode);
std::optional<RestoreMode> restore_mode_from_string(std::string_view value);

struct RecoveryOptions {
    bool enabled = true;
    RestoreMode mode = RestoreMode::SnapshotOnly;
    std::string fleet_name = "last_session";
    std::optional<std::string> script_name;
    std::uint32_t restart_delay_ms = 2000;
    std::uint32_t max_restarts = 3;
    std::uint32_t window_seconds = 600;
};

struct RecoveryStatus {
    bool configured = false;
    bool enabled = false;
    RestoreMode mode = RestoreMode::SnapshotOnly;
    std::string fleet_name;
    std::optional<std::string> script_name;
    std::uint32_t restart_delay_ms = 0;
    std::uint32_t max_restarts = 0;
    std::uint32_t window_seconds = 0;
    std::uint32_t supervisor_pid = 0;
    bool supervisor_running = false;
    std::string error;
};

struct RecoveryConfigureResult {
    bool ok = false;
    RecoveryStatus status;
    std::optional<FleetSaveResult> checkpoint;
    std::string error;
};

struct RecoveryRestoreResult {
    bool ok = false;
    std::optional<FleetLoadResult> fleet;
    std::optional<nxrth::lua::LuaExecutionResult> script;
    std::string error;
};

class RecoveryController {
public:
    static std::filesystem::path plan_path();

    // Configures deterministic out-of-process recovery. Exact bot secrets are
    // checkpointed through FleetStore and never included in this result.
    static RecoveryConfigureResult configure(const RecoveryOptions& options,
                                              nxrth::bot::BotManager& manager);
    static RecoveryConfigureResult disable();
    static RecoveryStatus status();

    // Starts the sibling NxrthSupervisor.exe for this process when needed.
    // Intended for the desktop app only, never the headless MCP process.
    static bool ensure_supervisor(std::string* error = nullptr);

    // Called only during startup for an explicit --recover-plan launch.
    static RecoveryRestoreResult restore_from_plan(
        const std::filesystem::path& requested_plan,
        nxrth::bot::BotManager& manager,
        nxrth::proxy::ProxyPool& proxy_pool);
};

// Owner-thread periodic checkpoint. It writes only when the fleet roster or
// AutomationConfig handle changed, and re-establishes the supervisor if needed.
class RecoveryRuntime {
public:
    RecoveryRuntime(nxrth::bot::BotManager& manager,
                    nxrth::proxy::ProxyPool& proxy_pool);
    void tick();

private:
    nxrth::bot::BotManager& manager_;
    nxrth::proxy::ProxyPool& proxy_pool_;
    std::chrono::steady_clock::time_point next_tick_{};
    std::uint64_t saved_generation_ = UINT64_MAX;
    std::shared_ptr<const nxrth::bot::AutomationConfig> saved_config_;
    std::string saved_fleet_name_;
};

}  // namespace nxrth::recovery
