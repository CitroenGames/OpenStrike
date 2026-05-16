#include "openstrike/nav/navigation.hpp"

#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>
#include <stdexcept>
#include <system_error>

namespace openstrike
{
namespace
{
constexpr float kEpsilon = 0.001F;
constexpr float kWalkableNormalZ = 0.70F;
constexpr std::uint32_t kUndefinedPlace = 0;
constexpr std::uint8_t kCompletelyVisible = 0x02;

float length_squared(Vec3 value)
{
    return openstrike::dot(value, value);
}

float length(Vec3 value)
{
    return std::sqrt(length_squared(value));
}

float distance_2d_squared(Vec3 lhs, Vec3 rhs)
{
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    return (dx * dx) + (dy * dy);
}

float distance_2d(Vec3 lhs, Vec3 rhs)
{
    return std::sqrt(distance_2d_squared(lhs, rhs));
}

int team_slot(int team_id)
{
    if (team_id < 0)
    {
        return 0;
    }
    return team_id % kMaxNavTeams;
}

std::uint32_t direction_index(NavDirection direction)
{
    return static_cast<std::uint32_t>(direction);
}

std::uint32_t ladder_direction_index(bool up)
{
    return up ? 0U : 1U;
}

bool close_enough(float lhs, float rhs, float tolerance = 0.01F)
{
    return std::fabs(lhs - rhs) <= tolerance;
}

bool ranges_overlap(float a_min, float a_max, float b_min, float b_max, float tolerance = 0.01F)
{
    return a_min <= b_max + tolerance && b_min <= a_max + tolerance;
}

float clamp_to_range(float value, float min_value, float max_value)
{
    return std::max(min_value, std::min(value, max_value));
}

std::filesystem::path nav_relative_path_for_world(const LoadedWorld& world)
{
    return std::filesystem::path("maps") / (world.name + ".nav");
}

std::optional<std::filesystem::path> writable_nav_path_for_world(const LoadedWorld& world, const ContentFileSystem& filesystem)
{
    const std::filesystem::path relative = nav_relative_path_for_world(world);
    if (std::optional<std::filesystem::path> resolved = filesystem.resolve(relative, "MOD"))
    {
        return *resolved;
    }

    const std::vector<SearchPath> mod_paths = filesystem.search_paths("MOD");
    if (!mod_paths.empty())
    {
        return mod_paths.front().root / relative;
    }

    const std::vector<SearchPath> game_paths = filesystem.search_paths("GAME");
    if (!game_paths.empty())
    {
        return game_paths.front().root / relative;
    }

    return std::nullopt;
}

std::vector<unsigned char> read_binary_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("cannot open file");
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0)
    {
        throw std::runtime_error("cannot size file");
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
    {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return bytes;
}

class BinaryReader
{
public:
    explicit BinaryReader(const std::vector<unsigned char>& bytes) : bytes_(bytes) {}

    [[nodiscard]] bool valid() const
    {
        return valid_;
    }

    [[nodiscard]] std::uint8_t u8()
    {
        if (!require(1))
        {
            return 0;
        }
        return bytes_[offset_++];
    }

    [[nodiscard]] std::uint16_t u16()
    {
        if (!require(2))
        {
            return 0;
        }
        const std::uint16_t value = static_cast<std::uint16_t>(bytes_[offset_]) |
                                    (static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8U);
        offset_ += 2;
        return value;
    }

    [[nodiscard]] std::uint32_t u32()
    {
        if (!require(4))
        {
            return 0;
        }
        const std::uint32_t value = static_cast<std::uint32_t>(bytes_[offset_]) |
                                    (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8U) |
                                    (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16U) |
                                    (static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24U);
        offset_ += 4;
        return value;
    }

    [[nodiscard]] std::int32_t s32()
    {
        return static_cast<std::int32_t>(u32());
    }

    [[nodiscard]] float f32()
    {
        const std::uint32_t raw = u32();
        float value = 0.0F;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    [[nodiscard]] Vec3 vec3()
    {
        return {f32(), f32(), f32()};
    }

    [[nodiscard]] std::string source_string()
    {
        const std::uint16_t length = u16();
        if (length == 0 || !require(length))
        {
            return {};
        }

        std::string text(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        if (!text.empty() && text.back() == '\0')
        {
            text.pop_back();
        }
        return text;
    }

private:
    bool require(std::size_t count)
    {
        if (!valid_ || offset_ + count > bytes_.size())
        {
            valid_ = false;
            return false;
        }
        return true;
    }

    const std::vector<unsigned char>& bytes_;
    std::size_t offset_ = 0;
    bool valid_ = true;
};

class BinaryWriter
{
public:
    void u8(std::uint8_t value)
    {
        bytes_.push_back(value);
    }

    void u16(std::uint16_t value)
    {
        bytes_.push_back(static_cast<unsigned char>(value & 0xFFU));
        bytes_.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    }

    void u32(std::uint32_t value)
    {
        bytes_.push_back(static_cast<unsigned char>(value & 0xFFU));
        bytes_.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
        bytes_.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
        bytes_.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    }

    void s32(std::int32_t value)
    {
        u32(static_cast<std::uint32_t>(value));
    }

    void f32(float value)
    {
        std::uint32_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        u32(raw);
    }

    void vec3(Vec3 value)
    {
        f32(value.x);
        f32(value.y);
        f32(value.z);
    }

    void source_string(std::string_view text)
    {
        const std::size_t stored_size = text.size() + 1;
        u16(static_cast<std::uint16_t>(std::min<std::size_t>(stored_size, 65535U)));
        const std::size_t write_size = std::min<std::size_t>(text.size(), 65534U);
        bytes_.insert(bytes_.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(write_size));
        u8(0);
    }

    [[nodiscard]] const std::vector<unsigned char>& bytes() const
    {
        return bytes_;
    }

private:
    std::vector<unsigned char> bytes_;
};

struct TriangleCluster
{
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
};

TriangleCluster cluster_from_triangle(const WorldTriangle& triangle)
{
    TriangleCluster cluster;
    for (Vec3 point : triangle.points)
    {
        cluster.min_x = std::min(cluster.min_x, point.x);
        cluster.min_y = std::min(cluster.min_y, point.y);
        cluster.max_x = std::max(cluster.max_x, point.x);
        cluster.max_y = std::max(cluster.max_y, point.y);
        cluster.min_z = std::min(cluster.min_z, point.z);
        cluster.max_z = std::max(cluster.max_z, point.z);
    }
    return cluster;
}

bool cluster_can_merge(const TriangleCluster& lhs, const TriangleCluster& rhs)
{
    if (std::fabs(((lhs.min_z + lhs.max_z) * 0.5F) - ((rhs.min_z + rhs.max_z) * 0.5F)) > kNavStepHeight)
    {
        return false;
    }

    return ranges_overlap(lhs.min_x, lhs.max_x, rhs.min_x, rhs.max_x, kNavGenerationStepSize) &&
           ranges_overlap(lhs.min_y, lhs.max_y, rhs.min_y, rhs.max_y, kNavGenerationStepSize);
}

void merge_cluster(TriangleCluster& target, const TriangleCluster& source)
{
    target.min_x = std::min(target.min_x, source.min_x);
    target.min_y = std::min(target.min_y, source.min_y);
    target.max_x = std::max(target.max_x, source.max_x);
    target.max_y = std::max(target.max_y, source.max_y);
    target.min_z = std::min(target.min_z, source.min_z);
    target.max_z = std::max(target.max_z, source.max_z);
}

float connection_cost(const NavArea& area, const NavArea* from_area, const NavLadder* ladder, float length_override)
{
    if (from_area == nullptr)
    {
        return 0.0F;
    }

    float distance = 0.0F;
    if (ladder != nullptr)
    {
        distance = ladder->length;
    }
    else if (length_override > 0.0F)
    {
        distance = length_override;
    }
    else
    {
        distance = length(area.center() - from_area->center());
    }

    float cost = from_area->cost_so_far + distance;
    if ((area.attributes & NavMeshCrouch) != 0)
    {
        cost += 20.0F * distance;
    }
    if ((area.attributes & NavMeshJump) != 0)
    {
        cost += 5.0F * distance;
    }
    if (area.danger_for_team(0) > 0.0F)
    {
        cost += area.danger_for_team(0) * 100.0F * distance;
    }
    if ((area.attributes & NavMeshAvoid) != 0)
    {
        cost += 20.0F * distance;
    }
    return cost;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

void write_area(BinaryWriter& writer, const NavArea& area)
{
    writer.u32(area.id);
    writer.s32(static_cast<std::int32_t>(area.attributes));
    writer.vec3(area.north_west_corner);
    writer.vec3(area.south_east_corner);
    writer.f32(area.north_east_z);
    writer.f32(area.south_west_z);

    for (std::size_t direction = 0; direction < area.connections.size(); ++direction)
    {
        const std::vector<NavConnection>& connections = area.connections[direction];
        writer.u32(static_cast<std::uint32_t>(connections.size()));
        for (const NavConnection& connection : connections)
        {
            writer.u32(connection.area_id);
        }
    }

    std::uint8_t saved_spot_count = 0;
    for (const NavHidingSpot& spot : area.hiding_spots)
    {
        if (spot.saved && saved_spot_count != 255U)
        {
            ++saved_spot_count;
        }
    }
    writer.u8(saved_spot_count);
    std::uint8_t written_spots = 0;
    for (const NavHidingSpot& spot : area.hiding_spots)
    {
        if (!spot.saved || written_spots == saved_spot_count)
        {
            continue;
        }
        writer.u32(spot.id);
        writer.vec3(spot.position);
        writer.u8(spot.flags);
        ++written_spots;
    }

    writer.u32(static_cast<std::uint32_t>(area.spot_encounters.size()));
    for (const NavSpotEncounter& encounter : area.spot_encounters)
    {
        writer.u32(encounter.from_area_id);
        writer.u8(static_cast<std::uint8_t>(encounter.from_direction));
        writer.u32(encounter.to_area_id);
        writer.u8(static_cast<std::uint8_t>(encounter.to_direction));
        const std::uint8_t spot_count = static_cast<std::uint8_t>(std::min<std::size_t>(encounter.spots.size(), 255U));
        writer.u8(spot_count);
        for (std::size_t index = 0; index < spot_count; ++index)
        {
            writer.u32(encounter.spots[index].spot_id);
            writer.u8(static_cast<std::uint8_t>(std::clamp(encounter.spots[index].t, 0.0F, 1.0F) * 255.0F));
        }
    }

    writer.u16(static_cast<std::uint16_t>(area.place));

    for (const std::vector<std::uint32_t>& ladder_list : area.ladders)
    {
        writer.u32(static_cast<std::uint32_t>(ladder_list.size()));
        for (std::uint32_t ladder_id : ladder_list)
        {
            writer.u32(ladder_id);
        }
    }

    for (float occupy_time : area.earliest_occupy_time)
    {
        writer.f32(occupy_time);
    }

    for (std::size_t corner = 0; corner < 4; ++corner)
    {
        writer.f32(1.0F);
    }

    writer.u32(static_cast<std::uint32_t>(area.potentially_visible_areas.size()));
    for (const auto& [area_id, attributes] : area.potentially_visible_areas)
    {
        writer.u32(area_id);
        writer.u8(attributes);
    }
    writer.u32(area.inherit_visibility_from_area_id);

    writer.u8(static_cast<std::uint8_t>(std::min<std::size_t>(area.approach_areas.size(), 255U)));
    for (const NavApproachInfo& approach : area.approach_areas)
    {
        writer.u32(approach.here_area_id);
        writer.u32(approach.previous_area_id);
        writer.u8(static_cast<std::uint8_t>(approach.previous_to_here));
        writer.u32(approach.next_area_id);
        writer.u8(static_cast<std::uint8_t>(approach.here_to_next));
    }
}

NavError read_area(BinaryReader& reader, std::uint32_t version, std::uint32_t sub_version, const std::vector<std::string>& places, NavArea& area)
{
    area.id = reader.u32();
    if (version <= 8)
    {
        area.attributes = reader.u8();
    }
    else if (version < 13)
    {
        area.attributes = reader.u16();
    }
    else
    {
        area.attributes = static_cast<std::uint32_t>(reader.s32());
    }

    area.north_west_corner = reader.vec3();
    area.south_east_corner = reader.vec3();
    area.north_east_z = reader.f32();
    area.south_west_z = reader.f32();

    for (std::size_t direction = 0; direction < 4; ++direction)
    {
        const std::uint32_t count = reader.u32();
        area.connections[direction].reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const std::uint32_t area_id = reader.u32();
            if (area_id != area.id)
            {
                area.connections[direction].push_back({area_id, -1.0F});
            }
        }
    }

    const std::uint8_t hiding_spot_count = reader.u8();
    for (std::uint8_t index = 0; index < hiding_spot_count; ++index)
    {
        NavHidingSpot spot;
        if (version == 1)
        {
            spot.id = index + 1U;
            spot.position = reader.vec3();
            spot.flags = NavHidingSpot::InCover;
        }
        else
        {
            spot.id = reader.u32();
            spot.position = reader.vec3();
            spot.flags = reader.u8();
        }
        area.hiding_spots.push_back(spot);
    }

    if (version < 15)
    {
        const std::uint8_t approach_count = reader.u8();
        for (std::uint8_t index = 0; index < approach_count; ++index)
        {
            (void)reader.u32();
            (void)reader.u32();
            (void)reader.u8();
            (void)reader.u32();
            (void)reader.u8();
        }
    }

    const std::uint32_t encounter_count = reader.u32();
    if (version < 3)
    {
        for (std::uint32_t index = 0; index < encounter_count; ++index)
        {
            (void)reader.u32();
            (void)reader.u32();
            (void)reader.vec3();
            (void)reader.vec3();
            const std::uint8_t spot_count = reader.u8();
            for (std::uint8_t spot_index = 0; spot_index < spot_count; ++spot_index)
            {
                (void)reader.f32();
                (void)reader.f32();
                (void)reader.f32();
                (void)reader.f32();
            }
        }
        return reader.valid() ? NavError::Ok : NavError::InvalidFile;
    }

    area.spot_encounters.reserve(encounter_count);
    for (std::uint32_t index = 0; index < encounter_count; ++index)
    {
        NavSpotEncounter encounter;
        encounter.from_area_id = reader.u32();
        encounter.from_direction = static_cast<NavDirection>(reader.u8());
        encounter.to_area_id = reader.u32();
        encounter.to_direction = static_cast<NavDirection>(reader.u8());
        const std::uint8_t spot_count = reader.u8();
        for (std::uint8_t spot_index = 0; spot_index < spot_count; ++spot_index)
        {
            encounter.spots.push_back({reader.u32(), static_cast<float>(reader.u8()) / 255.0F});
        }
        area.spot_encounters.push_back(std::move(encounter));
    }

    if (version < 5)
    {
        return reader.valid() ? NavError::Ok : NavError::InvalidFile;
    }

    const std::uint16_t place_entry = reader.u16();
    area.place = place_entry <= places.size() ? place_entry : kUndefinedPlace;

    if (version < 7)
    {
        return reader.valid() ? NavError::Ok : NavError::InvalidFile;
    }

    for (std::size_t direction = 0; direction < 2; ++direction)
    {
        const std::uint32_t count = reader.u32();
        area.ladders[direction].reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            area.ladders[direction].push_back(reader.u32());
        }
    }

    if (version < 8)
    {
        return reader.valid() ? NavError::Ok : NavError::InvalidFile;
    }

    for (float& occupy_time : area.earliest_occupy_time)
    {
        occupy_time = reader.f32();
    }

    if (version < 11)
    {
        return reader.valid() ? NavError::Ok : NavError::InvalidFile;
    }

    for (std::size_t corner = 0; corner < 4; ++corner)
    {
        (void)reader.f32();
    }

    if (version < 16)
    {
        return reader.valid() ? NavError::Ok : NavError::InvalidFile;
    }

    const std::uint32_t visible_area_count = reader.u32();
    area.potentially_visible_areas.reserve(visible_area_count);
    for (std::uint32_t index = 0; index < visible_area_count; ++index)
    {
        area.potentially_visible_areas.emplace_back(reader.u32(), reader.u8());
    }
    area.inherit_visibility_from_area_id = reader.u32();

    if (sub_version == 1)
    {
        const std::uint8_t approach_count = reader.u8();
        area.approach_areas.reserve(approach_count);
        for (std::uint8_t index = 0; index < approach_count; ++index)
        {
            NavApproachInfo approach;
            approach.here_area_id = reader.u32();
            approach.previous_area_id = reader.u32();
            approach.previous_to_here = static_cast<NavTraverseType>(reader.u8());
            approach.next_area_id = reader.u32();
            approach.here_to_next = static_cast<NavTraverseType>(reader.u8());
            area.approach_areas.push_back(approach);
        }
    }
    else if (sub_version > 1)
    {
        return NavError::BadFileVersion;
    }

    return reader.valid() ? NavError::Ok : NavError::InvalidFile;
}

void write_ladder(BinaryWriter& writer, const NavLadder& ladder)
{
    writer.u32(ladder.id);
    writer.f32(ladder.width);
    writer.vec3(ladder.top);
    writer.vec3(ladder.bottom);
    writer.f32(ladder.length);
    writer.u32(direction_index(ladder.direction));
    writer.u32(ladder.top_forward_area_id);
    writer.u32(ladder.top_left_area_id);
    writer.u32(ladder.top_right_area_id);
    writer.u32(ladder.top_behind_area_id);
    writer.u32(ladder.bottom_area_id);
}

NavLadder read_ladder(BinaryReader& reader, std::uint32_t version)
{
    NavLadder ladder;
    ladder.id = reader.u32();
    ladder.width = reader.f32();
    ladder.top = reader.vec3();
    ladder.bottom = reader.vec3();
    ladder.length = reader.f32();
    ladder.set_direction(static_cast<NavDirection>(reader.u32()));
    if (version == 6)
    {
        (void)reader.u8();
    }
    ladder.top_forward_area_id = reader.u32();
    ladder.top_left_area_id = reader.u32();
    ladder.top_right_area_id = reader.u32();
    ladder.top_behind_area_id = reader.u32();
    ladder.bottom_area_id = reader.u32();
    return ladder;
}
}

bool NavExtent::contains(Vec3 position) const
{
    return position.x >= lo.x && position.x <= hi.x && position.y >= lo.y && position.y <= hi.y && position.z >= lo.z &&
           position.z <= hi.z;
}

bool NavExtent::overlaps(const NavExtent& other) const
{
    return lo.x <= other.hi.x && hi.x >= other.lo.x && lo.y <= other.hi.y && hi.y >= other.lo.y && lo.z <= other.hi.z &&
           hi.z >= other.lo.z;
}

void NavArea::build(Vec3 north_west, Vec3 north_east, Vec3 south_east, Vec3 south_west)
{
    north_west_corner = north_west;
    south_east_corner = south_east;
    north_east_z = north_east.z;
    south_west_z = south_west.z;
}

void NavArea::connect_to(std::uint32_t area_id, NavDirection direction, float connection_length)
{
    if (area_id == 0 || area_id == id)
    {
        return;
    }

    std::vector<NavConnection>& list = connections[direction_index(direction)];
    const auto it = std::find_if(list.begin(), list.end(), [&](const NavConnection& connection) {
        return connection.area_id == area_id;
    });
    if (it == list.end())
    {
        list.push_back({area_id, connection_length});
    }
    else
    {
        it->length = connection_length;
    }
}

void NavArea::disconnect(std::uint32_t area_id)
{
    for (std::vector<NavConnection>& list : connections)
    {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const NavConnection& connection) {
            return connection.area_id == area_id;
        }), list.end());
    }
}

