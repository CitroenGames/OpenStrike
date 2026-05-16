#include "openstrike/game/team_system.hpp"

#include "openstrike/core/console.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/network/network_session.hpp"
#include "openstrike/network/network_stream.hpp"
#include "openstrike/network/network_system.hpp"
#include "openstrike/world/world.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kTeamUserCommandMagic = 0x554D5453U; // STMU
constexpr std::uint32_t kTeamSnapshotMagic = 0x534D5453U;    // STMS
constexpr std::uint16_t kTeamPayloadVersion = 1;
constexpr std::uint16_t kMaxTeamSnapshotTeams = 4;
constexpr std::uint16_t kMaxTeamSnapshotPlayers = 64;

std::string lower_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::optional<int> parse_int(std::string_view text)
{
    int value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return std::nullopt;
    }
    return value;
}

void write_i32(NetworkByteWriter& writer, int value)
{
    writer.write_u32(static_cast<std::uint32_t>(value));
}

bool read_i32(NetworkByteReader& reader, int& value)
{
    std::uint32_t raw = 0;
    if (!reader.read_u32(raw))
    {
        return false;
    }
    value = static_cast<int>(raw);
    return true;
}

void write_bool(NetworkByteWriter& writer, bool value)
{
    writer.write_u8(value ? 1 : 0);
}

bool read_bool(NetworkByteReader& reader, bool& value)
{
    std::uint8_t raw = 0;
    if (!reader.read_u8(raw))
    {
        return false;
    }
    if (raw > 1)
    {
        return false;
    }
    value = raw != 0;
    return true;
}

bool write_count(NetworkByteWriter& writer, std::size_t count)
{
    if (count > std::numeric_limits<std::uint16_t>::max())
    {
        return false;
    }
    writer.write_u16(static_cast<std::uint16_t>(count));
    return true;
}

int human_team_from_cvar(std::string_view value)
{
    const std::string normalized = lower_copy(value);
    if (normalized == "ct" || normalized == "cts" || normalized == "counterterrorist" || normalized == "counter-terrorist")
    {
        return TEAM_CT;
    }
    if (normalized == "t" || normalized == "ts" || normalized == "terrorist" || normalized == "terrorists")
    {
        return TEAM_TERRORIST;
    }
    return TEAM_UNASSIGNED;
}

bool team_matches_spawn(const WorldSpawnPoint& spawn, int team)
{
    if (spawn.team_id == team)
    {
        return true;
    }
    if (team == TEAM_TERRORIST && spawn.class_name == "info_player_terrorist")
    {
        return true;
    }
    if (team == TEAM_CT && spawn.class_name == "info_player_counterterrorist")
    {
        return true;
    }
    return false;
}

bool general_spawn(const WorldSpawnPoint& spawn)
{
    return spawn.team_id == TEAM_UNASSIGNED || spawn.class_name == "info_player_start" ||
           spawn.class_name == "info_player_deathmatch";
}

bool valid_join_failure(std::uint8_t failure)
{
    switch (static_cast<TeamJoinFailedReason>(failure))
    {
    case TeamJoinFailedReason::ChangedTooOften:
    case TeamJoinFailedReason::BothTeamsFull:
    case TeamJoinFailedReason::TerroristsFull:
    case TeamJoinFailedReason::CtsFull:
    case TeamJoinFailedReason::CannotJoinSpectator:
    case TeamJoinFailedReason::HumansCanOnlyJoinTs:
    case TeamJoinFailedReason::HumansCanOnlyJoinCts:
    case TeamJoinFailedReason::TooManyTs:
    case TeamJoinFailedReason::TooManyCts:
    case TeamJoinFailedReason::None:
        return true;
    }
    return false;
}

bool remote_client_only(const ConsoleCommandContext& context)
{
    return context.network != nullptr && context.network->client().is_connected() && !context.network->server().is_running();
}

std::uint32_t command_team_player_id(const ConsoleCommandContext& context)
{
    if (context.network != nullptr)
    {
        return local_team_connection_id(*context.network);
    }
    return kLocalTeamPlayerId;
}

void broadcast_context_team_snapshot(const ConsoleCommandContext& context)
{
    if (context.network == nullptr || context.teams == nullptr || !context.network->server().is_running())
    {
        return;
    }

    const std::vector<unsigned char> payload = make_team_snapshot_payload(context.teams->make_snapshot());
    context.network->broadcast_server_snapshot(payload, 0);
}

