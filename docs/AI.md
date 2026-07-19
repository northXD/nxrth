# Built-in AI Operator

Nxrth's `AI` tab lets a user connect an LLM provider and operate the currently
open application. The API key is kept in process memory for the current UI
session. It is never saved to a config, fleet backup, script, log, MCP response,
or recovery plan.

### Providers

The provider dropdown is a preset table. Every provider speaks one of two wire
formats: the **Anthropic Messages API**, or the **OpenAI Chat Completions API**.
Anthropic is the only preset on the Anthropic wire; all others — including Google
Gemini — use the OpenAI wire, because they each expose an OpenAI-compatible
`/chat/completions` endpoint that takes a `Bearer <key>` header. Selecting a
preset prefills its endpoint and a default model:

| Preset | Wire | Endpoint | Get a key |
| --- | --- | --- | --- |
| OpenAI | OpenAI | official default | platform.openai.com/api-keys |
| Anthropic (Claude) | Anthropic | official default | console.anthropic.com/settings/keys |
| Google Gemini | OpenAI | generativelanguage.googleapis.com/v1beta/openai/… | aistudio.google.com/apikey |
| OpenRouter | OpenAI | openrouter.ai/api/v1/… | openrouter.ai/keys |
| Groq | OpenAI | api.groq.com/openai/v1/… | console.groq.com/keys |
| DeepSeek | OpenAI | api.deepseek.com/v1/… | platform.deepseek.com/api_keys |
| xAI (Grok) | OpenAI | api.x.ai/v1/… | console.x.ai |
| Mistral | OpenAI | api.mistral.ai/v1/… | console.mistral.ai/api-keys |
| Together | OpenAI | api.together.xyz/v1/… | api.together.ai/settings/api-keys |
| Ollama (local) | OpenAI | localhost:11434/v1/… | none — any non-empty value |
| Custom (OpenAI-compatible) | OpenAI | set via the Endpoint button | provider-specific |

Both the model and endpoint stay editable after picking a preset. A blank
endpoint selects the wire's official default (OpenAI or Anthropic). A custom
endpoint must use HTTPS, except for `localhost` development endpoints (e.g.
Ollama / LM Studio).

Text entered into the AI chat is sent to the selected provider. If a user pastes
a login token into that chat so the model can call a login tool, that provider
necessarily receives the token. Use the native Add Bot form or local Lua editor
instead when login material must not be sent to a third-party AI provider.

## Execution Model

The controller sends the compiled system prompt from `src/ai/system_prompt.h`
and the authoritative schemas returned by `McpServer::tool_definitions()`.
Provider HTTP runs on a worker thread. Model-requested tools are dispatched by
`AiController::pump()` on the ImGui thread, which is the only thread that owns
`BotManager` and `ProxyPool`.

Two modes are available:

- `Ask for approval`: pauses before each model-requested tool batch and shows
  Allow/Deny controls. This is the default.
- `Autonomous`: executes valid tool calls without the approval pause.

One user turn is bounded to 12 provider rounds, 32 tool calls, a 4 MiB provider
response, and a 120 second HTTP request. Stop cancels an active HTTP request. A
tool callback already executing on the UI thread finishes before cancellation
takes effect.

The system prompt requires the model to inspect state, use supplied tools only,
verify mutations, respect automation scope, and report partial failure. It also
forbids reconstructing redacted values or claiming that recovery is enabled
without a confirming tool result.

## Secret Boundary

Redaction is an outbound boundary, not a storage mutation:

- Protected fleet backups preserve the exact ltoken/provider record, GrowID,
  password, custom proxy credentials, proxy policy, rotating-login intent, and
  automation config. Those exact bytes are encrypted locally with Windows
  CurrentUser DPAPI before being written.
- A legacy TXT export also preserves login material needed for another login,
  but is plaintext and cannot represent every policy field.
- AI/MCP responses never return backup contents. They return counts, bot IDs,
  formats, sizes, fingerprints, and success/error metadata only.
- `script_read` returns a redacted view. `script_execute` reads the exact local
  source internally and returns redacted output/error only.
- Bot/session logs redact recognized token, password, proxy, and long opaque
  secret values.

Therefore a saved `.fleet` or explicit `.txt` can log in again with the same
ltoken, while fleet backup APIs never return that ltoken to the model. Script
inspection uses conservative pattern-based redaction; secrets that must never
reach the selected AI provider should be entered through Add Bot or another
native local control instead of being pasted into AI chat or arbitrary script
text.

## Useful Tool Flows

Add bots and verify them:

1. Call `session_login`, `accounts_spawn`, or `lua_execute`/`script_execute`.
2. Read `session_list` and poll `session_status` for returned bot IDs.
3. Read logs only for failed or stuck sessions.

Save and restore a fleet:

1. Call `fleet_save` with a safe name and `format: "protected"`.
2. Use `fleet_backups_list` to verify the backup exists.
3. Call `fleet_load`; use its returned ID remap because restored bot IDs may
   differ.

Configure crash recovery:

1. Save a reusable Lua file with `script_write` when a restart script is needed.
2. Call `recovery_configure` with `snapshot_only`, `script_only`, or
   `snapshot_then_script`.
3. Call `recovery_status` and require both `enabled` and `supervisor_running`.
4. Call `recovery_disable` to stop monitoring.

The built-in AI and an external MCP client both bind to the same live desktop
fleet. There is no shadow bot manager in desktop mode.
