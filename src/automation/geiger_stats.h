// Nxrth - process-global geiger farm aggregate.
//
// Every bot runs its own GeigerModule instance, so per-module tallies only reflect
// one bot. This shared, thread-safe store accumulates FLEET-WIDE totals (per-item
// loot counts + total finds) so the single Discord webhook message shows combined
// fleet numbers. It also holds the webhook message-id map (keyed by webhook URL)
// so the whole fleet EDITS one message instead of spamming a new one per find.
// Both survive app restarts via data/geiger_stats.json.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace nxrth::automation {

struct GeigerAgg {
    std::unordered_map<std::uint16_t, std::uint64_t> counts;  // item id -> fleet total
    std::uint64_t total_finds = 0;                            // total prizes found (fleet)
};

// Record one prize find + its gained loot (item id -> amount) into the fleet
// aggregate; persists to data/geiger_stats.json. Thread-safe.
void geiger_record_find(const std::unordered_map<std::uint16_t, std::uint32_t>& gained);
// Snapshot the current fleet aggregate.
GeigerAgg geiger_stats_snapshot();

// Discord webhook message-id store (keyed by the full webhook URL) so all bots
// edit ONE fleet message. "" = none yet.
std::string geiger_webhook_message_id(const std::string& url);
void set_geiger_webhook_message_id(const std::string& url, const std::string& id);
void clear_geiger_webhook_message_id(const std::string& url);

}  // namespace nxrth::automation
