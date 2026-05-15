#include "openstrike/core/command_line.hpp"

#include <algorithm>
#include <utility>

namespace openstrike
{
namespace
{
bool option_name_matches(std::string_view argument, std::string_view name)
{
    if (!argument.starts_with("--"))
    {
        return false;
    }

    argument.remove_prefix(2);
    const std::size_t equals = argument.find('=');
    const std::string_view key = argument.substr(0, equals);
    return key == name;
}
}

CommandLine::CommandLine(int argc, char** argv)
{
    arguments_.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index)
    {
        arguments_.emplace_back(argv[index]);
    }
}

CommandLine::CommandLine(std::vector<std::string> arguments)
    : arguments_(std::move(arguments))
{
}

bool CommandLine::has_flag(std::string_view name) const
{
    return std::any_of(arguments_.begin(), arguments_.end(), [name](const std::string& argument) {
        return argument == "--" + std::string(name) || option_name_matches(argument, name);
    });
}

std::optional<std::string> CommandLine::option(std::string_view name) const
{
    for (const std::string& argument : arguments_)
    {
        if (!option_name_matches(argument, name))
        {
            continue;
        }

        const std::size_t equals = argument.find('=');
        if (equals == std::string::npos)
        {
            return std::string{};
        }

        return argument.substr(equals + 1);
    }

    return std::nullopt;
}

std::vector<std::string> CommandLine::startup_commands() const
{
    std::vector<std::string> commands;

    for (std::size_t index = 0; index < arguments_.size(); ++index)
    {
        const std::string& argument = arguments_[index];
        if (argument.size() <= 1 || argument[0] != '+')
        {
            continue;
        }

        std::string command = argument.substr(1);
        while (index + 1 < arguments_.size())
        {
            const std::string& next = arguments_[index + 1];
            if (!next.empty() && (next[0] == '+' || next.starts_with("--")))
            {
                break;
            }

            command += ' ';
            command += next;
            ++index;
        }

        commands.push_back(std::move(command));
    }

    return commands;
}

const std::vector<std::string>& CommandLine::arguments() const
{
    return arguments_;
}
}