void execute_or_send_join_game(ConsoleCommandContext& context)
{
    if (remote_client_only(context))
    {
        const std::vector<unsigned char> payload =
            make_team_user_command_payload(TeamUserCommand{.kind = TeamUserCommandKind::JoinGame});
        context.network->send_client_user_command(payload, 0);
        return;
    }

    if (context.teams != nullptr)
    {
        context.teams->mark_join_game(command_team_player_id(context));
    }
}

void execute_or_send_join_team(ConsoleCommandContext& context, int requested_team, bool force)
{
    if (remote_client_only(context))
    {
        if (context.teams != nullptr)
        {
            context.teams->mark_join_request_pending(local_team_connection_id(*context.network), requested_team);
        }
        const std::vector<unsigned char> payload = make_team_user_command_payload(TeamUserCommand{
            .kind = TeamUserCommandKind::JoinTeam,
            .requested_team = requested_team,
            .force = force,
        });
        context.network->send_client_user_command(payload, 0);
        return;
    }

    if (context.teams == nullptr)
    {
        return;
    }

    const std::uint32_t player_id = command_team_player_id(context);
    const LoadedWorld* world = context.world != nullptr ? context.world->current_world() : nullptr;
    const TeamJoinRules rules = team_join_rules_from_context(context.variables, world);
    const TeamJoinResult result = context.teams->try_join_team(player_id, requested_team, force, rules);
    if (!result.accepted)
    {
        log_warning("jointeam failed: {}", to_string(result.reason));
    }
    broadcast_context_team_snapshot(context);
}
}

bool is_valid_team_id(int team)
{
    return team >= TEAM_UNASSIGNED && team <= TEAM_CT;
}

bool is_playing_team(int team)
{
    return team == TEAM_TERRORIST || team == TEAM_CT;
}

bool is_joinable_team_id(int team)
{
    return team == TEAM_UNASSIGNED || team == TEAM_SPECTATOR || is_playing_team(team);
}

std::string_view team_name(int team)
{
    switch (team)
    {
    case TEAM_UNASSIGNED:
        return "Unassigned";
    case TEAM_SPECTATOR:
        return "Spectator";
    case TEAM_TERRORIST:
        return "Terrorists";
    case TEAM_CT:
        return "Counter-Terrorists";
    default:
        return "Unknown";
    }
}

std::string_view team_short_name(int team)
{
    switch (team)
    {
    case TEAM_UNASSIGNED:
        return "UNASSIGNED";
    case TEAM_SPECTATOR:
        return "SPEC";
    case TEAM_TERRORIST:
        return "T";
    case TEAM_CT:
        return "CT";
    default:
        return "UNKNOWN";
    }
}

TeamColor team_color(int team)
{
    switch (team)
    {
    case TEAM_TERRORIST:
        return TeamColor{204, 148, 67};
    case TEAM_CT:
        return TeamColor{91, 140, 212};
    case TEAM_SPECTATOR:
        return TeamColor{160, 160, 170};
    default:
        return TeamColor{210, 210, 210};
    }
}

std::string_view to_string(TeamJoinFailedReason reason)
{
    switch (reason)
    {
    case TeamJoinFailedReason::ChangedTooOften:
        return "changed too often";
    case TeamJoinFailedReason::BothTeamsFull:
        return "both teams full";
    case TeamJoinFailedReason::TerroristsFull:
        return "terrorists full";
    case TeamJoinFailedReason::CtsFull:
        return "counter-terrorists full";
    case TeamJoinFailedReason::CannotJoinSpectator:
        return "cannot join spectator";
    case TeamJoinFailedReason::HumansCanOnlyJoinTs:
        return "humans can only join Terrorists";
    case TeamJoinFailedReason::HumansCanOnlyJoinCts:
        return "humans can only join Counter-Terrorists";
    case TeamJoinFailedReason::TooManyTs:
        return "too many Terrorists";
    case TeamJoinFailedReason::TooManyCts:
        return "too many Counter-Terrorists";
    case TeamJoinFailedReason::None:
        return "none";
    }
    return "unknown";
}

