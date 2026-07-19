#include "core/logger.h"

#include <algorithm>

namespace nxrth {

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

void Logger::Log(std::string msg, int bot_id) {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.push_back(LogLine{bot_id, std::move(msg)});
    while (lines_.size() > kCap) lines_.pop_front();
}

std::vector<LogLine> Logger::Snapshot(std::size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t count = std::min(max, lines_.size());
    return std::vector<LogLine>(lines_.end() - static_cast<std::ptrdiff_t>(count), lines_.end());
}

void Logger::Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.clear();
}

} // namespace nxrth
