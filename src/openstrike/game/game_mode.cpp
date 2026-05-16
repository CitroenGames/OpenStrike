#include "openstrike/game/game_mode.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace openstrike
{
namespace
{
constexpr int kGameModeFlagTmm = 4;
constexpr int kGameModeFlagShort = 32;

std::string lower_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string trim_copy(std::string_view text)
{
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0)
    {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0)
    {
        --last;
    }

    return std::string(text.substr(first, last - first));
}

bool equals_icase(std::string_view lhs, std::string_view rhs)
{
    return lower_copy(lhs) == lower_copy(rhs);
}

std::optional<int> parse_int(std::string_view text)
{
    const std::string trimmed = trim_copy(text);
    int value = 0;
    const auto* begin = trimmed.data();
    const auto* end = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return std::nullopt;
    }
    return value;
}

std::string cfg_stem(std::string_view cfg_file)
{
    std::string stem(cfg_file);
    if (stem.size() >= 4 && equals_icase(std::string_view(stem).substr(stem.size() - 4), ".cfg"))
    {
        stem.resize(stem.size() - 4);
    }
    return stem;
}

std::vector<std::string> tokenize_keyvalues(std::string_view text)
{
    std::vector<std::string> tokens;
    std::size_t cursor = 0;
    while (cursor < text.size())
    {
        const char ch = text[cursor];
        if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            ++cursor;
            continue;
        }

        if (ch == '/' && cursor + 1 < text.size() && text[cursor + 1] == '/')
        {
            cursor += 2;
            while (cursor < text.size() && text[cursor] != '\n' && text[cursor] != '\r')
            {
                ++cursor;
            }
            continue;
        }

        if (ch == '#')
        {
            while (cursor < text.size() && text[cursor] != '\n' && text[cursor] != '\r')
            {
                ++cursor;
            }
            continue;
        }

        if (ch == '{' || ch == '}')
        {
            tokens.emplace_back(1, ch);
            ++cursor;
            continue;
        }

        if (ch == '"')
        {
            ++cursor;
            std::string quoted;
            while (cursor < text.size())
            {
                const char current = text[cursor++];
                if (current == '"')
                {
                    break;
                }
                quoted.push_back(current);
            }
            tokens.push_back(std::move(quoted));
            continue;
        }

        std::string token;
        while (cursor < text.size())
        {
            const char current = text[cursor];
            if (std::isspace(static_cast<unsigned char>(current)) != 0 || current == '{' || current == '}' || current == '"')
            {
                break;
            }
            token.push_back(current);
            ++cursor;
        }
        if (!token.empty())
        {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

std::optional<std::pair<std::size_t, std::size_t>> object_after_key(
    const std::vector<std::string>& tokens, std::string_view key, std::size_t begin = 0, std::size_t end = std::string::npos)
{
    end = std::min(end, tokens.size());
    for (std::size_t index = begin; index + 1 < end; ++index)
    {
        if (!equals_icase(tokens[index], key) || tokens[index + 1] != "{")
        {
            continue;
        }

        int depth = 1;
        for (std::size_t close = index + 2; close < end; ++close)
        {
            if (tokens[close] == "{")
            {
                ++depth;
            }
            else if (tokens[close] == "}")
            {
                --depth;
                if (depth == 0)
                {
                    return std::pair{index + 2, close};
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<GameModeKey> game_mode_key_from_tokens(
    const std::vector<std::string>& tokens, std::size_t begin = 0, std::size_t end = std::string::npos)
{
    end = std::min(end, tokens.size());
    std::optional<int> game_type;
    std::optional<int> game_mode;

    for (std::size_t index = begin; index + 1 < end; ++index)
    {
        if (equals_icase(tokens[index], "default_game_type") || equals_icase(tokens[index], "game_type"))
        {
            game_type = parse_int(tokens[index + 1]);
        }
        else if (equals_icase(tokens[index], "default_game_mode") || equals_icase(tokens[index], "game_mode"))
        {
            game_mode = parse_int(tokens[index + 1]);
        }
    }

    if (!game_type || !game_mode)
    {
        return std::nullopt;
    }

    return GameModeKey{.game_type = *game_type, .game_mode = *game_mode};
}

std::optional<GameModeKey> game_mode_key_from_gamemodes_text(std::string_view text, std::string_view map_name)
{
    const std::vector<std::string> tokens = tokenize_keyvalues(text);
    const std::optional<std::pair<std::size_t, std::size_t>> maps = object_after_key(tokens, "maps");
    if (!maps)
    {
        return std::nullopt;
    }

    const std::optional<std::pair<std::size_t, std::size_t>> map = object_after_key(tokens, map_name, maps->first, maps->second);
    if (!map)
    {
        return std::nullopt;
    }

    return game_mode_key_from_tokens(tokens, map->first, map->second);
}

std::optional<GameModeKey> game_mode_key_from_map_kv(std::string_view text)
{
    return game_mode_key_from_tokens(tokenize_keyvalues(text));
}

std::optional<std::string> read_optional_text(const ContentFileSystem& filesystem, const std::filesystem::path& path)
{
    try
    {
        if (!filesystem.exists(path, "GAME"))
        {
            return std::nullopt;
        }
        return filesystem.read_text(path, "GAME");
    }
    catch (const std::exception& error)
    {
        log_warning("failed reading '{}': {}", path.generic_string(), error.what());
    }
    return std::nullopt;
}

std::optional<GameModeKey> map_default_game_mode(
    const ContentFileSystem& filesystem, std::string_view map_name)
{
    const std::string map = trim_copy(map_name);
    if (map.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path map_kv = std::filesystem::path("maps") / (map + ".kv");
    if (const std::optional<std::string> text = read_optional_text(filesystem, map_kv))
    {
        if (const std::optional<GameModeKey> key = game_mode_key_from_map_kv(*text))
        {
            return key;
        }
    }

    std::optional<GameModeKey> result;
    if (const std::optional<std::string> text = read_optional_text(filesystem, "gamemodes.txt"))
    {
        result = game_mode_key_from_gamemodes_text(*text, map);
    }
    if (const std::optional<std::string> text = read_optional_text(filesystem, "gamemodes_server.txt"))
    {
        if (const std::optional<GameModeKey> server_key = game_mode_key_from_gamemodes_text(*text, map))
        {
            result = server_key;
        }
    }
    return result;
}

const std::vector<GameModeDefinition>& game_modes()
{
    static const std::vector<GameModeDefinition> modes{
        {0,
            0,
            20,
            "casual",
            "Casual",
            "Bomb defusal or hostage rescue with simplified economy and relaxed team rules.",
            {"casual"},
            {"gamemode_casual.cfg", "gamemode_casual_offline.cfg", "gamemode_casual_server.cfg"}},
        {0,
            1,
            10,
            "competitive",
            "Competitive",
            "Classic 5v5 Counter-Strike rules with full team economy.",
            {"competitive", "comp"},
            {"gamemode_competitive.cfg", "gamemode_competitive_offline.cfg", "gamemode_competitive_server.cfg"}},
        {0,
            2,
            4,
            "scrimcomp2v2",
            "Wingman",
            "Two-on-two competitive play on compact bombsite layouts.",
            {"scrimcomp2v2", "wingman"},
            {"gamemode_competitive2v2.cfg", "gamemode_competitive2v2_offline.cfg", "gamemode_competitive2v2_server.cfg"}},
        {0,
            3,
            10,
            "scrimcomp5v5",
            "Weapons Expert",
            "Competitive rules with weapon purchasing constraints.",
            {"scrimcomp5v5", "weapons_expert", "weaponsexpert"},
            {"gamemode_competitive.cfg", "gamemode_competitive_offline.cfg", "op08_weapons_expert.cfg", "gamemode_competitive_server.cfg"}},
        {0,
            4,
            1,
            "new_user_training",
            "Training Day",
            "New player onboarding and training flow.",
            {"new_user_training", "trainingday"},
            {"gamemode_new_user_training.cfg"}},
        {1,
            0,
            10,
            "armsrace",
            "Arms Race",
            "Respawn gun progression mode.",
            {"ar", "armsrace", "arms_race", "gungameprogressive", "gungame_progressive"},
            {"gamemode_armsrace.cfg", "gamemode_armsrace_server.cfg"}},
        {1,
            1,
            10,
            "demolition",
            "Demolition",
            "Round-based gun progression on bomb maps.",
            {"demolition", "demo", "gungametrbomb", "gungame_trbomb"},
            {"gamemode_demolition.cfg", "gamemode_demolition_server.cfg"}},
        {1,
            2,
            16,
            "deathmatch",
            "Deathmatch",
            "Timed respawn combat with score-based victory.",
            {"dm", "deathmatch"},
            {"gamemode_deathmatch.cfg", "gamemode_deathmatch_server.cfg"}},
        {2,
            0,
            1,
            "training",
            "Training",
            "Legacy training mode.",
            {"training"},
            {"gamemode_training.cfg"}},
        {3,
            0,
            100,
            "custom",
            "Custom",
            "Custom game mode loaded from map or server rules.",
            {"custom"},
            {"gamemode_custom.cfg", "gamemode_custom_server.cfg"}},
        {4,
            0,
            5,
            "guardian",
            "Guardian",
            "Cooperative holdout missions against bots.",
            {"guard", "guardian", "cooperative"},
            {"gamemode_competitive.cfg", "gamemode_cooperative.cfg", "gamemode_cooperative_server.cfg"}},
        {4,
            1,
            10,
            "coopmission",
            "Co-op Strike",
            "Scripted cooperative missions.",
            {"coop", "coopmission", "coopmision", "coopstrike", "coop_strike"},
            {"gamemode_competitive.cfg", "gamemode_coopmission.cfg", "gamemode_coopmission_server.cfg"}},
        {5,
            0,
            12,
            "skirmish",
            "War Games",
            "Variation slot for War Games and operation rulesets.",
            {"skirmish", "wargames", "war_games"},
            {"gamemode_skirmish.cfg", "gamemode_skirmish_server.cfg"}},
        {6,
            0,
            18,
            "survival",
            "Danger Zone",
            "Battle royale survival mode.",
            {"survival", "dangerzone", "danger_zone"},
            {"gamemode_survival.cfg", "gamemode_survival_server.cfg"}},
        {7,
            0,
            100,
            "workshop",
            "Workshop",
            "Workshop-scripted mode.",
            {"workshop"},
            {"gamemode_workshop.cfg", "gamemode_workshop_server.cfg"}},
    };
    return modes;
}

const std::vector<SkirmishModeDefinition>& skirmish_modes()
{
    static const std::vector<SkirmishModeDefinition> modes{
        {0, {0, 0}, "", "None", "No additional War Games rules.", {}},
        {1, {0, 0}, "stabstabzap", "Stab Stab Zap", "Knives, recharging Zeus, and grenades.", {"op08_stab_stab_zap.cfg"}},
        {2, {1, 2}, "dm_freeforall", "Free For All", "Deathmatch where every player is an enemy.", {"gamemode_dm_freeforall.cfg"}},
        {3, {0, 0}, "flyingscoutsman", "Flying Scoutsman", "Scouts, knives, low gravity, and high precision.", {"op08_flying_scoutsman.cfg"}},
        {4, {0, 0}, "triggerdiscipline", "Trigger Discipline", "Missed shots hurt the shooter.", {"op08_trigger_discipline.cfg"}},
        {6, {1, 2}, "headshots", "Boom! Headshot!", "Only headshots deal real damage.", {"op08_headshots.cfg"}},
        {7, {1, 2}, "huntergatherers", "Hunter-Gatherers", "Team deathmatch with dropped dogtags.", {"op08_hunter_gatherers.cfg"}},
        {8, {0, 0}, "heavyassaultsuit", "Heavy Assault Suit", "Casual rules with purchasable heavy armor.", {"op08_heavy_assault_suit.cfg"}},
        {10, {1, 0}, "armsrace", "Arms Race", "Equivalent to Arms Race.", {}},
        {11, {1, 1}, "demolition", "Demolition", "Equivalent to Demolition.", {}},
        {12, {0, 0}, "retakes", "Retakes", "Card-based retake rounds on bomb sites.", {"gamemode_retakecasual.cfg"}},
    };
    return modes;
}

bool is_auto_alias(std::string_view alias)
{
    const std::string normalized = lower_copy(trim_copy(alias));
    return normalized == "default" || normalized == "auto";
}

void apply_game_mode_key(ConsoleVariables& variables, GameModeKey key)
{
    variables.set("game_type", std::to_string(key.game_type));
    variables.set("game_mode", std::to_string(key.game_mode));
}

std::string flagged_display_name(const GameModeDefinition& mode, int flags)
{
    const bool short_flag = (flags & kGameModeFlagShort) != 0;
    const bool tmm_flag = (flags & kGameModeFlagTmm) != 0;

    if (equals_icase(mode.internal_name, "competitive") && short_flag)
    {
        return "Short Competitive";
    }
    if (equals_icase(mode.internal_name, "deathmatch"))
    {
        if (short_flag)
        {
            return "Free For All Deathmatch";
        }
        if (tmm_flag)
        {
            return "Team Deathmatch";
        }
    }
    if (tmm_flag)
    {
        return std::string(mode.display_name) + " Team vs Team";
    }
    return std::string(mode.display_name);
}

bool should_execute_offline_cfg(std::string_view cfg_file, bool offline_practice)
{
    if (offline_practice)
    {
        return true;
    }
    return lower_copy(cfg_file).find("_offline.cfg") == std::string::npos;
}

void append_unique(std::vector<std::string>& result, std::string value)
{
    if (std::find(result.begin(), result.end(), value) == result.end())
    {
        result.push_back(std::move(value));
    }
}
}

void register_game_mode_variables(ConsoleVariables& variables)
{
    variables.register_variable("game_type", "0", "Counter-Strike game type selector.");
    variables.register_variable("game_mode", "1", "Counter-Strike game mode selector inside game_type.");
    variables.register_variable("sv_game_mode_flags", "0", "Counter-Strike game mode variation flags, such as tmm and short.");
    variables.register_variable("sv_skirmish_id", "0", "Counter-Strike War Games skirmish variation id.");
    variables.register_variable("maxplayers", "10", "Current game mode player capacity.");

    variables.register_variable("mp_maxrounds", "0", "Maximum rounds before a level change.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_roundtime", "5", "Round time in minutes.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_roundtime_defuse", "0", "Bomb defusal round time in minutes.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_freezetime", "6", "Freeze time before a round starts.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_friendlyfire", "0", "Whether teammates can damage each other.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_teammates_are_enemies", "0", "Whether all players are enemies.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_limitteams", "2", "Team imbalance limit.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_autoteambalance", "1", "Automatically rebalance teams.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_restartgame", "0", "Restart the current game after the specified delay.", ConsoleVariableFlagGameRule);
    variables.register_variable("bot_quota", "0", "Desired bot count.", ConsoleVariableFlagGameRule);
    variables.register_variable("bot_quota_mode", "normal", "Bot quota fill mode.", ConsoleVariableFlagGameRule);
    variables.register_variable("sv_deadtalk", "0", "Allow dead players to talk to living players.", ConsoleVariableFlagGameRule);
}

void register_game_mode_commands(CommandRegistry& commands)
{
    commands.register_command("game_alias", "Sets game_type and game_mode from a Counter-Strike game mode alias.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (invocation.args.empty())
        {
            log_warning("usage: game_alias <alias>");
            return;
        }

        if (!apply_game_mode_alias(context.variables, invocation.args[0]))
        {
            log_warning("unknown game mode alias '{}'", invocation.args[0]);
        }
    });

    commands.register_command("game_mode_status", "Prints the active Counter-Strike game mode cvar selection.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        const GameModeSelection selection = current_game_mode_selection(context.variables);
        log_info("game_type={} game_mode={} sv_game_mode_flags={} sv_skirmish_id={} ({})",
            selection.key.game_type,
            selection.key.game_mode,
            selection.flags,
            selection.skirmish_id,
            current_game_mode_display_name(context.variables));
    });

    commands.register_command("game_modes", "Lists known Counter-Strike game_type/game_mode combinations.", [](const CommandInvocation&, ConsoleCommandContext&) {
        for (const GameModeDefinition& mode : game_modes())
        {
            log_info("game_type={} game_mode={} {:<18} maxplayers={} aliases={}",
                mode.game_type,
                mode.game_mode,
                mode.display_name,
                mode.max_players,
                mode.aliases.empty() ? "" : mode.aliases.front());
        }
    });
}

const GameModeDefinition* find_game_mode(GameModeKey key)
{
    const auto& modes = game_modes();
    const auto it = std::find_if(modes.begin(), modes.end(), [&](const GameModeDefinition& mode) {
        return mode.game_type == key.game_type && mode.game_mode == key.game_mode;
    });
    return it == modes.end() ? nullptr : &*it;
}

const GameModeDefinition* find_game_mode_by_alias(std::string_view alias)
{
    const std::string normalized = lower_copy(trim_copy(alias));
    if (normalized.empty())
    {
        return nullptr;
    }

    for (const GameModeDefinition& mode : game_modes())
    {
        for (std::string_view mode_alias : mode.aliases)
        {
            if (equals_icase(mode_alias, normalized))
            {
                return &mode;
            }
        }
    }
    return nullptr;
}

const SkirmishModeDefinition* find_skirmish_mode(int id)
{
    const auto& modes = skirmish_modes();
    const auto it = std::find_if(modes.begin(), modes.end(), [&](const SkirmishModeDefinition& mode) {
        return mode.id == id;
    });
    return it == modes.end() ? nullptr : &*it;
}

GameModeSelection current_game_mode_selection(const ConsoleVariables& variables)
{
    GameModeSelection selection;
    selection.key.game_type = variables.get_int("game_type", 0);
    selection.key.game_mode = variables.get_int("game_mode", 1);
    selection.flags = variables.get_int("sv_game_mode_flags", 0);
    selection.skirmish_id = variables.get_int("sv_skirmish_id", 0);
    selection.mode = find_game_mode(selection.key);
    selection.skirmish = find_skirmish_mode(selection.skirmish_id);
    return selection;
}

std::string current_game_mode_display_name(const ConsoleVariables& variables)
{
    const GameModeSelection selection = current_game_mode_selection(variables);
    if (selection.skirmish != nullptr && selection.skirmish->id != 0)
    {
        return std::string(selection.skirmish->display_name);
    }
    if (selection.mode != nullptr)
    {
        return flagged_display_name(*selection.mode, selection.flags);
    }
    return "Custom";
}

std::vector<std::string> game_mode_cfg_files(const GameModeSelection& selection, bool offline_practice)
{
    std::vector<std::string> result;
    if (selection.mode != nullptr)
    {
        for (std::string_view cfg : selection.mode->cfg_files)
        {
            if (should_execute_offline_cfg(cfg, offline_practice))
            {
                append_unique(result, std::string(cfg));
            }
        }

        if (!equals_icase(selection.mode->internal_name, "custom"))
        {
            const std::string stem = cfg_stem("gamemode_" + std::string(selection.mode->internal_name));
            if ((selection.flags & kGameModeFlagShort) != 0)
            {
                append_unique(result, stem + "_short.cfg");
            }
            if ((selection.flags & kGameModeFlagTmm) != 0)
            {
                append_unique(result, stem + "_tmm.cfg");
            }
        }
    }

    if (selection.skirmish != nullptr && selection.skirmish->id != 0)
    {
        for (std::string_view cfg : selection.skirmish->cfg_files)
        {
            append_unique(result, std::string(cfg));
        }
    }

    return result;
}

bool apply_game_mode_alias(ConsoleVariables& variables, std::string_view alias)
{
    if (is_auto_alias(alias))
    {
        return false;
    }

    const GameModeDefinition* mode = find_game_mode_by_alias(alias);
    if (mode == nullptr)
    {
        return false;
    }

    apply_game_mode_key(variables, GameModeKey{.game_type = mode->game_type, .game_mode = mode->game_mode});
    return true;
}

bool apply_game_mode_alias(
    ConsoleVariables& variables,
    std::string_view alias,
    const ContentFileSystem& filesystem,
    std::string_view map_name)
{
    if (!is_auto_alias(alias))
    {
        return apply_game_mode_alias(variables, alias);
    }

    if (const std::optional<GameModeKey> key = map_default_game_mode(filesystem, map_name))
    {
        apply_game_mode_key(variables, *key);
    }
    return true;
}

void execute_game_mode_cfgs(ConsoleCommandContext& context, bool offline_practice)
{
    context.variables.reset_with_flags(ConsoleVariableFlagGameRule);

    GameModeSelection selection = current_game_mode_selection(context.variables);
    if (selection.mode != nullptr)
    {
        context.variables.set("maxplayers", std::to_string(selection.mode->max_players));
    }

    if (context.filesystem == nullptr || context.registry == nullptr)
    {
        return;
    }

    for (const std::string& cfg_file : game_mode_cfg_files(selection, offline_practice))
    {
        const std::filesystem::path cfg_path = std::filesystem::path("cfg") / cfg_file;
        if (!context.filesystem->exists(cfg_path, "GAME"))
        {
            continue;
        }

        try
        {
            CommandBuffer cfg_buffer;
            cfg_buffer.add_text(context.filesystem->read_text(cfg_path, "GAME"));
            log_info("exec '{}'", cfg_path.generic_string());
            cfg_buffer.execute(*context.registry, context);
        }
        catch (const std::exception& error)
        {
            log_warning("game mode cfg '{}' failed: {}", cfg_path.generic_string(), error.what());
        }
    }
}
}
