#include "automation/modules/webhook.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bot/bot_state.h"    // adonai::bot::to_string(BotStatus)
#include "net/http_client.h"  // adonai::net::HttpClient

namespace adonai::automation {
namespace {

// Fleet-wide rate-limit state. Guarded only for the quick check/update — never
// held across the HTTP POST (that was Mori's fleet-stalling bug).
std::mutex g_mu;
std::chrono::steady_clock::time_point g_last_post{};
bool g_inited = false;

std::string build_content(const std::vector<adonai::bot::BotView>& fleet) {
    std::size_t in_world = 0;
    for (const auto& b : fleet)
        if (!b.world_name.empty()) ++in_world;

    std::string s = "**Adonai fleet** — " + std::to_string(fleet.size()) + " bot(s), " +
                    std::to_string(in_world) + " in-world\n```\n";
    for (const auto& b : fleet) {
        s += (b.username.empty() ? "(token)" : b.username);
        s += "  ";
        s += adonai::bot::to_string(b.status);
        s += "  ";
        s += (b.world_name.empty() ? "-" : b.world_name);
        s += "  gems=" + std::to_string(b.gems);
        s += "  ping=" + std::to_string(b.ping_ms) + "ms\n";
        if (s.size() > 1800) {  // Discord hard-caps content at 2000 chars
            s += "...\n";
            break;
        }
    }
    s += "```";
    return s;
}

}  // namespace

void WebhookModule::tick(adonai::bot::BotContext& self, adonai::bot::FleetState& fleet) {
    const auto cfg = fleet.config_snapshot();
    const std::string url = cfg.param("webhook_url");
    if (url.empty()) return;

    const auto members = fleet.snapshot();
    if (members.empty()) return;

    // Leader election: only the lowest live bot id posts the fleet summary, so N
    // bots ticking this module still produce exactly ONE message per interval.
    std::uint32_t leader = std::numeric_limits<std::uint32_t>::max();
    for (const auto& b : members) leader = std::min(leader, b.id);
    if (self.bot_id() != leader) return;

    long interval_secs = 60;
    try {
        interval_secs = std::stol(cfg.param("webhook_interval_secs", "60"));
    } catch (...) {
    }
    if (interval_secs < 5) interval_secs = 5;

    // Quick locked section: decide whether it's time to post; release before HTTP.
    bool should_post = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto now = std::chrono::steady_clock::now();
        if (!g_inited || now - g_last_post >= std::chrono::seconds(interval_secs)) {
            g_last_post = now;
            g_inited = true;
            should_post = true;
        }
    }
    if (!should_post) return;

    nlohmann::json payload;
    payload["content"] = build_content(members);

    adonai::net::HttpClient client;
    adonai::net::HttpRequest opts;
    opts.headers.push_back({"Content-Type", "application/json"});
    opts.timeout_secs = 15;
    client.Post(url, payload.dump(), opts);  // fire-and-forget; errors ignored
}

}  // namespace adonai::automation