std::uint32_t local_team_connection_id(const NetworkSystem& network)
{
    return network.client().is_connected() ? network.client().connection_id() : kLocalTeamPlayerId;
}

std::vector<unsigned char> make_team_user_command_payload(const TeamUserCommand& command)
{
    NetworkByteWriter writer;
    writer.write_u32(kTeamUserCommandMagic);
    writer.write_u16(kTeamPayloadVersion);
    writer.write_u8(static_cast<std::uint8_t>(command.kind));
    write_i32(writer, command.requested_team);
    write_bool(writer, command.force);
    return writer.take_bytes();
}

std::optional<TeamUserCommand> read_team_user_command_payload(std::span<const unsigned char> payload)
{
    NetworkByteReader reader(payload);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t kind = 0;
    TeamUserCommand command;
    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u8(kind) ||
        !read_i32(reader, command.requested_team) || !read_bool(reader, command.force) || !reader.empty())
    {
        return std::nullopt;
    }
    if (magic != kTeamUserCommandMagic || version != kTeamPayloadVersion)
    {
        return std::nullopt;
    }
    if (kind != static_cast<std::uint8_t>(TeamUserCommandKind::JoinGame) &&
        kind != static_cast<std::uint8_t>(TeamUserCommandKind::JoinTeam))
    {
        return std::nullopt;
    }
    command.kind = static_cast<TeamUserCommandKind>(kind);
    return command;
}

std::vector<unsigned char> make_team_snapshot_payload(const TeamSnapshot& snapshot)
{
    NetworkByteWriter writer;
    writer.write_u32(kTeamSnapshotMagic);
    writer.write_u16(kTeamPayloadVersion);
    if (!write_count(writer, snapshot.teams.size()))
    {
        return {};
    }
    for (const TeamInfo& team : snapshot.teams)
    {
        write_i32(writer, team.id);
        write_i32(writer, team.score_total);
        write_i32(writer, team.score_first_half);
        write_i32(writer, team.score_second_half);
        write_i32(writer, team.score_overtime);
        write_bool(writer, team.surrendered);
        write_i32(writer, static_cast<int>(team.members.size()));
        write_i32(writer, team.alive_count);
    }

    if (!write_count(writer, snapshot.players.size()))
    {
        return {};
    }
    for (const TeamPlayerState& player : snapshot.players)
    {
        writer.write_u32(player.connection_id);
        write_i32(writer, player.current_team);
        write_i32(writer, player.pending_team);
        write_i32(writer, player.last_team);
        write_bool(writer, player.alive);
        write_bool(writer, player.joined_game);
        write_i32(writer, player.kills);
        write_i32(writer, player.deaths);
        write_i32(writer, player.ping_ms);
        writer.write_u8(static_cast<std::uint8_t>(player.last_join_failure));
    }
    return writer.take_bytes();
}

std::optional<TeamSnapshot> read_team_snapshot_payload(std::span<const unsigned char> payload)
{
    NetworkByteReader reader(payload);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t team_count = 0;
    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u16(team_count))
    {
        return std::nullopt;
    }
    if (magic != kTeamSnapshotMagic || version != kTeamPayloadVersion || team_count > kMaxTeamSnapshotTeams)
    {
        return std::nullopt;
    }

    TeamSnapshot snapshot;
    snapshot.teams.reserve(team_count);
    for (std::uint16_t index = 0; index < team_count; ++index)
    {
        TeamInfo team;
        int member_count = 0;
        if (!read_i32(reader, team.id) || !read_i32(reader, team.score_total) ||
            !read_i32(reader, team.score_first_half) || !read_i32(reader, team.score_second_half) ||
            !read_i32(reader, team.score_overtime) || !read_bool(reader, team.surrendered) ||
            !read_i32(reader, member_count) || !read_i32(reader, team.alive_count))
        {
            return std::nullopt;
        }
        if (!is_valid_team_id(team.id) || member_count < 0 || member_count > kMaxTeamSnapshotPlayers ||
            team.alive_count < 0 || team.alive_count > member_count)
        {
            return std::nullopt;
        }
        team.name = std::string(team_name(team.id));
        snapshot.teams.push_back(std::move(team));
    }

    std::uint16_t player_count = 0;
    if (!reader.read_u16(player_count))
    {
        return std::nullopt;
    }
    if (player_count > kMaxTeamSnapshotPlayers)
    {
        return std::nullopt;
    }
    snapshot.players.reserve(player_count);
    for (std::uint16_t index = 0; index < player_count; ++index)
    {
        TeamPlayerState player;
        std::uint8_t failure = 0;
        if (!reader.read_u32(player.connection_id) || !read_i32(reader, player.current_team) ||
            !read_i32(reader, player.pending_team) || !read_i32(reader, player.last_team) ||
            !read_bool(reader, player.alive) || !read_bool(reader, player.joined_game) ||
            !read_i32(reader, player.kills) || !read_i32(reader, player.deaths) ||
            !read_i32(reader, player.ping_ms) || !reader.read_u8(failure))
        {
            return std::nullopt;
        }
        if (!is_joinable_team_id(player.current_team) || !is_joinable_team_id(player.pending_team) ||
            !is_joinable_team_id(player.last_team) || !valid_join_failure(failure))
        {
            return std::nullopt;
        }
        if (player.alive && !is_playing_team(player.current_team))
        {
            return std::nullopt;
        }
        player.last_join_failure = static_cast<TeamJoinFailedReason>(failure);
        player.team_menu_requested = player.current_team == TEAM_UNASSIGNED ||
                                     player.last_join_failure != TeamJoinFailedReason::None;
        snapshot.players.push_back(player);
    }

    if (!reader.empty())
    {
        return std::nullopt;
    }
    return snapshot;
}