void NavArea::add_incoming_connection(std::uint32_t source_area_id, NavDirection incoming_edge_direction, float connection_length)
{
    if (source_area_id == 0 || source_area_id == id)
    {
        return;
    }

    std::vector<NavConnection>& list = incoming_connections[direction_index(incoming_edge_direction)];
    const auto it = std::find_if(list.begin(), list.end(), [&](const NavConnection& connection) {
        return connection.area_id == source_area_id;
    });
    if (it == list.end())
    {
        list.push_back({source_area_id, connection_length});
    }
}

void NavArea::add_ladder(std::uint32_t ladder_id, bool up)
{
    if (ladder_id == 0)
    {
        return;
    }

    std::vector<std::uint32_t>& list = ladders[ladder_direction_index(up)];
    if (std::find(list.begin(), list.end(), ladder_id) == list.end())
    {
        list.push_back(ladder_id);
    }
}

void NavArea::set_attributes(std::uint32_t bits)
{
    attributes |= bits;
}

void NavArea::remove_attributes(std::uint32_t bits)
{
    attributes &= ~bits;
}

void NavArea::mark_blocked(int team_id, bool is_blocked)
{
    if (team_id < 0)
    {
        for (bool& team_blocked : blocked)
        {
            team_blocked = is_blocked;
        }
        return;
    }
    blocked[team_slot(team_id)] = is_blocked;
}

