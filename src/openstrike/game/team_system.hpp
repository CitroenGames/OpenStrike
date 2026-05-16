#pragma once

#include "openstrike/core/math.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
class CommandRegistry;
class ConsoleVariables;
class EngineContext;
class NetworkSystem;
struct ConsoleCommandContext;
struct LoadedWorld;
struct NetworkEvent;
struct WorldSpawnPoint;

inline constexpr int TEAM_UNASSIGNED = 0;
inline constexpr int TEAM_SPECTATOR = 1;
inline constexpr int TEAM_TERRORIST = 2;
inline constexpr int TEAM_CT = 3;

inline constexpr std::uint32_t kLocalTeamPlayerId = 0;

enum class TeamJoinFailedReason : std::uint8_t
{
    ChangedTooOften = 0,
    BothTeamsFull = 1,
    TerroristsFull = 2,
    CtsFull = 3,
    CannotJoinSpectator = 4,
    HumansCanOnlyJoinTs = 5,
    HumansCanOnlyJoinCts = 6,
    TooManyTs = 7,
    TooManyCts = 8,
    None = 255,
};

enum class TeamUserCommandKind : std::uint8_t
{
    JoinGame = 1,
    JoinTeam = 2,
};

struct TeamColor
{
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
};

struct TeamInfo
{
    int id = TEAM_UNASSIGNED;
    std::string name;
    int score_total = 0;
    int score_first_half = 0;
    int score_second_half = 0;
    int score_overtime = 0;
    bool surrendered = false;
    std::vector<std::uint32_t> members;
    int alive_count = 0;
};

struct TeamPlayerState
{
    std::uint32_t connection_id = kLocalTeamPlayerId;
    int current_team = TEAM_UNASSIGNED;
    int pending_team = TEAM_UNASSIGNED;
    int last_team = TEAM_UNASSIGNED;
    bool alive = false;
    bool joined_game = false;
    bool team_menu_requested = false;
    bool join_request_pending = false;
    int kills = 0;
    int deaths = 0;
    int ping_ms = 0;
    TeamJoinFailedReason last_join_failure = TeamJoinFailedReason::None;
};

struct TeamJoinRules
{
    int max_players = 10;
    int terrorist_spawn_count = 0;
    int ct_spawn_count = 0;
    int general_spawn_count = 0;
    int limit_teams = 2;
    int spectator_slots = 10;
    bool allow_spectators = true;
    std::string human_team = "any";
    bool require_join_game = true;
};

struct TeamJoinResult
{
    bool accepted = false;
    int resolved_team = TEAM_UNASSIGNED;
    TeamJoinFailedReason reason = TeamJoinFailedReason::None;
    bool show_team_menu = false;
};

struct TeamUserCommand
{
    TeamUserCommandKind kind = TeamUserCommandKind::JoinGame;
    int requested_team = TEAM_UNASSIGNED;
    bool force = false;
};

struct TeamSnapshot
{
    std::vector<TeamInfo> teams;
    std::vector<TeamPlayerState> players;
};

[[nodiscard]] bool is_valid_team_id(int team);
[[nodiscard]] bool is_playing_team(int team);
[[nodiscard]] bool is_joinable_team_id(int team);
[[nodiscard]] std::string_view team_name(int team);
[[nodiscard]] std::string_view team_short_name(int team);
[[nodiscard]] TeamColor team_color(int team);
[[nodiscard]] std::string_view to_string(TeamJoinFailedReason reason);
[[nodiscard]] std::uint32_t local_team_connection_id(const NetworkSystem& network);

[[nodiscard]] std::vector<unsigned char> make_team_user_command_payload(const TeamUserCommand& command);
[[nodiscard]] std::optional<TeamUserCommand> read_team_user_command_payload(std::span<const unsigned char> payload);
[[nodiscard]] std::vector<unsigned char> make_team_snapshot_payload(const TeamSnapshot& snapshot);
[[nodiscard]] std::optional<TeamSnapshot> read_team_snapshot_payload(std::span<const unsigned char> payload);

[[nodiscard]] TeamJoinRules team_join_rules_from_context(const ConsoleVariables& variables, const LoadedWorld* world);

class TeamManager
{
public:
    TeamManager();

    void reset();
    void reset_for_new_world();

    [[nodiscard]] TeamInfo* find_team(int team);
    [[nodiscard]] const TeamInfo* find_team(int team) const;
    [[nodiscard]] const std::array<TeamInfo, 4>& teams() const;

    [[nodiscard]] TeamPlayerState& ensure_player(std::uint32_t connection_id);
    [[nodiscard]] TeamPlayerState* find_player(std::uint32_t connection_id);
    [[nodiscard]] const TeamPlayerState* find_player(std::uint32_t connection_id) const;
    void remove_player(std::uint32_t connection_id);
    [[nodiscard]] const std::vector<TeamPlayerState>& players() const;

    void mark_join_game(std::uint32_t connection_id);
    void mark_join_request_pending(std::uint32_t connection_id, int requested_team);
    void request_team_menu(std::uint32_t connection_id);
    void clear_team_menu_request(std::uint32_t connection_id);
    [[nodiscard]] bool should_show_team_menu(std::uint32_t connection_id, bool world_loaded) const;

    [[nodiscard]] int select_default_team(const TeamJoinRules& rules);
    [[nodiscard]] bool team_full(int team, const TeamJoinRules& rules, std::uint32_t moving_player = UINT32_MAX) const;
    [[nodiscard]] bool team_stacked(int new_team, int current_team, const TeamJoinRules& rules) const;
    [[nodiscard]] TeamJoinResult try_join_team(
        std::uint32_t connection_id,
        int requested_team,
        bool force,
        const TeamJoinRules& rules);

    void set_team_score(int team, int score);
    void add_team_score(int team, int delta);
    void mark_surrendered(int team);

    [[nodiscard]] TeamSnapshot make_snapshot() const;
    void apply_snapshot(const TeamSnapshot& snapshot, std::uint32_t local_connection_id);

    [[nodiscard]] std::optional<WorldSpawnPoint> select_spawn_point(
        const LoadedWorld& world,
        int team,
        std::uint32_t connection_id);

    [[nodiscard]] std::uint64_t revision() const;

private:
    void initialize_teams();
    void change_player_team(TeamPlayerState& player, int team);
    void rebuild_membership();
    [[nodiscard]] int count_team_members(int team, std::uint32_t moving_player = UINT32_MAX) const;
    [[nodiscard]] int team_capacity(int team, const TeamJoinRules& rules) const;
    void note_join_failure(TeamPlayerState& player, TeamJoinFailedReason reason, bool show_menu);
    void bump_revision();

    std::array<TeamInfo, 4> teams_{};
    std::vector<TeamPlayerState> players_;
    std::array<std::uint32_t, 4> spawn_cursors_{};
    std::uint32_t auto_assign_counter_ = 0;
    std::uint64_t revision_ = 1;
};

void register_team_variables(ConsoleVariables& variables);
void register_team_commands(CommandRegistry& commands);
bool handle_team_network_event(EngineContext& engine, const NetworkEvent& event);
void broadcast_team_snapshot(EngineContext& engine);
}