TeamJoinRules team_join_rules_from_context(const ConsoleVariables& variables, const LoadedWorld* world)
{
    TeamJoinRules rules;
    rules.max_players = std::max(1, variables.get_int("maxplayers", 10));
    rules.limit_teams = std::max(0, variables.get_int("mp_limitteams", 2));
    rules.spectator_slots = rules.max_players;
    rules.allow_spectators = variables.get_bool("mp_allowspectators", true);
    rules.human_team = variables.get_string("mp_humanteam", "any");

    if (world != nullptr)
    {
        for (const WorldSpawnPoint& spawn : world->spawn_points)
        {
            if (team_matches_spawn(spawn, TEAM_TERRORIST))
            {
                ++rules.terrorist_spawn_count;
            }
            else if (team_matches_spawn(spawn, TEAM_CT))
            {
                ++rules.ct_spawn_count;
            }
            else if (general_spawn(spawn))
            {
                ++rules.general_spawn_count;
            }
        }
    }

    return rules;
}

TeamManager::TeamManager()
{
    reset();
}

void TeamManager::reset()
{
    initialize_teams();
    players_.clear();
    spawn_cursors_ = {};
    auto_assign_counter_ = 0;
    bump_revision();
}

void TeamManager::reset_for_new_world()
{
    for (TeamPlayerState& player : players_)
    {
        player.last_team = player.current_team;
        player.current_team = TEAM_UNASSIGNED;
        player.pending_team = TEAM_UNASSIGNED;
        player.alive = false;
        player.joined_game = false;
        player.team_menu_requested = true;
        player.join_request_pending = false;
        player.last_join_failure = TeamJoinFailedReason::None;
    }
    spawn_cursors_ = {};
    rebuild_membership();
    bump_revision();
}

TeamInfo* TeamManager::find_team(int team)
{
    return is_valid_team_id(team) ? &teams_[static_cast<std::size_t>(team)] : nullptr;
}

const TeamInfo* TeamManager::find_team(int team) const
{
    return is_valid_team_id(team) ? &teams_[static_cast<std::size_t>(team)] : nullptr;
}

const std::array<TeamInfo, 4>& TeamManager::teams() const
{
    return teams_;
}

TeamPlayerState& TeamManager::ensure_player(std::uint32_t connection_id)
{
    if (TeamPlayerState* player = find_player(connection_id))
    {
        return *player;
    }

    TeamPlayerState player;
    player.connection_id = connection_id;
    players_.push_back(player);
    rebuild_membership();
    bump_revision();
    return players_.back();
}

TeamPlayerState* TeamManager::find_player(std::uint32_t connection_id)
{
    const auto it = std::find_if(players_.begin(), players_.end(), [&](const TeamPlayerState& player) {
        return player.connection_id == connection_id;
    });
    return it == players_.end() ? nullptr : &*it;
}

