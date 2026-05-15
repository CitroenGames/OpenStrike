#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
class CommandLine
{
public:
    CommandLine(int argc, char** argv);
    explicit CommandLine(std::vector<std::string> arguments);

    [[nodiscard]] bool has_flag(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> option(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> startup_commands() const;
    [[nodiscard]] const std::vector<std::string>& arguments() const;

private:
    std::vector<std::string> arguments_;
};
}
