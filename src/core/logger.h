// Nxrth — thread-safe ring-buffer logger. The ImGui console reads Snapshot().
#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace nxrth {

struct LogLine {
    int bot_id;       // -1 = global / manager line
    std::string text; // already formatted (no trailing newline)
};

class Logger {
public:
    static Logger& Instance();

    // Thread-safe. bot_id tags the line so the shared console can be filtered.
    void Log(std::string msg, int bot_id = -1);

    // Copy of the most recent `max` lines (oldest first).
    std::vector<LogLine> Snapshot(std::size_t max = 1000) const;

    void Clear();

private:
    Logger() = default;
    mutable std::mutex mu_;
    std::deque<LogLine> lines_;
    static constexpr std::size_t kCap = 5000;
};

// Convenience free function mirroring the Rust `logger::log`.
inline void log(std::string msg, int bot_id = -1) {
    Logger::Instance().Log(std::move(msg), bot_id);
}

} // namespace nxrth
