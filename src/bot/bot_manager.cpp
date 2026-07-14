// Adonai — BotManager implementation (port spec 09 §3.1-§3.17, §5).
#include "bot/bot_manager.h"

#include <string>
#include <utility>

#include "automation/automation.h"  // adonai::automation::build_all (native modules)
#include "core/logger.h"      // adonai::log
#include "world/items.h"      // adonai::world::ItemsDat::load

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace adonai::bot {

namespace {

// Set the OS thread name to "adonai-bot-<id>" (spec §7: the only literal `mori`
// token renamed). Win32-only; a no-op elsewhere. First action on the new thread.
void set_thread_name(std::uint32_t id) {
#ifdef _WIN32
    std::wstring name = L"adonai-bot-" + std::to_wstring(id);
    ::SetThreadDescription(::GetCurrentThread(), name.c_str());
#else
    (void)id;
#endif
}

// ASCII-only case fold A-Z -> a-z (spec §3.14: do NOT use locale tolower).
inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ascii_iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

}  // namespace

// §3.17 -------------------------------------------------------------------
std::optional<std::string> proxy_key(const std::optional<Socks5Config>& proxy) {
    if (!proxy) return std::nullopt;
    return proxy->host + ":" + std::to_string(proxy->port);
}

// §3.1 --------------------------------------------------------------------
BotManager::BotManager(EventSinkPtr sink)
    : items_dat_(std::make_shared<const adonai::world::ItemsDat>(adonai::world::ItemsDat::load())),
      sink_(std::move(sink)),
      fleet_(std::make_shared<FleetState>()) {}

BotManager::~BotManager() {
    for (auto& [id, entry] : bots) {
        (void)id;
        if (entry.cmd_tx) entry.cmd_tx->try_send(cmd::Disconnect{});
        if (entry.stop_flag) entry.stop_flag->store(true, std::memory_order_relaxed);
    }
    for (auto& [id, entry] : bots) {
        (void)id;
        if (entry.handle && entry.handle->joinable()) entry.handle->join();
    }
    for (auto& retired : retired_threads_) {
        if (retired.handle.joinable()) retired.handle.join();
    }
}

// §3.2 --------------------------------------------------------------------
void BotManager::reap_finished() {
    // Phase A — natural deaths: bots still in the map whose thread ended on its
    // own. Collect ids first (we mutate the map while draining).
    std::vector<std::uint32_t> finished;
    for (auto& [id, entry] : bots) {
        if (entry.handle && entry.done && entry.done->load(std::memory_order_acquire)) {
            finished.push_back(id);
        }
    }
    for (std::uint32_t id : finished) {
        auto node = bots.extract(id);
        if (!node) continue;
        BotEntry& e = node.mapped();
        if (e.handle && e.handle->joinable()) {
            e.handle->join();  // already finished -> returns immediately
        }
        if (sink_) sink_->bot_removed(id);
        if (fleet_) fleet_->erase(id);
    }

    // Phase B — retired threads (stopped by the user). Swap-erase WITHOUT
    // advancing i so the swapped-in element is re-checked.
    std::size_t i = 0;
    while (i < retired_threads_.size()) {
        RetiredThread& rt = retired_threads_[i];
        if (rt.done && rt.done->load(std::memory_order_acquire)) {
            if (rt.handle.joinable()) rt.handle.join();
            std::swap(retired_threads_[i], retired_threads_.back());
            retired_threads_.pop_back();
            // do NOT advance i
        } else {
            ++i;
        }
    }
}

// §3.3-§3.7 shared spawn body ---------------------------------------------
std::uint32_t BotManager::spawn_core(std::string entry_username, bool do_dedup,
                                     const std::string& dedup_name,
                                     std::optional<Socks5Config> proxy,
                                     std::optional<adonai::proxy::RotatingLoginProxy> login_proxy,
                                     BotFactory make_bot) {
    reap_finished();

    if (do_dedup) {
        if (auto existing = find_id_by_name(dedup_name)) {
            adonai::log("[Manager] Skipped '" + dedup_name + "' — already loaded as bot " +
                        std::to_string(*existing) + ".");
            return *existing;
        }
    }

    const std::uint32_t id = next_id_++;

    auto stop_flag = std::make_shared<std::atomic<bool>>(false);
    auto state = std::make_shared<SharedBotState>();  // BotState default: status Connecting
    auto queue = std::make_shared<CommandQueue<BotCommand>>();
    auto done = std::make_shared<std::atomic<bool>>(false);

    // Compute the capacity key BEFORE `proxy` is moved into the thread.
    std::optional<std::string> assigned_proxy_key = proxy_key(proxy);

    // Clones handed into the thread (Arc ref-count semantics).
    auto items = items_dat_;
    auto sink = sink_;
    auto fleet = fleet_;

    std::thread th([id, stop_flag, state, queue, done, items, sink, fleet,
                    proxy = std::move(proxy), login_proxy = std::move(login_proxy),
                    make_bot = std::move(make_bot)]() mutable {
        set_thread_name(id);
        // Panic isolation: one bot's exception must never call std::terminate.
        try {
            std::unique_ptr<Bot> bot =
                make_bot(std::move(proxy), std::move(login_proxy), stop_flag, state, queue,
                         items, id, sink, fleet);
            if (bot) {
                // Attach every native automation module. Each self-gates per tick
                // on the shared FleetState's AutomationConfig (UI toggles enable
                // them live), so attaching all of them once is correct. This is
                // Adonai's fleet-aware automation seam — replaces Mori's per-bot Lua.
                for (auto& mod : adonai::automation::build_all())
                    bot->add_automation_module(std::move(mod));
                bot->run(stop_flag);  // blocking main loop until stopped/disconnected
            }
            adonai::log("[Bot:" + std::to_string(id) + "] Stopped.", static_cast<int>(id));
        } catch (...) {
            adonai::log("[Bot:" + std::to_string(id) + "] Crashed.", static_cast<int>(id));
        }
        // Late try_sends must fail rather than leak; then mark finished LAST.
        queue->close_consumer();
        done->store(true, std::memory_order_release);
    });

    std::string added_username = entry_username;
    bots.emplace(id, BotEntry{std::move(entry_username), std::move(assigned_proxy_key), stop_flag,
                              state, queue, done, std::move(th)});

    if (sink_) sink_->bot_added(id, added_username);
    return id;
}

// §3.3 --------------------------------------------------------------------
std::uint32_t BotManager::spawn(const std::string& username, const std::string& password,
                                std::optional<Socks5Config> proxy,
                                std::optional<adonai::proxy::RotatingLoginProxy> login_proxy) {
    BotFactory factory;
    if (username.find('|') != std::string::npos) {
        factory = [ltoken = username](auto pr, auto lp, auto stop, auto st, auto rx, auto it,
                                      std::uint32_t id, auto sk, auto fl) {
            return Bot::create_ltoken(ltoken, std::move(pr), std::move(lp), stop, st,
                                      std::move(rx), it, id, sk, fl);
        };
    } else {
        factory = [uname = username, pass = password](auto pr, auto lp, auto stop, auto st,
                                                      auto rx, auto it, std::uint32_t id, auto sk,
                                                      auto fl) {
            return Bot::create(uname, pass, std::move(pr), std::move(lp), stop, st, std::move(rx),
                               it, id, sk, fl);
        };
    }
    return spawn_core(username, /*do_dedup=*/true, username, std::move(proxy),
                      std::move(login_proxy), std::move(factory));
}

// §3.5 — NO ltoken branch: always create_newly, even if username contains '|'.
std::uint32_t BotManager::spawn_newly(
    const std::string& username, const std::string& password,
    std::optional<Socks5Config> proxy,
    std::optional<adonai::proxy::RotatingLoginProxy> login_proxy) {
    BotFactory factory = [uname = username, pass = password](auto pr, auto lp, auto stop, auto st,
                                                             auto rx, auto it, std::uint32_t id,
                                                             auto sk, auto fl) {
        return Bot::create_newly(uname, pass, std::move(pr), std::move(lp), stop, st,
                                 std::move(rx), it, id, sk, fl);
    };
    return spawn_core(username, /*do_dedup=*/true, username, std::move(proxy),
                      std::move(login_proxy), std::move(factory));
}

// §3.6 — no dedup; empty entry username (never matches find_id_by_name).
std::uint32_t BotManager::spawn_ltoken(
    const std::string& ltoken_str, std::optional<Socks5Config> proxy,
    std::optional<adonai::proxy::RotatingLoginProxy> login_proxy) {
    BotFactory factory = [ltoken = ltoken_str](auto pr, auto lp, auto stop, auto st, auto rx,
                                               auto it, std::uint32_t id, auto sk, auto fl) {
        return Bot::create_ltoken(ltoken, std::move(pr), std::move(lp), stop, st, std::move(rx),
                                  it, id, sk, fl);
    };
    return spawn_core(std::string{}, /*do_dedup=*/false, std::string{}, std::move(proxy),
                      std::move(login_proxy), std::move(factory));
}

// §3.8 — non-blocking stop.
bool BotManager::stop(std::uint32_t id) {
    reap_finished();

    auto node = bots.extract(id);
    if (!node) return false;  // absent
    BotEntry& e = node.mapped();

    // Wake BOTH paths: a queue-blocked bot (Disconnect) and a run-loop-spinning
    // bot (the atomic). Do NOT join here — park the handle for a later reap.
    if (e.cmd_tx) e.cmd_tx->try_send(cmd::Disconnect{});
    if (e.stop_flag) e.stop_flag->store(true, std::memory_order_relaxed);

    if (e.handle) {
        retired_threads_.push_back(RetiredThread{std::move(*e.handle), e.done});
    }

    if (sink_) sink_->bot_removed(id);   // emit immediately (UI stays responsive)
    if (fleet_) fleet_->erase(id);
    return true;
}

// §3.16 -------------------------------------------------------------------
bool BotManager::stop_by_name(const std::string& name) {
    if (auto id = find_id_by_name(name)) return stop(*id);
    return false;
}

// §3.9 --------------------------------------------------------------------
std::vector<BotInfo> BotManager::list() {
    reap_finished();
    std::vector<BotInfo> out;
    out.reserve(bots.size());
    for (auto& [id, entry] : bots) {
        BotInfo info;
        info.id = id;
        info.username = entry.username;  // BotEntry.username, NOT BotState.username
        info.proxy_key = entry.proxy_key;  // resolved game-proxy "ip:port" (MCP correlation)
        if (entry.state) {
            entry.state->read([&](const BotState& s) {
                info.status = to_string(s.status);
                info.world = s.world_name;
                info.pos_x = s.pos_x;
                info.pos_y = s.pos_y;
                info.gems = s.gems;
                info.ping_ms = s.ping_ms;
                return 0;
            });
        }
        out.push_back(std::move(info));
    }
    return out;  // order unspecified; UI sorts by id
}

// §3.11 -------------------------------------------------------------------
std::optional<BotState> BotManager::get_state(std::uint32_t id) const {
    auto it = bots.find(id);
    if (it == bots.end() || !it->second.state) return std::nullopt;
    return it->second.state->snapshot();  // deep copy
}

// §3.12 -------------------------------------------------------------------
bool BotManager::send_cmd(std::uint32_t id, BotCommand cmd) const {
    auto it = bots.find(id);
    if (it == bots.end() || !it->second.cmd_tx) return false;
    return it->second.cmd_tx->try_send(std::move(cmd));
}

// §3.13 -------------------------------------------------------------------
bool BotManager::run_script(std::uint32_t id, std::string content) const {
    return send_cmd(id, cmd::RunScript{std::move(content)});
}

// §3.14 -------------------------------------------------------------------
std::optional<std::uint32_t> BotManager::find_id_by_name(const std::string& name) const {
    if (name.empty()) return std::nullopt;
    for (const auto& [id, entry] : bots) {
        if (ascii_iequals(entry.username, name)) return id;
    }
    return std::nullopt;
}

// §3.15 -------------------------------------------------------------------
std::optional<std::pair<std::shared_ptr<SharedBotState>, CmdSender>>
BotManager::find_by_name(const std::string& name) const {
    // NOTE: unlike find_id_by_name this does NOT early-return on empty name.
    for (const auto& [id, entry] : bots) {
        (void)id;
        if (ascii_iequals(entry.username, name)) {
            return std::make_pair(entry.state, entry.cmd_tx);
        }
    }
    return std::nullopt;
}

// §3.10 -------------------------------------------------------------------
std::unordered_map<std::string, std::size_t> BotManager::proxy_key_counts() const {
    std::unordered_map<std::string, std::size_t> counts;
    for (const auto& [id, entry] : bots) {
        (void)id;
        if (entry.proxy_key) counts[*entry.proxy_key] += 1;
    }
    return counts;
}

}  // namespace adonai::bot
