// Nxrth - embedded Lua 5.4 automation/runtime facade.
//
// LuaEngine deliberately executes on the thread that calls execute(). BotManager
// is owned by the desktop UI thread and is not internally serialized, so callers
// must invoke execute() from that same owner thread (never from a worker).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxrth::bot {
class BotManager;
}

namespace nxrth::proxy {
class ProxyPool;
}

namespace nxrth::lua {

struct LuaExecutionOptions {
    // The hook checks both limits every 10,000 VM instructions. Zero disables
    // the corresponding limit.
    std::size_t instruction_limit = 20'000'000;
    std::chrono::milliseconds time_limit{30'000};
    std::size_t output_limit = 512 * 1024;
    // The Executor's "Run on" target: when set, the script sees a global
    // SELECTED_BOT holding this bot id (a valid selector for bot.* actions); when
    // unset, SELECTED_BOT is false (fleet-wide run).
    std::optional<std::uint32_t> selected_bot;
};

struct LuaExecutionResult {
    bool ok = false;
    std::string output;
    std::string error;
    std::vector<unsigned int> added_bot_ids;
};

class LuaEngine {
public:
    LuaEngine(nxrth::bot::BotManager& manager, nxrth::proxy::ProxyPool& proxy_pool);
    ~LuaEngine();

    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;
    LuaEngine(LuaEngine&&) = delete;
    LuaEngine& operator=(LuaEngine&&) = delete;

    // Run a complete chunk. This is synchronous by design; call it only from
    // the BotManager/UI owner thread. Each execution gets a fresh Lua state so
    // globals and secrets cannot leak into the next script.
    LuaExecutionResult execute(std::string_view source,
                               const LuaExecutionOptions& options = {});

    // Safe to call from another thread. The active hook aborts at its next
    // instruction checkpoint; it does not touch BotManager.
    void request_stop() noexcept;
    bool running() const noexcept;

    // Pure validation for the record builder, shared-world append/dedupe, and
    // secret redaction helpers. Does not start a bot or perform network I/O.
    static bool self_test(std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nxrth::lua
