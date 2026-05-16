#pragma once

#include <cstddef>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openstrike
{
enum class LogLevel
{
    Trace,
    Info,
    Warning,
    Error
};

struct LogEntry
{
    LogLevel level = LogLevel::Info;
    std::string message;
};

class Logger
{
public:
    static Logger& instance();

    void set_min_level(LogLevel level);
    void write(LogLevel level, std::string_view message);
    [[nodiscard]] std::vector<std::string> recent_lines(std::size_t max_count = 200);
    [[nodiscard]] std::vector<LogEntry> recent_entries(std::size_t max_count = 200);
    void clear_history();

private:
    std::mutex mutex_;
    LogLevel min_level_ = LogLevel::Info;
    std::vector<LogEntry> history_;
};

template <typename... Args>
void log(LogLevel level, std::format_string<Args...> format, Args&&... args)
{
    Logger::instance().write(level, std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void log_trace(std::format_string<Args...> format, Args&&... args)
{
    log(LogLevel::Trace, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_info(std::format_string<Args...> format, Args&&... args)
{
    log(LogLevel::Info, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_warning(std::format_string<Args...> format, Args&&... args)
{
    log(LogLevel::Warning, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_error(std::format_string<Args...> format, Args&&... args)
{
    log(LogLevel::Error, format, std::forward<Args>(args)...);
}
}
