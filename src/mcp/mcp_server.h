#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "bot/bot_manager.h"
#include "proxy/proxy_pool.h"

namespace adonai::mcp {

class McpServer {
public:
    McpServer();
    McpServer(adonai::bot::BotManager& manager, adonai::proxy::ProxyPool& proxy_pool);

    // Returns nullopt for JSON-RPC notifications.
    std::optional<nlohmann::json> handle(const nlohmann::json& request);
    static nlohmann::json tool_definitions();

private:
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args);
    nlohmann::json read_resource(const std::string& uri);
    nlohmann::json bot_state_json(std::uint32_t bot_id, bool detailed) const;
    nlohmann::json world_json(std::uint32_t bot_id) const;
    nlohmann::json inventory_json(std::uint32_t bot_id) const;
    nlohmann::json chat_json(std::uint32_t bot_id, std::size_t after,
                             std::size_t limit) const;
    nlohmann::json logs_json(std::uint32_t bot_id, std::size_t limit) const;
    nlohmann::json automation_status_json();
    nlohmann::json floating_items_json(std::uint32_t bot_id, std::optional<float> radius_tiles,
                                       std::optional<std::uint32_t> item_id,
                                       const std::string& name_contains) const;
    bool enqueue(std::uint32_t bot_id, adonai::bot::BotCommand command) const;

    // The stdio/headless server owns these. The desktop bridge leaves them null
    // and binds the references to the application's live fleet instead.
    std::unique_ptr<adonai::bot::BotManager> owned_manager_;
    std::unique_ptr<adonai::proxy::ProxyPool> owned_proxy_pool_;
    adonai::bot::BotManager& manager_;
    adonai::proxy::ProxyPool& proxy_pool_;

    // accounts_spawn strategy=next cursor, persisted across calls for one source.
    std::string accounts_cursor_source_;
    std::size_t accounts_cursor_ = 0;
};

int run_stdio_server();
int run_self_test();

}  // namespace adonai::mcp
