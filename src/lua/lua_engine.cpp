#include "lua/lua_engine.h"

#include "automation/config_store.h"
#include "bot/bot.h"
#include "bot/bot_manager.h"
#include "proxy/proxy_pool.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if LUA_VERSION_NUM != 504
#error "Nxrth requires Lua 5.4"
#endif

namespace nxrth::lua {
namespace {

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool has_line_break(std::string_view value) {
    return value.find_first_of("\r\n") != std::string_view::npos;
}

bool base64ish(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '+' || c == '/' || c == '=' || c == '_' || c == '-';
}

bool looks_like_long_secret(std::string_view value) {
    if (value.size() < 80) return false;
    return std::all_of(value.begin(), value.end(), base64ish);
}

void replace_all(std::string& text, const std::string& needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        text.replace(at, needle.size(), replacement);
        at += replacement.size();
    }
}

// Lua output is user-visible and may be persisted by the UI. Provider scripts
// commonly print every table member before calling addBot(), so redaction cannot
// depend only on secrets already registered with the engine.
std::string redact_output(std::string text, const std::vector<std::string>& known_secrets = {}) {
    for (const auto& secret : known_secrets)
        if (secret.size() >= 8) replace_all(text, secret, "<redacted>");

    // Redact values following sensitive labels while preserving the rest of a
    // provider record or log line. Matching is ASCII case-insensitive.
    static constexpr std::string_view keys[] = {
        "refreshtoken:", "refresh_token:", "ubticket:", "ltoken:",
        "token:", "password:", "proxy:", "proxy_password:", "secret:"};
    std::string folded = lower(text);
    for (const auto key : keys) {
        std::size_t pos = 0;
        while ((pos = folded.find(key, pos)) != std::string::npos) {
            const std::size_t value_begin = pos + key.size();
            std::size_t value_end = value_begin;
            while (value_end < text.size() && text[value_end] != '|' &&
                   text[value_end] != '\r' && text[value_end] != '\n' &&
                   text[value_end] != '\t')
                ++value_end;
            text.replace(value_begin, value_end - value_begin, "<redacted>");
            folded = lower(text);
            pos = value_begin + std::string_view("<redacted>").size();
        }
    }

    // Catch the sample's `bot.name = bot.token` behavior and unlabeled dumps.
    std::size_t run = 0;
    while (run < text.size()) {
        while (run < text.size() && !base64ish(text[run])) ++run;
        std::size_t end = run;
        while (end < text.size() && base64ish(text[end])) ++end;
        if (end - run >= 80) {
            text.replace(run, end - run, "<redacted>");
            run += std::string_view("<redacted>").size();
        } else {
            run = end;
        }
    }
    return text;
}

std::vector<std::string> split_worlds(std::string_view value) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&] {
        current = trim(std::move(current));
        if (!current.empty()) out.push_back(std::move(current));
        current.clear();
    };
    for (char c : value) {
        if (c == ',' || c == ';' || c == '\n')
            flush();
        else
            current.push_back(c);
    }
    flush();
    return out;
}

std::string append_worlds(std::string existing, const std::vector<std::string>& additions) {
    auto worlds = split_worlds(existing);
    for (const auto& raw : additions) {
        for (auto value : split_worlds(raw)) {
            if (value.size() > 256 || has_line_break(value)) continue;
            const bool duplicate = std::any_of(worlds.begin(), worlds.end(), [&](const auto& old) {
                return iequals(trim(old), trim(value));
            });
            if (!duplicate) worlds.push_back(std::move(value));
        }
    }
    std::ostringstream joined;
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        if (i) joined << ", ";
        joined << worlds[i];
    }
    return joined.str();
}

std::string remove_worlds(std::string existing, const std::vector<std::string>& removals) {
    auto worlds = split_worlds(existing);
    std::vector<std::string> wanted;
    for (const auto& raw : removals) {
        auto parts = split_worlds(raw);
        wanted.insert(wanted.end(), parts.begin(), parts.end());
    }
    worlds.erase(std::remove_if(worlds.begin(), worlds.end(), [&](const auto& world) {
                     return std::any_of(wanted.begin(), wanted.end(), [&](const auto& value) {
                         return iequals(trim(world), trim(value));
                     });
                 }),
                 worlds.end());
    std::ostringstream joined;
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        if (i) joined << ", ";
        joined << worlds[i];
    }
    return joined.str();
}

struct ProviderFields {
    std::string token;
    std::string rid;
    std::string mac;
    std::string wk = "NONE0";
    std::string platform;
    std::string name;
    std::string cbits;
    std::string player_age;
    std::string vid;
};

bool safe_record_value(std::string_view value) {
    return value.find('|') == std::string_view::npos && !has_line_break(value);
}

std::optional<std::string> build_provider_record(const ProviderFields& f, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<std::string> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (f.token.empty()) return fail("token is required");
    if (f.rid.size() != 32) return fail("rid must contain exactly 32 characters");
    if (f.mac.empty()) return fail("mac is required");
    const std::string wk = f.wk.empty() ? "NONE0" : f.wk;
    if (wk.size() != 32 && !iequals(wk, "NONE0"))
        return fail("wk must be NONE0 or exactly 32 characters");

    const std::string_view values[] = {f.token, f.rid, f.mac, wk, f.platform,
                                       f.name,  f.cbits, f.player_age, f.vid};
    if (std::any_of(std::begin(values), std::end(values),
                    [](std::string_view v) { return !safe_record_value(v); }))
        return fail("provider fields may not contain pipes or line breaks");

    std::string record = "token:" + f.token + "|rid:" + f.rid + "|mac:" + f.mac +
                         "|wk:" + wk;
    if (!f.platform.empty()) record += "|platform:" + f.platform;
    // The compatibility script may overwrite name with the raw token. Omitting
    // it prevents a secret from becoming the BotManager display name.
    if (!f.name.empty() && f.name != f.token && f.name.size() <= 128 &&
        !looks_like_long_secret(f.name))
        record += "|name:" + f.name;
    if (!f.cbits.empty()) record += "|cbits:" + f.cbits;
    if (!f.player_age.empty()) record += "|playerAge:" + f.player_age;
    if (!f.vid.empty()) record += "|vid:" + f.vid;

    const auto parsed = nxrth::bot::parse_ltoken_string(record);
    if (!parsed || parsed->kind != nxrth::bot::LtokenRecord::Kind::ProviderToken)
        return fail("provider fields are not a valid token record");
    return record;
}

}  // namespace

struct LuaEngine::Impl {
    static constexpr std::size_t kMemoryLimitBytes = 64u * 1024u * 1024u;

    struct MemoryBudget {
        std::size_t used = 0;
        std::size_t peak = 0;
        std::size_t limit = kMemoryLimitBytes;
    };

    enum class Action {
        List,
        Status,
        Find,
        Local,
        Ping,
        Signal,
        IsInWorld,
        IsInTile,
        IsWearing,
        World,
        Floating,
        Players,
        Inventory,
        Tiles,
        Tile,
        Logs,
        ChatLog,
        Remove,
        Warp,
        Chat,
        Move,
        MoveRelative,
        MoveLeft,
        MoveRight,
        MoveUp,
        MoveDown,
        Walk,
        Punch,
        Place,
        Drop,
        Trash,
        Wear,
        Unwear,
        Wrench,
        Collect,
        Reconnect,
        Disconnect,
        Leave,
        Respawn,
        ActivateItem,
        ActivateTile,
        Enter,
        Face,
        WrenchPlayer,
        AcceptAccess,
        AutoCollect,
        AutoReconnect,
        WaitOnline,
    };

    nxrth::bot::BotManager& manager;
    nxrth::proxy::ProxyPool& proxy_pool;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> is_running{false};
    lua_State* state = nullptr;
    LuaExecutionOptions options;
    std::chrono::steady_clock::time_point deadline{};
    std::size_t instructions = 0;
    std::string output;
    std::vector<unsigned int> added_ids;
    std::vector<std::string> known_secrets;
    MemoryBudget memory_budget;

    explicit Impl(nxrth::bot::BotManager& m, nxrth::proxy::ProxyPool& p)
        : manager(m), proxy_pool(p) {}