void NavArea::increment_player_count(int team_id)
{
    std::uint8_t& count = player_count[team_slot(team_id)];
    if (count != 255U)
    {
        ++count;
    }
}

void NavArea::decrement_player_count(int team_id)
{
    std::uint8_t& count = player_count[team_slot(team_id)];
    if (count != 0U)
    {
        --count;
    }
}

void NavArea::increase_danger(int team_id, float amount)
{
    danger[team_slot(team_id)] += amount;
}

bool NavArea::has_attributes(std::uint32_t bits) const
{
    return (attributes & bits) != 0;
}

bool NavArea::is_blocked(int team_id, bool ignore_nav_blockers) const
{
    if (ignore_nav_blockers && (attributes & NavMeshNavBlocker) != 0)
    {
        return false;
    }

    if (team_id < 0)
    {
        return std::any_of(blocked.begin(), blocked.end(), [](bool value) {
            return value;
        }) || (attributes & (NavMeshBlockedPropDoor | NavMeshNavBlocker)) != 0;
    }

    return blocked[team_slot(team_id)] || (attributes & (NavMeshBlockedPropDoor | NavMeshNavBlocker)) != 0;
}

bool NavArea::has_avoidance_obstacle(float max_obstruction_height) const
{
    return avoidance_obstacle_height > max_obstruction_height;
}

bool NavArea::is_degenerate() const
{
    return north_west_corner.x >= south_east_corner.x || north_west_corner.y >= south_east_corner.y;
}

bool NavArea::is_flat(float epsilon) const
{
    const float z = north_west_corner.z;
    return std::fabs(north_east_z - z) <= epsilon && std::fabs(south_west_z - z) <= epsilon &&
           std::fabs(south_east_corner.z - z) <= epsilon;
}

bool NavArea::is_overlapping(Vec3 position, float tolerance) const
{
    return position.x >= north_west_corner.x - tolerance && position.x <= south_east_corner.x + tolerance &&
           position.y >= north_west_corner.y - tolerance && position.y <= south_east_corner.y + tolerance;
}

bool NavArea::is_overlapping(const NavArea& area) const
{
    return ranges_overlap(north_west_corner.x, south_east_corner.x, area.north_west_corner.x, area.south_east_corner.x) &&
           ranges_overlap(north_west_corner.y, south_east_corner.y, area.north_west_corner.y, area.south_east_corner.y);
}

bool NavArea::contains(Vec3 position, float beneath_limit) const
{
    if (!is_overlapping(position))
    {
        return false;
    }

    const float z = get_z(position);
    return position.z >= z - beneath_limit && position.z <= z + kNavHumanHeight;
}

bool NavArea::is_connected(std::uint32_t area_id, NavDirection direction) const
{
    const std::vector<NavConnection>& list = connections[direction_index(direction)];
    return std::any_of(list.begin(), list.end(), [&](const NavConnection& connection) {
        return connection.area_id == area_id;
    });
}

bool NavArea::is_connected(std::uint32_t area_id) const
{
    for (std::size_t direction = 0; direction < connections.size(); ++direction)
    {
        if (is_connected(area_id, static_cast<NavDirection>(direction)))
        {
            return true;
        }
    }
    return false;
}

std::size_t NavArea::adjacent_count(NavDirection direction) const
{
    return connections[direction_index(direction)].size();
}

float NavArea::get_z(Vec3 position) const
{
    const float dx = south_east_corner.x - north_west_corner.x;
    const float dy = south_east_corner.y - north_west_corner.y;
    if (std::fabs(dx) <= kEpsilon || std::fabs(dy) <= kEpsilon)
    {
        return north_west_corner.z;
    }

    const float tx = clamp_to_range((position.x - north_west_corner.x) / dx, 0.0F, 1.0F);
    const float ty = clamp_to_range((position.y - north_west_corner.y) / dy, 0.0F, 1.0F);
    const float north_z = north_west_corner.z + ((north_east_z - north_west_corner.z) * tx);
    const float south_z = south_west_z + ((south_east_corner.z - south_west_z) * tx);
    return north_z + ((south_z - north_z) * ty);
}

Vec3 NavArea::center() const
{
    const float x = (north_west_corner.x + south_east_corner.x) * 0.5F;
    const float y = (north_west_corner.y + south_east_corner.y) * 0.5F;
    return {x, y, get_z({x, y, 0.0F})};
}