const TeamPlayerState* TeamManager::find_player(std::uint32_t connection_id) const
{
    const auto it = std::find_if(players_.begin(), players_.end(), [&](const TeamPlayerState& player) {
        return player.connection_id == connection_id;
    });
    return it == players_.end() ? nullptr : &*it;
}

void TeamManager::remove_player(std::uint32_t connection_id)
{
    const auto old_size = players_.size();
    players_.erase(std::remove_if(players_.begin(), players_.end(), [&](const TeamPlayerState& player) {
                       return player.connection_id == connection_id;
                   }),
        players_.end());
    if (players_.size() != old_size)
    {
        rebuild_membership();
        bump_revision();
    }
}

const std::vector<TeamPlayerState>& TeamManager::players() const
{
    return players_;
}

void TeamManager::mark_join_game(std::uint32_t connection_id)
{
    TeamPlayerState& player = ensure_player(connection_id);
    if (!player.joined_game)
    {
        player.joined_game = true;
        player.team_menu_requested = player.current_team == TEAM_UNASSIGNED;
        player.last_join_failure = TeamJoinFailedReason::None;
        bump_revision();
    }
}

void TeamManager::mark_join_request_pending(std::uint32_t connection_id, int requested_team)
{
    TeamPlayerState& player = ensure_player(connection_id);
    player.pending_team = requested_team;
    player.join_request_pending = true;
    player.team_menu_requested = false;
    player.last_join_failure = TeamJoinFailedReason::None;
    bump_revision();
}

void TeamManager::request_team_menu(std::uint32_t connection_id)
{
    TeamPlayerState& player = ensure_player(connection_id);
    player.team_menu_requested = true;
    bump_revision();
}

void TeamManager::clear_team_menu_request(std::uint32_t connection_id)
{
    if (TeamPlayerState* player = find_player(connection_id))
    {
        if (player->team_menu_requested)
        {
            player->team_menu_requested = false;
            bump_revision();
        }
    }
}

bool TeamManager::should_show_team_menu(std::uint32_t connection_id, bool world_loaded) const
{
    if (!world_loaded)
    {
        return false;
    }
    const TeamPlayerState* player = find_player(connection_id);
    if (player == nullptr)
    {
        return true;
    }
    if (player->join_request_pending)
    {
        return false;
    }
    if (is_playing_team(player->current_team) || player->current_team == TEAM_SPECTATOR)
    {
        return player->team_menu_requested;
    }
    return true;
}

int TeamManager::select_default_team(const TeamJoinRules& rules)
{
    const int terrorist_count = count_team_members(TEAM_TERRORIST);
    const int ct_count = count_team_members(TEAM_CT);
    int team = TEAM_UNASSIGNED;
    if (terrorist_count < ct_count)
    {
        team = TEAM_TERRORIST;
    }
    else if (terrorist_count > ct_count)
    {
        team = TEAM_CT;
    }
    else if (teams_[TEAM_TERRORIST].score_total < teams_[TEAM_CT].score_total)
    {
        team = TEAM_TERRORIST;
    }
    else if (teams_[TEAM_CT].score_total < teams_[TEAM_TERRORIST].score_total)
    {
        team = TEAM_CT;
    }
    else
    {
        team = (auto_assign_counter_++ % 2U) == 0U ? TEAM_CT : TEAM_TERRORIST;
    }

    if (team_full(team, rules))
    {
        team = team == TEAM_TERRORIST ? TEAM_CT : TEAM_TERRORIST;
        if (team_full(team, rules))
        {
            return TEAM_UNASSIGNED;
        }
    }
    return team;
}

bool TeamManager::team_full(int team, const TeamJoinRules& rules, std::uint32_t moving_player) const
{
    return count_team_members(team, moving_player) >= team_capacity(team, rules);
}

bool TeamManager::team_stacked(int new_team, int current_team, const TeamJoinRules& rules) const
{
    if (new_team == current_team || rules.limit_teams == 0)
    {
        return false;
    }

    const int terrorists = count_team_members(TEAM_TERRORIST);
    const int cts = count_team_members(TEAM_CT);
    switch (new_team)
    {
    case TEAM_TERRORIST:
        if (current_team != TEAM_UNASSIGNED && current_team != TEAM_SPECTATOR)
        {
            return (terrorists + 1) > (cts + rules.limit_teams - 1);
        }
        return (terrorists + 1) > (cts + rules.limit_teams);
    case TEAM_CT:
        if (current_team != TEAM_UNASSIGNED && current_team != TEAM_SPECTATOR)
        {
            return (cts + 1) > (terrorists + rules.limit_teams - 1);
        }
        return (cts + 1) > (terrorists + rules.limit_teams);
    default:
        return false;
    }
}