    static void* allocate(void* user_data, void* pointer, std::size_t old_size,
                          std::size_t new_size) noexcept {
        auto& budget = *static_cast<MemoryBudget*>(user_data);

        if (!pointer) old_size = 0;  // Lua uses osize as a type tag for new blocks.
        if (new_size == 0) {
            std::free(pointer);
            budget.used = old_size <= budget.used ? budget.used - old_size : 0;
            return nullptr;
        }
        if (new_size == old_size) return pointer;

        if (new_size > old_size) {
            const auto growth = new_size - old_size;
            if (budget.used > budget.limit || growth > budget.limit - budget.used)
                return nullptr;
        }

        void* resized = std::realloc(pointer, new_size);
        if (!resized) return nullptr;

        if (new_size > old_size) {
            budget.used += new_size - old_size;
            budget.peak = std::max(budget.peak, budget.used);
        } else {
            const auto shrink = old_size - new_size;
            budget.used = shrink <= budget.used ? budget.used - shrink : 0;
        }
        return resized;
    }

    static Impl& self(lua_State* L) {
        return *static_cast<Impl*>(lua_touserdata(L, lua_upvalueindex(1)));
    }

    static int argument_base(lua_State* L) { return lua_istable(L, 1) ? 2 : 1; }

    void append(std::string value) {
        value = redact_output(std::move(value), known_secrets);
        if (output.size() >= options.output_limit) return;
        if (value.size() > options.output_limit - output.size())
            value.resize(options.output_limit - output.size());
        output += value;
    }

    static void hook(lua_State* L, lua_Debug*) {
        auto* impl = *reinterpret_cast<Impl**>(lua_getextraspace(L));
        if (!impl) return;
        impl->instructions += 10'000;
        const bool over_instructions = impl->options.instruction_limit != 0 &&
                                       impl->instructions > impl->options.instruction_limit;
        const bool over_time = impl->options.time_limit.count() != 0 &&
                               std::chrono::steady_clock::now() > impl->deadline;
        if (impl->stop_requested.load(std::memory_order_relaxed))
            luaL_error(L, "script stopped");
        if (over_instructions) luaL_error(L, "script instruction limit exceeded");
        if (over_time) luaL_error(L, "script time limit exceeded");
    }

