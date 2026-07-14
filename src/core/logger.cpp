#include "core/logger.h"

#include <algorithm>

namespace adonai {

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

void Logger::Log(std::string msg, int bot_id) {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.push_back(LogLine{bot_id, std::move(msg)});
    if (lines_.size() > kCap)
        lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kCap));
}

std::vector<LogLine> Logger::Snapshot(std::size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (lines_.size() <= max)
        return lines_;
    return std::vector<LogLine>(lines_.end() - static_cast<std::ptrdiff_t>(max), lines_.end());
}

void Logger::Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.clear();
}

} // namespace adonai