Vec3 NavArea::corner(NavCorner nav_corner) const
{
    switch (nav_corner)
    {
    case NavCorner::NorthWest:
        return north_west_corner;
    case NavCorner::NorthEast:
        return {south_east_corner.x, north_west_corner.y, north_east_z};
    case NavCorner::SouthEast:
        return south_east_corner;
    case NavCorner::SouthWest:
        return {north_west_corner.x, south_east_corner.y, south_west_z};
    case NavCorner::Count:
        break;
    }
    return north_west_corner;
}

NavExtent NavArea::extent() const
{
    const float min_z = std::min({north_west_corner.z, north_east_z, south_east_corner.z, south_west_z});
    const float max_z = std::max({north_west_corner.z, north_east_z, south_east_corner.z, south_west_z});
    return {{north_west_corner.x, north_west_corner.y, min_z}, {south_east_corner.x, south_east_corner.y, max_z}};
}

Vec3 NavArea::closest_point(Vec3 position) const
{
    const float x = clamp_to_range(position.x, north_west_corner.x, south_east_corner.x);
    const float y = clamp_to_range(position.y, north_west_corner.y, south_east_corner.y);
    return {x, y, get_z({x, y, 0.0F})};
}

float NavArea::distance_squared_to_point(Vec3 position) const
{
    return length_squared(position - closest_point(position));
}

NavDirection NavArea::compute_direction(Vec3 point) const
{
    const Vec3 c = center();
    const float dx = point.x - c.x;
    const float dy = point.y - c.y;
    if (std::fabs(dx) > std::fabs(dy))
    {
        return dx >= 0.0F ? NavDirection::East : NavDirection::West;
    }
    return dy >= 0.0F ? NavDirection::South : NavDirection::North;
}

bool NavArea::compute_portal(const NavArea& to, NavDirection direction, Vec3& portal_center, float& half_width) const
{
    switch (direction)
    {
    case NavDirection::North:
    case NavDirection::South:
    {
        const float min_x = std::max(north_west_corner.x, to.north_west_corner.x);
        const float max_x = std::min(south_east_corner.x, to.south_east_corner.x);
        if (max_x < min_x)
        {
            return false;
        }
        portal_center.x = (min_x + max_x) * 0.5F;
        portal_center.y = direction == NavDirection::North ? north_west_corner.y : south_east_corner.y;
        half_width = (max_x - min_x) * 0.5F;
        break;
    }
    case NavDirection::East:
    case NavDirection::West:
    {
        const float min_y = std::max(north_west_corner.y, to.north_west_corner.y);
        const float max_y = std::min(south_east_corner.y, to.south_east_corner.y);
        if (max_y < min_y)
        {
            return false;
        }
        portal_center.x = direction == NavDirection::East ? south_east_corner.x : north_west_corner.x;
        portal_center.y = (min_y + max_y) * 0.5F;
        half_width = (max_y - min_y) * 0.5F;
        break;
    }
    case NavDirection::Count:
        return false;
    }

    portal_center.z = get_z(portal_center);
    return true;
}

bool NavArea::compute_closest_point_in_portal(const NavArea& to, NavDirection direction, Vec3 from_position, Vec3& close_position) const
{
    Vec3 portal;
    float half_width = 0.0F;
    if (!compute_portal(to, direction, portal, half_width))
    {
        return false;
    }

    close_position = portal;
    if (direction == NavDirection::North || direction == NavDirection::South)
    {
        close_position.x = clamp_to_range(from_position.x, portal.x - half_width, portal.x + half_width);
    }
    else
    {
        close_position.y = clamp_to_range(from_position.y, portal.y - half_width, portal.y + half_width);
    }
    close_position.z = get_z(close_position);
    return true;
}

float NavArea::danger_for_team(int team_id, float current_time) const
{
    const float elapsed = std::max(0.0F, current_time - danger_timestamp);
    return std::max(0.0F, danger[team_slot(team_id)] - (elapsed * danger_decay_rate()));
}

float NavArea::danger_decay_rate() const
{
    return 1.0F / 120.0F;
}

void NavLadder::set_direction(NavDirection next_direction)
{
    direction = next_direction;
    switch (direction)
    {
    case NavDirection::North:
        normal = {0.0F, -1.0F, 0.0F};
        break;
    case NavDirection::East:
        normal = {1.0F, 0.0F, 0.0F};
        break;
    case NavDirection::South:
        normal = {0.0F, 1.0F, 0.0F};
        break;
    case NavDirection::West:
        normal = {-1.0F, 0.0F, 0.0F};
        break;
    case NavDirection::Count:
        normal = {};
        break;
    }
}

void NavLadder::connect_to(const NavArea& area)
{
    const float center_z = (top.z + bottom.z) * 0.5F;
    if (area.center().z > center_z)
    {
        const Vec3 direction_to_area = area.center() - top;
        NavDirection side = NavDirection::North;
        if (std::fabs(direction_to_area.x) > std::fabs(direction_to_area.y))
        {
            side = direction_to_area.x > 0.0F ? NavDirection::East : NavDirection::West;
        }
        else
        {
            side = direction_to_area.y > 0.0F ? NavDirection::South : NavDirection::North;
        }

        if (side == direction)
        {
            top_behind_area_id = area.id;
        }
        else if (side == opposite_direction(direction))
        {
            top_forward_area_id = area.id;
        }
        else if (side == direction_left(direction))
        {
            top_left_area_id = area.id;
        }
        else
        {
            top_right_area_id = area.id;
        }
    }
    else
    {
        bottom_area_id = area.id;
    }
}

Vec3 NavLadder::position_at_height(float height) const
{
    if (std::fabs(top.z - bottom.z) <= kEpsilon)
    {
        return bottom;
    }

    const float t = clamp_to_range((height - bottom.z) / (top.z - bottom.z), 0.0F, 1.0F);
    return bottom + ((top - bottom) * t);
}

bool NavLadder::is_connected(std::uint32_t area_id, bool up) const
{
    if (up)
    {
        return top_forward_area_id == area_id || top_left_area_id == area_id || top_right_area_id == area_id;
    }
    return bottom_area_id == area_id;
}

void NavPath::clear()
{
    segments.clear();
    reached_goal = false;
}

bool NavPath::valid() const
{
    return !segments.empty();
}

Vec3 NavPath::endpoint() const
{
    return segments.empty() ? Vec3{} : segments.back().position;
}

float NavPath::length() const
{
    if (segments.size() < 2)
    {
        return 0.0F;
    }

    float result = 0.0F;
    for (std::size_t index = 1; index < segments.size(); ++index)
    {
        result += std::sqrt(length_squared(segments[index].position - segments[index - 1].position));
    }
    return result;
}

bool NavPath::is_at_end(Vec3 position, float tolerance) const
{
    return valid() && distance_2d(position, endpoint()) <= tolerance;
}

std::optional<Vec3> NavPath::point_along_path(float distance_along) const
{
    if (segments.empty())
    {
        return std::nullopt;
    }
    if (segments.size() == 1 || distance_along <= 0.0F)
    {
        return segments.front().position;
    }

    float accumulated = 0.0F;
    for (std::size_t index = 1; index < segments.size(); ++index)
    {
        const Vec3 from = segments[index - 1].position;
        const Vec3 to = segments[index].position;
        const float segment_length = std::sqrt(length_squared(to - from));
        if (accumulated + segment_length >= distance_along)
        {
            const float t = segment_length <= kEpsilon ? 0.0F : (distance_along - accumulated) / segment_length;
            return from + ((to - from) * t);
        }
        accumulated += segment_length;
    }

    return endpoint();
}

std::optional<std::size_t> NavPath::segment_index_along_path(float distance_along) const
{
    if (segments.empty())
    {
        return std::nullopt;
    }

    float accumulated = 0.0F;
    for (std::size_t index = 1; index < segments.size(); ++index)
    {
        accumulated += std::sqrt(length_squared(segments[index].position - segments[index - 1].position));
        if (accumulated >= distance_along)
        {
            return index - 1;
        }
    }
    return segments.size() - 1;
}

std::optional<Vec3> NavPath::closest_point(Vec3 world_position, std::size_t start_index, std::size_t end_index) const
{
    if (segments.empty())
    {
        return std::nullopt;
    }

    if (end_index == 0 || end_index >= segments.size())
    {
        end_index = segments.size() - 1;
    }
    start_index = std::min(start_index, end_index);

    std::optional<Vec3> best;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t index = start_index; index <= end_index; ++index)
    {
        const float dist = distance_2d_squared(world_position, segments[index].position);
        if (dist < best_distance)
        {
            best_distance = dist;
            best = segments[index].position;
        }
    }
    return best;
}

