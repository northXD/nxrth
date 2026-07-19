#include "recovery/recovery_controller.h"

#include <algorithm>
#include <fstream>
#include <string_view>
#include <vector>

#include <windows.h>
#include <nlohmann/json.hpp>

#include "bot/bot_manager.h"
#include "proxy/proxy_pool.h"
#include "script/script_store.h"

namespace nxrth::recovery {
namespace {

using json = nlohmann::json;

struct StoredPlan {
    RecoveryOptions options;
    std::filesystem::path app_path;
    std::filesystem::path working_directory;
    std::uint32_t supervisor_pid = 0;
};

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::string path_text(const std::filesystem::path& path) { return utf8(path.wstring()); }

std::filesystem::path path_from_text(const std::string& text) {
    return std::filesystem::path(wide(text));
}

std::filesystem::path current_module_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

bool process_running(std::uint32_t pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

json plan_json(const StoredPlan& plan) {
    return {
        {"schema", "nxrth.recovery.v1"},
        {"enabled", plan.options.enabled},
        {"restore_mode", to_string(plan.options.mode)},
        {"fleet_name", plan.options.fleet_name},
        {"script_name", plan.options.script_name ? json(*plan.options.script_name) : json(nullptr)},
        {"restart_delay_ms", plan.options.restart_delay_ms},
        {"max_restarts", plan.options.max_restarts},
        {"window_seconds", plan.options.window_seconds},
        {"app_path", path_text(plan.app_path)},
        {"working_directory", path_text(plan.working_directory)},
        {"supervisor_pid", plan.supervisor_pid},
    };
}

std::optional<StoredPlan> parse_plan(const json& value, std::string* error) {
    try {
        if (!value.is_object() || value.value("schema", std::string{}) != "nxrth.recovery.v1")
            throw std::runtime_error("unsupported recovery plan");
        StoredPlan plan;
        plan.options.enabled = value.value("enabled", false);
        const auto mode = restore_mode_from_string(value.value("restore_mode", "snapshot_only"));
        if (!mode) throw std::runtime_error("invalid restore_mode");
        plan.options.mode = *mode;
        plan.options.fleet_name = value.value("fleet_name", "last_session");
        if (!FleetStore::valid_backup_name(plan.options.fleet_name))
            throw std::runtime_error("invalid fleet backup name");
        if (value.contains("script_name") && value["script_name"].is_string()) {
            const std::string script = value["script_name"].get<std::string>();
            if (!nxrth::script::ScriptStore::valid_name(script))
                throw std::runtime_error("invalid recovery script name");
            plan.options.script_name = script;
        }
        plan.options.restart_delay_ms =
            std::clamp(value.value("restart_delay_ms", 2000u), 500u, 60'000u);
        plan.options.max_restarts =
            std::clamp(value.value("max_restarts", 3u), 1u, 20u);
        plan.options.window_seconds =
            std::clamp(value.value("window_seconds", 600u), 30u, 86'400u);
        plan.app_path = path_from_text(value.value("app_path", std::string{}));
        plan.working_directory =
            path_from_text(value.value("working_directory", std::string{}));
        plan.supervisor_pid = value.value("supervisor_pid", 0u);
        return plan;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::optional<StoredPlan> load_plan(const std::filesystem::path& path, std::string* error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            if (error) *error = "recovery plan does not exist";
            return std::nullopt;
        }
        return parse_plan(json::parse(input), error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

bool save_plan(const StoredPlan& plan, std::string* error) {
    try {
        const auto path = RecoveryController::plan_path();
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot create recovery plan");
            output << plan_json(plan).dump(2);
            output.flush();
            if (!output) throw std::runtime_error("cannot write recovery plan");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("cannot replace recovery plan");
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

RecoveryStatus status_from(const StoredPlan& plan) {
    RecoveryStatus out;
    out.configured = true;
    out.enabled = plan.options.enabled;
    out.mode = plan.options.mode;
    out.fleet_name = plan.options.fleet_name;
    out.script_name = plan.options.script_name;
    out.restart_delay_ms = plan.options.restart_delay_ms;
    out.max_restarts = plan.options.max_restarts;
    out.window_seconds = plan.options.window_seconds;
    out.supervisor_pid = plan.supervisor_pid;
    out.supervisor_running = process_running(plan.supervisor_pid);
    return out;
}

bool mode_uses_snapshot(RestoreMode mode) {
    return mode == RestoreMode::SnapshotOnly || mode == RestoreMode::SnapshotThenScript;
}

bool mode_uses_script(RestoreMode mode) {
    return mode == RestoreMode::ScriptOnly || mode == RestoreMode::SnapshotThenScript;
}

std::wstring quote(std::wstring value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(c);
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(c);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

}  // namespace

const char* to_string(RestoreMode mode) {
    switch (mode) {
        case RestoreMode::SnapshotOnly: return "snapshot_only";
        case RestoreMode::ScriptOnly: return "script_only";
        case RestoreMode::SnapshotThenScript: return "snapshot_then_script";
    }
    return "snapshot_only";
}

std::optional<RestoreMode> restore_mode_from_string(std::string_view value) {
    if (value == "snapshot_only") return RestoreMode::SnapshotOnly;
    if (value == "script_only") return RestoreMode::ScriptOnly;
    if (value == "snapshot_then_script") return RestoreMode::SnapshotThenScript;
    return std::nullopt;
}

std::filesystem::path RecoveryController::plan_path() {
    return std::filesystem::current_path() / "data" / "recovery" / "recovery_plan.json";
}

RecoveryConfigureResult RecoveryController::configure(const RecoveryOptions& requested,
                                                       nxrth::bot::BotManager& manager) {
    RecoveryConfigureResult result;
    RecoveryOptions options = requested;
    options.restart_delay_ms = std::clamp(options.restart_delay_ms, 500u, 60'000u);
    options.max_restarts = std::clamp(options.max_restarts, 1u, 20u);
    options.window_seconds = std::clamp(options.window_seconds, 30u, 86'400u);
    if (!FleetStore::valid_backup_name(options.fleet_name)) {
        result.error = "invalid fleet backup name";
        return result;
    }
    if (mode_uses_script(options.mode)) {
        if (!options.script_name || !nxrth::script::ScriptStore::valid_name(*options.script_name)) {
            result.error = "a valid recovery script name is required";
            return result;
        }
        nxrth::script::ScriptStore scripts;
        std::string script_error;
        if (!scripts.read_exact(*options.script_name, &script_error)) {
            result.error = "recovery script is unavailable: " + script_error;
            return result;
        }
    }
    if (options.enabled && mode_uses_snapshot(options.mode)) {
        result.checkpoint = FleetStore::save(options.fleet_name, manager);
        if (!result.checkpoint->ok) {
            result.error = result.checkpoint->error;
            return result;
        }
    }

    StoredPlan plan;
    plan.options = options;
    plan.app_path = current_module_path();
    plan.working_directory = std::filesystem::current_path();
    std::string prior_error;
    if (auto prior = load_plan(plan_path(), &prior_error);
        prior && process_running(prior->supervisor_pid))
        plan.supervisor_pid = prior->supervisor_pid;
    if (plan.app_path.empty()) {
        result.error = "cannot resolve Nxrth executable path";
        return result;
    }
    if (!save_plan(plan, &result.error)) return result;
    if (options.enabled && !process_running(plan.supervisor_pid)) {
        std::string start_error;
        if (!ensure_supervisor(&start_error)) {
            result.error = start_error;
            result.status = status();
            return result;
        }
    }
    result.ok = true;
    result.status = status();
    return result;
}

RecoveryConfigureResult RecoveryController::disable() {
    RecoveryConfigureResult result;
    std::string error;
    auto plan = load_plan(plan_path(), &error);
    if (!plan) {
        result.error = error;
        return result;
    }
    plan->options.enabled = false;
    if (!save_plan(*plan, &result.error)) return result;
    result.ok = true;
    result.status = status_from(*plan);
    return result;
}

RecoveryStatus RecoveryController::status() {
    RecoveryStatus result;
    std::string error;
    auto plan = load_plan(plan_path(), &error);
    if (!plan) {
        result.error = error;
        return result;
    }
    return status_from(*plan);
}

bool RecoveryController::ensure_supervisor(std::string* error) {
    auto plan = load_plan(plan_path(), error);
    if (!plan || !plan->options.enabled) return false;
    if (process_running(plan->supervisor_pid)) return true;

    const auto module = current_module_path();
    const auto supervisor = module.parent_path() / "NxrthSupervisor.exe";
    if (!std::filesystem::exists(supervisor)) {
        if (error) *error = "NxrthSupervisor.exe is missing";
        return false;
    }
    const std::wstring command = quote(supervisor.wstring()) + L" --watch-pid " +
                                 std::to_wstring(GetCurrentProcessId()) + L" --plan " +
                                 quote(plan_path().wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(supervisor.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        plan->working_directory.c_str(), &startup, &info)) {
        if (error) *error = "failed to start NxrthSupervisor.exe";
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    plan->supervisor_pid = info.dwProcessId;
    if (!save_plan(*plan, error)) return false;
    return true;
}

RecoveryRestoreResult RecoveryController::restore_from_plan(
    const std::filesystem::path& requested_plan, nxrth::bot::BotManager& manager,
    nxrth::proxy::ProxyPool& proxy_pool) {
    RecoveryRestoreResult result;
    std::error_code ec;
    const auto expected = std::filesystem::weakly_canonical(plan_path(), ec);
    const auto requested = std::filesystem::weakly_canonical(requested_plan, ec);
    if (ec || expected != requested) {
        result.error = "recovery plan path is not trusted";
        return result;
    }
    std::string error;
    auto plan = load_plan(expected, &error);
    if (!plan || !plan->options.enabled) {
        result.error = error.empty() ? "recovery is disabled" : error;
        return result;
    }
    bool ok = true;
    if (mode_uses_snapshot(plan->options.mode)) {
        FleetLoadOptions options;
        options.replace_existing = false;
        options.restore_automation = true;
        result.fleet = FleetStore::load(plan->options.fleet_name, manager, proxy_pool, options);
        ok = result.fleet->ok;
        if (!ok) result.error = result.fleet->error;
    }
    if (mode_uses_script(plan->options.mode)) {
        nxrth::script::ScriptStore scripts;
        auto source = scripts.read_exact(*plan->options.script_name, &error);
        if (!source) {
            ok = false;
            if (!result.error.empty()) result.error += "; ";
            result.error += error;
        } else {
            nxrth::lua::LuaEngine engine(manager, proxy_pool);
            result.script = engine.execute(*source);
            if (!result.script->ok) {
                ok = false;
                if (!result.error.empty()) result.error += "; ";
                result.error += result.script->error;
            }
        }
    }
    result.ok = ok;
    return result;
}

RecoveryRuntime::RecoveryRuntime(nxrth::bot::BotManager& manager,
                                 nxrth::proxy::ProxyPool& proxy_pool)
    : manager_(manager), proxy_pool_(proxy_pool) {}

void RecoveryRuntime::tick() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_tick_) return;
    next_tick_ = now + std::chrono::seconds(2);
    const auto state = RecoveryController::status();
    if (!state.configured || !state.enabled) return;
    if (!state.supervisor_running) {
        std::string ignored;
        RecoveryController::ensure_supervisor(&ignored);
    }
    if (!mode_uses_snapshot(state.mode)) return;
    const auto config = manager_.fleet()->config_handle();
    const auto generation = manager_.launch_generation();
    if (generation == saved_generation_ && config == saved_config_ &&
        state.fleet_name == saved_fleet_name_)
        return;
    const auto saved = FleetStore::save(state.fleet_name, manager_);
    if (!saved.ok) return;
    saved_generation_ = generation;
    saved_config_ = config;
    saved_fleet_name_ = state.fleet_name;
}

}  // namespace nxrth::recovery
