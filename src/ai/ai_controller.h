#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ai/system_prompt.h"

namespace nxrth::ai {

enum class Provider {
    OpenAICompatible,
    Anthropic,
};

enum class Autonomy {
    // Every assistant tool batch is exposed through pending_approval() and is
    // executed only after approve(id, true).
    Ask,
    // Tool calls are executed on the next pump() without an approval pause.
    Autonomous,
};

enum class TranscriptRole {
    User,
    Assistant,
    Tool,
    Status,
    Error,
};

struct AiSettings {
    Provider provider = Provider::OpenAICompatible;
    std::string endpoint;  // Empty selects the provider's official endpoint.
    std::string model;
    std::string api_key;
    Autonomy autonomy = Autonomy::Ask;
    std::uint32_t max_tokens = 4096;
};

struct TranscriptEntry {
    TranscriptRole role = TranscriptRole::Status;
    std::string text;       // Always secret-redacted.
    std::string tool_name;  // Populated only for Tool entries.
    bool error = false;
};

struct ToolCallPreview {
    std::string id;
    std::string name;
    nlohmann::json arguments;  // Sensitive keys and long secret-like values redacted.
};

struct ApprovalRequest {
    std::uint64_t id = 0;
    std::vector<ToolCallPreview> calls;
};

// The callback receives the original (unredacted) arguments and may return any
// JSON value. AiController redacts the returned value before it is made visible
// to the model or transcript. It is never invoked by an HTTP worker: only by a
// caller-driven pump() invocation.
using ToolExecutor =
    std::function<nlohmann::json(const std::string& name,
                                 const nlohmann::json& arguments)>;

class AiController {
public:
    explicit AiController(
        nlohmann::json canonical_tools = nlohmann::json::array(),
        ToolExecutor executor = {},
        std::string system_prompt = std::string(kDefaultSystemPrompt));
    ~AiController();

    AiController(const AiController&) = delete;
    AiController& operator=(const AiController&) = delete;

    // Starts one bounded agent turn. HTTP runs on a worker; call pump() from the
    // application's owner/UI thread to accept responses and execute tools.
    bool submit(std::string user_message, AiSettings settings,
                std::string* error = nullptr);
    void pump();
    bool busy() const;
    void cancel();

    std::vector<TranscriptEntry> transcript_snapshot() const;
    std::optional<ApprovalRequest> pending_approval() const;

    // Records a decision; approved tools still wait for pump() before execution.
    bool approve(std::uint64_t approval_id, bool allow,
                 std::string* error = nullptr);

    // Conversation history can be cleared only while idle. Configuration
    // setters follow the same rule so an in-flight request has an immutable tool
    // contract.
    bool clear();
    bool set_tools(nlohmann::json canonical_tools, std::string* error = nullptr);
    bool set_tool_executor(ToolExecutor executor);
    bool set_system_prompt(std::string prompt, std::string* error = nullptr);

    std::string last_error() const;

    static std::string default_endpoint(Provider provider);

    // Pure fixture tests: schema conversion, provider response parsing, and
    // secret redaction. This function performs no network I/O.
    static bool self_test(std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nxrth::ai
