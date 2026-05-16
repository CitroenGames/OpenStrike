#pragma once

#include <deque>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openstrike
{
class ContentFileSystem;
class NetworkSystem;
class WorldManager;
class AudioSystem;
class NavigationSystem;
class LoadingScreenState;

struct ConsoleVariable
{
    std::string name;
    std::string value;
    std::string default_value;
    std::string description;
    std::uint32_t flags = 0;
};

class ConsoleVariables
{
public:
    ConsoleVariable& register_variable(std::string name, std::string default_value, std::string description = {}, std::uint32_t flags = 0);
    [[nodiscard]] ConsoleVariable* find(std::string_view name);
    [[nodiscard]] const ConsoleVariable* find(std::string_view name) const;

    bool set(std::string_view name, std::string value);
    [[nodiscard]] std::string get_string(std::string_view name, std::string_view fallback = {}) const;
    [[nodiscard]] bool get_bool(std::string_view name, bool fallback = false) const;
    [[nodiscard]] int get_int(std::string_view name, int fallback = 0) const;
    [[nodiscard]] double get_double(std::string_view name, double fallback = 0.0) const;
    [[nodiscard]] std::vector<ConsoleVariable> variables() const;

private:
    std::unordered_map<std::string, ConsoleVariable> variables_;
};

struct CommandInvocation
{
    std::string name;
    std::vector<std::string> args;
    std::string original_line;
};

class CommandBuffer;
class CommandRegistry;

struct ConsoleCommandContext
{
    ConsoleVariables& variables;
    CommandBuffer& command_buffer;
    const CommandRegistry* registry = nullptr;
    ContentFileSystem* filesystem = nullptr;
    WorldManager* world = nullptr;
    NetworkSystem* network = nullptr;
    AudioSystem* audio = nullptr;
    NavigationSystem* navigation = nullptr;
    LoadingScreenState* loading_screen = nullptr;
    std::function<void()> request_quit;
};

using ConsoleCommandHandler = std::function<void(const CommandInvocation&, ConsoleCommandContext&)>;

struct ConsoleCommand
{
    std::string name;
    std::string description;
    ConsoleCommandHandler handler;
};

class CommandRegistry
{
public:
    void register_command(std::string name, std::string description, ConsoleCommandHandler handler);
    bool execute(const std::string& command_line, ConsoleCommandContext& context) const;
    [[nodiscard]] const ConsoleCommand* find(std::string_view name) const;
    [[nodiscard]] std::vector<ConsoleCommand> commands() const;

private:
    std::unordered_map<std::string, ConsoleCommand> commands_;
};

class CommandBuffer
{
public:
    void add_text(std::string_view text);
    void insert_text(std::string_view text);
    void clear();
    [[nodiscard]] bool empty() const;
    std::size_t execute(const CommandRegistry& registry, ConsoleCommandContext& context, std::size_t max_commands = 256);

private:
    std::deque<std::string> commands_;
};

[[nodiscard]] std::vector<std::string> tokenize_command_line(std::string_view command_line);
}