TeamJoinResult TeamManager::try_join_team(
    std::uint32_t connection_id,
    int requested_team,
    bool force,
    const TeamJoinRules& rules)
{
    TeamPlayerState& player = ensure_player(connection_id);
    if (rules.require_join_game && !player.joined_game)
    {
        note_join_failure(player, TeamJoinFailedReason::ChangedTooOften, true);
        return TeamJoinResult{.reason = TeamJoinFailedReason::ChangedTooOften, .show_team_menu = true};
    }

    if (!is_joinable_team_id(requested_team))
    {
        log_warning("jointeam rejected invalid team id {}", requested_team);
        return TeamJoinResult{.reason = TeamJoinFailedReason::None};
    }

    if (!force && is_playing_team(player.current_team) && is_playing_team(requested_team) && requested_team != player.current_team)
    {
        note_join_failure(player, TeamJoinFailedReason::ChangedTooOften, true);
        return TeamJoinResult{.reason = TeamJoinFailedReason::ChangedTooOften, .show_team_menu = true};
    }

    int team = requested_team;
    const int human_team = human_team_from_cvar(rules.human_team);
    if (team == TEAM_UNASSIGNED && human_team != TEAM_UNASSIGNED)
    {
        team = human_team;
    }

    if (team == TEAM_UNASSIGNED)
    {
        team = select_default_team(rules);
        if (team == TEAM_UNASSIGNED)
        {
            note_join_failure(player, TeamJoinFailedReason::BothTeamsFull, true);
            return TeamJoinResult{.reason = TeamJoinFailedReason::BothTeamsFull, .show_team_menu = true};
        }
    }

    if (team == TEAM_SPECTATOR && !rules.allow_spectators)
    {
        note_join_failure(player, TeamJoinFailedReason::CannotJoinSpectator, false);
        return TeamJoinResult{.reason = TeamJoinFailedReason::CannotJoinSpectator};
    }

    if (team != TEAM_SPECTATOR && human_team != TEAM_UNASSIGNED && human_team != team)
    {
        const TeamJoinFailedReason reason = human_team == TEAM_TERRORIST ? TeamJoinFailedReason::HumansCanOnlyJoinTs :
                                                                           TeamJoinFailedReason::HumansCanOnlyJoinCts;
        note_join_failure(player, reason, true);
        return TeamJoinResult{.reason = reason, .show_team_menu = true};
    }

    if (team_full(team, rules, player.connection_id))
    {
        TeamJoinFailedReason reason = TeamJoinFailedReason::CannotJoinSpectator;
        if (team == TEAM_TERRORIST)
        {
            reason = TeamJoinFailedReason::TerroristsFull;
        }
        else if (team == TEAM_CT)
        {
            reason = TeamJoinFailedReason::CtsFull;
        }
        note_join_failure(player, reason, true);
        return TeamJoinResult{.reason = reason, .show_team_menu = true};
    }

    if (is_playing_team(team) && team_stacked(team, player.current_team, rules))
    {
        const TeamJoinFailedReason reason = team == TEAM_TERRORIST ? TeamJoinFailedReason::TooManyTs : TeamJoinFailedReason::TooManyCts;
        note_join_failure(player, reason, true);
        return TeamJoinResult{.reason = reason, .show_team_menu = true};
    }

    change_player_team(player, team);
    player.team_menu_requested = false;
    player.join_request_pending = false;
    player.last_join_failure = TeamJoinFailedReason::None;
    rebuild_membership();
    bump_revision();
    log_info("player {} joined {}", connection_id, team_name(team));
    return TeamJoinResult{.accepted = true, .resolved_team = team};
}

void TeamManager::set_team_score(int team, int score)
{
    if (TeamInfo* info = find_team(team))
    {
        info->score_total = score;
        bump_revision();
    }
}

void TeamManager::add_team_score(int team, int delta)
{
    if (TeamInfo* info = find_team(team))
    {
        info->score_total += delta;
        bump_revision();
    }
}

