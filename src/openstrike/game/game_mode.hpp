#pragma once

#include "openstrike/core/console.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
class ContentFileSystem;

struct GameModeKey
{
    int game_type = 0;
    int game_mode = 0;
};

struct GameModeDefinition
{
    int game_type = 0;
    int game_mode = 0;
    int max_players = 0;
    std::string_view internal_name;
    std::string_view display_name;
    std::string_view description;
    std::vector<std::string_view> aliases;
    std::vector<std::string_view> cfg_files;
};

struct SkirmishModeDefinition
{
    int id = 0;
    GameModeKey intended_mode;
    std::string_view internal_name;
    std::string_view display_name;
    std::string_view description;
    std::vector<std::string_view> cfg_files;
};

struct GameModeSelection
{
    GameModeKey key;
    int flags = 0;
    int skirmish_id = 0;
    const GameModeDefinition* mode = nullptr;
    const SkirmishModeDefinition* skirmish = nullptr;
};

void register_game_mode_variables(ConsoleVariables& variables);
void register_game_mode_commands(CommandRegistry& commands);

[[nodiscard]] const GameModeDefinition* find_game_mode(GameModeKey key);
[[nodiscard]] const GameModeDefinition* find_game_mode_by_alias(std::string_view alias);
[[nodiscard]] const SkirmishModeDefinition* find_skirmish_mode(int id);
[[nodiscard]] GameModeSelection current_game_mode_selection(const ConsoleVariables& variables);
[[nodiscard]] std::string current_game_mode_display_name(const ConsoleVariables& variables);
[[nodiscard]] std::vector<std::string> game_mode_cfg_files(const GameModeSelection& selection, bool offline_practice = false);

bool apply_game_mode_alias(ConsoleVariables& variables, std::string_view alias);
bool apply_game_mode_alias(
    ConsoleVariables& variables,
    std::string_view alias,
    const ContentFileSystem& filesystem,
    std::string_view map_name);
void execute_game_mode_cfgs(ConsoleCommandContext& context, bool offline_practice = false);
}
