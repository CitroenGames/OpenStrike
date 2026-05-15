#include "openstrike/core/log.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>

namespace openstrike
{
namespace
{
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

    std::string line;
    line.reserve(message.size() + 9);
    line += '[';
    line += level_name(level);
    line += "] ";
    line += message;

    history_.push_back(line);
    if (history_.size() > 512)
    {
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - 512));
    }

    std::clog << line << '\n';
}

std::vector<std::string> Logger::recent_lines(std::size_t max_count)
{
    std::scoped_lock lock(mutex_);
    if (history_.size() <= max_count)
    {
        return history_;
    }

    return std::vector<std::string>(history_.end() - static_cast<std::ptrdiff_t>(max_count), history_.end());
}
}
