// Nxrth — per-account device identity store implementation.
// Ported from Mori/account_devices.rs (§6 of docs/port-specs/02-constants-data.md).
#include "core/account_devices.h"

#include "core/constants.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace nxrth::account_devices {
namespace {

// --- process-global store lock (Meyers singleton, mirrors Rust STORE_LOCK) ----
std::mutex& store_lock() {
    static std::mutex m;
    return m;
}

// --- CSPRNG helpers ----------------------------------------------------------
// The Rust source draws a fresh UUIDv4 per helper and slices its raw bytes. The
// v4 version/variant bits live at bytes 6 & 8, outside the ranges any helper
// below observes (random_i32 uses bytes 0-3, random_mac 0-5), so plain uniform
// random bytes are equivalent for every output here.
std::array<std::uint8_t, 16> random_bytes16() {
    static thread_local std::mt19937_64 eng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    std::array<std::uint8_t, 16> b{};
    for (int i = 0; i < 16; i += 8) {
        std::uint64_t r = eng();
        std::memcpy(b.data() + i, &r, 8);
    }
    return b;
}

// 32 UPPERCASE hex chars over a fresh 16-byte value.
std::string random_hex32() {
    static constexpr char HX[] = "0123456789ABCDEF";
    auto b = random_bytes16();
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        s[2 * i] = HX[b[i] >> 4];
        s[2 * i + 1] = HX[b[i] & 0x0F];
    }
    return s;
}

// First 4 bytes of a fresh value interpreted as a little-endian int32, stringified.
std::string random_i32() {
    auto b = random_bytes16();
    std::int32_t v = 0;
    std::memcpy(&v, b.data(), 4);  // host is little-endian (x86)
    return std::to_string(v);
}

// Locally-administered, unicast MAC: lowercase "xx:xx:xx:xx:xx:xx".
std::string random_mac() {
    auto b = random_bytes16();
    std::uint8_t first = static_cast<std::uint8_t>((b[0] & 0xFE) | 0x02);
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", first, b[1],
                  b[2], b[3], b[4], b[5]);
    return std::string(buf);
}

// --- string helpers ----------------------------------------------------------
bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

bool ascii_ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

// trim + ASCII-lowercase; empty -> nullopt. All keys go through this.
std::optional<std::string> account_key(const std::string& username) {
    std::string t = trim(username);
    if (t.empty()) return std::nullopt;
    return ascii_lower(std::move(t));
}

// --- device factories --------------------------------------------------------
bool is_default_device(std::string_view rid, std::string_view mac,
                       std::string_view wk) {
    return ascii_ieq(rid, constants::DEFAULT_RID) &&
           ascii_ieq(mac, constants::DEFAULT_MAC) &&
           ascii_ieq(wk, constants::DEFAULT_WK);
}

// Fully random identity (random hash triple too).
AccountDevice generate_device() {
    AccountDevice d;
    d.rid = random_hex32();
    d.mac = random_mac();
    d.wk = random_hex32();
    d.hash = random_i32();
    d.hash2 = random_i32();
    d.zf = random_i32();
    return d;
}

// Random rid/mac/wk but the DEFAULT hash triple.
AccountDevice default_device() {
    AccountDevice d;
    d.rid = random_hex32();
    d.mac = random_mac();
    d.wk = random_hex32();
    d.hash = std::string(constants::DEFAULT_HASH);
    d.hash2 = std::string(constants::DEFAULT_HASH2);
    d.zf = std::string(constants::DEFAULT_ZF);
    return d;
}

// Login-token device layout: f0 | rid | mac | wk. Exactly 4 '|'-parts; parts
// 1..3 must be non-empty after trimming; part 0 ignored.
std::optional<std::array<std::string, 3>> parse_login_token_device(
    const std::string& login_token) {
    std::string t = trim(login_token);
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t p = t.find('|', start);
        if (p == std::string::npos) {
            parts.push_back(t.substr(start));
            break;
        }
        parts.push_back(t.substr(start, p - start));
        start = p + 1;
    }
    if (parts.size() != 4) return std::nullopt;
    for (auto& pp : parts) pp = trim(pp);
    if (parts[1].empty() || parts[2].empty() || parts[3].empty())
        return std::nullopt;
    return std::array<std::string, 3>{parts[1], parts[2], parts[3]};
}