void TeamManager::mark_surrendered(int team)
{
    if (TeamInfo* info = find_team(team))
    {
        info->surrendered = true;
        bump_revision();
    }
}

TeamSnapshot TeamManager::make_snapshot() const
{
    TeamSnapshot snapshot;
    snapshot.teams.assign(teams_.begin(), teams_.end());
    snapshot.players = players_;
    return snapshot;
}

void TeamManager::apply_snapshot(const TeamSnapshot& snapshot, std::uint32_t local_connection_id)
{
    initialize_teams();
    for (const TeamInfo& incoming : snapshot.teams)
    {
        if (TeamInfo* team = find_team(incoming.id))
        {
            team->score_total = incoming.score_total;
            team->score_first_half = incoming.score_first_half;
            team->score_second_half = incoming.score_second_half;
            team->score_overtime = incoming.score_overtime;
            team->surrendered = incoming.surrendered;
        }
    }

    players_ = snapshot.players;
    for (TeamPlayerState& player : players_)
    {
        player.team_menu_requested = player.current_team == TEAM_UNASSIGNED ||
                                     (player.connection_id == local_connection_id &&
                                         player.last_join_failure != TeamJoinFailedReason::None);
        player.join_request_pending = false;
    }
    rebuild_membership();
    bump_revision();
}

std::optional<WorldSpawnPoint> TeamManager::select_spawn_point(
    const LoadedWorld& world,
    int team,
    std::uint32_t connection_id)
{
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < world.spawn_points.size(); ++index)
    {
        if (team_matches_spawn(world.spawn_points[index], team))
        {
            candidates.push_back(index);
        }
    }
    if (candidates.empty())
    {
        for (std::size_t index = 0; index < world.spawn_points.size(); ++index)
        {
            if (general_spawn(world.spawn_points[index]))
            {
                candidates.push_back(index);
            }
        }
    }
    if (candidates.empty())
    {
        return std::nullopt;
    }

    std::uint32_t& cursor = spawn_cursors_[static_cast<std::size_t>(std::clamp(team, TEAM_UNASSIGNED, TEAM_CT))];
    const std::size_t selected = (static_cast<std::size_t>(cursor++) + connection_id) % candidates.size();
    return world.spawn_points[candidates[selected]];
}

std::uint64_t TeamManager::revision() const
{
    return revision_;
}

void TeamManager::initialize_teams()
{
    for (int team = TEAM_UNASSIGNED; team <= TEAM_CT; ++team)
    {
        teams_[static_cast<std::size_t>(team)] = TeamInfo{
            .id = team,
            .name = std::string(team_name(team)),
        };
    }
}

void TeamManager::change_player_team(TeamPlayerState& player, int team)
{
    player.last_team = player.current_team;
    player.current_team = team;
    player.pending_team = team;
    player.alive = is_playing_team(team);
    if (team == TEAM_SPECTATOR || team == TEAM_UNASSIGNED)
    {
        player.alive = false;
    }
}

void TeamManager::rebuild_membership()
{
    for (TeamInfo& team : teams_)
    {
        team.members.clear();
        team.alive_count = 0;
    }

    for (const TeamPlayerState& player : players_)
    {
        if (TeamInfo* team = find_team(player.current_team))
        {
            team->members.push_back(player.connection_id);
            if (player.alive)
            {
                ++team->alive_count;
            }
        }
    }
}

int TeamManager::count_team_members(int team, std::uint32_t moving_player) const
{
    return static_cast<int>(std::count_if(players_.begin(), players_.end(), [&](const TeamPlayerState& player) {
        return player.connection_id != moving_player && player.current_team == team;
    }));
}

int TeamManager::team_capacity(int team, const TeamJoinRules& rules) const
{
    if (team == TEAM_SPECTATOR)
    {
        return std::max(0, rules.spectator_slots);
    }
    const int default_team_capacity = std::max(1, rules.max_players / 2);
    if (team == TEAM_TERRORIST && rules.terrorist_spawn_count > 0)
    {
        return std::max(1, std::min(default_team_capacity, rules.terrorist_spawn_count));
    }
    if (team == TEAM_CT && rules.ct_spawn_count > 0)
    {
        return std::max(1, std::min(default_team_capacity, rules.ct_spawn_count));
    }
    return default_team_capacity;
}