void NavMesh::clear()
{
    areas_.clear();
    ladders_.clear();
    places_.clear();
    area_index_by_id_.clear();
    ladder_index_by_id_.clear();
    loaded_ = false;
    analyzed_ = false;
    out_of_date_ = false;
    file_version_ = kNavCurrentVersion;
    sub_version_ = kCounterStrikeNavSubVersion;
}

void NavMesh::generate_from_world(const LoadedWorld& world)
{
    clear();
    std::vector<TriangleCluster> clusters;
    for (const WorldTriangle& triangle : world.mesh.collision_triangles)
    {
        if (triangle.normal.z < kWalkableNormalZ)
        {
            continue;
        }

        const TriangleCluster triangle_cluster = cluster_from_triangle(triangle);
        bool merged = false;
        for (TriangleCluster& cluster : clusters)
        {
            if (cluster_can_merge(cluster, triangle_cluster))
            {
                merge_cluster(cluster, triangle_cluster);
                merged = true;
                break;
            }
        }

        if (!merged)
        {
            clusters.push_back(triangle_cluster);
        }
    }

    for (const TriangleCluster& cluster : clusters)
    {
        if (cluster.max_x - cluster.min_x <= kEpsilon || cluster.max_y - cluster.min_y <= kEpsilon)
        {
            continue;
        }

        NavArea* area = create_area();
        const float z = (cluster.min_z + cluster.max_z) * 0.5F;
        area->build({cluster.min_x, cluster.min_y, z}, {cluster.max_x, cluster.min_y, z}, {cluster.max_x, cluster.max_y, z},
            {cluster.min_x, cluster.max_y, z});
    }

    connect_generated_areas();
    compute_generated_analysis();
    rebuild_indices();
    loaded_ = !areas_.empty();
    analyzed_ = loaded_;
    out_of_date_ = false;
}

NavError NavMesh::load_file(const std::filesystem::path& path, std::uintmax_t expected_bsp_size)
{
    clear();

    std::vector<unsigned char> bytes;
    try
    {
        bytes = read_binary_file(path);
    }
    catch (...)
    {
        return NavError::CannotAccessFile;
    }

    BinaryReader reader(bytes);
    if (reader.u32() != kNavMagicNumber)
    {
        return NavError::InvalidFile;
    }

    file_version_ = reader.u32();
    if (!reader.valid() || file_version_ > kNavCurrentVersion || file_version_ < 4)
    {
        return NavError::BadFileVersion;
    }

    sub_version_ = 0;
    if (file_version_ >= 10)
    {
        sub_version_ = reader.u32();
    }

    if (file_version_ >= 4)
    {
        const std::uint32_t saved_bsp_size = reader.u32();
        out_of_date_ = expected_bsp_size != 0 && saved_bsp_size != expected_bsp_size;
    }

    analyzed_ = file_version_ >= 14 ? reader.u8() != 0 : false;

    if (file_version_ >= 5)
    {
        const std::uint16_t place_count = reader.u16();
        places_.reserve(place_count);
        for (std::uint16_t index = 0; index < place_count; ++index)
        {
            places_.push_back(reader.source_string());
        }
        if (file_version_ > 11)
        {
            (void)reader.u8();
        }
    }

    const std::uint32_t area_count = reader.u32();
    if (area_count == 0 || !reader.valid())
    {
        return NavError::InvalidFile;
    }

    areas_.reserve(area_count);
    for (std::uint32_t index = 0; index < area_count; ++index)
    {
        NavArea area;
        const NavError area_result = read_area(reader, file_version_, sub_version_, places_, area);
        if (area_result != NavError::Ok)
        {
            return area_result;
        }
        areas_.push_back(std::move(area));
    }

    if (file_version_ >= 6)
    {
        const std::uint32_t ladder_count = reader.u32();
        ladders_.reserve(ladder_count);
        for (std::uint32_t index = 0; index < ladder_count; ++index)
        {
            ladders_.push_back(read_ladder(reader, file_version_));
        }
    }

    if (!reader.valid())
    {
        return NavError::InvalidFile;
    }

    rebuild_indices();
    loaded_ = true;
    return NavError::Ok;
}

bool NavMesh::save_file(const std::filesystem::path& path, std::uintmax_t bsp_size) const
{
    BinaryWriter writer;
    writer.u32(kNavMagicNumber);
    writer.u32(kNavCurrentVersion);
    writer.u32(kCounterStrikeNavSubVersion);
    writer.u32(static_cast<std::uint32_t>(std::min<std::uintmax_t>(bsp_size, std::numeric_limits<std::uint32_t>::max())));
    writer.u8(analyzed_ ? 1U : 0U);

    writer.u16(static_cast<std::uint16_t>(std::min<std::size_t>(places_.size(), 65535U)));
    for (const std::string& place : places_)
    {
        writer.source_string(place);
    }
    writer.u8(0);

    writer.u32(static_cast<std::uint32_t>(areas_.size()));
    for (const NavArea& area : areas_)
    {
        write_area(writer, area);
    }

    writer.u32(static_cast<std::uint32_t>(ladders_.size()));
    for (const NavLadder& ladder : ladders_)
    {
        write_ladder(writer, ladder);
    }

    return write_bytes(path, writer.bytes());
}

NavArea* NavMesh::create_area()
{
    NavArea area;
    area.id = allocate_area_id();
    areas_.push_back(std::move(area));
    rebuild_indices();
    return &areas_.back();
}

NavLadder* NavMesh::create_ladder()
{
    NavLadder ladder;
    ladder.id = allocate_ladder_id();
    ladders_.push_back(std::move(ladder));
    rebuild_indices();
    return &ladders_.back();
}

NavArea* NavMesh::area_by_id(std::uint32_t id)
{
    const auto it = area_index_by_id_.find(id);
    return it == area_index_by_id_.end() ? nullptr : &areas_[it->second];
}

const NavArea* NavMesh::area_by_id(std::uint32_t id) const
{
    const auto it = area_index_by_id_.find(id);
    return it == area_index_by_id_.end() ? nullptr : &areas_[it->second];
}

NavLadder* NavMesh::ladder_by_id(std::uint32_t id)
{
    const auto it = ladder_index_by_id_.find(id);
    return it == ladder_index_by_id_.end() ? nullptr : &ladders_[it->second];
}

const NavLadder* NavMesh::ladder_by_id(std::uint32_t id) const
{
    const auto it = ladder_index_by_id_.find(id);
    return it == ladder_index_by_id_.end() ? nullptr : &ladders_[it->second];
}

const NavArea* NavMesh::nav_area_at(Vec3 position, float beneath_limit) const
{
    const NavArea* best = nullptr;
    float best_z = -std::numeric_limits<float>::max();
    for (const NavArea& area : areas_)
    {
        if (!area.contains(position, beneath_limit))
        {
            continue;
        }
        const float z = area.get_z(position);
        if (z <= position.z + 0.5F && z > best_z)
        {
            best = &area;
            best_z = z;
        }
    }
    return best;
}

const NavArea* NavMesh::nearest_nav_area(Vec3 position, bool any_z, float max_distance, bool check_ground) const
{
    const float max_distance_sq = max_distance * max_distance;
    const NavArea* best = nullptr;
    float best_distance = max_distance_sq;
    for (const NavArea& area : areas_)
    {
        const Vec3 close = area.closest_point(position);
        if (!any_z && check_ground && close.z > position.z + kNavStepHeight)
        {
            continue;
        }

        const float dist = any_z ? distance_2d_squared(position, close) : length_squared(position - close);
        if (dist < best_distance)
        {
            best = &area;
            best_distance = dist;
        }
    }
    return best;
}

std::optional<float> NavMesh::ground_height(Vec3 position) const
{
    if (const NavArea* area = nav_area_at(position))
    {
        return area->get_z(position);
    }
    if (const NavArea* area = nearest_nav_area(position, false, 120.0F, true))
    {
        return area->get_z(area->closest_point(position));
    }
    return std::nullopt;
}

