// Nxrth — accounts_stats.json / user:pass extractor (ported from Mori's web
// AccountsPage parseAccountStats). Feeds the bulk "Accounts" tab.
#pragma once
#include <string>
#include <vector>

namespace nxrth::core {

struct Account {
    std::string username;
    std::string password;
    std::string login_token;  // optional positional or provider-keyed ltoken record
};

// Extract accounts from `input`. If it parses as JSON, recursively collect any
// object carrying a username+password (many key spellings) plus optional
// login_token, and any "user:pass" string values. Otherwise falls back to
// line-based "user:pass" or keyed-ltoken parsing.
std::vector<Account> parse_account_stats(const std::string& input);

}  // namespace nxrth::core
