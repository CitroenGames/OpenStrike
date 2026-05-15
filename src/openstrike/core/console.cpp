#include "openstrike/core/console.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>
#include <system_error>

namespace openstrike
{
namespace
{
std::string normalize_name(std::string_view name)
{
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized;
}

std::vector<std::string> split_commands(std::string_view text)
{
    std::vector<std::string> commands;
    std::string current;
    bool in_quotes = false;

    for (const char ch : text)
    {
        if (ch == '"')
        {
            in_quotes = !in_quotes;
            current.push_back(ch);
            continue;
        }

        if (!in_quotes && (ch == ';' || ch == '\n' || ch == '\r'))
        {
            if (!current.empty())
            {
                commands.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty())
    {
        commands.push_back(current);
    }

    return commands;
}

std::string join_args(const std::vector<std::string>& args, std::size_t first)
{
    std::string value;
    for (std::size_t index = first; index < args.size(); ++index)
    {
        if (!value.empty())
        {
            value += ' ';
        }
        value += args[index];
    }
    return value;
}
}

ConsoleVariable& ConsoleVariables::register_variable(std::string name, std::string default_value, std::string description, std::uint32_t flags)
{
    const std::string key = normalize_name(name);
    auto [it, inserted] = variables_.try_emplace(key);
    ConsoleVariable& variable = it->second;
    if (inserted)
    {
        variable.name = std::move(name);
        variable.value = default_value;
        variable.default_value = std::move(default_value);
        variable.description = std::move(description);
        variable.flags = flags;
    }
    return variable;
}

ConsoleVariable* ConsoleVariables::find(std::string_view name)
{
    const auto it = variables_.find(normalize_name(name));
    return it == variables_.end() ? nullptr : &it->second;
}

const ConsoleVariable* ConsoleVariables::find(std::string_view name) const
{
    const auto it = variables_.find(normalize_name(name));
    return it == variables_.end() ? nullptr : &it->second;
}

bool ConsoleVariables::set(std::string_view name, std::string value)
{
    ConsoleVariable* variable = find(name);
    if (variable == nullptr)
    {
        return false;
    }

    variable->value = std::move(value);
    return true;
}

std::string ConsoleVariables::get_string(std::string_view name, std::string_view fallback) const
{
    if (const ConsoleVariable* variable = find(name))
    {
        return variable->value;
    }
    return std::string(fallback);
}

bool ConsoleVariables::get_bool(std::string_view name, bool fallback) const
{
    const ConsoleVariable* variable = find(name);
    if (variable == nullptr)
    {
        return fallback;
    }

    const std::string normalized = normalize_name(variable->value);
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

int ConsoleVariables::get_int(std::string_view name, int fallback) const
{
    const ConsoleVariable* variable = find(name);
    if (variable == nullptr)
    {
        return fallback;
    }

    int value = 0;
    const auto result = std::from_chars(variable->value.data(), variable->value.data() + variable->value.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

double ConsoleVariables::get_double(std::string_view name, double fallback) const
{
    const ConsoleVariable* variable = find(name);
    if (variable == nullptr)
    {
        return fallback;
    }

    double value = 0.0;
    const auto result = std::from_chars(variable->value.data(), variable->value.data() + variable->value.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

std::vector<ConsoleVariable> ConsoleVariables::variables() const
{
    std::vector<ConsoleVariable> result;
    result.reserve(variables_.size());
    for (const auto& [name, variable] : variables_)
    {
        (void)name;
        result.push_back(variable);
    }
    std::sort(result.begin(), result.end(), [](const ConsoleVariable& lhs, const ConsoleVariable& rhs) {
        return lhs.name < rhs.name;
    });
    return result;
}

void CommandRegistry::register_command(std::string name, std::string description, ConsoleCommandHandler handler)
{
    const std::string key = normalize_name(name);
    commands_[key] = ConsoleCommand{
        .name = std::move(name),
        .description = std::move(description),
        .handler = std::move(handler),
    };
}

bool CommandRegistry::execute(const std::string& command_line, ConsoleCommandContext& context) const
{
    const std::vector<std::string> tokens = tokenize_command_line(command_line);
    if (tokens.empty())
    {
        return true;
    }

    CommandInvocation invocation{
        .name = tokens.front(),
        .args = std::vector<std::string>(tokens.begin() + 1, tokens.end()),
        .original_line = command_line,
    };

    if (const ConsoleCommand* command = find(invocation.name))
    {
        command->handler(invocation, context);
        return true;
    }

    if (ConsoleVariable* variable = context.variables.find(invocation.name))
    {
        if (invocation.args.empty())
        {
            log_info("{} = \"{}\"", variable->name, variable->value);
        }
        else
        {
            variable->value = join_args(invocation.args, 0);
            log_info("{} set to \"{}\"", variable->name, variable->value);
        }
        return true;
    }

    log_warning("unknown command '{}'", invocation.name);
    return false;
}

const ConsoleCommand* CommandRegistry::find(std::string_view name) const
{
    const auto it = commands_.find(normalize_name(name));
    return it == commands_.end() ? nullptr : &it->second;
}

std::vector<ConsoleCommand> CommandRegistry::commands() const
{
    std::vector<ConsoleCommand> result;
    result.reserve(commands_.size());
    for (const auto& [name, command] : commands_)
    {
        (void)name;
        result.push_back(command);
    }
    std::sort(result.begin(), result.end(), [](const ConsoleCommand& lhs, const ConsoleCommand& rhs) {
        return lhs.name < rhs.name;
    });
    return result;
}

void CommandBuffer::add_text(std::string_view text)
{
    for (std::string& command : split_commands(text))
    {
        commands_.push_back(std::move(command));
    }
}

void CommandBuffer::insert_text(std::string_view text)
{
    std::vector<std::string> commands = split_commands(text);
    for (auto it = commands.rbegin(); it != commands.rend(); ++it)
    {
        commands_.push_front(std::move(*it));
    }
}

void CommandBuffer::clear()
{
    commands_.clear();
}

bool CommandBuffer::empty() const
{
    return commands_.empty();
}

std::size_t CommandBuffer::execute(const CommandRegistry& registry, ConsoleCommandContext& context, std::size_t max_commands)
{
    std::size_t executed = 0;
    while (!commands_.empty() && executed < max_commands)
    {
        std::string command = std::move(commands_.front());
        commands_.pop_front();
        registry.execute(command, context);
        ++executed;
    }
    return executed;
}

std::vector<std::string> tokenize_command_line(std::string_view command_line)
{
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;

    for (const char ch : command_line)
    {
        if (ch == '"')
        {
            in_quotes = !in_quotes;
            continue;
        }

        if (!in_quotes && std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            if (!current.empty())
            {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty())
    {
        tokens.push_back(current);
    }

    return tokens;
}
}
