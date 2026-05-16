#pragma once

#include "openstrike/core/math.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openstrike
{
class CommandRegistry;
class ConsoleVariables;
class ContentFileSystem;
struct LoadedWorld;
struct WorldTriangle;

constexpr std::uint32_t kNavMagicNumber = 0xFEEDFACEU;
constexpr std::uint32_t kNavCurrentVersion = 16;
constexpr std::uint32_t kCounterStrikeNavSubVersion = 1;
constexpr int kMaxNavTeams = 2;
constexpr float kNavGenerationStepSize = 25.0F;
constexpr float kNavStepHeight = 18.0F;
constexpr float kNavJumpHeight = 41.8F;
constexpr float kNavJumpCrouchHeight = 58.0F;
constexpr float kNavDeathDrop = 200.0F;
constexpr float kNavClimbUpHeight = kNavJumpCrouchHeight;
constexpr float kNavCliffHeight = 300.0F;
constexpr float kNavHalfHumanWidth = 16.0F;
constexpr float kNavHalfHumanHeight = 35.5F;
constexpr float kNavHumanHeight = 71.0F;
constexpr float kNavHumanEyeHeight = 62.0F;

enum class NavError
{
    Ok,
    CannotAccessFile,
    InvalidFile,
    BadFileVersion,
    FileOutOfDate,
    CorruptData
};

enum NavAttribute : std::uint32_t
{
    NavMeshInvalid = 0,
    NavMeshCrouch = 0x00000001U,
    NavMeshJump = 0x00000002U,
    NavMeshPrecise = 0x00000004U,
    NavMeshNoJump = 0x00000008U,
    NavMeshStop = 0x00000010U,
    NavMeshRun = 0x00000020U,
    NavMeshWalk = 0x00000040U,
    NavMeshAvoid = 0x00000080U,
    NavMeshTransient = 0x00000100U,
    NavMeshDontHide = 0x00000200U,
    NavMeshStand = 0x00000400U,
    NavMeshNoHostages = 0x00000800U,
    NavMeshStairs = 0x00001000U,
    NavMeshNoMerge = 0x00002000U,
    NavMeshObstacleTop = 0x00004000U,
    NavMeshCliff = 0x00008000U,
    NavMeshFirstCustom = 0x00010000U,
    NavMeshLastCustom = 0x04000000U,
    NavMeshBlockedPropDoor = 0x10000000U,
    NavMeshHasElevator = 0x40000000U,
    NavMeshNavBlocker = 0x80000000U
};

enum class NavDirection : std::uint8_t
{
    North = 0,
    East = 1,
    South = 2,
    West = 3,
    Count = 4
};

enum class NavTraverseType : std::uint8_t
{
    GoNorth = 0,
    GoEast = 1,
    GoSouth = 2,
    GoWest = 3,
    GoLadderUp = 4,
    GoLadderDown = 5,
    GoJump = 6,
    GoElevatorUp = 7,
    GoElevatorDown = 8,
    Count = 9
};

enum class NavCorner : std::uint8_t
{
    NorthWest = 0,
    NorthEast = 1,
    SouthEast = 2,
    SouthWest = 3,
    Count = 4
};

enum class NavRouteType
{
    Default,
    Fastest,
    Safest,
    Retreat
};

struct NavExtent
{
    Vec3 lo;
    Vec3 hi;

    [[nodiscard]] bool contains(Vec3 position) const;
    [[nodiscard]] bool overlaps(const NavExtent& other) const;
};

struct NavConnection
{
    std::uint32_t area_id = 0;
    float length = -1.0F;
};

struct NavHidingSpot
{
    enum Flags : std::uint8_t
    {
        InCover = 0x01,
        GoodSniperSpot = 0x02,
        IdealSniperSpot = 0x04,
        Exposed = 0x08
    };

    std::uint32_t id = 0;
    Vec3 position;
    std::uint8_t flags = 0;
    bool saved = true;
};

struct NavSpotOrder
{
    std::uint32_t spot_id = 0;
    float t = 0.0F;
};

struct NavSpotEncounter
{
    std::uint32_t from_area_id = 0;
    NavDirection from_direction = NavDirection::North;
    std::uint32_t to_area_id = 0;
    NavDirection to_direction = NavDirection::North;
    Vec3 path_from;
    Vec3 path_to;
    std::vector<NavSpotOrder> spots;
};

struct NavApproachInfo
{
    std::uint32_t here_area_id = 0;
    std::uint32_t previous_area_id = 0;
    NavTraverseType previous_to_here = NavTraverseType::Count;
    std::uint32_t next_area_id = 0;
    NavTraverseType here_to_next = NavTraverseType::Count;
};

struct NavArea
{
    std::uint32_t id = 0;
    Vec3 north_west_corner;
    Vec3 south_east_corner;
    float north_east_z = 0.0F;
    float south_west_z = 0.0F;
    std::uint32_t attributes = 0;
    std::uint32_t place = 0;

    std::array<std::vector<NavConnection>, 4> connections;
    std::array<std::vector<NavConnection>, 4> incoming_connections;
    std::array<std::vector<std::uint32_t>, 2> ladders;
    std::vector<NavHidingSpot> hiding_spots;
    std::vector<NavSpotEncounter> spot_encounters;
    std::vector<NavApproachInfo> approach_areas;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> potentially_visible_areas;
    std::uint32_t inherit_visibility_from_area_id = 0;

    std::array<float, kMaxNavTeams> danger{};
    float danger_timestamp = 0.0F;
    std::array<float, kMaxNavTeams> cleared_timestamp{};
    std::array<float, kMaxNavTeams> earliest_occupy_time{};
    std::array<std::uint8_t, kMaxNavTeams> player_count{};
    std::array<bool, kMaxNavTeams> blocked{};
    float avoidance_obstacle_height = 0.0F;
    bool underwater = false;

    std::uint32_t parent_id = 0;
    NavTraverseType parent_how = NavTraverseType::Count;
    float total_cost = 0.0F;
    float cost_so_far = 0.0F;
    float path_length_so_far = 0.0F;

    void build(Vec3 north_west, Vec3 north_east, Vec3 south_east, Vec3 south_west);
    void connect_to(std::uint32_t area_id, NavDirection direction, float length = -1.0F);
    void disconnect(std::uint32_t area_id);
    void add_incoming_connection(std::uint32_t source_area_id, NavDirection incoming_edge_direction, float length = -1.0F);
    void add_ladder(std::uint32_t ladder_id, bool up);
    void set_attributes(std::uint32_t bits);
    void remove_attributes(std::uint32_t bits);
    void mark_blocked(int team_id, bool is_blocked);
    void increment_player_count(int team_id);
    void decrement_player_count(int team_id);
    void increase_danger(int team_id, float amount);

    [[nodiscard]] bool has_attributes(std::uint32_t bits) const;
    [[nodiscard]] bool is_blocked(int team_id = -1, bool ignore_nav_blockers = false) const;
    [[nodiscard]] bool has_avoidance_obstacle(float max_obstruction_height = kNavStepHeight) const;
    [[nodiscard]] bool is_degenerate() const;
    [[nodiscard]] bool is_flat(float epsilon = 0.1F) const;
    [[nodiscard]] bool is_overlapping(Vec3 position, float tolerance = 0.0F) const;
    [[nodiscard]] bool is_overlapping(const NavArea& area) const;
    [[nodiscard]] bool contains(Vec3 position, float beneath_limit = 120.0F) const;
    [[nodiscard]] bool is_connected(std::uint32_t area_id, NavDirection direction) const;
    [[nodiscard]] bool is_connected(std::uint32_t area_id) const;
    [[nodiscard]] std::size_t adjacent_count(NavDirection direction) const;
    [[nodiscard]] float get_z(Vec3 position) const;
    [[nodiscard]] Vec3 center() const;
    [[nodiscard]] Vec3 corner(NavCorner corner) const;
    [[nodiscard]] NavExtent extent() const;
    [[nodiscard]] Vec3 closest_point(Vec3 position) const;
    [[nodiscard]] float distance_squared_to_point(Vec3 position) const;
    [[nodiscard]] NavDirection compute_direction(Vec3 point) const;
    [[nodiscard]] bool compute_portal(const NavArea& to, NavDirection direction, Vec3& center, float& half_width) const;
    [[nodiscard]] bool compute_closest_point_in_portal(const NavArea& to, NavDirection direction, Vec3 from_position, Vec3& close_position) const;
    [[nodiscard]] float danger_for_team(int team_id, float current_time = 0.0F) const;
    [[nodiscard]] float danger_decay_rate() const;
};

struct NavLadder
{
    std::uint32_t id = 0;
    float width = 0.0F;
    Vec3 top;
    Vec3 bottom;
    float length = 0.0F;
    NavDirection direction = NavDirection::North;
    Vec3 normal{0.0F, -1.0F, 0.0F};
    std::uint32_t top_forward_area_id = 0;
    std::uint32_t top_left_area_id = 0;
    std::uint32_t top_right_area_id = 0;
    std::uint32_t top_behind_area_id = 0;
    std::uint32_t bottom_area_id = 0;

    void set_direction(NavDirection direction);
    void connect_to(const NavArea& area);

    [[nodiscard]] Vec3 position_at_height(float height) const;
    [[nodiscard]] bool is_connected(std::uint32_t area_id, bool up) const;
};

struct NavPathSegment
{
    std::uint32_t area_id = 0;
    NavTraverseType how = NavTraverseType::Count;
    Vec3 position;
    std::uint32_t ladder_id = 0;
};

struct NavPath
{
    std::vector<NavPathSegment> segments;
    bool reached_goal = false;

    void clear();
    [[nodiscard]] bool valid() const;
    [[nodiscard]] Vec3 endpoint() const;
    [[nodiscard]] float length() const;
    [[nodiscard]] bool is_at_end(Vec3 position, float tolerance = 25.0F) const;
    [[nodiscard]] std::optional<Vec3> point_along_path(float distance_along) const;
    [[nodiscard]] std::optional<std::size_t> segment_index_along_path(float distance_along) const;
    [[nodiscard]] std::optional<Vec3> closest_point(Vec3 world_position, std::size_t start_index = 0, std::size_t end_index = 0) const;
};

struct NavPathQuery
{
    Vec3 start;
    Vec3 goal;
    float max_path_length = 0.0F;
    int team_id = -1;
    bool ignore_nav_blockers = false;
};

class NavMesh
{
public:
    void clear();
    void generate_from_world(const LoadedWorld& world);
    [[nodiscard]] NavError load_file(const std::filesystem::path& path, std::uintmax_t expected_bsp_size = 0);
    [[nodiscard]] bool save_file(const std::filesystem::path& path, std::uintmax_t bsp_size = 0) const;

    [[nodiscard]] NavArea* create_area();
    [[nodiscard]] NavLadder* create_ladder();
    [[nodiscard]] NavArea* area_by_id(std::uint32_t id);
    [[nodiscard]] const NavArea* area_by_id(std::uint32_t id) const;
    [[nodiscard]] NavLadder* ladder_by_id(std::uint32_t id);
    [[nodiscard]] const NavLadder* ladder_by_id(std::uint32_t id) const;
    [[nodiscard]] const NavArea* nav_area_at(Vec3 position, float beneath_limit = 120.0F) const;
    [[nodiscard]] const NavArea* nearest_nav_area(Vec3 position, bool any_z = false, float max_distance = 10000.0F, bool check_ground = true) const;
    [[nodiscard]] std::optional<float> ground_height(Vec3 position) const;
    [[nodiscard]] NavPath build_path(const NavPathQuery& query);
    [[nodiscard]] float travel_distance(Vec3 start, Vec3 goal, float max_path_length = 0.0F);

    [[nodiscard]] const std::vector<NavArea>& areas() const;
    [[nodiscard]] std::vector<NavArea>& areas();
    [[nodiscard]] const std::vector<NavLadder>& ladders() const;
    [[nodiscard]] const std::vector<std::string>& places() const;
    [[nodiscard]] bool loaded() const;
    [[nodiscard]] bool analyzed() const;
    [[nodiscard]] bool out_of_date() const;
    [[nodiscard]] std::uint32_t file_version() const;
    [[nodiscard]] std::uint32_t sub_version() const;

    void set_analyzed(bool analyzed);
    void set_out_of_date(bool out_of_date);

private:
    friend class NavigationSystem;

    void rebuild_indices();
    void connect_generated_areas();
    void compute_generated_analysis();
    void compress_ids();
    [[nodiscard]] std::uint32_t allocate_area_id() const;
    [[nodiscard]] std::uint32_t allocate_ladder_id() const;
    [[nodiscard]] std::uint32_t allocate_hiding_spot_id() const;

    std::vector<NavArea> areas_;
    std::vector<NavLadder> ladders_;
    std::vector<std::string> places_;
    std::unordered_map<std::uint32_t, std::size_t> area_index_by_id_;
    std::unordered_map<std::uint32_t, std::size_t> ladder_index_by_id_;
    bool loaded_ = false;
    bool analyzed_ = false;
    bool out_of_date_ = false;
    std::uint32_t file_version_ = kNavCurrentVersion;
    std::uint32_t sub_version_ = kCounterStrikeNavSubVersion;
};

class NavPathFollower
{
public:
    void reset();
    void set_path(const NavPath* path);

    [[nodiscard]] std::optional<Vec3> update(Vec3 position, float ahead_range = 50.0F);
    [[nodiscard]] bool stuck() const;
    [[nodiscard]] float stuck_duration() const;

private:
    const NavPath* path_ = nullptr;
    std::size_t segment_index_ = 0;
    Vec3 stuck_position_;
    float stuck_duration_ = 0.0F;
};

class NavigationSystem
{
public:
    void clear();
    void sync_world(const LoadedWorld* world, const ContentFileSystem& filesystem);
    [[nodiscard]] bool generate_from_world(const LoadedWorld& world);
    [[nodiscard]] NavError load_for_world(const LoadedWorld& world, const ContentFileSystem& filesystem);
    [[nodiscard]] bool save_for_world(const LoadedWorld& world, const ContentFileSystem& filesystem) const;
    [[nodiscard]] NavMesh& mesh();
    [[nodiscard]] const NavMesh& mesh() const;

    static void register_variables(ConsoleVariables& variables);
    static void register_commands(CommandRegistry& commands);

private:
    NavMesh mesh_;
    std::uint64_t observed_world_generation_ = 0;
};

[[nodiscard]] NavDirection opposite_direction(NavDirection direction);
[[nodiscard]] NavDirection direction_left(NavDirection direction);
[[nodiscard]] NavDirection direction_right(NavDirection direction);
[[nodiscard]] NavTraverseType traverse_from_direction(NavDirection direction);
[[nodiscard]] std::string_view to_string(NavError error);
[[nodiscard]] std::string_view to_string(NavDirection direction);
[[nodiscard]] std::string_view to_string(NavTraverseType traverse);
}
