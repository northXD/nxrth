// Nxrth — per-account device identity store (FLEET-WIDE, process-global).
// Ported from Nxrth/account_devices.rs. Persists a stable, unique device identity
// (rid/mac/wk + hash/hash2/zf) per account into data/account_devices.json so that
// every bot in the fleet presents as a distinct device. All access serializes on
// one process-global mutex around a read-modify-write of the JSON file.
#pragma once
#include <map>
#include <optional>
#include <string>

namespace nxrth::account_devices {

// One account's device identity (serde JSON <-> struct in the Rust original).
struct AccountDevice {
    std::string rid;
    std::string mac;
    std::string wk;
    std::string hash;
    std::string hash2;
    std::string zf;
};

// On-disk container: { "devices": { <key>: AccountDevice, ... } }.
// key = trimmed, ASCII-lowercased username. Ordered (BTreeMap -> std::map) so the
// serialized JSON is stable across runs.
struct AccountDeviceStore {
    std::map<std::string, AccountDevice> devices;
};

// Returns the stable device identity for `username`, creating & persisting a fresh
// fully-random one if the account has no entry yet, or if its stored entry is the
// shared DEFAULT placeholder (self-heal — all bots looking identical is a ban
// vector). Returns std::nullopt only when `username` trims to empty.
// Throws std::runtime_error if the store cannot be written to disk.
std::optional<AccountDevice> get_or_create(const std::string& username);

// Imports the (rid|mac|wk) device carried in a login token (layout: f0|rid|mac|wk,
// field0 ignored). Returns true iff the store was modified and written. A token
// carrying only the shared DEFAULT device never overwrites an existing real
// identity, and never stamps the same rid/mac/wk across the fleet. When the real
// device fields change, the derived hash triple is reset to defaults.
// Throws std::runtime_error if the store cannot be written to disk.
bool upsert_from_login_token(const std::string& username,
                             const std::string& login_token);

}  // namespace nxrth::account_devices
