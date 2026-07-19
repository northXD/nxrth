#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

struct Plan {
    bool enabled = false;
    std::filesystem::path app_path;
    std::filesystem::path working_directory;
    std::uint32_t restart_delay_ms = 2000;
    std::uint32_t max_restarts = 3;
    std::uint32_t window_seconds = 600;
};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring quote(std::wstring value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(c);
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(c);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool load_plan(const std::filesystem::path& path, Plan& out) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        const json data = json::parse(input);
        if (data.value("schema", std::string{}) != "nxrth.recovery.v1") return false;
        Plan plan;
        plan.enabled = data.value("enabled", false);
        plan.app_path = std::filesystem::path(wide(data.value("app_path", std::string{})));
        plan.working_directory = std::filesystem::path(
            wide(data.value("working_directory", std::string{})));
        plan.restart_delay_ms =
            std::clamp(data.value("restart_delay_ms", 2000u), 500u, 60'000u);
        plan.max_restarts = std::clamp(data.value("max_restarts", 3u), 1u, 20u);
        plan.window_seconds = std::clamp(data.value("window_seconds", 600u), 30u, 86'400u);
        if (plan.app_path.empty() || plan.working_directory.empty()) return false;
        out = std::move(plan);
        return true;
    } catch (...) {
        return false;
    }
}

void append_log(const std::filesystem::path& plan_path, const std::string& message) {
    try {
        const auto path = plan_path.parent_path() / "supervisor.log";
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) return;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char stamp[32]{};
        std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u",
                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        out << stamp << " " << message << "\n";
    } catch (...) {
    }
}

bool launch_app(const Plan& plan, const std::filesystem::path& plan_path,
                HANDLE& process, DWORD& pid, DWORD* launch_error = nullptr) {
    std::wstring command = quote(plan.app_path.wstring()) + L" --recover-plan " +
                           quote(plan_path.wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(plan.app_path.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, plan.working_directory.c_str(),
                        &startup, &info)) {
        if (launch_error) *launch_error = GetLastError();
        return false;
    }
    CloseHandle(info.hThread);
    process = info.hProcess;
    pid = info.dwProcessId;
    return true;
}

std::optional<DWORD> parse_pid(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) != "--watch-pid") continue;
        try {
            const unsigned long value = std::stoul(argv[i + 1]);
            if (value != 0 && value <= MAXDWORD) return static_cast<DWORD>(value);
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> parse_plan_path(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--plan")
            return std::filesystem::path(argv[i + 1]);
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Nxrth.Supervisor.v1");
    if (!mutex) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    const auto initial_pid = parse_pid(argc, argv);
    const auto plan_path_arg = parse_plan_path(argc, argv);
    if (!initial_pid || !plan_path_arg) {
        CloseHandle(mutex);
        return 2;
    }
    std::error_code ec;
    const auto plan_path = std::filesystem::absolute(*plan_path_arg, ec);
    if (ec) {
        CloseHandle(mutex);
        return 2;
    }

    Plan plan;
    if (!load_plan(plan_path, plan) || !plan.enabled) {
        CloseHandle(mutex);
        return 3;
    }

    HANDLE watched = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 *initial_pid);
    if (!watched) {
        CloseHandle(mutex);
        return 4;
    }
    DWORD watched_pid = *initial_pid;
    append_log(plan_path, "watching app pid=" + std::to_string(watched_pid));

    using Clock = std::chrono::steady_clock;
    std::vector<Clock::time_point> restart_attempts;
    for (;;) {
        const DWORD wait = WaitForSingleObject(watched, 1000);
        if (wait == WAIT_TIMEOUT) {
            Plan refreshed;
            if (!load_plan(plan_path, refreshed) || !refreshed.enabled) {
                append_log(plan_path, "disabled; supervisor exiting");
                CloseHandle(watched);
                break;
            }
            plan = std::move(refreshed);
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            append_log(plan_path, "process wait failed");
            CloseHandle(watched);
            break;
        }

        DWORD exit_code = 1;
        GetExitCodeProcess(watched, &exit_code);
        CloseHandle(watched);
        watched = nullptr;
        if (exit_code == 0) {
            append_log(plan_path, "clean app exit; supervisor exiting");
            break;
        }

        Plan refreshed;
        if (!load_plan(plan_path, refreshed) || !refreshed.enabled) {
            append_log(plan_path, "app exited and recovery is disabled");
            break;
        }
        plan = std::move(refreshed);
        append_log(plan_path, "abnormal app exit code=" + std::to_string(exit_code));
        bool restarted = false;
        std::uint32_t launch_attempts = 0;
        for (;;) {
            const auto now = Clock::now();
            const auto window = std::chrono::seconds(plan.window_seconds);
            restart_attempts.erase(
                std::remove_if(restart_attempts.begin(), restart_attempts.end(),
                               [&](auto at) { return now - at > window; }),
                restart_attempts.end());
            if (launch_attempts >= plan.max_restarts ||
                restart_attempts.size() >= plan.max_restarts) {
                append_log(plan_path,
                           "restart budget exhausted; manual restart required");
                break;
            }

            const auto prior_attempts =
                std::max<std::size_t>(restart_attempts.size(), launch_attempts);
            const std::uint32_t exponent = static_cast<std::uint32_t>(
                std::min<std::size_t>(prior_attempts, 5));
            restart_attempts.push_back(now);
            ++launch_attempts;
            const std::uint32_t delay = std::min<std::uint32_t>(
                60'000u, plan.restart_delay_ms * (1u << exponent));
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));

            Plan before_launch;
            if (!load_plan(plan_path, before_launch) || !before_launch.enabled) {
                append_log(plan_path, "disabled during restart delay; supervisor exiting");
                break;
            }
            plan = std::move(before_launch);
            DWORD launch_error = ERROR_SUCCESS;
            if (launch_app(plan, plan_path, watched, watched_pid, &launch_error)) {
                append_log(plan_path, "restarted app pid=" + std::to_string(watched_pid));
                restarted = true;
                break;
            }
            append_log(plan_path, "failed to relaunch app error=" +
                                      std::to_string(launch_error) +
                                      "; retrying within restart budget");
        }
        if (!restarted) break;
    }

    if (watched) CloseHandle(watched);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
