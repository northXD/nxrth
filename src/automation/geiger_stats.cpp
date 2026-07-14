// Adonai - process-global geiger farm aggregate implementation.
#include "automation/geiger_stats.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace adonai::automation {
namespace {

std::mutex& mu() {
    static std::mutex m;
    return m;
}

struct State {
    GeigerAgg agg;
    std::unordered_map<std::string, std::string> messages;  // webhook url -> message id
    bool loaded = false;
};
State& state() {
    static State s;
    return s;
}

std::filesystem::path stats_path() {
    std::filesystem::path p;
    try {
        p = std::filesystem::current_path();
    } catch (...) {
        p = ".";
    }
    return p / "data" / "geiger_stats.json";
}

// Callers hold mu().
void load_locked() {
    State& s = state();
    if (s.loaded) return;
    s.loaded = true;  // one-shot even on failure (start empty)
    try {
        std::ifstream in(stats_path(), std::ios::binary);
        if (!in) return;
        std::stringstream ss;
        ss << in.rdbuf();
        const auto j = nlohmann::json::parse(ss.str());
        s.agg.total_finds = j.value("total_finds", static_cast<std::uint64_t>(0));
        if (j.contains("counts") && j["counts"].is_object()) {
            for (auto it = j["counts"].begin(); it != j["counts"].end(); ++it) {
                try {
                    s.agg.counts[static_cast<std::uint16_t>(std::stoul(it.key()))] =
                        it.value().get<std::uint64_t>();
                } catch (...) {
                }
            }
        }
        if (j.contains("messages") && j["messages"].is_object()) {
            for (auto it = j["messages"].begin(); it != j["messages"].end(); ++it)
                s.messages[it.key()] = it.value().get<std::string>();
        }
    } catch (...) {
        // any parse failure -> keep the empty defaults
    }
}

// Callers hold mu().
void save_locked() {
    const State& s = state();
    nlohmann::json j;
    j["total_finds"] = s.agg.total_finds;
    nlohmann::json counts = nlohmann::json::object();
    for (const auto& [id, c] : s.agg.counts) counts[std::to_string(id)] = c;
    j["counts"] = std::move(counts);
    nlohmann::json msgs = nlohmann::json::object();
    for (const auto& [u, id] : s.messages) msgs[u] = id;
    j["messages"] = std::move(msgs);
    try {
        const auto p = stats_path();
        std::error_code ec;
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << j.dump(2);
    } catch (...) {
    }
}

}  // namespace

void geiger_record_find(const std::unordered_map<std::uint16_t, std::uint32_t>& gained) {
    std::lock_guard<std::mutex> lk(mu());
    load_locked();
    State& s = state();
    s.agg.total_finds += 1;
    for (const auto& [id, amt] : gained) s.agg.counts[id] += amt;
    save_locked();
}

GeigerAgg geiger_stats_snapshot() {
    std::lock_guard<std::mutex> lk(mu());
    load_locked();
    return state().agg;
}

std::string geiger_webhook_message_id(const std::string& url) {
    std::lock_guard<std::mutex> lk(mu());
    load_locked();
    auto it = state().messages.find(url);
    return it != state().messages.end() ? it->second : std::string();
}

void set_geiger_webhook_message_id(const std::string& url, const std::string& id) {
    std::lock_guard<std::mutex> lk(mu());
    load_locked();
    state().messages[url] = id;
    save_locked();
}

void clear_geiger_webhook_message_id(const std::string& url) {
    std::lock_guard<std::mutex> lk(mu());
    load_locked();
    state().messages.erase(url);
    save_locked();
}

}  // namespace adonai::automation
