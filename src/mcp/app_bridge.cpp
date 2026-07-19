#include "mcp/app_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include "mcp/mcp_server.h"

namespace nxrth::mcp {
namespace {

constexpr wchar_t kPipeName[] = LR"(\\.\pipe\Nxrth.AppMcp.v1)";
constexpr std::size_t kMaxMessageBytes = 64u * 1024u * 1024u;

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

private:
    HANDLE value_;
};

bool write_all(HANDLE pipe, std::string_view data) {
    while (!data.empty()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(data.size(), static_cast<std::size_t>(MAXDWORD)));
        if (!WriteFile(pipe, data.data(), chunk, &written, nullptr) || written == 0)
            return false;
        data.remove_prefix(written);
    }
    return true;
}

bool read_line_blocking(HANDLE pipe, std::string& out) {
    out.clear();
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) return false;
        out.append(buffer, read);
        if (out.size() > kMaxMessageBytes) throw std::runtime_error("app bridge message is too large");
        const auto newline = out.find('\n');
        if (newline != std::string::npos) {
            out.resize(newline);
            return true;
        }
    }
}

bool read_line_polling(HANDLE pipe, const std::atomic<bool>& running, std::string& out) {
    out.clear();
    char buffer[8192];
    while (running.load(std::memory_order_acquire)) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        DWORD read = 0;
        const DWORD wanted = std::min<DWORD>(available, sizeof(buffer));
        if (!ReadFile(pipe, buffer, wanted, &read, nullptr) || read == 0) return false;
        out.append(buffer, read);
        if (out.size() > kMaxMessageBytes) throw std::runtime_error("app bridge message is too large");
        const auto newline = out.find('\n');
        if (newline != std::string::npos) {
            out.resize(newline);
            return true;
        }
    }
    return false;
}

}  // namespace

struct AppMcpBridgeServer::Impl {
    struct Pending {
        nlohmann::json request;
        std::promise<std::optional<nlohmann::json>> completion;
    };

    Impl(nxrth::bot::BotManager& manager, nxrth::proxy::ProxyPool& proxy_pool)
        : server(manager, proxy_pool, true), thread([this] { serve(); }) {}

    ~Impl() { stop(); }

    void stop() {
        const bool was_running = running.exchange(false, std::memory_order_acq_rel);

        // Wake a thread currently blocked in ConnectNamedPipe.
        std::unique_ptr<Handle> wake;
        if (was_running) {
            wake = std::make_unique<Handle>(CreateFileW(
                kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        }
        if (thread.joinable()) thread.join();

        std::deque<std::shared_ptr<Pending>> abandoned;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            abandoned.swap(queue);
        }
        for (const auto& pending : abandoned) {
            try {
                pending->completion.set_exception(
                    std::make_exception_ptr(std::runtime_error("desktop MCP bridge stopped")));
            } catch (...) {
            }
        }
    }

    std::size_t pump(std::size_t max_requests) {
        std::size_t handled = 0;
        while (handled < max_requests) {
            std::shared_ptr<Pending> pending;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (queue.empty()) break;
                pending = std::move(queue.front());
                queue.pop_front();
            }
            try {
                pending->completion.set_value(server.handle(pending->request));
            } catch (...) {
                pending->completion.set_exception(std::current_exception());
            }
            ++handled;
        }
        return handled;
    }

    void serve() {
        Handle pipe(CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 64 * 1024, 64 * 1024, 0, nullptr));
        if (!pipe) {
            running.store(false, std::memory_order_release);
            return;
        }
        listening.store(true, std::memory_order_release);

        while (running.load(std::memory_order_acquire)) {
            const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr);
            if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
                if (!running.load(std::memory_order_acquire)) break;
                continue;
            }

            std::string line;
            try {
                if (read_line_polling(pipe.get(), running, line) && !line.empty()) {
                    auto pending = std::make_shared<Pending>();
                    pending->request = nlohmann::json::parse(line);
                    auto future = pending->completion.get_future();
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        queue.push_back(pending);
                    }

                    while (running.load(std::memory_order_acquire) &&
                           future.wait_for(std::chrono::milliseconds(25)) !=
                               std::future_status::ready) {
                    }

                    if (running.load(std::memory_order_acquire)) {
                        const auto response = future.get();
                        nlohmann::json envelope = {{"ok", true},
                                                   {"has_response", response.has_value()}};
                        if (response) envelope["response"] = *response;
                        std::string output = envelope.dump();
                        output.push_back('\n');
                        write_all(pipe.get(), output);
                    }
                }
            } catch (const std::exception& e) {
                nlohmann::json envelope = {{"ok", false}, {"error", e.what()}};
                std::string output = envelope.dump();
                output.push_back('\n');
                write_all(pipe.get(), output);
            }
            FlushFileBuffers(pipe.get());
            DisconnectNamedPipe(pipe.get());
        }
        listening.store(false, std::memory_order_release);
    }

    McpServer server;
    std::mutex queue_mutex;
    std::deque<std::shared_ptr<Pending>> queue;
    std::atomic<bool> running{true};
    std::atomic<bool> listening{false};
    std::thread thread;
};

AppMcpBridgeServer::AppMcpBridgeServer(nxrth::bot::BotManager& manager,
                                       nxrth::proxy::ProxyPool& proxy_pool)
    : impl_(std::make_unique<Impl>(manager, proxy_pool)) {}

AppMcpBridgeServer::~AppMcpBridgeServer() = default;

std::size_t AppMcpBridgeServer::pump(std::size_t max_requests) {
    return impl_ ? impl_->pump(max_requests) : 0;
}

bool AppMcpBridgeServer::is_listening() const {
    return impl_ && impl_->listening.load(std::memory_order_acquire);
}

void AppMcpBridgeServer::stop() {
    if (impl_) impl_->stop();
}

bool forward_request_to_app(const nlohmann::json& request,
                            std::optional<nlohmann::json>& response) {
    if (!WaitNamedPipeW(kPipeName, 5000)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_SEM_TIMEOUT) return false;
    }

    Handle pipe(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr));
    if (!pipe) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY) return false;
        throw std::runtime_error("cannot connect to desktop MCP bridge (Windows error " +
                                 std::to_string(error) + ")");
    }

    std::string input = request.dump();
    input.push_back('\n');
    if (!write_all(pipe.get(), input))
        throw std::runtime_error("desktop MCP bridge request write failed");

    std::string line;
    if (!read_line_blocking(pipe.get(), line))
        throw std::runtime_error("desktop MCP bridge closed without a response");
    const auto envelope = nlohmann::json::parse(line);
    if (!envelope.value("ok", false))
        throw std::runtime_error(envelope.value("error", "desktop MCP bridge failed"));
    if (envelope.value("has_response", false))
        response = envelope.at("response");
    else
        response.reset();
    return true;
}

}  // namespace nxrth::mcp
