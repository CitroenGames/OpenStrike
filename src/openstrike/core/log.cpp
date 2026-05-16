#include "openstrike/core/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>

namespace openstrike
{
namespace
{
constexpr std::size_t kMaxHistory = 512;

const char* level_name(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warn";
    case LogLevel::Error:
        return "error";
    }

    return "unknown";
}

std::string format_line(LogLevel level, std::string_view message)
{
    std::string line;
    line.reserve(message.size() + 9);
    line += '[';
    line += level_name(level);
    line += "] ";
    line.append(message.data(), message.size());
    return line;
}
}

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::set_min_level(LogLevel level)
{
    std::scoped_lock lock(mutex_);
    min_level_ = level;
}

void Logger::write(LogLevel level, std::string_view message)
{
    std::scoped_lock lock(mutex_);
    if (level < min_level_)
    {
        return;
    }

    LogEntry entry{level, std::string(message)};
    const std::string line = format_line(level, entry.message);

    history_.push_back(std::move(entry));
    if (history_.size() > kMaxHistory)
    {
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - kMaxHistory));
    }

    std::clog << line << '\n';
}

std::vector<std::string> Logger::recent_lines(std::size_t max_count)
{
    std::scoped_lock lock(mutex_);
    const std::size_t start = history_.size() > max_count ? history_.size() - max_count : 0;
    std::vector<std::string> lines;
    lines.reserve(history_.size() - start);
    for (std::size_t i = start; i < history_.size(); ++i)
    {
        lines.push_back(format_line(history_[i].level, history_[i].message));
    }
    return lines;
}

std::vector<LogEntry> Logger::recent_entries(std::size_t max_count)
{
    std::scoped_lock lock(mutex_);
    if (history_.size() <= max_count)
    {
        return history_;
    }
    return std::vector<LogEntry>(history_.end() - static_cast<std::ptrdiff_t>(max_count), history_.end());
}

void Logger::clear_history()
{
    std::scoped_lock lock(mutex_);
    history_.clear();
}
}