// --- persistence -------------------------------------------------------------
fs::path storage_path() {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec) cwd = fs::path(".");
    return cwd / "data" / "account_devices.json";
}

// Tolerant read: any I/O or JSON error (missing/corrupt file, malformed entry)
// yields an empty store. Never throws. All-or-nothing like serde's deserialize.
AccountDeviceStore read_store(const fs::path& path) {
    AccountDeviceStore store;
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return store;
        json j = json::parse(f);
        if (!j.is_object() || !j.contains("devices")) return store;
        const json& devs = j.at("devices");
        if (!devs.is_object()) return store;
        AccountDeviceStore parsed;
        for (auto it = devs.begin(); it != devs.end(); ++it) {
            const json& d = it.value();
            AccountDevice dev;
            dev.rid = d.at("rid").get<std::string>();
            dev.mac = d.at("mac").get<std::string>();
            dev.wk = d.at("wk").get<std::string>();
            dev.hash = d.at("hash").get<std::string>();
            dev.hash2 = d.at("hash2").get<std::string>();
            dev.zf = d.at("zf").get<std::string>();
            parsed.devices.emplace(it.key(), std::move(dev));
        }
        return parsed;
    } catch (...) {
        return AccountDeviceStore{};
    }
}

// create_dir_all(parent) + pretty JSON overwrite. Throws on I/O failure.
void write_store(const fs::path& path, const AccountDeviceStore& store) {
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            throw std::runtime_error(
                "failed to create data directory for account_devices.json: " +
                ec.message());
        }
    }
    json devices = json::object();
    for (const auto& [k, d] : store.devices) {
        devices[k] = {{"rid", d.rid},     {"mac", d.mac},
                      {"wk", d.wk},        {"hash", d.hash},
                      {"hash2", d.hash2},  {"zf", d.zf}};
    }
    json root;
    root["devices"] = std::move(devices);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw std::runtime_error(
            "failed to open account_devices.json for writing");
    }
    f << root.dump(2);
    if (!f) {
        throw std::runtime_error("failed to write account_devices.json");
    }
}

}  // namespace

std::optional<AccountDevice> get_or_create(const std::string& username) {
    auto key = account_key(username);
    if (!key) return std::nullopt;

    std::lock_guard<std::mutex> guard(store_lock());
    fs::path path = storage_path();
    AccountDeviceStore store = read_store(path);

    auto it = store.devices.find(*key);
    if (it != store.devices.end() &&
        !is_default_device(it->second.rid, it->second.mac, it->second.wk)) {
        return it->second;  // existing, non-default identity
    }
    // Absent, or the shared DEFAULT placeholder -> generate & persist a fresh one.
    AccountDevice device = generate_device();
    store.devices[*key] = device;
    write_store(path, store);
    return device;
}

bool upsert_from_login_token(const std::string& username,
                             const std::string& login_token) {
    auto key = account_key(username);
    if (!key) return false;

    auto parsed = parse_login_token_device(login_token);
    if (!parsed) return false;
    const std::string& rid = (*parsed)[0];
    const std::string& mac = (*parsed)[1];
    const std::string& wk = (*parsed)[2];

    std::lock_guard<std::mutex> guard(store_lock());
    fs::path path = storage_path();
    AccountDeviceStore store = read_store(path);

    if (is_default_device(rid, mac, wk)) {
        // Never stamp the shared DEFAULT identity fleet-wide.
        if (store.devices.find(*key) != store.devices.end()) {
            return false;  // keep the existing real identity
        }
        store.devices[*key] = generate_device();
        write_store(path, store);
        return true;
    }

    // Real per-account device carried in the token: import it.
    AccountDevice device;
    auto it = store.devices.find(*key);
    if (it != store.devices.end()) {
        device = it->second;
    } else {
        device = default_device();
    }
    if (device.rid != rid || device.mac != mac || device.wk != wk) {
        // Device fields changed -> derived hash triple is stale; reset to default.
        device.hash = std::string(constants::DEFAULT_HASH);
        device.hash2 = std::string(constants::DEFAULT_HASH2);
        device.zf = std::string(constants::DEFAULT_ZF);
    }
    device.rid = rid;
    device.mac = mac;
    device.wk = wk;
    store.devices[*key] = std::move(device);
    write_store(path, store);
    return true;
}

}  // namespace nxrth::account_devices
