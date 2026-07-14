#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include <nlohmann/json.hpp>

namespace adonai::bot {
class BotManager;
}

namespace adonai::proxy {
class ProxyPool;
}

namespace adonai::mcp {

// Hosts the MCP engine inside the desktop application. Pipe I/O happens on a
// worker thread, while pump() executes requests on the UI thread that owns the
// BotManager and ProxyPool.
class AppMcpBridgeServer {
public:
    AppMcpBridgeServer(adonai::bot::BotManager& manager,
                       adonai::proxy::ProxyPool& proxy_pool);
    ~AppMcpBridgeServer();

    AppMcpBridgeServer(const AppMcpBridgeServer&) = delete;
    AppMcpBridgeServer& operator=(const AppMcpBridgeServer&) = delete;

    std::size_t pump(std::size_t max_requests = 32);
    bool is_listening() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns false only when the desktop application is not listening. Once a
// connection is established, transport/protocol failures throw so callers do
// not silently split work into a second headless fleet.
bool forward_request_to_app(const nlohmann::json& request,
                            std::optional<nlohmann::json>& response);

}  // namespace adonai::mcp