NavPath NavMesh::build_path(const NavPathQuery& query)
{
    NavPath path;
    if (areas_.empty())
    {
        return path;
    }

    rebuild_indices();
    NavArea* start_area = nullptr;
    if (const NavArea* found = nearest_nav_area(query.start + Vec3{0.0F, 0.0F, 1.0F}))
    {
        start_area = area_by_id(found->id);
    }
    NavArea* goal_area = nullptr;
    if (const NavArea* found = nav_area_at(query.goal))
    {
        goal_area = area_by_id(found->id);
    }

    if (start_area == nullptr)
    {
        return path;
    }

    for (NavArea& area : areas_)
    {
        area.parent_id = 0;
        area.parent_how = NavTraverseType::Count;
        area.total_cost = 0.0F;
        area.cost_so_far = std::numeric_limits<float>::max();
        area.path_length_so_far = 0.0F;
    }

    if (goal_area == nullptr)
    {
        if (const NavArea* nearest_goal = nearest_nav_area(query.goal, true, 10000.0F, false))
        {
            goal_area = area_by_id(nearest_goal->id);
        }
    }
    if (goal_area == nullptr)
    {
        return path;
    }

    if (start_area == goal_area)
    {
        path.segments.push_back({start_area->id, NavTraverseType::Count, query.start, 0});
        path.segments.push_back({start_area->id, NavTraverseType::Count, query.goal, 0});
        path.reached_goal = true;
        return path;
    }

    struct OpenEntry
    {
        float cost = 0.0F;
        std::uint32_t area_id = 0;

        bool operator<(const OpenEntry& other) const
        {
            return cost > other.cost;
        }
    };

    std::priority_queue<OpenEntry> open;
    start_area->cost_so_far = connection_cost(*start_area, nullptr, nullptr, -1.0F);
    start_area->total_cost = length(start_area->center() - query.goal);
    open.push({start_area->total_cost, start_area->id});

    std::uint32_t closest_area_id = start_area->id;
    float closest_distance = start_area->total_cost;
    std::vector<std::uint32_t> closed;

    while (!open.empty())
    {
        const OpenEntry entry = open.top();
        open.pop();

        NavArea* area = area_by_id(entry.area_id);
        if (area == nullptr || entry.cost > area->total_cost + kEpsilon)
        {
            continue;
        }
        if (area->is_blocked(query.team_id, query.ignore_nav_blockers))
        {
            continue;
        }
        if (area->id == goal_area->id || area->contains(query.goal))
        {
            closest_area_id = area->id;
            path.reached_goal = true;
            break;
        }

        closed.push_back(area->id);

        const auto consider = [&](NavArea& next_area, NavTraverseType how, const NavLadder* ladder, float edge_length) {
            if (next_area.id == area->id || next_area.is_blocked(query.team_id, query.ignore_nav_blockers))
            {
                return;
            }

            const float new_cost = connection_cost(next_area, area, ladder, edge_length);
            if (new_cost < 0.0F)
            {
                return;
            }

            const float delta_length = edge_length > 0.0F ? edge_length : length(next_area.center() - area->center());
            const float new_path_length = area->path_length_so_far + delta_length;
            if (query.max_path_length > 0.0F && new_path_length > query.max_path_length)
            {
                return;
            }

            if (new_cost >= next_area.cost_so_far)
            {
                return;
            }

            const float remaining = length(next_area.center() - query.goal);
            if (remaining < closest_distance)
            {
                closest_distance = remaining;
                closest_area_id = next_area.id;
            }

            next_area.cost_so_far = new_cost;
            next_area.total_cost = new_cost + remaining;
            next_area.path_length_so_far = new_path_length;
            next_area.parent_id = area->id;
            next_area.parent_how = how;
            open.push({next_area.total_cost, next_area.id});
        };

        for (std::size_t direction = 0; direction < 4; ++direction)
        {
            const NavTraverseType how = traverse_from_direction(static_cast<NavDirection>(direction));
            for (const NavConnection& connection : area->connections[direction])
            {
                if (NavArea* next_area = area_by_id(connection.area_id))
                {
                    consider(*next_area, how, nullptr, connection.length);
                }
            }
        }

        for (std::uint32_t ladder_id : area->ladders[0])
        {
            const NavLadder* ladder = ladder_by_id(ladder_id);
            if (ladder == nullptr)
            {
                continue;
            }
            const std::array<std::uint32_t, 3> top_areas{ladder->top_forward_area_id, ladder->top_left_area_id, ladder->top_right_area_id};
            for (std::uint32_t top_area_id : top_areas)
            {
                if (NavArea* next_area = area_by_id(top_area_id))
                {
                    consider(*next_area, NavTraverseType::GoLadderUp, ladder, ladder->length);
                }
            }
        }
        for (std::uint32_t ladder_id : area->ladders[1])
        {
            const NavLadder* ladder = ladder_by_id(ladder_id);
            if (ladder == nullptr)
            {
                continue;
            }
            if (NavArea* next_area = area_by_id(ladder->bottom_area_id))
            {
                consider(*next_area, NavTraverseType::GoLadderDown, ladder, ladder->length);
            }
        }
    }

    NavArea* closest = area_by_id(closest_area_id);
    if (closest == nullptr)
    {
        return path;
    }

    std::vector<std::uint32_t> reverse_ids;
    for (NavArea* area = closest; area != nullptr; area = area->parent_id != 0 ? area_by_id(area->parent_id) : nullptr)
    {
        reverse_ids.push_back(area->id);
        if (area->id == start_area->id)
        {
            break;
        }
        if (reverse_ids.size() > areas_.size())
        {
            path.clear();
            return path;
        }
    }
    std::reverse(reverse_ids.begin(), reverse_ids.end());
    if (reverse_ids.empty() || reverse_ids.front() != start_area->id)
    {
        path.clear();
        return path;
    }

    for (std::size_t index = 0; index < reverse_ids.size(); ++index)
    {
        NavArea* area = area_by_id(reverse_ids[index]);
        if (area == nullptr)
        {
            path.clear();
            return path;
        }

        Vec3 position = index == 0 ? query.start : area->center();
        if (index > 0)
        {
            const NavArea* previous = area_by_id(reverse_ids[index - 1]);
            if (previous != nullptr)
            {
                Vec3 portal;
                float half_width = 0.0F;
                const NavDirection direction = previous->compute_direction(area->center());
                if (previous->compute_portal(*area, direction, portal, half_width))
                {
                    position = portal;
                }
            }
        }

        path.segments.push_back({area->id, area->parent_how, position, 0});
    }

    if (!path.segments.empty())
    {
        path.segments.push_back({closest->id, NavTraverseType::Count, path.reached_goal ? query.goal : closest->center(), 0});
    }
    return path;
}

float NavMesh::travel_distance(Vec3 start, Vec3 goal, float max_path_length)
{
    NavPath path = build_path({start, goal, max_path_length});
    return path.reached_goal ? path.length() : -1.0F;
}

const std::vector<NavArea>& NavMesh::areas() const
{
    return areas_;
}

std::vector<NavArea>& NavMesh::areas()
{
    return areas_;
}

const std::vector<NavLadder>& NavMesh::ladders() const
{
    return ladders_;
}

const std::vector<std::string>& NavMesh::places() const
{
    return places_;
}

bool NavMesh::loaded() const
{
    return loaded_;
}

bool NavMesh::analyzed() const
{
    return analyzed_;
}

bool NavMesh::out_of_date() const
{
    return out_of_date_;
}

std::uint32_t NavMesh::file_version() const
{
    return file_version_;
}

std::uint32_t NavMesh::sub_version() const
{
    return sub_version_;
}

void NavMesh::set_analyzed(bool analyzed)
{
    analyzed_ = analyzed;
}

void NavMesh::set_out_of_date(bool out_of_date)
{
    out_of_date_ = out_of_date;
}

void NavMesh::rebuild_indices()
{
    area_index_by_id_.clear();
    ladder_index_by_id_.clear();
    for (std::size_t index = 0; index < areas_.size(); ++index)
    {
        area_index_by_id_[areas_[index].id] = index;
    }
    for (std::size_t index = 0; index < ladders_.size(); ++index)
    {
        ladder_index_by_id_[ladders_[index].id] = index;
    }

    for (NavArea& area : areas_)
    {
        for (std::vector<NavConnection>& incoming : area.incoming_connections)
        {
            incoming.clear();
        }
    }

    for (NavArea& area : areas_)
    {
        for (std::size_t direction = 0; direction < 4; ++direction)
        {
            for (NavConnection& connection : area.connections[direction])
            {
                const NavArea* connected = area_by_id(connection.area_id);
                if (connected == nullptr)
                {
                    continue;
                }
                connection.length = length(connected->center() - area.center());
                const NavDirection reverse = opposite_direction(static_cast<NavDirection>(direction));
                if (!connected->is_connected(area.id, reverse))
                {
                    if (NavArea* destination = area_by_id(connected->id))
                    {
                        destination->add_incoming_connection(area.id, reverse, connection.length);
                    }
                }
            }
        }

        for (NavSpotEncounter& encounter : area.spot_encounters)
        {
            const NavArea* from_area = area_by_id(encounter.from_area_id);
            const NavArea* to_area = area_by_id(encounter.to_area_id);
            if (from_area != nullptr && to_area != nullptr)
            {
                float half_width = 0.0F;
                (void)from_area->compute_portal(*to_area, encounter.to_direction, encounter.path_to, half_width);
                (void)from_area->compute_portal(*to_area, encounter.from_direction, encounter.path_from, half_width);
                encounter.path_from.z = from_area->get_z(encounter.path_from) + kNavHalfHumanHeight;
                encounter.path_to.z = to_area->get_z(encounter.path_to) + kNavHalfHumanHeight;
            }
        }
    }
}

