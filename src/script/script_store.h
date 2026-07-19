// Nxrth - local Lua script workspace.
//
// Scripts are stored exactly under <working-directory>/scripts. Exact reads,
// imports, and exports are local-only operations: AI-facing callers must use
// read_redacted() so credentials embedded in a script are never disclosed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxrth::script {

struct ScriptMetadata {
    // Canonical filename, including the .lua extension.
    std::string name;
    std::uintmax_t size_bytes = 0;
    // Secret-safe content identity, e.g. "sha256:0123456789abcdef".
    std::string fingerprint;
};

class ScriptStore {
public:
    static constexpr std::size_t kMaxScriptBytes = 1024 * 1024;

    ScriptStore();

    const std::filesystem::path& workspace() const noexcept { return workspace_; }

    // Names may be supplied as "startup" or "startup.lua". The base name must
    // match [A-Za-z0-9_-]{1,64}; paths, traversal, and other extensions fail.
    static bool valid_name(std::string_view name) noexcept;

    std::vector<ScriptMetadata> list(std::string* error = nullptr) const;

    // The only read API suitable for MCP/AI responses.
    std::optional<std::string> read_redacted(std::string_view name,
                                             std::string* error = nullptr) const;

    // Local execution/UI only. Never return this value through an AI-facing API.
    std::optional<std::string> read_exact(std::string_view name,
                                         std::string* error = nullptr) const;

    // Writes exact source through a same-directory temporary file followed by
    // an atomic replace. Existing valid scripts are never truncated in place.
    bool write(std::string_view name, std::string_view source,
               std::string* error = nullptr) const;
    bool remove(std::string_view name, std::string* error = nullptr) const;

    // Local UI helpers for native file dialogs. These preserve exact source and
    // therefore must not be exposed directly through MCP or model tool output.
    bool import_file(const std::filesystem::path& source_path,
                     std::string_view stored_name, std::string* error = nullptr) const;
    bool export_file(std::string_view stored_name,
                     const std::filesystem::path& destination_path,
                     std::string* error = nullptr) const;

    static std::string redact_for_ai(std::string_view source);

    // Filesystem and redaction checks only; performs no network or bot action.
    static bool self_test(std::string* error = nullptr);

private:
    std::optional<std::filesystem::path> resolve(std::string_view name,
                                                 std::string* error) const;
    bool ensure_workspace(std::string* error) const;

    std::filesystem::path workspace_;
};

}  // namespace nxrth::script