    static std::optional<std::string> table_string(lua_State* L, int table,
                                                    const char* field) {
        table = lua_absindex(L, table);
        lua_getfield(L, table, field);
        std::optional<std::string> result;
        if (lua_isstring(L, -1)) {
            std::size_t n = 0;
            const char* value = lua_tolstring(L, -1, &n);
            result = std::string(value, n);
        } else if (lua_isnumber(L, -1)) {
            result = std::to_string(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);
        return result;
    }

    static std::optional<bool> table_bool(lua_State* L, int table, const char* field) {
        table = lua_absindex(L, table);
        lua_getfield(L, table, field);
        std::optional<bool> result;
        if (lua_isboolean(L, -1)) result = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return result;
    }

    static std::string checked_text(lua_State* L, int index, std::size_t max,
                                    const char* label) {
        std::size_t n = 0;
        const char* text = luaL_checklstring(L, index, &n);
        if (n > max) luaL_error(L, "%s is too long", label);
        return std::string(text, n);
    }

    std::optional<std::uint32_t> resolve_id(lua_State* L, int index) {
        if (lua_isinteger(L, index)) {
            const auto raw = lua_tointeger(L, index);
            if (raw < 0 || raw > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
            const auto id = static_cast<std::uint32_t>(raw);
            return manager.get_state(id) ? std::optional{id} : std::nullopt;
        }
        if (lua_isstring(L, index)) {
            std::size_t n = 0;
            const char* name = lua_tolstring(L, index, &n);
            return manager.find_id_by_name(std::string(name, n));
        }
        return std::nullopt;
    }

    static void push_bot_info(lua_State* L, const nxrth::bot::BotInfo& info) {
        lua_createtable(L, 0, 10);
        lua_pushinteger(L, info.id); lua_setfield(L, -2, "id");
        lua_pushlstring(L, info.username.data(), info.username.size()); lua_setfield(L, -2, "name");
        lua_pushlstring(L, info.status.data(), info.status.size()); lua_setfield(L, -2, "status");
        lua_pushlstring(L, info.world.data(), info.world.size()); lua_setfield(L, -2, "world");
        lua_pushnumber(L, info.pos_x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, info.pos_y); lua_setfield(L, -2, "y");
        lua_pushinteger(L, info.gems); lua_setfield(L, -2, "gems");
        lua_pushinteger(L, info.ping_ms); lua_setfield(L, -2, "ping");
        lua_pushboolean(L, info.status == "connected" || info.status == "in_game");
        lua_setfield(L, -2, "online");
        if (info.proxy_key) {
            lua_pushlstring(L, info.proxy_key->data(), info.proxy_key->size());
            lua_setfield(L, -2, "proxy");
        }
    }

    static int l_print(lua_State* L) {
        auto& s = self(L);
        const int count = lua_gettop(L);
        for (int i = 1; i <= count; ++i) {
            if (i > 1) s.append("\t");
            std::size_t n = 0;
            const char* str = luaL_tolstring(L, i, &n);
            s.append(std::string(str, n));
            lua_pop(L, 1);
        }
        s.append("\n");
        return 0;
    }

    // Compatibility marker used by SurferBot-style scripts to select their
    // table-based addBot branch. The return value is intentionally conservative;
    // scripts in Nxrth should inspect inventory rather than predict slot prices.
    static int l_calculate_backpack_cost(lua_State* L) {
        luaL_optinteger(L, 1, 0);
        lua_pushinteger(L, 0);
        return 1;
    }

    static int l_add_bot(lua_State* L) {
        auto& s = self(L);
        luaL_checktype(L, 1, LUA_TTABLE);

        if (table_bool(L, 1, "connect").value_or(true) == false) {
            lua_pushboolean(L, 0);
            lua_pushliteral(L, "connect=false is not supported; bot was not added");
            return 2;
        }

        try {
            const auto token = table_string(L, 1, "token");
            const auto refresh = table_string(L, 1, "refreshToken");
            const auto password = table_string(L, 1, "password");
            const auto username = table_string(L, 1, "username");
            const auto growid = table_string(L, 1, "growid");
            const auto name = table_string(L, 1, "name");

            std::string login_record;
            bool token_login = token.has_value() || refresh.has_value();
            if (token_login) {
                const std::string source = token ? *token : *refresh;
                s.known_secrets.push_back(source);
                if (source.find('|') != std::string::npos &&
                    nxrth::bot::parse_ltoken_string(source)) {
                    login_record = source;
                } else {
                    ProviderFields fields;
                    fields.token = source;
                    fields.rid = table_string(L, 1, "rid").value_or("");
                    fields.mac = table_string(L, 1, "mac").value_or("");
                    fields.wk = table_string(L, 1, "wk").value_or("NONE0");
                    fields.platform = table_string(L, 1, "platform").value_or(
                        table_string(L, 1, "platformID").value_or(""));
                    fields.name = name.value_or("");
                    fields.cbits = table_string(L, 1, "cbits").value_or("");
                    fields.player_age = table_string(L, 1, "playerAge").value_or(
                        table_string(L, 1, "player_age").value_or(""));
                    fields.vid = table_string(L, 1, "vid").value_or("");
                    std::string problem;
                    auto built = build_provider_record(fields, &problem);
                    if (!built) {
                        lua_pushboolean(L, 0);
                        lua_pushlstring(L, problem.data(), problem.size());
                        return 2;
                    }
                    login_record = std::move(*built);
                }
            }

            const auto proxy_value = table_string(L, 1, "proxy");
            const auto proxy_bool = table_bool(L, 1, "proxy");
            auto rotating_login = table_bool(L, 1, "rotatingLogin");
            if (!rotating_login) rotating_login = table_bool(L, 1, "rotating_login");
            bool explicit_auto = false;
            bool direct = false;
            std::optional<nxrth::net::Socks5Config> proxy;
            auto proxy_policy = nxrth::bot::ProxyPolicy::Direct;
            if (proxy_bool) {
                direct = !*proxy_bool;
                explicit_auto = *proxy_bool;
            } else if (proxy_value) {
                const auto mode = lower(trim(*proxy_value));
                explicit_auto = mode == "auto" || mode == "configured" || mode == "pool";
                direct = mode == "none" || mode == "direct" || mode == "off";
                if (!explicit_auto && !direct) {
                    auto parsed = nxrth::proxy::parse_proxy_lines(*proxy_value);
                    if (parsed.size() != 1)
                        throw std::runtime_error("custom proxy must contain exactly one endpoint");
                    proxy = parsed.front().to_socks5();
                    proxy_policy = nxrth::bot::ProxyPolicy::Custom;
                }
            }
            // No proxy -> a direct login uses the real IP. We DON'T block that: if the
            // user disabled proxies (or never added any) and adds a bot anyway, honour
            // it and go direct. Only surface a real error when the user EXPLICITLY asked
            // for a pool proxy that isn't available.
            if (!direct && !proxy) {
                if (explicit_auto && !s.proxy_pool.config().enabled)
                    throw std::runtime_error("proxy='auto' requires an enabled game proxy pool");
                if (s.proxy_pool.config().enabled) {
                    proxy = s.proxy_pool.choose(s.manager.proxy_key_counts());
                    if (explicit_auto && !proxy)
                        throw std::runtime_error("proxy='auto' did not resolve a game proxy");
                    if (proxy) proxy_policy = nxrth::bot::ProxyPolicy::Pool;
                }
                // pool disabled + no proxy requested -> fall through to a direct spawn.
            }

            std::optional<nxrth::proxy::RotatingLoginProxy> login_proxy;
            std::uint32_t id = 0;
            if (token_login) {
                const auto parsed = nxrth::bot::parse_ltoken_string(login_record);
                if (!parsed) throw std::runtime_error("invalid token record");
                const bool provider = parsed->kind == nxrth::bot::LtokenRecord::Kind::ProviderToken;
                if (provider && rotating_login.value_or(false)) {
                    login_proxy = s.proxy_pool.rotating_login_proxy();
                    if (!login_proxy)
                        throw std::runtime_error(
                            "rotatingLogin=true requires an enabled rotating-login proxy pool");
                }
                id = s.manager.spawn_ltoken(login_record, std::move(proxy),
                                            std::move(login_proxy), proxy_policy);
            } else {
                const std::string user = username.value_or(growid.value_or(name.value_or("")));
                if (user.empty() || !password || password->empty())
                    throw std::runtime_error("addBot requires provider token fields or username/password");
                s.known_secrets.push_back(*password);
                if (rotating_login.value_or(true))
                    login_proxy = s.proxy_pool.rotating_login_proxy();
                id = s.manager.spawn(user, *password, std::move(proxy),
                                     std::move(login_proxy), proxy_policy);
            }
            s.added_ids.push_back(id);
            lua_pushboolean(L, 1);
            lua_pushinteger(L, id);
            return 2;
        } catch (const std::exception& e) {
            const std::string message = redact_output(e.what(), s.known_secrets);
            lua_pushboolean(L, 0);
            lua_pushlstring(L, message.data(), message.size());
            return 2;
        }
    }

    static int l_action(lua_State* L) {
        auto& s = self(L);
        const auto action = static_cast<Action>(lua_tointeger(L, lua_upvalueindex(2)));
        int base = argument_base(L);

        if (action == Action::List) {
            const auto list = s.manager.list();
            lua_createtable(L, static_cast<int>(list.size()), 0);
            int index = 1;
            for (const auto& info : list) {
                push_bot_info(L, info);
                lua_rawseti(L, -2, index++);
            }
            return 1;
        }
        if (action == Action::Find) {
            const auto name = checked_text(L, base, 128, "name");
            const auto id = s.manager.find_id_by_name(name);
            if (id) lua_pushinteger(L, *id); else lua_pushnil(L);
            return 1;
        }

        const auto id = s.resolve_id(L, base);
        if (!id) {
            lua_pushboolean(L, 0);
            lua_pushliteral(L, "bot not found");
            return 2;
        }
        if (action == Action::Status) {
            for (const auto& info : s.manager.list()) {
                if (info.id == *id) {
                    push_bot_info(L, info);
                    return 1;
                }
            }
            lua_pushnil(L);
            return 1;
        }
        if (action == Action::Local || action == Action::Ping || action == Action::Signal ||
            action == Action::IsInWorld || action == Action::IsInTile ||
            action == Action::IsWearing) {
            const auto state = s.manager.get_state(*id);
            if (!state) { lua_pushnil(L); return 1; }
            if (action == Action::Local) {
                lua_createtable(L, 0, 6);
                lua_pushinteger(L, *id); lua_setfield(L, -2, "bot_id");
                lua_pushlstring(L, state->username.data(), state->username.size());
                lua_setfield(L, -2, "name");
                lua_pushnumber(L, state->pos_x); lua_setfield(L, -2, "x");
                lua_pushnumber(L, state->pos_y); lua_setfield(L, -2, "y");
                lua_pushlstring(L, state->world_name.data(), state->world_name.size());
                lua_setfield(L, -2, "world");
                return 1;
            }
            if (action == Action::Ping) { lua_pushinteger(L, state->ping_ms); return 1; }
            if (action == Action::Signal) {
                if (!state->geiger_signal) { lua_pushnil(L); return 1; }
                lua_createtable(L, 0, 4);
                lua_pushinteger(L, state->geiger_signal->x); lua_setfield(L, -2, "x");
                lua_pushinteger(L, state->geiger_signal->y); lua_setfield(L, -2, "y");
                const char* type = nxrth::bot::as_str(state->geiger_signal->area_type);
                lua_pushstring(L, type); lua_setfield(L, -2, "type");
                lua_pushinteger(L, static_cast<lua_Integer>(state->geiger_signal->timestamp_ms));
                lua_setfield(L, -2, "timestamp_ms");
                return 1;
            }
            if (action == Action::IsInWorld) {
                bool value = !state->world_name.empty();
                if (!lua_isnoneornil(L, base + 1))
                    value = iequals(state->world_name,
                                    checked_text(L, base + 1, 128, "world"));
                lua_pushboolean(L, value); return 1;
            }
            if (action == Action::IsInTile) {
                const auto x = static_cast<std::int32_t>(luaL_checkinteger(L, base + 1));
                const auto y = static_cast<std::int32_t>(luaL_checkinteger(L, base + 2));
                lua_pushboolean(L, static_cast<std::int32_t>(state->pos_x) == x &&
                                   static_cast<std::int32_t>(state->pos_y) == y);
                return 1;
            }
            const auto item_id = static_cast<std::uint16_t>(luaL_checkinteger(L, base + 1));
            const bool wearing = std::any_of(state->inventory.begin(), state->inventory.end(),
                                              [&](const auto& slot) {
                                                  return slot.item_id == item_id && slot.is_active;
                                              });
            lua_pushboolean(L, wearing); return 1;
        }
        if (action == Action::World || action == Action::Floating ||
            action == Action::Players || action == Action::Inventory ||
            action == Action::Tiles || action == Action::Tile ||
            action == Action::Logs || action == Action::ChatLog) {
            const auto state = s.manager.get_state(*id);
            if (!state) {
                lua_pushnil(L);
                return 1;
            }
            if (action == Action::World) {
                lua_createtable(L, 0, 8);
                lua_pushlstring(L, state->world_name.data(), state->world_name.size());
                lua_setfield(L, -2, "name");
                lua_pushinteger(L, state->world_width); lua_setfield(L, -2, "width");
                lua_pushinteger(L, state->world_height); lua_setfield(L, -2, "height");
                lua_pushinteger(L, static_cast<lua_Integer>(state->tiles.size()));
                lua_setfield(L, -2, "tile_count");
                lua_pushinteger(L, static_cast<lua_Integer>(state->objects.size()));
                lua_setfield(L, -2, "floating_count");
                lua_pushinteger(L, static_cast<lua_Integer>(state->players.size()));
                lua_setfield(L, -2, "player_count");
                lua_pushnumber(L, state->pos_x); lua_setfield(L, -2, "x");
                lua_pushnumber(L, state->pos_y); lua_setfield(L, -2, "y");
                return 1;
            }
            if (action == Action::Floating) {
                lua_createtable(L, static_cast<int>(state->objects.size()), 0);
                int index = 1;
                std::optional<std::uint32_t> wanted_item;
                std::optional<double> wanted_radius;
                std::string wanted_name;
                if (lua_istable(L, base + 1)) {
                    lua_getfield(L, base + 1, "item_id");
                    if (lua_isinteger(L, -1))
                        wanted_item = static_cast<std::uint32_t>(lua_tointeger(L, -1));
                    lua_pop(L, 1);
                    lua_getfield(L, base + 1, "radius");
                    if (lua_isnumber(L, -1)) wanted_radius = lua_tonumber(L, -1);
                    lua_pop(L, 1);
                    wanted_name = lower(table_string(L, base + 1, "name_contains").value_or(""));
                }
                for (const auto& object : state->objects) {
                    if (wanted_item && object.item_id != *wanted_item) continue;
                    const auto* item = s.manager.items_dat()
                                           ? s.manager.items_dat()->find_by_id(object.item_id)
                                           : nullptr;
                    if (!wanted_name.empty() &&
                        (!item || lower(item->name).find(wanted_name) == std::string::npos))
                        continue;
                    const float dx = object.x / 32.0f - state->pos_x;
                    const float dy = object.y / 32.0f - state->pos_y;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (wanted_radius && distance > *wanted_radius) continue;
                    lua_createtable(L, 0, 7);
                    lua_pushinteger(L, object.uid); lua_setfield(L, -2, "uid");
                    lua_pushinteger(L, object.item_id); lua_setfield(L, -2, "item_id");
                    lua_pushinteger(L, object.count); lua_setfield(L, -2, "count");
                    if (item) {
                        lua_pushlstring(L, item->name.data(), item->name.size());
                        lua_setfield(L, -2, "name");
                    }
                    lua_pushlstring(L, state->world_name.data(), state->world_name.size());
                    lua_setfield(L, -2, "world");
                    lua_pushnumber(L, object.x); lua_setfield(L, -2, "x");
                    lua_pushnumber(L, object.y); lua_setfield(L, -2, "y");
                    lua_pushnumber(L, object.x / 32.0f); lua_setfield(L, -2, "tile_x");
                    lua_pushnumber(L, object.y / 32.0f); lua_setfield(L, -2, "tile_y");
                    lua_pushnumber(L, distance); lua_setfield(L, -2, "distance");
                    lua_pushnumber(L, distance); lua_setfield(L, -2, "distance_tiles");
                    lua_createtable(L, 0, 2);
                    lua_pushnumber(L, object.x); lua_setfield(L, -2, "x");
                    lua_pushnumber(L, object.y); lua_setfield(L, -2, "y");
                    lua_setfield(L, -2, "position_pixels");
                    lua_createtable(L, 0, 2);
                    lua_pushnumber(L, object.x / 32.0f); lua_setfield(L, -2, "x");
                    lua_pushnumber(L, object.y / 32.0f); lua_setfield(L, -2, "y");
                    lua_setfield(L, -2, "position_tiles");
                    lua_rawseti(L, -2, index++);
                }
                return 1;
            }
            if (action == Action::Players) {
                lua_createtable(L, static_cast<int>(state->players.size()), 0);
                int index = 1;
                for (const auto& player : state->players) {
                    lua_createtable(L, 0, 5);
                    lua_pushinteger(L, player.net_id); lua_setfield(L, -2, "net_id");
                    lua_pushlstring(L, player.name.data(), player.name.size());
                    lua_setfield(L, -2, "name");
                    lua_pushnumber(L, player.pos_x); lua_setfield(L, -2, "x");
                    lua_pushnumber(L, player.pos_y); lua_setfield(L, -2, "y");
                    lua_pushlstring(L, player.country.data(), player.country.size());
                    lua_setfield(L, -2, "country");
                    lua_rawseti(L, -2, index++);
                }
                return 1;
            }
            if (action == Action::Inventory) {
                lua_createtable(L, static_cast<int>(state->inventory.size()), 2);
                lua_pushinteger(L, state->inventory_size); lua_setfield(L, -2, "capacity");
                lua_pushinteger(L, state->gems); lua_setfield(L, -2, "gems");
                int index = 1;
                for (const auto& slot : state->inventory) {
                    lua_createtable(L, 0, 4);
                    lua_pushinteger(L, slot.item_id); lua_setfield(L, -2, "item_id");
                    if (s.manager.items_dat()) {
                        if (const auto* item = s.manager.items_dat()->find_by_id(slot.item_id)) {
                            lua_pushlstring(L, item->name.data(), item->name.size());
                            lua_setfield(L, -2, "name");
                        }
                    }
                    lua_pushinteger(L, slot.amount); lua_setfield(L, -2, "amount");
                    lua_pushboolean(L, slot.is_active); lua_setfield(L, -2, "active");
                    lua_pushinteger(L, slot.action_type); lua_setfield(L, -2, "action_type");
                    lua_rawseti(L, -2, index++);
                }
                return 1;
            }
            auto push_tile = [&](std::size_t tile_index) {
                const auto& tile = state->tiles[tile_index];
                const auto width = std::max<std::uint32_t>(state->world_width, 1);
                lua_createtable(L, 0, 7);
                lua_pushinteger(L, static_cast<lua_Integer>(tile_index % width));
                lua_setfield(L, -2, "x");
                lua_pushinteger(L, static_cast<lua_Integer>(tile_index / width));
                lua_setfield(L, -2, "y");
                lua_pushinteger(L, tile.fg_item_id); lua_setfield(L, -2, "fg");
                lua_pushinteger(L, tile.fg_item_id); lua_setfield(L, -2, "fg_item_id");
                lua_pushinteger(L, tile.bg_item_id); lua_setfield(L, -2, "bg");
                lua_pushinteger(L, tile.bg_item_id); lua_setfield(L, -2, "bg_item_id");
                lua_pushinteger(L, tile.flags); lua_setfield(L, -2, "flags");
                lua_pushinteger(L, static_cast<lua_Integer>(tile.tile_type.index()));
                lua_setfield(L, -2, "extra_type");
                std::uint8_t collision = 0;
                if (s.manager.items_dat()) {
                    if (const auto* fg = s.manager.items_dat()->find_by_id(tile.fg_item_id)) {
                        collision = fg->collision_type;
                        lua_pushlstring(L, fg->name.data(), fg->name.size());
                        lua_setfield(L, -2, "item_name");
                    }
                    if (const auto* bg = s.manager.items_dat()->find_by_id(tile.bg_item_id)) {
                        lua_pushlstring(L, bg->name.data(), bg->name.size());
                        lua_setfield(L, -2, "background_name");
                    }
                }
                lua_pushinteger(L, collision); lua_setfield(L, -2, "collision_type");
                lua_pushboolean(L, collision == 1 || collision == 6);
                lua_setfield(L, -2, "blocked");
            };
            if (action == Action::Tile) {
                const auto x = luaL_checkinteger(L, base + 1);
                const auto y = luaL_checkinteger(L, base + 2);
                if (x < 0 || y < 0 || static_cast<std::uint64_t>(x) >= state->world_width ||
                    static_cast<std::uint64_t>(y) >= state->world_height) {
                    lua_pushnil(L);
                    return 1;
                }
                const auto index = static_cast<std::size_t>(y) * state->world_width +
                                   static_cast<std::size_t>(x);
                if (index >= state->tiles.size()) lua_pushnil(L); else push_tile(index);
                return 1;
            }
            if (action == Action::Tiles) {
                lua_createtable(L, static_cast<int>(state->tiles.size()), 0);
                for (std::size_t i = 0; i < state->tiles.size(); ++i) {
                    push_tile(i);
                    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
                }
                return 1;
            }
            const auto& ring = action == Action::Logs ? state->console : state->chat;
            const auto requested = std::clamp<lua_Integer>(luaL_optinteger(L, base + 1, 100),
                                                            0, 300);
            const auto take = std::min<std::size_t>(ring.size(), static_cast<std::size_t>(requested));
            lua_createtable(L, static_cast<int>(take), 0);
            const auto first = ring.size() - take;
            for (std::size_t i = 0; i < take; ++i) {
                const auto line = redact_output(ring[first + i], s.known_secrets);
                lua_pushlstring(L, line.data(), line.size());
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }
        if (action == Action::Remove) {
            lua_pushboolean(L, s.manager.stop(*id));
            return 1;
        }
        if (action == Action::AutoCollect && lua_isnoneornil(L, base + 1)) {
            const auto state = s.manager.get_state(*id);
            lua_pushboolean(L, state && state->auto_collect);
            return 1;
        }
        if (action == Action::WaitOnline) {
            const auto timeout = std::clamp<lua_Integer>(luaL_optinteger(L, base + 1, 30'000),
                                                         0, 300'000);
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(timeout);
            bool online = false;
            while (std::chrono::steady_clock::now() < until &&
                   !s.stop_requested.load(std::memory_order_relaxed)) {
                if (s.options.time_limit.count() != 0 &&
                    std::chrono::steady_clock::now() > s.deadline)
                    return luaL_error(L, "script time limit exceeded");
                for (const auto& info : s.manager.list()) {
                    if (info.id == *id && (info.status == "connected" || info.status == "in_game")) {
                        online = true;
                        break;
                    }
                }
                if (online) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            lua_pushboolean(L, online);
            return 1;
        }

        if (action == Action::MoveRelative || action == Action::MoveLeft ||
            action == Action::MoveRight || action == Action::MoveUp ||
            action == Action::MoveDown) {
            const auto state = s.manager.get_state(*id);
            if (!state) { lua_pushboolean(L, 0); return 1; }
            std::int32_t x = static_cast<std::int32_t>(state->pos_x);
            std::int32_t y = static_cast<std::int32_t>(state->pos_y);
            if (action == Action::MoveRelative) {
                x += static_cast<std::int32_t>(luaL_checkinteger(L, base + 1));
                y += static_cast<std::int32_t>(luaL_checkinteger(L, base + 2));
            } else {
                const auto range = static_cast<std::int32_t>(luaL_optinteger(L, base + 1, 1));
                if (action == Action::MoveLeft) x -= range;
                if (action == Action::MoveRight) x += range;
                if (action == Action::MoveUp) y -= range;
                if (action == Action::MoveDown) y += range;
            }
            lua_pushboolean(L, s.manager.send_cmd(*id, nxrth::bot::cmd::Move{x, y}));
            return 1;
        }

        using namespace nxrth::bot;
        bool queued = false;
        switch (action) {
            case Action::Warp: {
                auto world = checked_text(L, base + 1, 128, "world");
                auto door = lua_isnoneornil(L, base + 2)
                                ? std::string{}
                                : checked_text(L, base + 2, 128, "door");
                queued = s.manager.send_cmd(*id, cmd::Warp{std::move(world), std::move(door)});
                break;
            }
            case Action::Chat:
                queued = s.manager.send_cmd(*id, cmd::Say{checked_text(L, base + 1, 512, "text")});
                break;
            case Action::Move:
                queued = s.manager.send_cmd(*id, cmd::Move{
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 2))});
                break;
            case Action::Walk:
                queued = s.manager.send_cmd(*id, cmd::FindPath{
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 2))});
                break;
            case Action::Punch:
                queued = s.manager.send_cmd(*id, cmd::Hit{
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 2))});
                break;
            case Action::Place:
                queued = s.manager.send_cmd(*id, cmd::Place{
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 2)),
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 3))});
                break;
            case Action::Drop:
                queued = s.manager.send_cmd(*id, cmd::Drop{
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 2))});
                break;
            case Action::Trash:
                queued = s.manager.send_cmd(*id, cmd::Trash{
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 2))});
                break;
            case Action::Wear:
                queued = s.manager.send_cmd(*id, cmd::Wear{
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1))}); break;
            case Action::Unwear:
                queued = s.manager.send_cmd(*id, cmd::Unwear{
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1))}); break;
            case Action::Wrench:
                queued = s.manager.send_cmd(*id, cmd::Wrench{
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 2))}); break;
            case Action::Collect:
                if (lua_isnoneornil(L, base + 1))
                    queued = s.manager.send_cmd(*id, cmd::CollectNearby{});
                else
                    queued = s.manager.send_cmd(*id, cmd::CollectObject{
                        static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1)),
                        static_cast<float>(luaL_optnumber(L, base + 2, 3.0))});
                break;
            case Action::Reconnect: queued = s.manager.send_cmd(*id, cmd::Reconnect{}); break;
            case Action::Disconnect: queued = s.manager.send_cmd(*id, cmd::Disconnect{}); break;
            case Action::Leave: queued = s.manager.send_cmd(*id, cmd::LeaveWorld{}); break;
            case Action::Respawn: queued = s.manager.send_cmd(*id, cmd::Respawn{}); break;
            case Action::ActivateItem:
                queued = s.manager.send_cmd(*id, cmd::ActivateItem{
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1))}); break;
            case Action::ActivateTile:
                queued = s.manager.send_cmd(*id, cmd::ActivateTile{
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 1)),
                    static_cast<std::int32_t>(luaL_checkinteger(L, base + 2))}); break;
            case Action::Enter: {
                std::optional<std::string> password;
                if (!lua_isnoneornil(L, base + 1))
                    password = checked_text(L, base + 1, 128, "door password");
                queued = s.manager.send_cmd(*id, cmd::Enter{std::move(password)}); break;
            }
            case Action::Face:
                queued = s.manager.send_cmd(*id, cmd::Face{lua_toboolean(L, base + 1) != 0}); break;
            case Action::WrenchPlayer:
                queued = s.manager.send_cmd(*id, cmd::WrenchPlayer{
                    static_cast<std::uint32_t>(luaL_checkinteger(L, base + 1))}); break;
            case Action::AcceptAccess:
                queued = s.manager.send_cmd(*id, cmd::AcceptAccess{}); break;
            case Action::AutoCollect:
                queued = s.manager.send_cmd(*id, cmd::SetAutoCollect{
                    lua_toboolean(L, base + 1) != 0}); break;
            case Action::AutoReconnect:
                queued = s.manager.send_cmd(*id, cmd::SetAutoReconnect{
                    lua_toboolean(L, base + 1) != 0}); break;
            default: break;
        }
        lua_pushboolean(L, queued);
        return 1;
    }

    void persist_config(nxrth::bot::AutomationConfig cfg) {
        manager.fleet()->set_config(cfg);
        nxrth::automation::save_automation_config(cfg);
    }

    static int l_automation_enable(lua_State* L) {
        auto& s = self(L);
        const int base = argument_base(L);
        const auto module = checked_text(L, base, 64, "module");
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.enabled[module] = lua_toboolean(L, base + 1) != 0;
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_automation_setparam(lua_State* L) {
        auto& s = self(L);
        const int base = argument_base(L);
        const auto key = checked_text(L, base, 128, "parameter");
        const auto value = checked_text(L, base + 1, 4096, "value");
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.params[key] = value;
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_automation_getparam(lua_State* L) {
        auto& s = self(L);
        const int base = argument_base(L);
        const auto key = checked_text(L, base, 128, "parameter");
        const auto cfg = s.manager.fleet()->config_snapshot();
        const auto it = cfg.params.find(key);
        if (it == cfg.params.end()) lua_pushnil(L);
        else lua_pushlstring(L, it->second.data(), it->second.size());
        return 1;
    }

    static int l_automation_setbots(lua_State* L) {
        auto& s = self(L);
        const int base = argument_base(L);
        const auto module = checked_text(L, base, 64, "module");
        luaL_checktype(L, base + 1, LUA_TTABLE);
        std::vector<std::uint32_t> ids;
        const auto count = lua_rawlen(L, base + 1);
        for (std::size_t i = 1; i <= count; ++i) {
            lua_rawgeti(L, base + 1, static_cast<lua_Integer>(i));
            if (auto id = s.resolve_id(L, -1)) ids.push_back(*id);
            lua_pop(L, 1);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.module_bot_ids[module] = std::move(ids);
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_automation_allbots(lua_State* L) {
        auto& s = self(L);
        const int base = argument_base(L);
        const auto module = checked_text(L, base, 64, "module");
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.module_bot_ids.erase(module);
        cfg.module_groups.erase(module);
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static std::vector<std::string> world_arguments(lua_State* L, int base) {
        std::vector<std::string> result;
        if (lua_istable(L, base)) {
            const auto count = lua_rawlen(L, base);
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(L, base, static_cast<lua_Integer>(i));
                if (lua_isstring(L, -1)) result.push_back(checked_text(L, -1, 2048, "world"));
                lua_pop(L, 1);
            }
        } else {
            for (int i = base; i <= lua_gettop(L); ++i)
                result.push_back(checked_text(L, i, 2048, "world"));
        }
        return result;
    }

    static int l_geiger_enable(lua_State* L) {
        auto& s = self(L);
        const int base = argument_base(L);
        const bool enabled = lua_isnoneornil(L, base) || lua_toboolean(L, base) != 0;
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.enabled["geiger"] = enabled;
        // autogeiger.enable(true) is intentionally fleet-wide. Use
        // automation.setbots("geiger", {...}) afterwards for a scoped run.
        if (enabled) {
            cfg.module_bot_ids.erase("geiger");
            cfg.module_groups.erase("geiger");
        }
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_geiger_disable(lua_State* L) {
        auto& s = self(L);
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.enabled["geiger"] = false;
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_geiger_world(lua_State* L) {
        auto& s = self(L);
        const int kind = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
        const int base = argument_base(L);
        const auto additions = world_arguments(L, base);
        const char* key = kind == 0 ? "geiger_hunt_worlds"
                          : kind == 1 ? "geiger_depot_worlds"
                                      : "geiger_pickup_worlds";
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.params[key] = append_worlds(cfg.param(key), additions);
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_geiger_setworlds(lua_State* L) {
        auto& s = self(L);
        const int kind = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
        const int base = argument_base(L);
        const auto values = world_arguments(L, base);
        const char* key = kind == 0 ? "geiger_hunt_worlds"
                          : kind == 1 ? "geiger_depot_worlds"
                                      : "geiger_pickup_worlds";
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.params[key] = append_worlds({}, values);
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_geiger_removeworld(lua_State* L) {
        auto& s = self(L);
        const int kind = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
        const int base = argument_base(L);
        const auto removals = world_arguments(L, base);
        const char* key = kind == 0 ? "geiger_hunt_worlds"
                          : kind == 1 ? "geiger_depot_worlds"
                                      : "geiger_pickup_worlds";
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.params[key] = remove_worlds(cfg.param(key), removals);
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_geiger_clearworlds(lua_State* L) {
        auto& s = self(L);
        const int kind = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
        const char* key = kind == 0 ? "geiger_hunt_worlds"
                          : kind == 1 ? "geiger_depot_worlds"
                                      : "geiger_pickup_worlds";
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.params[key].clear();
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_geiger_getworlds(lua_State* L) {
        auto& s = self(L);
        const int kind = static_cast<int>(lua_tointeger(L, lua_upvalueindex(2)));
        const char* key = kind == 0 ? "geiger_hunt_worlds"
                          : kind == 1 ? "geiger_depot_worlds"
                                      : "geiger_pickup_worlds";
        const auto cfg = s.manager.fleet()->config_snapshot();
        const auto worlds = split_worlds(cfg.param(key));
        lua_createtable(L, static_cast<int>(worlds.size()), 0);
        for (std::size_t i = 0; i < worlds.size(); ++i) {
            lua_pushlstring(L, worlds[i].data(), worlds[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    static int l_geiger_setoption(lua_State* L) {
        auto& s = self(L);
        const int base = argument_base(L);
        std::string key = checked_text(L, base, 128, "option");
        if (key.rfind("geiger_", 0) != 0) key = "geiger_" + key;
        std::string value;
        if (lua_isboolean(L, base + 1)) {
            value = lua_toboolean(L, base + 1) ? "1" : "0";
        } else if (lua_isstring(L, base + 1) || lua_isnumber(L, base + 1)) {
            std::size_t n = 0;
            const char* raw = lua_tolstring(L, base + 1, &n);
            if (n > 4096) return luaL_error(L, "option value is too long");
            value.assign(raw, n);
        } else {
            return luaL_error(L, "option value must be a string, number, or boolean");
        }
        auto cfg = s.manager.fleet()->config_snapshot();
        cfg.params[key] = std::move(value);
        s.persist_config(std::move(cfg));
        lua_pushboolean(L, 1);
        return 1;
    }

    static int l_geiger_status(lua_State* L) {
        auto& s = self(L);
        const auto cfg = s.manager.fleet()->config_snapshot();
        lua_createtable(L, 0, 6);
        lua_pushboolean(L, cfg.is_on("geiger")); lua_setfield(L, -2, "enabled");
        const auto push_world_list = [&](const char* key, const char* field) {
            const auto worlds = split_worlds(cfg.param(key));
            lua_createtable(L, static_cast<int>(worlds.size()), 0);
            for (std::size_t i = 0; i < worlds.size(); ++i) {
                lua_pushlstring(L, worlds[i].data(), worlds[i].size());
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(L, -2, field);
        };
        push_world_list("geiger_hunt_worlds", "worlds");
        push_world_list("geiger_depot_worlds", "storage_worlds");
        push_world_list("geiger_pickup_worlds", "geiger_storage_worlds");
        const auto scope = cfg.module_bot_ids.find("geiger");
        lua_pushboolean(L, scope == cfg.module_bot_ids.end());
        lua_setfield(L, -2, "all_bots");
        if (scope != cfg.module_bot_ids.end()) {
            lua_createtable(L, static_cast<int>(scope->second.size()), 0);
            for (std::size_t i = 0; i < scope->second.size(); ++i) {
                lua_pushinteger(L, scope->second[i]);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(L, -2, "bot_ids");
        }
        return 1;
    }

    static int l_sleep(lua_State* L) {
        auto& s = self(L);
        const auto ms = std::clamp<lua_Integer>(luaL_checkinteger(L, 1), 0, 60'000);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            if (s.stop_requested.load(std::memory_order_relaxed))
                return luaL_error(L, "script stopped");
            if (s.options.time_limit.count() != 0 &&
                std::chrono::steady_clock::now() > s.deadline)
                return luaL_error(L, "script time limit exceeded");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }

    static int l_coroutine_create(lua_State* L) {
        auto& s = self(L);
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_State* coroutine = lua_newthread(L);
        *reinterpret_cast<Impl**>(lua_getextraspace(coroutine)) = &s;
        lua_pushvalue(L, 1);
        lua_xmove(L, coroutine, 1);
        lua_sethook(coroutine, hook, LUA_MASKCOUNT, 10'000);
        return 1;
    }

    void push_self_closure(lua_CFunction fn) {
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, fn, 1);
    }

    void set_action(const char* name, Action action) {
        lua_pushlightuserdata(state, this);
        lua_pushinteger(state, static_cast<lua_Integer>(action));
        lua_pushcclosure(state, l_action, 2);
        lua_setfield(state, -2, name);
    }

    void set_geiger_fn(const char* name, lua_CFunction fn, int kind) {
        lua_pushlightuserdata(state, this);
        lua_pushinteger(state, kind);
        lua_pushcclosure(state, fn, 2);
        lua_setfield(state, -2, name);
    }

    // _G __index: Lua calls this only for a global that was never assigned. Reading
    // an undeclared variable is almost always a typo (e.g. print(a) where `a` was
    // never defined), so raise an error with the source line instead of yielding
    // nil. luaL_error prepends the caller's [string "executor"]:N: location, which
    // execute() rewrites to "line N:".
    static int l_strict_index(lua_State* L) {
        const char* key = lua_tostring(L, 2);
        return luaL_error(L, "undeclared variable '%s' (nil global — declare it first)",
                          key ? key : "?");
    }

    void install() {
        push_self_closure(l_print); lua_setglobal(state, "print");
        push_self_closure(l_add_bot); lua_setglobal(state, "addBot");
        lua_pushcfunction(state, l_calculate_backpack_cost);
        lua_setglobal(state, "calculateBackpackCost");
        lua_pushliteral(state, "TOKEN"); lua_setglobal(state, "TOKEN");
        push_self_closure(l_sleep); lua_setglobal(state, "sleep");

        // Lua hooks are per-coroutine. Replace create so child threads inherit
        // the same budget/deadline hook, and remove wrap (which hides its thread).
        lua_getglobal(state, "coroutine");
        if (lua_istable(state, -1)) {
            push_self_closure(l_coroutine_create); lua_setfield(state, -2, "create");
            lua_pushnil(state); lua_setfield(state, -2, "wrap");
        }
        lua_pop(state, 1);

        lua_newtable(state);
        set_action("list", Action::List);
        set_action("status", Action::Status);
        set_action("get", Action::Status);
        set_action("find", Action::Find);
        set_action("localPlayer", Action::Local);
        set_action("getLocal", Action::Local);
        set_action("ping", Action::Ping);
        set_action("getPing", Action::Ping);
        set_action("signal", Action::Signal);
        set_action("getSignal", Action::Signal);
        set_action("isInWorld", Action::IsInWorld);
        set_action("isInTile", Action::IsInTile);
        set_action("isWearing", Action::IsWearing);
        set_action("world", Action::World);
        set_action("getWorld", Action::World);
        set_action("floating", Action::Floating);
        set_action("getFloatingItems", Action::Floating);
        set_action("getObjects", Action::Floating);
        set_action("players", Action::Players);
        set_action("getPlayers", Action::Players);
        set_action("inventory", Action::Inventory);
        set_action("getInventory", Action::Inventory);
        set_action("tiles", Action::Tiles);
        set_action("getTiles", Action::Tiles);
        set_action("tile", Action::Tile);
        set_action("getTile", Action::Tile);
        set_action("logs", Action::Logs);
        set_action("getConsole", Action::Logs);
        set_action("chatlog", Action::ChatLog);
        set_action("remove", Action::Remove);
        set_action("warp", Action::Warp);
        set_action("chat", Action::Chat);
        set_action("say", Action::Chat);
        set_action("move", Action::Move);
        set_action("moveTile", Action::Move);
        set_action("moveTo", Action::MoveRelative);
        set_action("moveLeft", Action::MoveLeft);
        set_action("moveRight", Action::MoveRight);
        set_action("moveUp", Action::MoveUp);
        set_action("moveDown", Action::MoveDown);
        set_action("walk", Action::Walk);
        set_action("findPath", Action::Walk);
        set_action("punch", Action::Punch);
        set_action("hit", Action::Punch);
        set_action("place", Action::Place);
        set_action("drop", Action::Drop);
        set_action("trash", Action::Trash);
        set_action("wear", Action::Wear);
        set_action("use", Action::Wear);
        set_action("unwear", Action::Unwear);
        set_action("wrench", Action::Wrench);
        set_action("collect", Action::Collect);
        set_action("collectObject", Action::Collect);
        set_action("reconnect", Action::Reconnect);
        set_action("connect", Action::Reconnect);
        set_action("disconnect", Action::Disconnect);
        set_action("leave", Action::Leave);
        set_action("leaveWorld", Action::Leave);
        set_action("respawn", Action::Respawn);
        set_action("activateitem", Action::ActivateItem);
        set_action("activateItem", Action::ActivateItem);
        set_action("activatetile", Action::ActivateTile);
        set_action("activateTile", Action::ActivateTile);
        set_action("active", Action::ActivateTile);
        set_action("enter", Action::Enter);
        set_action("face", Action::Face);
        set_action("setDirection", Action::Face);
        set_action("wrenchplayer", Action::WrenchPlayer);
        set_action("wrenchPlayer", Action::WrenchPlayer);
        set_action("acceptaccess", Action::AcceptAccess);
        set_action("acceptAccess", Action::AcceptAccess);
        set_action("autocollect", Action::AutoCollect);
        set_action("auto_collect", Action::AutoCollect);
        set_action("setAutoCollect", Action::AutoCollect);
        set_action("autoreconnect", Action::AutoReconnect);
        set_action("autoReconnect", Action::AutoReconnect);
        set_action("auto_reconnect", Action::AutoReconnect);
        set_action("setAutoReconnect", Action::AutoReconnect);
        set_action("waitonline", Action::WaitOnline);
        set_action("waitOnline", Action::WaitOnline);
        lua_setglobal(state, "bot");

        // Plural alias for scripts written against either convention.
        lua_getglobal(state, "bot"); lua_setglobal(state, "bots");

        // Common global compatibility names.
        lua_getglobal(state, "bot"); lua_getfield(state, -1, "list");
        lua_setglobal(state, "getBots"); lua_pop(state, 1);
        lua_getglobal(state, "bot"); lua_getfield(state, -1, "status");
        lua_setglobal(state, "getBot"); lua_pop(state, 1);
        lua_getglobal(state, "bot"); lua_getfield(state, -1, "remove");
        lua_setglobal(state, "removeBot"); lua_pop(state, 1);

        // Read-only world namespace aliases. Snapshots are copied from BotState;
        // Lua never observes or mutates the bot worker's live containers.
        lua_newtable(state);
        const char* world_aliases[][2] = {
            {"floatingItems", "floating"}, {"objects", "floating"},
            {"players", "players"}, {"inventory", "inventory"},
            {"tiles", "tiles"}, {"tile", "tile"}, {"status", "world"}};
        for (const auto& alias : world_aliases) {
            lua_getglobal(state, "bot");
            lua_getfield(state, -1, alias[1]);
            lua_remove(state, -2);
            lua_setfield(state, -2, alias[0]);
        }
        lua_setglobal(state, "world");

        lua_newtable(state);
        push_self_closure(l_automation_enable); lua_setfield(state, -2, "enable");
        push_self_closure(l_automation_setparam); lua_setfield(state, -2, "setparam");
        push_self_closure(l_automation_getparam); lua_setfield(state, -2, "getparam");
        push_self_closure(l_automation_setbots); lua_setfield(state, -2, "setbots");
        push_self_closure(l_automation_allbots); lua_setfield(state, -2, "allbots");
        lua_setglobal(state, "automation");

        lua_newtable(state);
        push_self_closure(l_geiger_enable); lua_setfield(state, -2, "enable");
        push_self_closure(l_geiger_disable); lua_setfield(state, -2, "disable");
        set_geiger_fn("addworld", l_geiger_world, 0);
        set_geiger_fn("addstorageworld", l_geiger_world, 1);
        set_geiger_fn("addgeigerstorageworld", l_geiger_world, 2);
        set_geiger_fn("setworlds", l_geiger_setworlds, 0);
        set_geiger_fn("setstorageworlds", l_geiger_setworlds, 1);
        set_geiger_fn("setgeigerstorageworlds", l_geiger_setworlds, 2);
        set_geiger_fn("removeworld", l_geiger_removeworld, 0);
        set_geiger_fn("removestorageworld", l_geiger_removeworld, 1);
        set_geiger_fn("removegeigerstorageworld", l_geiger_removeworld, 2);
        set_geiger_fn("clearworlds", l_geiger_clearworlds, 0);
        set_geiger_fn("clearstorageworlds", l_geiger_clearworlds, 1);
        set_geiger_fn("cleargeigerstorageworlds", l_geiger_clearworlds, 2);
        set_geiger_fn("getworlds", l_geiger_getworlds, 0);
        set_geiger_fn("getstorageworlds", l_geiger_getworlds, 1);
        set_geiger_fn("getgeigerstorageworlds", l_geiger_getworlds, 2);
        push_self_closure(l_geiger_setoption); lua_setfield(state, -2, "setoption");
        push_self_closure(l_geiger_status); lua_setfield(state, -2, "status");
        push_self_closure(l_geiger_status); lua_setfield(state, -2, "getconfig");
        push_self_closure(l_geiger_status); lua_setfield(state, -2, "getConfig");
        set_action("getSignal", Action::Signal);
        lua_setglobal(state, "autogeiger");

        // The Executor's "Run on" target: the bot id this run should act on (a
        // valid selector for bot.* actions), or false for a fleet-wide run.
        if (options.selected_bot)
            lua_pushinteger(state, static_cast<lua_Integer>(*options.selected_bot));
        else
            lua_pushboolean(state, 0);
        lua_setglobal(state, "SELECTED_BOT");

        // Strict global reads (must come AFTER every API global is installed):
        // referencing an undeclared global now errors instead of silently
        // returning nil. Assignments still create globals; only reads of names
        // that were never set are rejected.
        lua_pushglobaltable(state);
        lua_createtable(state, 0, 1);
        lua_pushcfunction(state, l_strict_index);
        lua_setfield(state, -2, "__index");
        lua_setmetatable(state, -2);
        lua_pop(state, 1);
    }
};

LuaEngine::LuaEngine(nxrth::bot::BotManager& manager,
                     nxrth::proxy::ProxyPool& proxy_pool)
    : impl_(std::make_unique<Impl>(manager, proxy_pool)) {}

LuaEngine::~LuaEngine() {
    request_stop();
    if (impl_->state) lua_close(impl_->state);
}

LuaExecutionResult LuaEngine::execute(std::string_view source,
                                      const LuaExecutionOptions& options) {
    LuaExecutionResult result;
    if (impl_->is_running.exchange(true)) {
        result.error = "another Lua script is already running";
        return result;
    }
    struct RunningGuard {
        std::atomic<bool>& flag;
        ~RunningGuard() { flag.store(false); }
    } guard{impl_->is_running};

    impl_->stop_requested.store(false);
    impl_->options = options;
    impl_->instructions = 0;
    impl_->output.clear();
    impl_->added_ids.clear();
    impl_->known_secrets.clear();
    impl_->deadline = std::chrono::steady_clock::now() + options.time_limit;
    impl_->memory_budget = {};
    impl_->state = lua_newstate(Impl::allocate, &impl_->memory_budget);
    if (!impl_->state) {
        result.error = "could not create Lua state within memory limit";
        return result;
    }
    struct StateGuard {
        Impl& impl;
        ~StateGuard() { lua_close(impl.state); impl.state = nullptr; }
    } state_guard{*impl_};

    *reinterpret_cast<Impl**>(lua_getextraspace(impl_->state)) = impl_.get();
    // Load only deterministic, in-memory libraries. os.exit/os.execute, io,
    // package loading, and debug.sethook must not be reachable from executor
    // scripts: they could terminate the desktop process or disable our limits.
    static const luaL_Reg safe_libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {nullptr, nullptr},
    };
    for (const auto* lib = safe_libraries; lib->func; ++lib) {
        luaL_requiref(impl_->state, lib->name, lib->func, 1);
        lua_pop(impl_->state, 1);
    }
    lua_pushnil(impl_->state); lua_setglobal(impl_->state, "dofile");
    lua_pushnil(impl_->state); lua_setglobal(impl_->state, "loadfile");
    impl_->install();
    lua_sethook(impl_->state, Impl::hook, LUA_MASKCOUNT, 10'000);

    const int loaded = luaL_loadbuffer(impl_->state, source.data(), source.size(), "executor");
    int status = loaded;
    if (status == LUA_OK) status = lua_pcall(impl_->state, 0, LUA_MULTRET, 0);
    if (status == LUA_OK) {
        result.ok = true;
    } else {
        std::size_t n = 0;
        const char* message = lua_tolstring(impl_->state, -1, &n);
        result.error = redact_output(message ? std::string(message, n) : "unknown Lua error",
                                     impl_->known_secrets);
    }
    result.output = redact_output(std::move(impl_->output), impl_->known_secrets);
    result.error = redact_output(std::move(result.error), impl_->known_secrets);
    // Make Lua's chunk-prefixed error locations human-readable so callers (the
    // Executor UI and the AI's lua_execute tool) can point at the failing line:
    //   [string "executor"]:12: '=' expected   ->   line 12: '=' expected
    {
        const std::string needle = "[string \"executor\"]:";
        for (std::size_t pos = result.error.find(needle); pos != std::string::npos;
             pos = result.error.find(needle, pos)) {
            result.error.replace(pos, needle.size(), "line ");
        }
    }
    result.added_bot_ids = std::move(impl_->added_ids);
    return result;
}

void LuaEngine::request_stop() noexcept { impl_->stop_requested.store(true); }

bool LuaEngine::running() const noexcept { return impl_->is_running.load(); }

bool LuaEngine::self_test(std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    ProviderFields fields;
    fields.token = std::string(120, 'A') + "+/==";
    fields.rid = std::string(32, 'B');
    fields.mac = "02:00:00:00:00:00";
    fields.wk = "NONE0";
    fields.platform = "1";
    fields.name = fields.token;  // must never become the visible name
    fields.cbits = "1536";
    fields.player_age = "25";
    fields.vid = "00000000-0000-0000-0000-000000000000";
    std::string problem;
    const auto record = build_provider_record(fields, &problem);
    if (!record) return fail("provider record: " + problem);
    if (record->find("|name:") != std::string::npos)
        return fail("provider record leaked token through name");
    const auto parsed = nxrth::bot::parse_ltoken_string(*record);
    if (!parsed || parsed->token != fields.token || parsed->player_age.value_or("") != "25")
        return fail("provider record did not round-trip");

    const auto worlds = append_worlds("ALPHA, beta|door", {"alpha", "GAMMA", "BETA|DOOR"});
    if (worlds != "ALPHA, beta|door, GAMMA") return fail("world append/dedupe failed");

    const std::string printed = "token: " + fields.token + "\nname: " + fields.token;
    const auto redacted = redact_output(printed);
    if (redacted.find(fields.token) != std::string::npos ||
        redacted.find("<redacted>") == std::string::npos)
        return fail("secret redaction failed");

    // Exercise a real VM without accepting a bot or performing network I/O.
    // The malformed provider table fails before proxy selection/spawn.
    try {
        nxrth::bot::BotManager manager(nullptr);
        auto proxy_pool = nxrth::proxy::ProxyPool::load_default();
        LuaEngine engine(manager, proxy_pool);
        const std::string vm_secret = std::string(128, 'Z') + "+/==";
        const std::string script =
            "assert(TOKEN == 'TOKEN')\n"
            "assert(type(calculateBackpackCost) == 'function')\n"
            "assert(rawget(_G,'os') == nil and rawget(_G,'io') == nil and "
            "rawget(_G,'package') == nil and rawget(_G,'debug') == nil)\n"
            "assert(rawget(_G,'dofile') == nil and rawget(_G,'loadfile') == nil)\n"
            "assert(SELECTED_BOT == false)\n"
            "assert(type(autogeiger.enable) == 'function')\n"
            "assert(type(autogeiger.addworld) == 'function')\n"
            "local ok, err = addBot({token='" + vm_secret +
            "', rid='SHORT', mac='02:00:00:00:00:00', wk='NONE0', proxy='auto'})\n"
            "assert(ok == false and type(err) == 'string')\n"
            "print('token: " + vm_secret + "')\n";
        const auto vm = engine.execute(script);
        if (!vm.ok) return fail("Lua VM binding test failed: " + vm.error);
        if (vm.output.find(vm_secret) != std::string::npos ||
            vm.output.find("<redacted>") == std::string::npos)
            return fail("Lua VM print redaction failed");

        const auto runtime_error = engine.execute("error('" + vm_secret + "')");
        if (runtime_error.ok || runtime_error.error.find(vm_secret) != std::string::npos ||
            runtime_error.error.find("<redacted>") == std::string::npos)
            return fail("Lua VM runtime-error redaction failed");

        LuaExecutionOptions limited;
        limited.instruction_limit = 50'000;
        limited.time_limit = std::chrono::milliseconds(1000);
        const auto limited_result = engine.execute("while true do end", limited);
        if (limited_result.ok ||
            limited_result.error.find("instruction limit") == std::string::npos)
            return fail("Lua VM instruction hook failed");

        const auto memory_limited = engine.execute(
            "local value = string.rep('x', 70 * 1024 * 1024)\nreturn #value");
        if (memory_limited.ok || memory_limited.error.find("memory") == std::string::npos)
            return fail("Lua VM memory limit failed");

        const auto after_memory_limit = engine.execute("assert(2 + 2 == 4)");
        if (!after_memory_limit.ok)
            return fail("Lua VM did not recover after memory limit: " + after_memory_limit.error);
    } catch (const std::exception& e) {
        return fail(std::string("Lua VM self-test exception: ") + e.what());
    }
    return true;
}

}  // namespace nxrth::lua