void NavMesh::connect_generated_areas()
{
    for (NavArea& area : areas_)
    {
        for (std::vector<NavConnection>& connections : area.connections)
        {
            connections.clear();
        }
    }

    for (std::size_t left_index = 0; left_index < areas_.size(); ++left_index)
    {
        for (std::size_t right_index = left_index + 1; right_index < areas_.size(); ++right_index)
        {
            NavArea& a = areas_[left_index];
            NavArea& b = areas_[right_index];
            std::optional<NavDirection> a_to_b;
            std::optional<NavDirection> b_to_a;

            if (close_enough(a.south_east_corner.x, b.north_west_corner.x, kNavGenerationStepSize) &&
                ranges_overlap(a.north_west_corner.y, a.south_east_corner.y, b.north_west_corner.y, b.south_east_corner.y,
                    kNavGenerationStepSize))
            {
                a_to_b = NavDirection::East;
                b_to_a = NavDirection::West;
            }
            else if (close_enough(a.north_west_corner.x, b.south_east_corner.x, kNavGenerationStepSize) &&
                     ranges_overlap(a.north_west_corner.y, a.south_east_corner.y, b.north_west_corner.y, b.south_east_corner.y,
                         kNavGenerationStepSize))
            {
                a_to_b = NavDirection::West;
                b_to_a = NavDirection::East;
            }
            else if (close_enough(a.south_east_corner.y, b.north_west_corner.y, kNavGenerationStepSize) &&
                     ranges_overlap(a.north_west_corner.x, a.south_east_corner.x, b.north_west_corner.x, b.south_east_corner.x,
                         kNavGenerationStepSize))
            {
                a_to_b = NavDirection::South;
                b_to_a = NavDirection::North;
            }
            else if (close_enough(a.north_west_corner.y, b.south_east_corner.y, kNavGenerationStepSize) &&
                     ranges_overlap(a.north_west_corner.x, a.south_east_corner.x, b.north_west_corner.x, b.south_east_corner.x,
                         kNavGenerationStepSize))
            {
                a_to_b = NavDirection::North;
                b_to_a = NavDirection::South;
            }

            if (!a_to_b || !b_to_a)
            {
                continue;
            }

            const float z_delta = b.center().z - a.center().z;
            if (z_delta <= kNavClimbUpHeight && z_delta >= -kNavDeathDrop)
            {
                a.connect_to(b.id, *a_to_b);
                if (z_delta > kNavStepHeight)
                {
                    b.set_attributes(NavMeshJump);
                }
                if (z_delta < -kNavCliffHeight)
                {
                    a.set_attributes(NavMeshCliff);
                }
            }
            if (-z_delta <= kNavClimbUpHeight && -z_delta >= -kNavDeathDrop)
            {
                b.connect_to(a.id, *b_to_a);
                if (-z_delta > kNavStepHeight)
                {
                    a.set_attributes(NavMeshJump);
                }
                if (-z_delta < -kNavCliffHeight)
                {
                    b.set_attributes(NavMeshCliff);
                }
            }
        }
    }
}

void NavMesh::compute_generated_analysis()
{
    std::uint32_t next_spot_id = allocate_hiding_spot_id();
    for (NavArea& area : areas_)
    {
        area.hiding_spots.clear();
        if ((area.attributes & NavMeshDontHide) == 0 && area.south_east_corner.x - area.north_west_corner.x >= kNavHalfHumanWidth * 2.0F &&
            area.south_east_corner.y - area.north_west_corner.y >= kNavHalfHumanWidth * 2.0F)
        {
            const std::array<Vec3, 4> corners{
                Vec3{area.north_west_corner.x + kNavHalfHumanWidth, area.north_west_corner.y + kNavHalfHumanWidth, 0.0F},
                Vec3{area.south_east_corner.x - kNavHalfHumanWidth, area.north_west_corner.y + kNavHalfHumanWidth, 0.0F},
                Vec3{area.south_east_corner.x - kNavHalfHumanWidth, area.south_east_corner.y - kNavHalfHumanWidth, 0.0F},
                Vec3{area.north_west_corner.x + kNavHalfHumanWidth, area.south_east_corner.y - kNavHalfHumanWidth, 0.0F},
            };
            for (Vec3 corner : corners)
            {
                corner.z = area.get_z(corner);
                area.hiding_spots.push_back({next_spot_id++, corner, NavHidingSpot::InCover, true});
            }
        }

        area.approach_areas.clear();
        std::vector<std::pair<std::uint32_t, NavTraverseType>> adjacent;
        for (std::size_t direction = 0; direction < 4; ++direction)
        {
            for (const NavConnection& connection : area.connections[direction])
            {
                adjacent.emplace_back(connection.area_id, traverse_from_direction(static_cast<NavDirection>(direction)));
            }
        }
        for (std::size_t index = 0; index + 1 < adjacent.size() && area.approach_areas.size() < 16; ++index)
        {
            area.approach_areas.push_back({area.id, adjacent[index].first, adjacent[index].second, adjacent[index + 1].first, adjacent[index + 1].second});
        }
        area.potentially_visible_areas.clear();
        for (const NavArea& other : areas_)
        {
            if (other.id != area.id && distance_2d(area.center(), other.center()) < 4096.0F)
            {
                area.potentially_visible_areas.emplace_back(other.id, kCompletelyVisible);
            }
        }
    }
}

void NavMesh::compress_ids()
{
    std::uint32_t next_id = 1;
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    for (NavArea& area : areas_)
    {
        remap[area.id] = next_id;
        area.id = next_id++;
    }

    for (NavArea& area : areas_)
    {
        for (std::vector<NavConnection>& list : area.connections)
        {
            for (NavConnection& connection : list)
            {
                connection.area_id = remap[connection.area_id];
            }
        }
        for (NavSpotEncounter& encounter : area.spot_encounters)
        {
            encounter.from_area_id = remap[encounter.from_area_id];
            encounter.to_area_id = remap[encounter.to_area_id];
        }
        for (NavApproachInfo& approach : area.approach_areas)
        {
            approach.here_area_id = remap[approach.here_area_id];
            approach.previous_area_id = remap[approach.previous_area_id];
            approach.next_area_id = remap[approach.next_area_id];
        }
    }
    rebuild_indices();
}

std::uint32_t NavMesh::allocate_area_id() const
{
    std::uint32_t result = 1;
    for (const NavArea& area : areas_)
    {
        result = std::max(result, area.id + 1);
    }
    return result;
}

std::uint32_t NavMesh::allocate_ladder_id() const
{
    std::uint32_t result = 1;
    for (const NavLadder& ladder : ladders_)
    {
        result = std::max(result, ladder.id + 1);
    }
    return result;
}

std::uint32_t NavMesh::allocate_hiding_spot_id() const
{
    std::uint32_t result = 1;
    for (const NavArea& area : areas_)
    {
        for (const NavHidingSpot& spot : area.hiding_spots)
        {
            result = std::max(result, spot.id + 1);
        }
    }
    return result;
}

void NavPathFollower::reset()
{
    segment_index_ = 0;
    stuck_position_ = {};
    stuck_duration_ = 0.0F;
}

void NavPathFollower::set_path(const NavPath* path)
{
    path_ = path;
    reset();
}

std::optional<Vec3> NavPathFollower::update(Vec3 position, float ahead_range)
{
    if (path_ == nullptr || !path_->valid())
    {
        return std::nullopt;
    }

    if (distance_2d(position, stuck_position_) < 1.0F)
    {
        stuck_duration_ += 1.0F;
    }
    else
    {
        stuck_position_ = position;
        stuck_duration_ = 0.0F;
    }

    while (segment_index_ + 1 < path_->segments.size() &&
           distance_2d(position, path_->segments[segment_index_].position) < std::max(16.0F, ahead_range * 0.25F))
    {
        ++segment_index_;
    }

    const float distance_to_follow = std::min(path_->length(), ahead_range + static_cast<float>(segment_index_) * ahead_range);
    return path_->point_along_path(distance_to_follow).value_or(path_->endpoint());
}

