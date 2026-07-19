#include "script/script_store.h"

#include "protocol/crypto.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nxrth::script {
namespace {

constexpr std::string_view kLuaExtension = ".lua";
constexpr std::string_view kRedacted = "<redacted>";

void clear_error(std::string* error) {
    if (error) error->clear();
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

template <typename T>
std::optional<T> fail_optional(std::string* error, std::string message) {
    fail(error, std::move(message));
    return std::nullopt;
}

char lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

std::string lower_ascii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return lower_ascii(c); });
    return out;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i)
        if (lower_ascii(lhs[i]) != lower_ascii(rhs[i])) return false;
    return true;
}

bool identifier_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_';
}

bool base64ish(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '+' || c == '/' || c == '=' || c == '_' ||
           c == '-';
}

bool sensitive_key(std::string_view key) {
    static constexpr std::string_view keys[] = {
        "refresh_token", "refreshtoken", "proxy_password", "webhook_url",
        "webhookurl",    "tankidpass",   "password",       "passwd",
        "ltoken",        "token",        "proxy",          "webhook",
        "api_key",       "apikey",       "authorization",  "bearer",
        "secret",        "tokens",       "accounts",       "account_list",
        "account_data",  "accounts_data", "credentials",    "login_records",
    };
    if (std::any_of(std::begin(keys), std::end(keys), [&](std::string_view candidate) {
            return ascii_iequals(key, candidate);
        }))
        return true;

    // Cover application-specific variants such as oauthToken, provider_token,
    // proxyUrl, discordWebhook, and accountPassword without maintaining an
    // ever-growing spelling list.
    const std::string folded = lower_ascii(key);
    return folded.ends_with("token") || folded.ends_with("password") ||
           folded.find("proxy") != std::string::npos ||
           folded.find("webhook") != std::string::npos;
}

bool has_lua_extension(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    return ascii_iequals(extension, kLuaExtension);
}

std::optional<std::string> canonical_filename(std::string_view raw) {
    if (raw.empty()) return std::nullopt;

    std::string base(raw);
    if (base.size() >= kLuaExtension.size() &&
        ascii_iequals(std::string_view(base).substr(base.size() - kLuaExtension.size()),
                      kLuaExtension)) {
        base.resize(base.size() - kLuaExtension.size());
    }

    if (base.empty() || base.size() > 64) return std::nullopt;
    for (char c : base) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) == 0 && c != '_' && c != '-') return std::nullopt;
    }

    // Win32 treats these as devices even when an extension is present.
    const std::string folded = lower_ascii(base);
    if (folded == "con" || folded == "prn" || folded == "aux" || folded == "nul" ||
        (folded.size() == 4 && folded[3] >= '1' && folded[3] <= '9' &&
         (folded.starts_with("com") || folded.starts_with("lpt"))))
        return std::nullopt;
    return base + std::string(kLuaExtension);
}

bool is_reparse_or_symlink(const std::filesystem::path& path, std::error_code& ec) {
    ec.clear();
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) return false;
    if (std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

std::optional<std::string> read_bounded_file(const std::filesystem::path& path,
                                             std::string* error) {
    std::error_code ec;
    if (is_reparse_or_symlink(path, ec))
        return fail_optional<std::string>(error, "Refusing to read a linked script file");
    if (ec)
        return fail_optional<std::string>(error,
                                          "Could not inspect script file: " + ec.message());

    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status))
        return fail_optional<std::string>(error, "Script file does not exist");

    const auto announced_size = std::filesystem::file_size(path, ec);
    if (ec)
        return fail_optional<std::string>(error,
                                          "Could not inspect script size: " + ec.message());
    if (announced_size > ScriptStore::kMaxScriptBytes)
        return fail_optional<std::string>(error, "Script exceeds the 1 MiB limit");

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return fail_optional<std::string>(error, "Could not open script file for reading");

    std::string result;
    result.reserve(static_cast<std::size_t>(announced_size));
    std::array<char, 16 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        if (result.size() + static_cast<std::size_t>(count) > ScriptStore::kMaxScriptBytes)
            return fail_optional<std::string>(error, "Script exceeds the 1 MiB limit");
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (input.bad())
        return fail_optional<std::string>(error, "Could not finish reading script file");
    return result;
}

