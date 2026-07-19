// Trusted local fleet persistence. Credentials are preserved byte-for-byte in
// a DPAPI CurrentUser encrypted document; public result types contain metadata
// only and are therefore safe to return through MCP.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nxrth::bot {
class BotManager;
}
namespace nxrth::proxy {
class ProxyPool;
}

namespace nxrth::recovery {

enum class FleetBackupFormat { Protected, LegacyText };

struct FleetBackupInfo {
    std::string name;
    FleetBackupFormat format = FleetBackupFormat::Protected;
    std::uintmax_t file_size = 0;
    std::int64_t modified_unix_ms = 0;
};

struct FleetSaveResult {
    bool ok = false;
    std::string name;
    std::size_t bot_count = 0;
    std::uint64_t launch_generation = 0;
    std::uintmax_t encrypted_bytes = 0;
    std::size_t lossy_record_count = 0;  // legacy TXT cannot carry every policy field
    std::string error;
};

struct FleetLoadIssue {
    std::size_t record_index = 0;
    std::optional<std::uint32_t> old_bot_id;
    std::string error;
};

struct FleetLoadResult {
    bool ok = false;
    std::string name;
    FleetBackupFormat format = FleetBackupFormat::Protected;
    std::size_t record_count = 0;
    std::size_t stopped_count = 0;
    std::size_t spawned_count = 0;
    bool automation_restored = false;
    std::uint64_t launch_generation = 0;
    std::vector<std::uint32_t> new_bot_ids;
    std::unordered_map<std::uint32_t, std::uint32_t> id_remap;
    std::vector<FleetLoadIssue> issues;
    std::string error;
};

struct FleetLoadOptions {
    // Merge without changing the active automation profile unless the caller
    // explicitly requests a full restore. This is the safe UI/MCP default;
    // crash recovery opts in to automation restoration separately.
    bool replace_existing = false;
    bool restore_automation = false;
};

class FleetStore {
public:
    // All public name-based operations are confined to <cwd>/data/fleets.
    // Names are 1..80 ASCII letters/digits/'-'/'_' and never include a path.
    static std::filesystem::path directory();
    static bool valid_backup_name(std::string_view name);
    static std::optional<std::filesystem::path> backup_path(
        std::string_view name, FleetBackupFormat format = FleetBackupFormat::Protected);

    static std::vector<FleetBackupInfo> list();

    // Protected backups contain exact credentials, passwords, custom proxy
    // authentication, rotating-login intent, and AutomationConfig. Nothing is
    // redacted before encryption.
    static FleetSaveResult save(std::string_view name, const nxrth::bot::BotManager& manager);
    static FleetSaveResult save_legacy_text(std::string_view name,
                                            const nxrth::bot::BotManager& manager);
    static FleetLoadResult load(std::string_view name, nxrth::bot::BotManager& manager,
                                nxrth::proxy::ProxyPool& proxy_pool,
                                FleetLoadOptions options = {});

    // Imports the Bots-tab's current one-record-per-line TXT representation.
    // It intentionally has no automation snapshot or rotating-login metadata.
    static FleetLoadResult load_legacy_text(std::string_view name,
                                            nxrth::bot::BotManager& manager,
                                            nxrth::proxy::ProxyPool& proxy_pool,
                                            FleetLoadOptions options = {});

    // Trusted desktop file-dialog adapters. Unlike name-based MCP operations,
    // these accept an arbitrary user-selected path and must not be exposed as
    // an unrestricted remote tool.
    static FleetSaveResult save_to_path(const std::filesystem::path& path,
                                        FleetBackupFormat format,
                                        const nxrth::bot::BotManager& manager);
    static FleetLoadResult load_from_path(const std::filesystem::path& path,
                                          FleetBackupFormat format,
                                          nxrth::bot::BotManager& manager,
                                          nxrth::proxy::ProxyPool& proxy_pool,
                                          FleetLoadOptions options = {});

    // DPAPI + JSON exact-byte roundtrip and AutomationConfig id-remap checks.
    // Does not construct/spawn a bot, open a socket, or touch the filesystem.
    static bool self_test(std::string* error = nullptr);
};

}  // namespace nxrth::recovery