void TeamManager::note_join_failure(TeamPlayerState& player, TeamJoinFailedReason reason, bool show_menu)
{
    player.last_join_failure = reason;
    player.join_request_pending = false;
    player.team_menu_requested = show_menu;
    bump_revision();
}

void TeamManager::bump_revision()
{
    ++revision_;
}

void register_team_variables(ConsoleVariables& variables)
{
    variables.register_variable("mp_force_pick_time", "15", "Seconds shown on the team-pick countdown.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_allowspectators", "1", "Allows players to join spectators.", ConsoleVariableFlagGameRule);
    variables.register_variable("mp_humanteam", "any", "Restricts humans to T, CT, or any.", ConsoleVariableFlagGameRule);
}

void register_team_commands(CommandRegistry& commands)
{
    commands.register_command("joingame", "Marks the local player ready to choose a team.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        execute_or_send_join_game(context);
    });

    commands.register_command("jointeam", "jointeam <0 auto|1 spec|2 T|3 CT> [force]", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (invocation.args.empty())
        {
            log_warning("usage: jointeam <0|1|2|3> [force]");
            return;
        }

        const std::optional<int> requested_team = parse_int(invocation.args[0]);
        if (!requested_team)
        {
            log_warning("jointeam failed: invalid team '{}'", invocation.args[0]);
            return;
        }

        bool force = false;
        if (invocation.args.size() >= 2)
        {
            force = parse_int(invocation.args[1]).value_or(0) > 0;
        }
        execute_or_send_join_team(context, *requested_team, force);
    });

    commands.register_command("spectate", "Join spectators.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        execute_or_send_join_team(context, TEAM_SPECTATOR, true);
    });

    const auto show_team_menu = [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.teams != nullptr)
        {
            context.teams->request_team_menu(command_team_player_id(context));
        }
        execute_or_send_join_game(context);
    };
    commands.register_command("teammenu", "Open the team selection menu.", show_team_menu);
    commands.register_command("chooseteam", "Open the team selection menu.", show_team_menu);

    commands.register_command("team_status", "Print team scores and membership.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.teams == nullptr)
        {
            return;
        }
        for (const TeamInfo& team : context.teams->teams())
        {
            log_info("{} score={} players={} alive={} surrendered={}",
                team.name,
                team.score_total,
                team.members.size(),
                team.alive_count,
                team.surrendered ? "yes" : "no");
        }
    });
}

bool handle_team_network_event(EngineContext& engine, const NetworkEvent& event)
{
    switch (event.type)
    {
    case NetworkEventType::ClientConnected:
        static_cast<void>(engine.teams.ensure_player(event.connection_id));
        broadcast_team_snapshot(engine);
        return false;
    case NetworkEventType::ClientDisconnected:
        engine.teams.remove_player(event.connection_id);
        broadcast_team_snapshot(engine);
        return false;
    case NetworkEventType::UserCommandReceived:
        if (const std::optional<TeamUserCommand> command = read_team_user_command_payload(event.payload))
        {
            if (command->kind == TeamUserCommandKind::JoinGame)
            {
                engine.teams.mark_join_game(event.connection_id);
            }
            else if (command->kind == TeamUserCommandKind::JoinTeam)
            {
                const TeamJoinRules rules = team_join_rules_from_context(engine.variables, engine.world.current_world());
                const TeamJoinResult result =
                    engine.teams.try_join_team(event.connection_id, command->requested_team, command->force, rules);
                if (!result.accepted)
                {
                    log_warning("player {} jointeam failed: {}", event.connection_id, to_string(result.reason));
                }
            }
            broadcast_team_snapshot(engine);
            return true;
        }
        break;
    case NetworkEventType::SnapshotReceived:
        if (const std::optional<TeamSnapshot> snapshot = read_team_snapshot_payload(event.payload))
        {
            engine.teams.apply_snapshot(*snapshot, local_team_connection_id(engine.network));
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

void broadcast_team_snapshot(EngineContext& engine)
{
    if (!engine.network.server().is_running())
    {
        return;
    }

    const std::vector<unsigned char> payload = make_team_snapshot_payload(engine.teams.make_snapshot());
    engine.network.broadcast_server_snapshot(payload, 0);
}
}