class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFileGuard() {
        if (path_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    void release() noexcept { path_.clear(); }

private:
    std::filesystem::path path_;
};

bool atomic_write_file(const std::filesystem::path& destination, std::string_view data,
                       bool require_existing_parent, std::string* error) {
    if (data.size() > ScriptStore::kMaxScriptBytes)
        return fail(error, "Script exceeds the 1 MiB limit");

    std::error_code ec;
    const auto parent = destination.parent_path();
    if (parent.empty()) return fail(error, "Destination has no parent directory");

    if (require_existing_parent) {
        const auto parent_status = std::filesystem::status(parent, ec);
        if (ec || !std::filesystem::is_directory(parent_status))
            return fail(error, "Destination directory does not exist");
    }

    if (std::filesystem::exists(destination, ec)) {
        if (ec) return fail(error, "Could not inspect destination: " + ec.message());
        if (is_reparse_or_symlink(destination, ec))
            return fail(error, "Refusing to replace a linked file");
        if (ec) return fail(error, "Could not inspect destination: " + ec.message());
        const auto status = std::filesystem::status(destination, ec);
        if (ec || !std::filesystem::is_regular_file(status))
            return fail(error, "Destination is not a regular file");
    } else if (ec) {
        return fail(error, "Could not inspect destination: " + ec.message());
    }

    std::filesystem::path temporary;
    for (int attempt = 0; attempt < 20; ++attempt) {
        temporary = parent /
                    ("." + destination.filename().string() + ".tmp-" +
                     nxrth::protocol::random_hex(16));
        if (!std::filesystem::exists(temporary, ec) && !ec) break;
        temporary.clear();
        ec.clear();
    }
    if (temporary.empty()) return fail(error, "Could not reserve a temporary script file");

    TemporaryFileGuard cleanup(temporary);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return fail(error, "Could not open temporary script file");
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        output.flush();
        if (!output) return fail(error, "Could not write temporary script file");
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto move_error =
            std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return fail(error, "Could not atomically replace script: " + move_error.message());
    }
#else
    std::filesystem::rename(temporary, destination, ec);
    if (ec) return fail(error, "Could not atomically replace script: " + ec.message());
#endif

    cleanup.release();
    return true;
}

std::size_t skip_spaces(std::string_view text, std::size_t pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
            text[pos] == '\n'))
        ++pos;
    return pos;
}

// Returns the closing delimiter position (exclusive) for a Lua long string
// beginning at pos, or npos when pos is not a long-string opener.
std::size_t lua_long_string_end(std::string_view text, std::size_t pos,
                                std::size_t* content_begin) {
    if (pos >= text.size() || text[pos] != '[') return std::string_view::npos;
    std::size_t cursor = pos + 1;
    while (cursor < text.size() && text[cursor] == '=') ++cursor;
    if (cursor >= text.size() || text[cursor] != '[') return std::string_view::npos;

    const std::size_t equals = cursor - (pos + 1);
    *content_begin = cursor + 1;
    std::string closing = "]" + std::string(equals, '=') + "]";
    const auto close = text.find(closing, *content_begin);
    return close == std::string_view::npos ? text.size() : close;
}

bool inline_username_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.' || c == '@';
}

bool inline_credential_delimiter(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\'' ||
           c == '"' || c == ']' || c == '}' || c == ')' || c == ',' || c == ';' ||
           c == '|';
}

