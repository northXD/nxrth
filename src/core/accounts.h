// Adonai — accounts_stats.json / user:pass extractor (ported from Mori's web
// AccountsPage parseAccountStats). Feeds the bulk "Accounts" tab.
#pragma once
#include <string>
#include <vector>

namespace adonai::core {

struct Account {
    std::string username;
    std::string password;
    std::string login_token;  // optional ltoken (login_token/loginToken/ltoken)
};

// Extract accounts from `input`. If it parses as JSON, recursively collect any
// object carrying a username+password (many key spellings) plus optional
// login_token, and any "user:pass" string values. Otherwise falls back to
// line-based "user:pass" parsing. Deduplicated by lowercase username.
std::vector<Account> parse_account_stats(const std::string& input);

}  // namespace adonai::core