bool NavPathFollower::stuck() const
{
    return stuck_duration_ > 5.0F;
}

float NavPathFollower::stuck_duration() const
{
    return stuck_duration_;
}

void NavigationSystem::clear()
{
    mesh_.clear();
    observed_world_generation_ = 0;
}

void NavigationSystem::sync_world(const LoadedWorld* world, const ContentFileSystem& filesystem)
{
    if (world == nullptr)
    {
        mesh_.clear();
        observed_world_generation_ = 0;
        return;
    }

    const std::uint64_t world_generation = (static_cast<std::uint64_t>(world->checksum) << 32U) ^ world->map_revision ^ world->byte_size;
    if (observed_world_generation_ == world_generation)
    {
        return;
    }

    observed_world_generation_ = world_generation;
    const NavError load_result = load_for_world(*world, filesystem);
    if (load_result == NavError::CannotAccessFile)
    {
        (void)generate_from_world(*world);
    }
}

bool NavigationSystem::generate_from_world(const LoadedWorld& world)
{
    mesh_.generate_from_world(world);
    return mesh_.loaded();
}

NavError NavigationSystem::load_for_world(const LoadedWorld& world, const ContentFileSystem& filesystem)
{
    const std::filesystem::path relative = nav_relative_path_for_world(world);
    const std::optional<std::filesystem::path> resolved = filesystem.resolve(relative, "GAME");
    if (!resolved)
    {
        mesh_.clear();
        return NavError::CannotAccessFile;
    }

    const NavError result = mesh_.load_file(*resolved, world.byte_size);
    if (result == NavError::Ok)
    {
        log_info("loaded navigation mesh '{}' with {} areas", relative.generic_string(), mesh_.areas().size());
    }
    else
    {
        log_warning("failed to load navigation mesh '{}': {}", relative.generic_string(), to_string(result));
    }
    return result;
}

bool NavigationSystem::save_for_world(const LoadedWorld& world, const ContentFileSystem& filesystem) const
{
    const std::optional<std::filesystem::path> path = writable_nav_path_for_world(world, filesystem);
    if (!path)
    {
        return false;
    }
    return mesh_.save_file(*path, world.byte_size);
}

NavMesh& NavigationSystem::mesh()
{
    return mesh_;
}

const NavMesh& NavigationSystem::mesh() const
{
    return mesh_;
}

void NavigationSystem::register_variables(ConsoleVariables& variables)
{
    variables.register_variable("nav_edit", "0", "Enables navigation mesh edit/debug display mode.");
    variables.register_variable("nav_show_area_ids", "0", "Shows navigation area IDs in debug output.");
    variables.register_variable("nav_show_danger", "0", "Shows current navigation danger values.");
}

void NavigationSystem::register_commands(CommandRegistry& commands)
{
    commands.register_command("nav_generate", "Generate a navigation mesh from the loaded world collision.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.navigation == nullptr || context.world == nullptr || context.world->current_world() == nullptr)
        {
            log_warning("nav_generate failed: no world is loaded");
            return;
        }
        if (context.navigation->generate_from_world(*context.world->current_world()))
        {
            log_info("generated navigation mesh with {} areas", context.navigation->mesh().areas().size());
        }
        else
        {
            log_warning("nav_generate produced no walkable areas");
        }
    });

    commands.register_command("nav_load", "Load the current map's .nav file.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.navigation == nullptr || context.world == nullptr || context.filesystem == nullptr || context.world->current_world() == nullptr)
        {
            log_warning("nav_load failed: no world is loaded");
            return;
        }
        const NavError result = context.navigation->load_for_world(*context.world->current_world(), *context.filesystem);
        log_info("nav_load: {}", to_string(result));
    });

    commands.register_command("nav_save", "Save the active navigation mesh as maps/<map>.nav.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.navigation == nullptr || context.world == nullptr || context.filesystem == nullptr || context.world->current_world() == nullptr)
        {
            log_warning("nav_save failed: no world is loaded");
            return;
        }
        if (!context.navigation->mesh().loaded())
        {
            log_warning("nav_save failed: no navigation mesh is loaded");
            return;
        }
        log_info("nav_save: {}", context.navigation->save_for_world(*context.world->current_world(), *context.filesystem) ? "ok" : "failed");
    });

    commands.register_command("nav_analyze", "Rebuild generated hiding, approach, visibility, and encounter analysis.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.navigation == nullptr || !context.navigation->mesh().loaded())
        {
            log_warning("nav_analyze failed: no navigation mesh is loaded");
            return;
        }
        context.navigation->mesh().compute_generated_analysis();
        context.navigation->mesh().set_analyzed(true);
        log_info("nav_analyze complete");
    });

    commands.register_command("nav_path_test", "Build and print a path: nav_path_test sx sy sz gx gy gz.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (context.navigation == nullptr || !context.navigation->mesh().loaded() || invocation.args.size() != 6)
        {
            log_warning("usage: nav_path_test sx sy sz gx gy gz");
            return;
        }

        std::array<float, 6> values{};
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            try
            {
                values[index] = std::stof(invocation.args[index]);
            }
            catch (...)
            {
                log_warning("nav_path_test failed: invalid coordinate '{}'", invocation.args[index]);
                return;
            }
        }

        NavPath path = context.navigation->mesh().build_path({{values[0], values[1], values[2]}, {values[3], values[4], values[5]}});
        log_info("nav_path_test reached={} segments={} length={}", path.reached_goal ? "yes" : "no", path.segments.size(), path.length());
    });
}

NavDirection opposite_direction(NavDirection direction)
{
    switch (direction)
    {
    case NavDirection::North:
        return NavDirection::South;
    case NavDirection::East:
        return NavDirection::West;
    case NavDirection::South:
        return NavDirection::North;
    case NavDirection::West:
        return NavDirection::East;
    case NavDirection::Count:
        break;
    }
    return NavDirection::North;
}

NavDirection direction_left(NavDirection direction)
{
    switch (direction)
    {
    case NavDirection::North:
        return NavDirection::West;
    case NavDirection::East:
        return NavDirection::North;
    case NavDirection::South:
        return NavDirection::East;
    case NavDirection::West:
        return NavDirection::South;
    case NavDirection::Count:
        break;
    }
    return NavDirection::North;
}

NavDirection direction_right(NavDirection direction)
{
    switch (direction)
    {
    case NavDirection::North:
        return NavDirection::East;
    case NavDirection::East:
        return NavDirection::South;
    case NavDirection::South:
        return NavDirection::West;
    case NavDirection::West:
        return NavDirection::North;
    case NavDirection::Count:
        break;
    }
    return NavDirection::North;
}

NavTraverseType traverse_from_direction(NavDirection direction)
{
    switch (direction)
    {
    case NavDirection::North:
        return NavTraverseType::GoNorth;
    case NavDirection::East:
        return NavTraverseType::GoEast;
    case NavDirection::South:
        return NavTraverseType::GoSouth;
    case NavDirection::West:
        return NavTraverseType::GoWest;
    case NavDirection::Count:
        break;
    }
    return NavTraverseType::Count;
}

std::string_view to_string(NavError error)
{
    switch (error)
    {
    case NavError::Ok:
        return "ok";
    case NavError::CannotAccessFile:
        return "cannot-access-file";
    case NavError::InvalidFile:
        return "invalid-file";
    case NavError::BadFileVersion:
        return "bad-file-version";
    case NavError::FileOutOfDate:
        return "file-out-of-date";
    case NavError::CorruptData:
        return "corrupt-data";
    }
    return "unknown";
}

std::string_view to_string(NavDirection direction)
{
    switch (direction)
    {
    case NavDirection::North:
        return "north";
    case NavDirection::East:
        return "east";
    case NavDirection::South:
        return "south";
    case NavDirection::West:
        return "west";
    case NavDirection::Count:
        return "count";
    }
    return "unknown";
}

std::string_view to_string(NavTraverseType traverse)
{
    switch (traverse)
    {
    case NavTraverseType::GoNorth:
        return "north";
    case NavTraverseType::GoEast:
        return "east";
    case NavTraverseType::GoSouth:
        return "south";
    case NavTraverseType::GoWest:
        return "west";
    case NavTraverseType::GoLadderUp:
        return "ladder-up";
    case NavTraverseType::GoLadderDown:
        return "ladder-down";
    case NavTraverseType::GoJump:
        return "jump";
    case NavTraverseType::GoElevatorUp:
        return "elevator-up";
    case NavTraverseType::GoElevatorDown:
        return "elevator-down";
    case NavTraverseType::Count:
        return "count";
    }
    return "unknown";
}
}