// Credential lists are often stored under generic names such as `data`, so
// key-based redaction alone is insufficient. Conservatively redact user:pass
// shaped values inside Lua string literals. False positives only affect the
// AI-visible inspection copy; the exact local script is never changed.
void redact_inline_string_credentials(std::string& text) {
    struct Span { std::size_t begin = 0; std::size_t end = 0; };
    std::vector<Span> strings;

    for (std::size_t pos = 0; pos < text.size();) {
        if (text[pos] == '\'' || text[pos] == '"') {
            const char quote = text[pos++];
            const std::size_t begin = pos;
            bool escaped = false;
            while (pos < text.size()) {
                if (!escaped && text[pos] == quote) break;
                if (!escaped && text[pos] == '\\')
                    escaped = true;
                else
                    escaped = false;
                ++pos;
            }
            strings.push_back({begin, pos});
            if (pos < text.size()) ++pos;
            continue;
        }
        std::size_t content_begin = 0;
        const auto close = lua_long_string_end(text, pos, &content_begin);
        if (close != std::string_view::npos) {
            strings.push_back({content_begin, close});
            pos = close;
            while (pos < text.size() && text[pos] == ']') ++pos;
            continue;
        }
        ++pos;
    }

    std::vector<Span> matches;
    for (const auto span : strings) {
        std::size_t pos = span.begin;
        while (pos < span.end) {
            if (!inline_username_char(text[pos]) ||
                (pos > span.begin && inline_username_char(text[pos - 1]))) {
                ++pos;
                continue;
            }
            std::size_t colon = pos;
            while (colon < span.end && inline_username_char(text[colon]) &&
                   colon - pos <= 64)
                ++colon;
            const std::size_t user_length = colon - pos;
            if (user_length < 2 || user_length > 64 || colon >= span.end ||
                text[colon] != ':') {
                pos = std::max(pos + 1, colon);
                continue;
            }
            std::size_t end = colon + 1;
            while (end < span.end && !inline_credential_delimiter(text[end]) &&
                   end - (colon + 1) <= 256)
                ++end;
            const std::size_t password_length = end - (colon + 1);
            const bool scheme = password_length >= 2 && text[colon + 1] == '/' &&
                                text[colon + 2] == '/';
            bool digits_only = password_length != 0;
            for (std::size_t i = colon + 1; i < end && digits_only; ++i)
                digits_only = std::isdigit(static_cast<unsigned char>(text[i])) != 0;
            if (password_length >= 3 && password_length <= 256 && !scheme && !digits_only) {
                matches.push_back({pos, end});
                pos = end;
            } else {
                pos = colon + 1;
            }
        }
    }

    for (auto it = matches.rbegin(); it != matches.rend(); ++it)
        text.replace(it->begin, it->end - it->begin, kRedacted);
}

void redact_labeled_values(std::string& text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (!identifier_char(text[pos]) ||
            (pos > 0 && identifier_char(text[pos - 1]))) {
            ++pos;
            continue;
        }

        std::size_t key_end = pos;
        while (key_end < text.size() && identifier_char(text[key_end])) ++key_end;
        if (!sensitive_key(std::string_view(text).substr(pos, key_end - pos))) {
            pos = key_end;
            continue;
        }

        std::size_t delimiter = key_end;
        // JSON/table keys may quote the key itself: "token": "...".
        if (delimiter < text.size() && (text[delimiter] == '\'' || text[delimiter] == '"'))
            ++delimiter;
        if (delimiter < text.size() && text[delimiter] == ']') ++delimiter;
        delimiter = skip_spaces(text, delimiter);
        if (delimiter >= text.size() ||
            (text[delimiter] != '=' && text[delimiter] != ':' && text[delimiter] != '|')) {
            pos = key_end;
            continue;
        }

        std::size_t value_begin = skip_spaces(text, delimiter + 1);
        if (value_begin >= text.size()) {
            pos = value_begin;
            continue;
        }

        if (text[value_begin] == '\'' || text[value_begin] == '"') {
            const char quote = text[value_begin];
            std::size_t value_end = value_begin + 1;
            bool escaped = false;
            while (value_end < text.size()) {
                if (!escaped && text[value_end] == quote) break;
                if (!escaped && text[value_end] == '\\')
                    escaped = true;
                else
                    escaped = false;
                ++value_end;
            }
            const std::size_t content = value_begin + 1;
            text.replace(content, value_end - content, kRedacted);
            pos = content + kRedacted.size();
            continue;
        }

        std::size_t long_content = 0;
        const auto long_end = lua_long_string_end(text, value_begin, &long_content);
        if (long_end != std::string_view::npos) {
            text.replace(long_content, long_end - long_content, kRedacted);
            pos = long_content + kRedacted.size();
            continue;
        }

        std::size_t value_end = value_begin;
        while (value_end < text.size() && text[value_end] != '|' &&
               text[value_end] != ',' && text[value_end] != ';' &&
               text[value_end] != '\r' && text[value_end] != '\n' &&
               text[value_end] != '\t' && text[value_end] != ' ' &&
               text[value_end] != '}' && text[value_end] != ']')
            ++value_end;
        if (value_end > value_begin) {
            text.replace(value_begin, value_end - value_begin, kRedacted);
            pos = value_begin + kRedacted.size();
        } else {
            pos = key_end;
        }
    }
}

void redact_long_base64ish_runs(std::string& text) {
    std::size_t run = 0;
    while (run < text.size()) {
        while (run < text.size() && !base64ish(text[run])) ++run;
        std::size_t end = run;
        while (end < text.size() && base64ish(text[end])) ++end;
        if (end - run >= 80) {
            text.replace(run, end - run, kRedacted);
            run += kRedacted.size();
        } else {
            run = end;
        }
    }
}

struct SelfTestCleanup {
    std::vector<std::filesystem::path> paths;
    ~SelfTestCleanup() {
        for (const auto& path : paths) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

}  // namespace

ScriptStore::ScriptStore()
    : workspace_(std::filesystem::absolute(std::filesystem::current_path() / "scripts")
                     .lexically_normal()) {}

bool ScriptStore::valid_name(std::string_view name) noexcept {
    try {
        return canonical_filename(name).has_value();
    } catch (...) {
        return false;
    }
}

bool ScriptStore::ensure_workspace(std::string* error) const {
    std::error_code ec;
    std::filesystem::create_directories(workspace_, ec);
    if (ec) return fail(error, "Could not create scripts workspace: " + ec.message());

    if (is_reparse_or_symlink(workspace_, ec))
        return fail(error, "Scripts workspace may not be a link or reparse point");
    if (ec) return fail(error, "Could not inspect scripts workspace: " + ec.message());

    const auto status = std::filesystem::status(workspace_, ec);
    if (ec || !std::filesystem::is_directory(status))
        return fail(error, "Scripts workspace is not a directory");
    return true;
}

std::optional<std::filesystem::path> ScriptStore::resolve(std::string_view name,
                                                          std::string* error) const {
    const auto filename = canonical_filename(name);
    if (!filename)
        return fail_optional<std::filesystem::path>(
            error, "Script name must match [A-Za-z0-9_-]{1,64} with optional .lua");
    if (!ensure_workspace(error)) return std::nullopt;
    return workspace_ / *filename;
}

std::vector<ScriptMetadata> ScriptStore::list(std::string* error) const {
    clear_error(error);
    if (!ensure_workspace(error)) return {};

    std::vector<ScriptMetadata> scripts;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(workspace_, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto& entry = *it;
        std::error_code entry_error;
        if (is_reparse_or_symlink(entry.path(), entry_error) || entry_error) continue;
        if (!entry.is_regular_file(entry_error) || entry_error ||
            !has_lua_extension(entry.path()))
            continue;

        const auto filename = entry.path().filename().string();
        if (!valid_name(filename)) continue;

        auto source = read_bounded_file(entry.path(), error);
        if (!source) return {};
        scripts.push_back({filename, static_cast<std::uintmax_t>(source->size()),
                           "sha256:" +
                               nxrth::protocol::sha256_hex(*source).substr(0, 16)});
    }
    if (ec) {
        fail(error, "Could not enumerate scripts workspace: " + ec.message());
        return {};
    }

    std::sort(scripts.begin(), scripts.end(), [](const auto& lhs, const auto& rhs) {
        return lower_ascii(lhs.name) < lower_ascii(rhs.name);
    });
    return scripts;
}

std::optional<std::string> ScriptStore::read_exact(std::string_view name,
                                                   std::string* error) const {
    clear_error(error);
    auto path = resolve(name, error);
    if (!path) return std::nullopt;
    return read_bounded_file(*path, error);
}

std::optional<std::string> ScriptStore::read_redacted(std::string_view name,
                                                      std::string* error) const {
    auto source = read_exact(name, error);
    if (!source) return std::nullopt;
    return redact_for_ai(*source);
}

bool ScriptStore::write(std::string_view name, std::string_view source,
                        std::string* error) const {
    clear_error(error);
    auto path = resolve(name, error);
    if (!path) return false;
    return atomic_write_file(*path, source, true, error);
}

bool ScriptStore::remove(std::string_view name, std::string* error) const {
    clear_error(error);
    auto path = resolve(name, error);
    if (!path) return false;

    std::error_code ec;
    if (is_reparse_or_symlink(*path, ec))
        return fail(error, "Refusing to remove a linked file");
    if (ec) return fail(error, "Could not inspect script file: " + ec.message());

    if (!std::filesystem::remove(*path, ec)) {
        if (ec) return fail(error, "Could not remove script: " + ec.message());
        return fail(error, "Script file does not exist");
    }
    return true;
}

bool ScriptStore::import_file(const std::filesystem::path& source_path,
                              std::string_view stored_name, std::string* error) const {
    clear_error(error);
    if (!has_lua_extension(source_path))
        return fail(error, "Imported script must have a .lua extension");
    auto source = read_bounded_file(source_path, error);
    if (!source) return false;
    return write(stored_name, *source, error);
}

bool ScriptStore::export_file(std::string_view stored_name,
                              const std::filesystem::path& destination_path,
                              std::string* error) const {
    clear_error(error);
    if (!has_lua_extension(destination_path))
        return fail(error, "Export destination must have a .lua extension");
    auto source = read_exact(stored_name, error);
    if (!source) return false;
    std::error_code ec;
    const auto destination = std::filesystem::absolute(destination_path, ec).lexically_normal();
    if (ec) return fail(error, "Could not resolve export destination: " + ec.message());
    return atomic_write_file(destination, *source, true, error);
}

std::string ScriptStore::redact_for_ai(std::string_view source) {
    std::string redacted(source);
    redact_labeled_values(redacted);
    redact_inline_string_credentials(redacted);
    redact_long_base64ish_runs(redacted);
    return redacted;
}

bool ScriptStore::self_test(std::string* error) {
    clear_error(error);
    try {
        ScriptStore store;
        const std::string suffix = nxrth::protocol::random_hex(12);
        const std::string name = "nxrth_store_test_" + suffix;
        const std::string imported_name = "nxrth_store_copy_" + suffix;
        const auto stored_path = store.workspace() / (name + ".lua");
        const auto imported_path = store.workspace() / (imported_name + ".lua");
        const auto external_path =
            std::filesystem::temp_directory_path() / ("nxrth_export_" + suffix + ".lua");
        SelfTestCleanup cleanup{{stored_path, imported_path, external_path}};

        const std::string fake_token = std::string(96, 'A');
        const std::string fake_password = "self_test_password_placeholder";
        const std::string fake_proxy = "socks5://user:pass@127.0.0.1:1080";
        const std::string fake_webhook = "https://invalid.example/webhook/test-placeholder";
        const std::string fake_account = "user:shortpass";
        const std::string fake_generic_account = "second_user:anotherpass";
        const std::string source =
            "local token = \"" + fake_token + "\"\n" +
            "local password = '" + fake_password + "'\n" +
            "local proxy = \"" + fake_proxy + "\"\n" +
            "local webhook = [[" + fake_webhook + "]]\n" +
            "local accounts = [[" + fake_account + "]]\n" +
            "local data = [[" + fake_generic_account + "]]\nreturn true\n";

        std::string detail;
        if (!valid_name(name) || valid_name("../escape") || valid_name("C:\\escape.lua") ||
            valid_name("bad.txt"))
            return fail(error, "Script name validation failed");
        if (!store.write(name, source, &detail))
            return fail(error, "Script write self-test failed: " + detail);

        const auto exact = store.read_exact(name + ".lua", &detail);
        if (!exact || *exact != source)
            return fail(error, "Exact script read self-test failed: " + detail);

        const auto redacted = store.read_redacted(name, &detail);
        if (!redacted || redacted->find(kRedacted) == std::string::npos ||
            redacted->find(fake_token) != std::string::npos ||
            redacted->find(fake_password) != std::string::npos ||
            redacted->find(fake_proxy) != std::string::npos ||
            redacted->find(fake_webhook) != std::string::npos ||
            redacted->find(fake_account) != std::string::npos ||
            redacted->find(fake_generic_account) != std::string::npos)
            return fail(error, "AI script redaction self-test failed");

        const auto scripts = store.list(&detail);
        const auto listed = std::find_if(scripts.begin(), scripts.end(), [&](const auto& item) {
            return item.name == name + ".lua" && item.size_bytes == source.size() &&
                   item.fingerprint.rfind("sha256:", 0) == 0;
        });
        if (listed == scripts.end())
            return fail(error, "Script metadata self-test failed: " + detail);

        const std::string oversized(kMaxScriptBytes + 1, 'x');
        if (store.write(name, oversized, &detail))
            return fail(error, "Oversized script was accepted");
        const auto after_failed_write = store.read_exact(name, &detail);
        if (!after_failed_write || *after_failed_write != source)
            return fail(error, "Failed write changed the existing script");

        if (!store.export_file(name, external_path, &detail))
            return fail(error, "Script export self-test failed: " + detail);
        if (!store.import_file(external_path, imported_name, &detail))
            return fail(error, "Script import self-test failed: " + detail);
        const auto imported = store.read_exact(imported_name, &detail);
        if (!imported || *imported != source)
            return fail(error, "Imported script did not preserve exact source");

        return true;
    } catch (const std::exception& e) {
        return fail(error, std::string("ScriptStore self-test exception: ") + e.what());
    } catch (...) {
        return fail(error, "ScriptStore self-test failed with an unknown exception");
    }
}

}  // namespace nxrth::script
