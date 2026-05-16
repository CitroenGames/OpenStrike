#include "openstrike/world/world.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_model.hpp"
#include "openstrike/world/source_displacements.hpp"
#include "openstrike/world/source_static_props.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kBspIdent = 0x50534256;
constexpr std::size_t kBspLumpCount = 64;
constexpr std::size_t kBspHeaderSize = 8 + (kBspLumpCount * 16) + 4;
constexpr std::size_t kEntitiesLump = 0;
constexpr std::size_t kPlanesLump = 1;
constexpr std::size_t kTexDataLump = 2;
constexpr std::size_t kVerticesLump = 3;
constexpr std::size_t kNodesLump = 5;
constexpr std::size_t kTexInfoLump = 6;
constexpr std::size_t kFacesLump = 7;
constexpr std::size_t kLightingLump = 8;
constexpr std::size_t kLeavesLump = 10;
constexpr std::size_t kEdgesLump = 12;
constexpr std::size_t kSurfEdgesLump = 13;
constexpr std::size_t kModelsLump = 14;
constexpr std::size_t kLeafBrushesLump = 17;
constexpr std::size_t kBrushesLump = 18;
constexpr std::size_t kBrushSidesLump = 19;
constexpr std::size_t kPakFileLump = 40;
constexpr std::size_t kTexDataStringDataLump = 43;
constexpr std::size_t kTexDataStringTableLump = 44;
constexpr std::size_t kLightingHdrLump = 53;
constexpr std::size_t kFacesHdrLump = 58;
constexpr std::size_t kMapFlagsLump = 59;
constexpr std::uint32_t kSurfaceSky2D = 0x0002;
constexpr std::uint32_t kSurfaceSky = 0x0004;
constexpr std::uint32_t kSurfaceWarp = 0x0008;
constexpr std::uint32_t kSurfaceNoDraw = 0x0080;
constexpr std::uint32_t kSurfaceSkip = 0x0200;
constexpr std::uint8_t kSolidNone = 0;
constexpr std::uint8_t kSolidBbox = 2;
constexpr std::uint8_t kSolidVPhysics = 6;
constexpr std::uint32_t kSurfaceBumpLight = 0x0800;
constexpr std::uint32_t kContentsSolid = 0x00000001;
constexpr std::uint32_t kContentsWindow = 0x00000002;
constexpr std::uint32_t kContentsGrate = 0x00000008;
constexpr std::uint32_t kContentsWater = 0x00000020;
constexpr std::uint32_t kContentsMoveable = 0x00004000;
constexpr std::uint32_t kContentsPlayerClip = 0x00010000;
constexpr std::uint32_t kContentsMonsterClip = 0x00020000;
constexpr std::uint32_t kContentsMonster = 0x02000000;
constexpr std::uint32_t kMaskSolid = kContentsSolid | kContentsMoveable | kContentsWindow | kContentsMonster | kContentsGrate;
constexpr std::uint32_t kMaskPlayerSolid = kMaskSolid | kContentsPlayerClip;
constexpr std::uint32_t kBspWorldStaticCollisionMask = kMaskSolid | kContentsPlayerClip | kContentsMonsterClip;
constexpr std::uint32_t kMapFlagLightstylesWithCsm = 0x00000040;
constexpr std::uint8_t kUnusedLightStyle = 255;
constexpr std::size_t kMaxLightStyles = 4;
constexpr std::size_t kBumpLightmapCount = 4;
constexpr std::uint32_t kMaxLightmapAtlasDimension = 8192;

struct BspLump
{
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t version = 0;
};

struct ParsedEntity
{
    std::unordered_map<std::string, std::string> properties;
};

struct BspEdge
{
    std::uint16_t vertices[2]{};
};

struct BspPlane
{
    Vec3 normal;
    float dist = 0.0F;
    std::int32_t type = 0;
};

struct BspNode
{
    std::array<std::int32_t, 2> children{};
};

struct BspLeaf
{
    std::uint32_t contents = 0;
    std::uint16_t first_leaf_brush = 0;
    std::uint16_t leaf_brush_count = 0;
};

struct BspModel
{
    Vec3 mins;
    Vec3 maxs;
    Vec3 origin;
    std::int32_t head_node = 0;
    std::int32_t first_face = 0;
    std::int32_t face_count = 0;
};

struct BspBrushSide
{
    std::uint16_t plane = 0;
    std::int16_t texinfo = -1;
    std::int16_t dispinfo = -1;
    std::uint8_t bevel = 0;
    std::uint8_t thin = 0;
};

struct BspBrush
{
    std::int32_t first_side = 0;
    std::int32_t side_count = 0;
    std::uint32_t contents = 0;
};

struct BspTexInfo
{
    std::array<std::array<float, 4>, 2> texture_vecs{};
    std::array<std::array<float, 4>, 2> lightmap_vecs{};
    std::uint32_t flags = 0;
    std::int32_t texdata = -1;
};

struct BspTexData
{
    std::string name;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
};

struct BspFace
{
    std::uint16_t plane = 0;
    std::uint8_t side = 0;
    std::int32_t first_edge = 0;
    std::int16_t edge_count = 0;
    std::int16_t texinfo = -1;
    std::int16_t dispinfo = -1;
    std::array<std::uint8_t, kMaxLightStyles> styles{kUnusedLightStyle, kUnusedLightStyle, kUnusedLightStyle, kUnusedLightStyle};
    std::int32_t light_offset = -1;
    std::array<std::int32_t, 2> lightmap_mins{};
    std::array<std::int32_t, 2> lightmap_size{};
};

struct RenderMeshChunk
{
    std::vector<WorldMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct FaceLightmapPlacement
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    bool valid = false;
};

struct BspLightmapData
{
    std::span<const unsigned char> bytes;
    bool has_lighting = false;
    bool prefer_hdr_faces = false;
    bool lightstyles_with_csm = false;
};

struct BspLightmapBuildResult
{
    WorldLightmapAtlas atlas;
    std::vector<FaceLightmapPlacement> placements;
};

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

void normalize_slashes(std::string& path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
}

bool has_suffix(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && lower_copy(text.substr(text.size() - suffix.size())) == suffix;
}

std::string remove_suffix(std::string text, std::string_view suffix)
{
    if (has_suffix(text, suffix))
    {
        text.resize(text.size() - suffix.size());
    }
    return text;
}

std::string canonical_map_name(std::string_view map_name)
{
    std::string name = trim_copy(map_name);
    normalize_slashes(name);
    if (name.rfind("maps/", 0) == 0)
    {
        name.erase(0, 5);
    }
    name = remove_suffix(std::move(name), ".bsp");
    name = remove_suffix(std::move(name), ".level.json");
    return name;
}

std::vector<std::filesystem::path> map_candidates(std::string_view map_name)
{
    std::string token = trim_copy(map_name);
    normalize_slashes(token);
    const std::filesystem::path token_path(token);
    const std::string canonical = canonical_map_name(token);

    std::vector<std::filesystem::path> candidates;
    if (!token.empty())
    {
        candidates.push_back(token_path);
    }

    if (!canonical.empty())
    {
        candidates.emplace_back("maps/" + canonical + ".bsp");
        candidates.emplace_back(canonical + ".bsp");
        candidates.emplace_back("assets/levels/" + canonical + ".level.json");
        candidates.emplace_back(canonical + ".level.json");
    }

    return candidates;
}

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
    {
        throw std::runtime_error("unexpected end of file");
    }

    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::int32_t read_s32_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(read_u32_le(bytes, offset));
}

std::uint16_t read_u16_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size())
    {
        throw std::runtime_error("unexpected end of file");
    }

    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint16_t read_u16_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size())
    {
        throw std::runtime_error("unexpected end of buffer");
    }

    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t read_u32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
    {
        throw std::runtime_error("unexpected end of buffer");
    }

    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::int32_t read_s32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(read_u32_le(bytes, offset));
}

float read_f32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    const std::uint32_t raw = read_u32_le(bytes, offset);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::int16_t read_s16_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(read_u16_le(bytes, offset));
}

float read_f32_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    const std::uint32_t raw = read_u32_le(bytes, offset);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::uint32_t fnv1a32(const std::vector<unsigned char>& bytes)
{
    std::uint32_t hash = 2166136261U;
    for (const unsigned char byte : bytes)
    {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

std::vector<unsigned char> read_binary_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0)
    {
        throw std::runtime_error("failed to query file size: " + path.string());
    }

    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
    {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    if (!file && !file.eof())
    {
        throw std::runtime_error("failed to read file: " + path.string());
    }

    return bytes;
}

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void skip_whitespace(std::string_view text, std::size_t& cursor)
{
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
    {
        ++cursor;
    }
}

std::string parse_quoted(std::string_view text, std::size_t& cursor)
{
    if (cursor >= text.size() || text[cursor] != '"')
    {
        return {};
    }

    ++cursor;
    std::string value;
    while (cursor < text.size())
    {
        const char ch = text[cursor++];
        if (ch == '"')
        {
            break;
        }

        if (ch == '\\' && cursor < text.size())
        {
            value.push_back(text[cursor++]);
            continue;
        }

        value.push_back(ch);
    }

    return value;
}

std::vector<ParsedEntity> parse_entity_lump(std::string_view text)
{
    std::vector<ParsedEntity> entities;
    std::size_t cursor = 0;

    while (cursor < text.size())
    {
        skip_whitespace(text, cursor);
        if (cursor >= text.size())
        {
            break;
        }

        if (text[cursor] != '{')
        {
            ++cursor;
            continue;
        }

        ++cursor;
        ParsedEntity entity;
        while (cursor < text.size())
        {
            skip_whitespace(text, cursor);
            if (cursor >= text.size())
            {
                break;
            }

            if (text[cursor] == '}')
            {
                ++cursor;
                break;
            }

            const std::string key = parse_quoted(text, cursor);
            skip_whitespace(text, cursor);
            const std::string value = parse_quoted(text, cursor);
            if (!key.empty())
            {
                entity.properties[key] = value;
            }
        }

        if (!entity.properties.empty())
        {
            entities.push_back(std::move(entity));
        }
    }

    return entities;
}

std::optional<float> parse_float(std::string_view text)
{
    float value = 0.0F;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return std::nullopt;
    }

    return value;
}

std::optional<std::uint8_t> parse_u8(std::string_view text)
{
    const std::optional<float> parsed = parse_float(text);
    if (!parsed || !std::isfinite(*parsed) || *parsed < 0.0F || *parsed > 255.0F)
    {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(*parsed);
}

std::optional<float> parse_prop_scale(std::string_view text)
{
    const std::optional<float> parsed = parse_float(text);
    if (!parsed || !std::isfinite(*parsed) || *parsed <= 0.0F)
    {
        return std::nullopt;
    }
    return std::clamp(*parsed, 0.001F, 1024.0F);
}

std::optional<Vec3> parse_vec3(std::string_view text)
{
    std::istringstream stream{std::string(text)};
    std::array<std::string, 3> tokens{};
    if (!(stream >> tokens[0] >> tokens[1] >> tokens[2]))
    {
        return std::nullopt;
    }

    const std::optional<float> x = parse_float(tokens[0]);
    const std::optional<float> y = parse_float(tokens[1]);
    const std::optional<float> z = parse_float(tokens[2]);
    if (!x || !y || !z)
    {
        return std::nullopt;
    }

    return Vec3{*x, *y, *z};
}

std::optional<std::array<float, 4>> parse_light_value(std::string_view text)
{
    std::istringstream stream{std::string(text)};
    std::array<float, 4> values{255.0F, 255.0F, 255.0F, 200.0F};
    std::string token;
    std::size_t count = 0;
    while (count < values.size() && stream >> token)
    {
        const std::optional<float> value = parse_float(token);
        if (!value)
        {
            return std::nullopt;
        }
        values[count++] = *value;
    }

    if (count == 0)
    {
        return std::nullopt;
    }

    if (count == 3)
    {
        values[3] = std::max({values[0], values[1], values[2], 1.0F});
    }

    return values;
}

std::optional<std::string> find_property(const ParsedEntity& entity, std::string_view name)
{
    const auto it = entity.properties.find(std::string(name));
    if (it == entity.properties.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> find_property_any(
    const ParsedEntity& entity,
    std::initializer_list<std::string_view> names)
{
    for (std::string_view name : names)
    {
        if (std::optional<std::string> value = find_property(entity, name))
        {
            return value;
        }
    }
    return std::nullopt;
}

bool is_spawn_class(std::string_view class_name)
{
    return class_name == "info_player_start" || class_name == "info_player_deathmatch" ||
           class_name == "info_player_terrorist" || class_name == "info_player_counterterrorist" ||
           class_name == "info_player_teamspawn";
}

int spawn_team_id(const ParsedEntity& entity, std::string_view class_name)
{
    if (class_name == "info_player_terrorist")
    {
        return 2;
    }
    if (class_name == "info_player_counterterrorist")
    {
        return 3;
    }
    if (class_name == "info_player_teamspawn")
    {
        if (const std::optional<std::string> team = find_property_any(entity, {"TeamNum", "teamnum", "team", "team_id"}))
        {
            if (const std::optional<float> parsed = parse_float(*team))
            {
                const int team_id = static_cast<int>(*parsed);
                if (team_id >= 0 && team_id <= 3)
                {
                    return team_id;
                }
            }
        }
    }
    return 0;
}

std::vector<WorldSpawnPoint> collect_spawn_points(const std::vector<ParsedEntity>& entities)
{
    std::vector<WorldSpawnPoint> spawns;
    for (const ParsedEntity& entity : entities)
    {
        const std::optional<std::string> class_name = find_property(entity, "classname");
        if (!class_name || !is_spawn_class(*class_name))
        {
            continue;
        }

        WorldSpawnPoint spawn;
        spawn.class_name = *class_name;
        spawn.team_id = spawn_team_id(entity, *class_name);
        if (const std::optional<std::string> origin = find_property(entity, "origin"))
        {
            spawn.origin = parse_vec3(*origin).value_or(Vec3{});
        }

        if (const std::optional<std::string> angles = find_property(entity, "angles"))
        {
            spawn.angles = parse_vec3(*angles).value_or(Vec3{});
        }
        else if (const std::optional<std::string> angle = find_property(entity, "angle"))
        {
            if (const std::optional<float> yaw = parse_float(*angle))
            {
                spawn.angles.y = *yaw;
            }
        }

        spawns.push_back(std::move(spawn));
    }

    return spawns;
}

std::vector<WorldEntity> collect_world_entities(const std::vector<ParsedEntity>& entities)
{
    std::vector<WorldEntity> world_entities;
    world_entities.reserve(entities.size());
    for (const ParsedEntity& entity : entities)
    {
        WorldEntity world_entity;
        world_entity.properties = entity.properties;
        if (const std::optional<std::string> class_name = find_property(entity, "classname"))
        {
            world_entity.class_name = *class_name;
        }
        world_entities.push_back(std::move(world_entity));
    }
    return world_entities;
}

bool is_entity_prop_class(std::string_view class_name)
{
    const std::string lower = lower_copy(class_name);
    return lower == "static_prop" || lower == "prop_static" || lower == "prop_dynamic" || lower == "prop_dynamic_override" ||
           lower == "prop_physics" || lower == "prop_physics_multiplayer" || lower == "prop_detail";
}

std::uint8_t default_entity_prop_solid(std::string_view class_name)
{
    const std::string lower = lower_copy(class_name);
    if (lower == "static_prop" || lower == "prop_static")
    {
        return kSolidVPhysics;
    }
    return kSolidNone;
}

bool is_point_light_class(std::string_view class_name)
{
    const std::string lower = lower_copy(class_name);
    return lower == "light" || lower == "light_spot" || lower == "light_dynamic";
}

float light_radius_from_entity(const ParsedEntity& entity, float brightness)
{
    for (std::string_view key : {"_distance", "distance", "_zero_percent_distance"})
    {
        if (const std::optional<std::string> text = find_property(entity, key))
        {
            if (const std::optional<float> parsed = parse_float(*text); parsed && *parsed > 1.0F)
            {
                return std::clamp(*parsed, 16.0F, 8192.0F);
            }
        }
    }

    const float derived = std::sqrt(std::max(brightness, 1.0F)) * 32.0F;
    return std::clamp(derived, 128.0F, 2048.0F);
}

std::vector<WorldLight> collect_lights(const std::vector<ParsedEntity>& entities)
{
    std::vector<WorldLight> lights;
    for (const ParsedEntity& entity : entities)
    {
        const std::optional<std::string> class_name = find_property(entity, "classname");
        if (!class_name || !is_point_light_class(*class_name))
        {
            continue;
        }

        const std::optional<std::string> origin_text = find_property(entity, "origin");
        const std::optional<Vec3> origin = origin_text ? parse_vec3(*origin_text) : std::optional<Vec3>{};
        if (!origin)
        {
            continue;
        }

        constexpr std::array<float, 4> default_light{255.0F, 255.0F, 255.0F, 200.0F};
        const std::optional<std::string> light_text = find_property(entity, "_light");
        const std::array<float, 4> light_value = light_text ? parse_light_value(*light_text).value_or(default_light) : default_light;

        WorldLight light;
        light.kind = WorldLightKind::Point;
        light.position = *origin;
        light.color = {
            std::clamp(light_value[0] / 255.0F, 0.0F, 8.0F),
            std::clamp(light_value[1] / 255.0F, 0.0F, 8.0F),
            std::clamp(light_value[2] / 255.0F, 0.0F, 8.0F),
        };
        light.intensity = std::max(light_value[3] / 255.0F, 0.0F);
        light.radius = light_radius_from_entity(entity, light_value[3]);
        lights.push_back(light);
    }

    return lights;
}

void apply_entity_angles(WorldProp& prop, const ParsedEntity& entity)
{
    if (const std::optional<std::string> angles = find_property(entity, "angles"))
    {
        prop.angles = parse_vec3(*angles).value_or(Vec3{});
        return;
    }

    if (const std::optional<std::string> angle = find_property(entity, "angle"))
    {
        if (const std::optional<float> yaw = parse_float(*angle))
        {
            prop.angles.y = *yaw;
        }
    }
}

void apply_entity_color(WorldProp& prop, const ParsedEntity& entity)
{
    if (const std::optional<std::string> color = find_property(entity, "rendercolor"))
    {
        if (const std::optional<Vec3> parsed = parse_vec3(*color))
        {
            prop.color[0] = std::clamp(parsed->x / 255.0F, 0.0F, 1.0F);
            prop.color[1] = std::clamp(parsed->y / 255.0F, 0.0F, 1.0F);
            prop.color[2] = std::clamp(parsed->z / 255.0F, 0.0F, 1.0F);
        }
    }

    if (const std::optional<std::string> alpha = find_property(entity, "renderamt"))
    {
        if (const std::optional<float> parsed = parse_float(*alpha))
        {
            prop.color[3] = std::clamp(*parsed / 255.0F, 0.0F, 1.0F);
        }
    }
}

std::vector<WorldProp> collect_entity_props(const std::vector<ParsedEntity>& entities)
{
    std::vector<WorldProp> props;
    for (const ParsedEntity& entity : entities)
    {
        const std::optional<std::string> class_name = find_property(entity, "classname");
        const std::optional<std::string> model = find_property(entity, "model");
        if (!class_name || !model || !is_entity_prop_class(*class_name))
        {
            continue;
        }

        WorldProp prop;
        prop.kind = WorldPropKind::EntityProp;
        prop.class_name = *class_name;
        prop.model_path = *model;
        normalize_slashes(prop.model_path);
        prop.solid = default_entity_prop_solid(*class_name);
        if (const std::optional<std::string> origin = find_property(entity, "origin"))
        {
            prop.origin = parse_vec3(*origin).value_or(Vec3{});
        }
        apply_entity_angles(prop, entity);
        apply_entity_color(prop, entity);
        if (const std::optional<std::string> skin = find_property(entity, "skin"))
        {
            if (const std::optional<float> parsed = parse_float(*skin))
            {
                prop.skin = static_cast<std::int32_t>(*parsed);
            }
        }
        if (const std::optional<std::string> solid = find_property(entity, "solid"))
        {
            prop.solid = parse_u8(*solid).value_or(prop.solid);
        }
        if (const std::optional<std::string> scale = find_property(entity, "modelscale"))
        {
            prop.scale = parse_prop_scale(*scale).value_or(prop.scale);
        }
        props.push_back(std::move(prop));
    }
    return props;
}

BspLump read_lump(const std::vector<unsigned char>& bytes, std::size_t lump_index)
{
    const std::size_t offset = 8 + (lump_index * 16);
    return BspLump{
        .offset = read_u32_le(bytes, offset),
        .length = read_u32_le(bytes, offset + 4),
        .version = read_u32_le(bytes, offset + 8),
    };
}

void validate_lump_range(const std::vector<unsigned char>& bytes, const BspLump& lump, std::string_view name)
{
    const std::uint64_t begin = lump.offset;
    const std::uint64_t end = begin + lump.length;
    if (end > bytes.size() || end < begin)
    {
        throw std::runtime_error("BSP " + std::string(name) + " lump points outside the file");
    }
}

std::string entity_lump_text(const std::vector<unsigned char>& bytes, const BspLump& lump)
{
    validate_lump_range(bytes, lump, "entity");

    return std::string(reinterpret_cast<const char*>(bytes.data() + lump.offset), static_cast<std::size_t>(lump.length));
}

template <typename T, typename Reader>
std::vector<T> read_lump_array(const std::vector<unsigned char>& bytes, const BspLump& lump, std::size_t stride, std::string_view name, Reader reader)
{
    validate_lump_range(bytes, lump, name);
    if (stride == 0 || (lump.length % stride) != 0)
    {
        throw std::runtime_error("BSP " + std::string(name) + " lump has an invalid size");
    }

    std::vector<T> values;
    values.reserve(lump.length / stride);
    for (std::size_t offset = lump.offset; offset < static_cast<std::size_t>(lump.offset) + lump.length; offset += stride)
    {
        values.push_back(reader(offset));
    }

    return values;
}

Vec3 cross(Vec3 lhs, Vec3 rhs)
{
    return Vec3{
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

float dot(Vec3 lhs, Vec3 rhs)
{
    return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

float length(Vec3 value)
{
    return std::sqrt(dot(value, value));
}

Vec3 normalize(Vec3 value)
{
    const float value_length = length(value);
    if (value_length <= 0.00001F)
    {
        return Vec3{0.0F, 0.0F, 1.0F};
    }

    return value * (1.0F / value_length);
}

void include_bounds(WorldMesh& mesh, Vec3 point)
{
    if (!mesh.has_bounds)
    {
        mesh.bounds_min = point;
        mesh.bounds_max = point;
        mesh.has_bounds = true;
        return;
    }

    mesh.bounds_min.x = std::min(mesh.bounds_min.x, point.x);
    mesh.bounds_min.y = std::min(mesh.bounds_min.y, point.y);
    mesh.bounds_min.z = std::min(mesh.bounds_min.z, point.z);
    mesh.bounds_max.x = std::max(mesh.bounds_max.x, point.x);
    mesh.bounds_max.y = std::max(mesh.bounds_max.y, point.y);
    mesh.bounds_max.z = std::max(mesh.bounds_max.z, point.z);
}

bool has_any_surface_flag(std::uint32_t flags, std::uint32_t mask)
{
    return (flags & mask) != 0;
}

bool should_render_surface(std::uint32_t flags)
{
    constexpr std::uint32_t skip_flags = kSurfaceSky2D | kSurfaceSky | kSurfaceNoDraw | kSurfaceSkip;
    return !has_any_surface_flag(flags, skip_flags);
}

bool should_collide_surface(std::uint32_t flags)
{
    constexpr std::uint32_t skip_flags = kSurfaceSky2D | kSurfaceSky | kSurfaceWarp | kSurfaceSkip;
    return !has_any_surface_flag(flags, skip_flags);
}

bool should_collide_brush(std::uint32_t contents)
{
    // Matches UrbanStrike/Source world VPhysics static brush groups:
    // MASK_SOLID without grates, CONTENTS_GRATE, CONTENTS_PLAYERCLIP, CONTENTS_MONSTERCLIP.
    // Water/slime are separate fluid models in Source and must not become solid world collision here.
    return (contents & kBspWorldStaticCollisionMask) != 0;
}

bool triangle_blocks_player_movement(const WorldTriangle& triangle)
{
    return triangle.contents == 0 || (triangle.contents & kMaskPlayerSolid) != 0;
}

std::vector<BspPlane> read_bsp_planes(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspPlane>(bytes, read_lump(bytes, kPlanesLump), 20, "planes", [&](std::size_t offset) {
        return BspPlane{
            .normal = {
                read_f32_le(bytes, offset),
                read_f32_le(bytes, offset + 4),
                read_f32_le(bytes, offset + 8),
            },
            .dist = read_f32_le(bytes, offset + 12),
            .type = read_s32_le(bytes, offset + 16),
        };
    });
}

std::vector<Vec3> read_bsp_vertices(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<Vec3>(bytes, read_lump(bytes, kVerticesLump), 12, "vertices", [&](std::size_t offset) {
        return Vec3{
            read_f32_le(bytes, offset),
            read_f32_le(bytes, offset + 4),
            read_f32_le(bytes, offset + 8),
        };
    });
}

std::vector<BspNode> read_bsp_nodes(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspNode>(bytes, read_lump(bytes, kNodesLump), 32, "nodes", [&](std::size_t offset) {
        return BspNode{{
            read_s32_le(bytes, offset + 4),
            read_s32_le(bytes, offset + 8),
        }};
    });
}

std::vector<BspEdge> read_bsp_edges(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspEdge>(bytes, read_lump(bytes, kEdgesLump), 4, "edges", [&](std::size_t offset) {
        return BspEdge{{
            read_u16_le(bytes, offset),
            read_u16_le(bytes, offset + 2),
        }};
    });
}

std::vector<BspLeaf> read_bsp_leaves(const std::vector<unsigned char>& bytes)
{
    const BspLump lump = read_lump(bytes, kLeavesLump);
    const std::size_t stride = lump.version == 0 && lump.length != 0 && (lump.length % 56U) == 0 ? 56U : 32U;
    return read_lump_array<BspLeaf>(bytes, lump, stride, "leaves", [&](std::size_t offset) {
        return BspLeaf{
            .contents = read_u32_le(bytes, offset),
            .first_leaf_brush = read_u16_le(bytes, offset + 24),
            .leaf_brush_count = read_u16_le(bytes, offset + 26),
        };
    });
}

std::vector<BspModel> read_bsp_models(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspModel>(bytes, read_lump(bytes, kModelsLump), 48, "models", [&](std::size_t offset) {
        return BspModel{
            .mins = {
                read_f32_le(bytes, offset),
                read_f32_le(bytes, offset + 4),
                read_f32_le(bytes, offset + 8),
            },
            .maxs = {
                read_f32_le(bytes, offset + 12),
                read_f32_le(bytes, offset + 16),
                read_f32_le(bytes, offset + 20),
            },
            .origin = {
                read_f32_le(bytes, offset + 24),
                read_f32_le(bytes, offset + 28),
                read_f32_le(bytes, offset + 32),
            },
            .head_node = read_s32_le(bytes, offset + 36),
            .first_face = read_s32_le(bytes, offset + 40),
            .face_count = read_s32_le(bytes, offset + 44),
        };
    });
}

std::vector<BspBrushSide> read_bsp_brush_sides(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspBrushSide>(bytes, read_lump(bytes, kBrushSidesLump), 8, "brush sides", [&](std::size_t offset) {
        return BspBrushSide{
            .plane = read_u16_le(bytes, offset),
            .texinfo = read_s16_le(bytes, offset + 2),
            .dispinfo = read_s16_le(bytes, offset + 4),
            .bevel = bytes[offset + 6],
            .thin = bytes[offset + 7],
        };
    });
}

std::vector<BspBrush> read_bsp_brushes(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspBrush>(bytes, read_lump(bytes, kBrushesLump), 12, "brushes", [&](std::size_t offset) {
        return BspBrush{
            .first_side = read_s32_le(bytes, offset),
            .side_count = read_s32_le(bytes, offset + 4),
            .contents = read_u32_le(bytes, offset + 8),
        };
    });
}

std::vector<std::uint16_t> read_bsp_leaf_brushes(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<std::uint16_t>(bytes, read_lump(bytes, kLeafBrushesLump), 2, "leaf brushes", [&](std::size_t offset) {
        return read_u16_le(bytes, offset);
    });
}

std::vector<std::int32_t> read_bsp_surfedges(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<std::int32_t>(bytes, read_lump(bytes, kSurfEdgesLump), 4, "surfedges", [&](std::size_t offset) {
        return read_s32_le(bytes, offset);
    });
}

std::vector<BspTexInfo> read_bsp_texinfo(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspTexInfo>(bytes, read_lump(bytes, kTexInfoLump), 72, "texinfo", [&](std::size_t offset) {
        BspTexInfo texinfo;
        for (std::size_t axis = 0; axis < 2; ++axis)
        {
            for (std::size_t component = 0; component < 4; ++component)
            {
                texinfo.texture_vecs[axis][component] = read_f32_le(bytes, offset + (axis * 16) + (component * 4));
            }
        }
        for (std::size_t axis = 0; axis < 2; ++axis)
        {
            for (std::size_t component = 0; component < 4; ++component)
            {
                texinfo.lightmap_vecs[axis][component] = read_f32_le(bytes, offset + 32 + (axis * 16) + (component * 4));
            }
        }
        texinfo.flags = read_u32_le(bytes, offset + 64);
        texinfo.texdata = read_s32_le(bytes, offset + 68);
        return texinfo;
    });
}

std::string read_texture_name(const std::vector<unsigned char>& string_data, const std::vector<std::int32_t>& string_table, std::int32_t name_index)
{
    if (name_index < 0 || static_cast<std::size_t>(name_index) >= string_table.size())
    {
        return {};
    }

    const std::int32_t string_offset = string_table[static_cast<std::size_t>(name_index)];
    if (string_offset < 0 || static_cast<std::size_t>(string_offset) >= string_data.size())
    {
        return {};
    }

    const auto begin = string_data.begin() + string_offset;
    const auto end = std::find(begin, string_data.end(), '\0');
    std::string name(begin, end);
    normalize_slashes(name);
    return lower_copy(name);
}

std::vector<BspTexData> read_bsp_texdata(const std::vector<unsigned char>& bytes)
{
    const BspLump string_data_lump = read_lump(bytes, kTexDataStringDataLump);
    validate_lump_range(bytes, string_data_lump, "texdata string data");
    const std::vector<unsigned char> string_data(bytes.begin() + string_data_lump.offset, bytes.begin() + string_data_lump.offset + string_data_lump.length);

    const std::vector<std::int32_t> string_table =
        read_lump_array<std::int32_t>(bytes, read_lump(bytes, kTexDataStringTableLump), 4, "texdata string table", [&](std::size_t offset) {
            return read_s32_le(bytes, offset);
        });

    return read_lump_array<BspTexData>(bytes, read_lump(bytes, kTexDataLump), 32, "texdata", [&](std::size_t offset) {
        const std::int32_t name_index = read_s32_le(bytes, offset + 12);
        const std::int32_t width = read_s32_le(bytes, offset + 16);
        const std::int32_t height = read_s32_le(bytes, offset + 20);
        return BspTexData{
            .name = read_texture_name(string_data, string_table, name_index),
            .width = static_cast<std::uint32_t>(std::max(width, 1)),
            .height = static_cast<std::uint32_t>(std::max(height, 1)),
        };
    });
}

std::unordered_map<std::string, std::vector<unsigned char>> read_bsp_pakfile_assets(const std::vector<unsigned char>& bytes)
{
    constexpr std::uint32_t kZipLocalHeaderSignature = 0x04034B50U;
    constexpr std::uint16_t kZipStoredMethod = 0;

    const BspLump pak_lump = read_lump(bytes, kPakFileLump);
    validate_lump_range(bytes, pak_lump, "pakfile");
    std::span<const unsigned char> pak(bytes.data() + pak_lump.offset, pak_lump.length);
    std::unordered_map<std::string, std::vector<unsigned char>> assets;

    std::size_t cursor = 0;
    while (cursor + 30 <= pak.size() && read_u32_le(pak, cursor) == kZipLocalHeaderSignature)
    {
        const std::uint16_t method = read_u16_le(pak, cursor + 8);
        const std::uint32_t compressed_size = read_u32_le(pak, cursor + 18);
        const std::uint32_t uncompressed_size = read_u32_le(pak, cursor + 22);
        const std::uint16_t name_length = read_u16_le(pak, cursor + 26);
        const std::uint16_t extra_length = read_u16_le(pak, cursor + 28);
        const std::size_t name_offset = cursor + 30;
        const std::size_t data_offset = name_offset + name_length + extra_length;
        const std::size_t next = data_offset + compressed_size;
        if (data_offset > pak.size() || next > pak.size() || next < data_offset)
        {
            break;
        }

        std::string name(reinterpret_cast<const char*>(pak.data() + name_offset), name_length);
        normalize_slashes(name);
        name = lower_copy(name);
        if (method == kZipStoredMethod && compressed_size == uncompressed_size && !name.empty())
        {
            assets.try_emplace(name, pak.begin() + data_offset, pak.begin() + data_offset + compressed_size);
        }

        cursor = next;
    }

    return assets;
}

BspLightmapData read_bsp_lightmap_data(const std::vector<unsigned char>& bytes)
{
    BspLightmapData data;
    bool selected_hdr_lighting = false;

    const BspLump hdr_lump = read_lump(bytes, kLightingHdrLump);
    if (hdr_lump.length > 0)
    {
        validate_lump_range(bytes, hdr_lump, "HDR lighting");
        data.bytes = std::span<const unsigned char>(bytes.data() + hdr_lump.offset, hdr_lump.length);
        data.has_lighting = true;
        selected_hdr_lighting = true;
    }
    else
    {
        const BspLump ldr_lump = read_lump(bytes, kLightingLump);
        if (ldr_lump.length > 0)
        {
            validate_lump_range(bytes, ldr_lump, "lighting");
            data.bytes = std::span<const unsigned char>(bytes.data() + ldr_lump.offset, ldr_lump.length);
            data.has_lighting = true;
        }
    }

    const BspLump hdr_faces = read_lump(bytes, kFacesHdrLump);
    if (hdr_faces.length > 0)
    {
        validate_lump_range(bytes, hdr_faces, "HDR faces");
    }
    data.prefer_hdr_faces = selected_hdr_lighting && hdr_faces.length > 0;

    const BspLump map_flags = read_lump(bytes, kMapFlagsLump);
    if (map_flags.length > 0)
    {
        validate_lump_range(bytes, map_flags, "map flags");
        if (map_flags.length >= 4)
        {
            data.lightstyles_with_csm = (read_u32_le(bytes, map_flags.offset) & kMapFlagLightstylesWithCsm) != 0;
        }
    }

    return data;
}

std::vector<BspFace> read_bsp_faces(const std::vector<unsigned char>& bytes, bool prefer_hdr_faces)
{
    BspLump lump = read_lump(bytes, kFacesLump);
    if (prefer_hdr_faces)
    {
        lump = read_lump(bytes, kFacesHdrLump);
    }

    return read_lump_array<BspFace>(bytes, lump, 56, "faces", [&](std::size_t offset) {
        BspFace face;
        face.plane = read_u16_le(bytes, offset);
        face.side = bytes[offset + 2];
        face.first_edge = read_s32_le(bytes, offset + 4);
        face.edge_count = read_s16_le(bytes, offset + 8);
        face.texinfo = read_s16_le(bytes, offset + 10);
        face.dispinfo = read_s16_le(bytes, offset + 12);
        for (std::size_t style = 0; style < kMaxLightStyles; ++style)
        {
            face.styles[style] = bytes[offset + 16 + style];
        }
        face.light_offset = read_s32_le(bytes, offset + 20);
        face.lightmap_mins[0] = read_s32_le(bytes, offset + 28);
        face.lightmap_mins[1] = read_s32_le(bytes, offset + 32);
        face.lightmap_size[0] = read_s32_le(bytes, offset + 36);
        face.lightmap_size[1] = read_s32_le(bytes, offset + 40);
        return face;
    });
}

std::uint32_t surface_flags_for_face(const BspFace& face, const std::vector<BspTexInfo>& texinfo)
{
    if (face.texinfo < 0 || static_cast<std::size_t>(face.texinfo) >= texinfo.size())
    {
        return 0;
    }

    return texinfo[static_cast<std::size_t>(face.texinfo)].flags;
}

const BspTexInfo* texinfo_for_face(const BspFace& face, const std::vector<BspTexInfo>& texinfo)
{
    if (face.texinfo < 0 || static_cast<std::size_t>(face.texinfo) >= texinfo.size())
    {
        return nullptr;
    }

    return &texinfo[static_cast<std::size_t>(face.texinfo)];
}

Vec2 texture_coordinate(Vec3 point, const BspTexInfo& texinfo, const WorldMaterial& material)
{
    const float width = static_cast<float>(std::max<std::uint32_t>(material.width, 1));
    const float height = static_cast<float>(std::max<std::uint32_t>(material.height, 1));
    const auto& s = texinfo.texture_vecs[0];
    const auto& t = texinfo.texture_vecs[1];
    return Vec2{
        ((point.x * s[0]) + (point.y * s[1]) + (point.z * s[2]) + s[3]) / width,
        ((point.x * t[0]) + (point.y * t[1]) + (point.z * t[2]) + t[3]) / height,
    };
}

std::uint32_t next_power_of_two(std::uint32_t value)
{
    std::uint32_t result = 1;
    while (result < value && result < kMaxLightmapAtlasDimension)
    {
        result *= 2;
    }
    return std::max<std::uint32_t>(result, 1);
}

std::size_t light_style_count(const BspFace& face)
{
    std::size_t count = 0;
    for (const std::uint8_t style : face.styles)
    {
        if (style != kUnusedLightStyle)
        {
            ++count;
        }
    }
    return count;
}

std::uint32_t lightmap_width(const BspFace& face)
{
    return face.lightmap_size[0] >= 0 ? static_cast<std::uint32_t>(face.lightmap_size[0]) + 1U : 0U;
}

std::uint32_t lightmap_height(const BspFace& face)
{
    return face.lightmap_size[1] >= 0 ? static_cast<std::uint32_t>(face.lightmap_size[1]) + 1U : 0U;
}

std::uint64_t lightmap_required_bytes(const BspFace& face, const BspTexInfo& texinfo, const BspLightmapData& lighting)
{
    const std::uint32_t width = lightmap_width(face);
    const std::uint32_t height = lightmap_height(face);
    const std::size_t style_count = light_style_count(face);
    if (width == 0 || height == 0 || style_count == 0)
    {
        return 0;
    }

    const std::uint64_t sample_count = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t bump_factor = (texinfo.flags & kSurfaceBumpLight) != 0 ? kBumpLightmapCount : 1U;
    const std::uint64_t groups_per_style = bump_factor + (lighting.lightstyles_with_csm ? 1U : 0U);
    const std::uint64_t group_count = static_cast<std::uint64_t>(style_count) * groups_per_style;
    if (group_count == 0 || sample_count > (std::numeric_limits<std::uint64_t>::max() / (group_count * 4U)))
    {
        return 0;
    }
    return sample_count * group_count * 4U;
}

bool face_has_valid_lightmap(const BspFace& face, const BspTexInfo& texinfo, const BspLightmapData& lighting)
{
    if (!lighting.has_lighting || face.light_offset < 0)
    {
        return false;
    }

    const std::uint32_t width = lightmap_width(face);
    const std::uint32_t height = lightmap_height(face);
    if (width == 0 || height == 0 || width > kMaxLightmapAtlasDimension || height > kMaxLightmapAtlasDimension)
    {
        return false;
    }

    const std::uint64_t required = lightmap_required_bytes(face, texinfo, lighting);
    const std::uint64_t begin = static_cast<std::uint64_t>(face.light_offset);
    const std::uint64_t end = begin + required;
    return required > 0 && end <= lighting.bytes.size() && end >= begin;
}

float tex_light_to_linear(std::uint8_t value, std::int8_t exponent)
{
    return std::ldexp(static_cast<float>(value) / 255.0F, exponent);
}

std::array<float, 4> decode_lightmap_sample(const BspLightmapData& lighting, const BspFace& face, std::uint32_t width, std::uint32_t x, std::uint32_t y)
{
    const std::uint64_t sample_index = (static_cast<std::uint64_t>(y) * width) + x;
    const std::uint64_t sample_offset = static_cast<std::uint64_t>(face.light_offset) + (sample_index * 4U);
    if (sample_offset + 4U > lighting.bytes.size())
    {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }

    const std::int8_t exponent = static_cast<std::int8_t>(lighting.bytes[static_cast<std::size_t>(sample_offset + 3U)]);
    return {
        tex_light_to_linear(lighting.bytes[static_cast<std::size_t>(sample_offset + 0U)], exponent),
        tex_light_to_linear(lighting.bytes[static_cast<std::size_t>(sample_offset + 1U)], exponent),
        tex_light_to_linear(lighting.bytes[static_cast<std::size_t>(sample_offset + 2U)], exponent),
        1.0F,
    };
}

BspLightmapBuildResult build_lightmap_atlas(
    const std::vector<BspFace>& faces,
    const std::vector<BspTexInfo>& texinfo,
    const BspLightmapData& lighting)
{
    BspLightmapBuildResult result;
    result.placements.resize(faces.size());

    struct RectRequest
    {
        std::size_t face_index = 0;
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        std::uint32_t padded_width = 3;
        std::uint32_t padded_height = 3;
    };

    std::vector<RectRequest> requests;
    std::uint32_t widest_rect = 1;
    std::uint64_t total_area = 0;
    for (std::size_t face_index = 0; face_index < faces.size(); ++face_index)
    {
        const BspFace& face = faces[face_index];
        const BspTexInfo* face_texinfo = texinfo_for_face(face, texinfo);
        if (face_texinfo == nullptr || !should_render_surface(face_texinfo->flags) || !face_has_valid_lightmap(face, *face_texinfo, lighting))
        {
            continue;
        }

        const std::uint32_t width = lightmap_width(face);
        const std::uint32_t height = lightmap_height(face);
        RectRequest request{
            .face_index = face_index,
            .width = width,
            .height = height,
            .padded_width = width + 2U,
            .padded_height = height + 2U,
        };
        widest_rect = std::max(widest_rect, request.padded_width);
        total_area += static_cast<std::uint64_t>(request.padded_width) * request.padded_height;
        requests.push_back(request);
    }

    if (requests.empty())
    {
        return result;
    }

    std::sort(requests.begin(), requests.end(), [](const RectRequest& lhs, const RectRequest& rhs) {
        if (lhs.padded_height != rhs.padded_height)
        {
            return lhs.padded_height > rhs.padded_height;
        }
        return lhs.padded_width > rhs.padded_width;
    });

    std::uint32_t desired_width = next_power_of_two(static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(total_area)))));
    desired_width = std::max(desired_width, next_power_of_two(widest_rect));
    desired_width = std::max<std::uint32_t>(desired_width, 64U);

    std::uint32_t atlas_width = 0;
    std::uint32_t atlas_height = 0;
    std::vector<FaceLightmapPlacement> packed_placements;
    const auto try_pack = [&](std::uint32_t width) -> bool {
        packed_placements.assign(faces.size(), {});
        std::uint32_t cursor_x = 0;
        std::uint32_t cursor_y = 0;
        std::uint32_t row_height = 0;
        for (const RectRequest& request : requests)
        {
            if (request.padded_width > width)
            {
                return false;
            }

            if (cursor_x > 0 && cursor_x + request.padded_width > width)
            {
                cursor_y += row_height;
                cursor_x = 0;
                row_height = 0;
            }

            if (cursor_y + request.padded_height > kMaxLightmapAtlasDimension)
            {
                return false;
            }

            packed_placements[request.face_index] = FaceLightmapPlacement{
                .x = cursor_x,
                .y = cursor_y,
                .width = request.width,
                .height = request.height,
                .valid = true,
            };
            cursor_x += request.padded_width;
            row_height = std::max(row_height, request.padded_height);
        }

        atlas_width = width;
        atlas_height = next_power_of_two(std::max<std::uint32_t>(cursor_y + row_height, 1U));
        return atlas_height <= kMaxLightmapAtlasDimension;
    };

    for (std::uint32_t width = desired_width; width <= kMaxLightmapAtlasDimension; width *= 2U)
    {
        if (try_pack(width))
        {
            break;
        }
        if (width == kMaxLightmapAtlasDimension)
        {
            break;
        }
    }

    if (atlas_width == 0 || atlas_height == 0 || packed_placements.empty())
    {
        log_warning("BSP lightmaps exceed the maximum {}x{} atlas; static baked lighting disabled",
            kMaxLightmapAtlasDimension,
            kMaxLightmapAtlasDimension);
        return result;
    }

    result.placements = std::move(packed_placements);
    result.atlas.width = atlas_width;
    result.atlas.height = atlas_height;
    result.atlas.rgba.assign(static_cast<std::size_t>(atlas_width) * atlas_height * 4U, 1.0F);
    result.atlas.has_baked_samples = true;

    const auto write_pixel = [&](std::uint32_t x, std::uint32_t y, const std::array<float, 4>& color) {
        const std::size_t offset = ((static_cast<std::size_t>(y) * atlas_width) + x) * 4U;
        result.atlas.rgba[offset + 0] = color[0];
        result.atlas.rgba[offset + 1] = color[1];
        result.atlas.rgba[offset + 2] = color[2];
        result.atlas.rgba[offset + 3] = color[3];
    };

    for (const RectRequest& request : requests)
    {
        const FaceLightmapPlacement& placement = result.placements[request.face_index];
        if (!placement.valid)
        {
            continue;
        }

        const BspFace& face = faces[request.face_index];
        for (std::uint32_t y = 0; y < request.padded_height; ++y)
        {
            const std::uint32_t sample_y = std::clamp<std::uint32_t>(y == 0 ? 0U : y - 1U, 0U, request.height - 1U);
            for (std::uint32_t x = 0; x < request.padded_width; ++x)
            {
                const std::uint32_t sample_x = std::clamp<std::uint32_t>(x == 0 ? 0U : x - 1U, 0U, request.width - 1U);
                write_pixel(placement.x + x,
                    placement.y + y,
                    decode_lightmap_sample(lighting, face, request.width, sample_x, sample_y));
            }
        }
    }

    log_info("built BSP lightmap atlas {}x{} for {} face(s)", result.atlas.width, result.atlas.height, requests.size());
    return result;
}

Vec2 lightmap_coordinate(
    Vec3 point,
    const BspTexInfo& texinfo,
    const BspFace& face,
    const FaceLightmapPlacement& placement,
    const WorldLightmapAtlas& atlas)
{
    if (!placement.valid || atlas.width == 0 || atlas.height == 0)
    {
        return {0.5F, 0.5F};
    }

    const auto& s = texinfo.lightmap_vecs[0];
    const auto& t = texinfo.lightmap_vecs[1];
    const float luxel_s = (point.x * s[0]) + (point.y * s[1]) + (point.z * s[2]) + s[3] - static_cast<float>(face.lightmap_mins[0]) + 0.5F;
    const float luxel_t = (point.x * t[0]) + (point.y * t[1]) + (point.z * t[2]) + t[3] - static_cast<float>(face.lightmap_mins[1]) + 0.5F;
    Vec2 uv{
        (static_cast<float>(placement.x + 1U) + luxel_s) / static_cast<float>(atlas.width),
        (static_cast<float>(placement.y + 1U) + luxel_t) / static_cast<float>(atlas.height),
    };
    uv.x = std::clamp(uv.x, 0.0F, 1.0F);
    uv.y = std::clamp(uv.y, 0.0F, 1.0F);
    return uv;
}

WorldMeshVertex make_world_vertex(
    Vec3 point,
    Vec3 normal,
    const BspTexInfo& texinfo,
    const WorldMaterial& material,
    const BspFace& face,
    const FaceLightmapPlacement* placement,
    const WorldLightmapAtlas& atlas)
{
    WorldMeshVertex vertex;
    vertex.position = point;
    vertex.normal = normal;
    vertex.texcoord = texture_coordinate(point, texinfo, material);
    if (placement != nullptr && placement->valid && atlas.has_baked_samples)
    {
        vertex.lightmap_texcoord = lightmap_coordinate(point, texinfo, face, *placement, atlas);
        vertex.lightmap_weight = 1.0F;
    }
    return vertex;
}

Vec2 vec2_bilerp(Vec2 c0, Vec2 c1, Vec2 c2, Vec2 c3, float row, float column)
{
    return {
        (c0.x * (1.0F - row) * (1.0F - column)) + (c1.x * row * (1.0F - column)) + (c2.x * row * column) +
            (c3.x * (1.0F - row) * column),
        (c0.y * (1.0F - row) * (1.0F - column)) + (c1.y * row * (1.0F - column)) + (c2.y * row * column) +
            (c3.y * (1.0F - row) * column),
    };
}

std::uint32_t material_index_for_texinfo(
    WorldMesh& mesh,
    std::vector<RenderMeshChunk>& chunks,
    std::unordered_map<std::int32_t, std::uint32_t>& texdata_to_material,
    const BspTexInfo* texinfo,
    const std::vector<BspTexData>& texdata)
{
    const std::int32_t texdata_index = texinfo != nullptr ? texinfo->texdata : -1;
    if (const auto it = texdata_to_material.find(texdata_index); it != texdata_to_material.end())
    {
        return it->second;
    }

    WorldMaterial material;
    if (texdata_index >= 0 && static_cast<std::size_t>(texdata_index) < texdata.size())
    {
        const BspTexData& source = texdata[static_cast<std::size_t>(texdata_index)];
        material.name = source.name.empty() ? "__missing" : source.name;
        material.width = source.width;
        material.height = source.height;
    }
    else
    {
        material.name = "__missing";
    }

    const std::uint32_t material_index = static_cast<std::uint32_t>(mesh.materials.size());
    mesh.materials.push_back(std::move(material));
    chunks.emplace_back();
    texdata_to_material.emplace(texdata_index, material_index);
    return material_index;
}

std::optional<std::vector<Vec3>> polygon_for_face(
    const BspFace& face, const std::vector<Vec3>& vertices, const std::vector<BspEdge>& edges, const std::vector<std::int32_t>& surfedges)
{
    if (face.edge_count < 3 || face.first_edge < 0)
    {
        return std::nullopt;
    }

    const std::size_t first_edge = static_cast<std::size_t>(face.first_edge);
    const std::size_t edge_count = static_cast<std::size_t>(face.edge_count);
    if (first_edge + edge_count > surfedges.size())
    {
        return std::nullopt;
    }

    std::vector<Vec3> polygon;
    polygon.reserve(edge_count);
    for (std::size_t edge_offset = 0; edge_offset < edge_count; ++edge_offset)
    {
        const std::int32_t edge_ref = surfedges[first_edge + edge_offset];
        const std::uint32_t edge_index = edge_ref < 0 ? static_cast<std::uint32_t>(-edge_ref) : static_cast<std::uint32_t>(edge_ref);
        if (edge_index >= edges.size())
        {
            return std::nullopt;
        }

        const BspEdge& edge = edges[edge_index];
        const std::uint16_t vertex_index = edge_ref < 0 ? edge.vertices[1] : edge.vertices[0];
        if (vertex_index >= vertices.size())
        {
            return std::nullopt;
        }

        polygon.push_back(vertices[vertex_index]);
    }

    return polygon;
}

Vec3 polygon_normal(const std::vector<Vec3>& polygon)
{
    Vec3 normal{};
    for (std::size_t index = 0; index < polygon.size(); ++index)
    {
        const Vec3 current = polygon[index];
        const Vec3 next = polygon[(index + 1) % polygon.size()];
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
    }

    return normalize(normal);
}

std::vector<Vec3> base_winding_for_plane(Vec3 normal, float dist)
{
    normal = normalize(normal);
    const Vec3 origin = normal * dist;
    const Vec3 up = std::fabs(normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
    const Vec3 tangent = normalize(cross(up, normal));
    const Vec3 bitangent = cross(normal, tangent);
    constexpr float extent = 131072.0F;

    return {
        origin - (tangent * extent) - (bitangent * extent),
        origin + (tangent * extent) - (bitangent * extent),
        origin + (tangent * extent) + (bitangent * extent),
        origin - (tangent * extent) + (bitangent * extent),
    };
}

std::vector<Vec3> clip_polygon_to_plane(const std::vector<Vec3>& polygon, Vec3 normal, float dist)
{
    constexpr float epsilon = 0.01F;
    if (polygon.empty())
    {
        return {};
    }

    std::vector<Vec3> clipped;
    clipped.reserve(polygon.size() + 1);
    Vec3 previous = polygon.back();
    float previous_distance = dot(normal, previous) - dist;
    bool previous_inside = previous_distance <= epsilon;

    for (const Vec3 current : polygon)
    {
        const float current_distance = dot(normal, current) - dist;
        const bool current_inside = current_distance <= epsilon;
        if (current_inside != previous_inside)
        {
            const float denominator = previous_distance - current_distance;
            if (std::fabs(denominator) > 0.00001F)
            {
                const float t = std::clamp(previous_distance / denominator, 0.0F, 1.0F);
                clipped.push_back(previous + ((current - previous) * t));
            }
        }

        if (current_inside)
        {
            clipped.push_back(current);
        }

        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }

    return clipped;
}

void collect_world_model_brushes_r(
    std::int32_t node_index,
    const std::vector<BspNode>& nodes,
    const std::vector<BspLeaf>& leaves,
    const std::vector<std::uint16_t>& leaf_brushes,
    std::vector<bool>& referenced)
{
    if (node_index < 0)
    {
        const std::int32_t leaf_index = -1 - node_index;
        if (leaf_index < 0 || static_cast<std::size_t>(leaf_index) >= leaves.size())
        {
            return;
        }

        const BspLeaf& leaf = leaves[static_cast<std::size_t>(leaf_index)];
        const std::size_t first = leaf.first_leaf_brush;
        const std::size_t count = leaf.leaf_brush_count;
        if (first + count > leaf_brushes.size())
        {
            return;
        }

        for (std::size_t index = first; index < first + count; ++index)
        {
            const std::uint16_t brush_index = leaf_brushes[index];
            if (brush_index < referenced.size())
            {
                referenced[brush_index] = true;
            }
        }
        return;
    }

    if (static_cast<std::size_t>(node_index) >= nodes.size())
    {
        return;
    }

    const BspNode& node = nodes[static_cast<std::size_t>(node_index)];
    collect_world_model_brushes_r(node.children[0], nodes, leaves, leaf_brushes, referenced);
    collect_world_model_brushes_r(node.children[1], nodes, leaves, leaf_brushes, referenced);
}

std::vector<bool> collect_world_model_brushes(
    const std::vector<BspBrush>& brushes,
    const std::vector<BspModel>& models,
    const std::vector<BspNode>& nodes,
    const std::vector<BspLeaf>& leaves,
    const std::vector<std::uint16_t>& leaf_brushes)
{
    std::vector<bool> referenced(brushes.size(), false);
    if (brushes.empty())
    {
        return referenced;
    }

    if (models.empty() || leaves.empty() || leaf_brushes.empty())
    {
        std::fill(referenced.begin(), referenced.end(), true);
        return referenced;
    }

    collect_world_model_brushes_r(models.front().head_node, nodes, leaves, leaf_brushes, referenced);
    return referenced;
}

std::optional<std::vector<Vec3>> polygon_for_brush_side(
    const BspBrush& brush,
    std::size_t side_offset,
    const std::vector<BspBrushSide>& brush_sides,
    const std::vector<BspPlane>& planes)
{
    if (brush.first_side < 0 || brush.side_count <= 0)
    {
        return std::nullopt;
    }

    const std::size_t first_side = static_cast<std::size_t>(brush.first_side);
    const std::size_t side_count = static_cast<std::size_t>(brush.side_count);
    if (first_side + side_count > brush_sides.size() || side_offset >= side_count)
    {
        return std::nullopt;
    }

    const BspBrushSide& source_side = brush_sides[first_side + side_offset];
    if (source_side.plane >= planes.size() || source_side.bevel != 0)
    {
        return std::nullopt;
    }

    const BspPlane& source_plane = planes[source_side.plane];
    std::vector<Vec3> polygon = base_winding_for_plane(source_plane.normal, source_plane.dist);
    for (std::size_t clip_offset = 0; clip_offset < side_count && polygon.size() >= 3; ++clip_offset)
    {
        if (clip_offset == side_offset)
        {
            continue;
        }

        const BspBrushSide& clip_side = brush_sides[first_side + clip_offset];
        if (clip_side.plane >= planes.size() || clip_side.bevel != 0)
        {
            continue;
        }

        const BspPlane& clip_plane = planes[clip_side.plane];
        polygon = clip_polygon_to_plane(polygon, clip_plane.normal, clip_plane.dist);
    }

    if (polygon.size() < 3)
    {
        return std::nullopt;
    }

    return polygon;
}

void append_render_triangle(
    RenderMeshChunk& chunk,
    Vec3 a,
    Vec3 b,
    Vec3 c,
    Vec3 normal,
    const BspTexInfo& texinfo,
    const WorldMaterial& material,
    const BspFace& face,
    const FaceLightmapPlacement* placement,
    const WorldLightmapAtlas& atlas)
{
    const std::uint32_t first_index = static_cast<std::uint32_t>(chunk.vertices.size());
    chunk.vertices.push_back(make_world_vertex(a, normal, texinfo, material, face, placement, atlas));
    chunk.vertices.push_back(make_world_vertex(b, normal, texinfo, material, face, placement, atlas));
    chunk.vertices.push_back(make_world_vertex(c, normal, texinfo, material, face, placement, atlas));
    chunk.indices.push_back(first_index);
    chunk.indices.push_back(first_index + 1);
    chunk.indices.push_back(first_index + 2);
}

void append_collision_triangle(
    WorldMesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 normal, std::uint32_t surface_flags, std::uint32_t contents = 0)
{
    WorldTriangle triangle;
    triangle.points[0] = a;
    triangle.points[1] = b;
    triangle.points[2] = c;
    triangle.normal = normal;
    triangle.surface_flags = surface_flags;
    triangle.contents = contents;
    mesh.collision_triangles.push_back(triangle);
}

void append_displacement_surface(
    WorldMesh& mesh,
    RenderMeshChunk* render_chunk,
    const SourceDisplacementSurface& surface,
    const BspTexInfo& texinfo,
    const WorldMaterial* material,
    const BspFace& face,
    const FaceLightmapPlacement* placement,
    const WorldLightmapAtlas& atlas,
    std::uint32_t surface_flags,
    bool collide_surface)
{
    for (const SourceDisplacementVertex& vertex : surface.vertices)
    {
        include_bounds(mesh, vertex.position);
    }

    if (render_chunk != nullptr && material != nullptr)
    {
        std::array<Vec2, 4> corner_texcoords{};
        std::array<Vec2, 4> corner_lightmap_texcoords{};
        for (std::size_t index = 0; index < surface.corners.size(); ++index)
        {
            corner_texcoords[index] = texture_coordinate(surface.corners[index], texinfo, *material);
            if (placement != nullptr && placement->valid && atlas.has_baked_samples)
            {
                corner_lightmap_texcoords[index] = lightmap_coordinate(surface.corners[index], texinfo, face, *placement, atlas);
            }
        }

        const std::uint32_t first_vertex = static_cast<std::uint32_t>(render_chunk->vertices.size());
        render_chunk->vertices.reserve(render_chunk->vertices.size() + surface.vertices.size());
        for (const SourceDisplacementVertex& source_vertex : surface.vertices)
        {
            WorldMeshVertex vertex;
            vertex.position = source_vertex.position;
            vertex.normal = source_vertex.normal;
            vertex.texcoord = vec2_bilerp(corner_texcoords[0],
                corner_texcoords[1],
                corner_texcoords[2],
                corner_texcoords[3],
                source_vertex.row_fraction,
                source_vertex.column_fraction);
            if (placement != nullptr && placement->valid && atlas.has_baked_samples)
            {
                vertex.lightmap_texcoord = vec2_bilerp(corner_lightmap_texcoords[0],
                    corner_lightmap_texcoords[1],
                    corner_lightmap_texcoords[2],
                    corner_lightmap_texcoords[3],
                    source_vertex.row_fraction,
                    source_vertex.column_fraction);
                vertex.lightmap_weight = 1.0F;
            }
            render_chunk->vertices.push_back(vertex);
        }

        render_chunk->indices.reserve(render_chunk->indices.size() + surface.indices.size());
        for (const std::uint32_t source_index : surface.indices)
        {
            if (source_index < surface.vertices.size())
            {
                render_chunk->indices.push_back(first_vertex + source_index);
            }
        }
    }

    if (!collide_surface)
    {
        return;
    }

    const std::uint32_t contents = surface.contents != 0 ? surface.contents : kContentsSolid;
    for (std::size_t index = 0; index + 2 < surface.indices.size(); index += 3)
    {
        const std::uint32_t ia = surface.indices[index + 0];
        const std::uint32_t ib = surface.indices[index + 1];
        const std::uint32_t ic = surface.indices[index + 2];
        if (ia >= surface.vertices.size() || ib >= surface.vertices.size() || ic >= surface.vertices.size())
        {
            continue;
        }

        const Vec3 a = surface.vertices[ia].position;
        const Vec3 b = surface.vertices[ib].position;
        const Vec3 c = surface.vertices[ic].position;
        const Vec3 area = cross(b - a, c - a);
        if (length(area) <= 0.001F)
        {
            continue;
        }
        append_collision_triangle(mesh, a, b, c, normalize(area), surface_flags, contents);
    }
}

std::size_t append_bsp_brush_collision(
    WorldMesh& mesh,
    const std::vector<BspPlane>& planes,
    const std::vector<BspTexInfo>& texinfo,
    const std::vector<BspBrushSide>& brush_sides,
    const std::vector<BspBrush>& brushes,
    const std::vector<bool>& referenced_brushes)
{
    const std::size_t before = mesh.collision_triangles.size();
    for (std::size_t brush_index = 0; brush_index < brushes.size(); ++brush_index)
    {
        if (brush_index >= referenced_brushes.size() || !referenced_brushes[brush_index])
        {
            continue;
        }

        const BspBrush& brush = brushes[brush_index];
        if (!should_collide_brush(brush.contents) || brush.first_side < 0 || brush.side_count <= 0)
        {
            continue;
        }

        const std::size_t first_side = static_cast<std::size_t>(brush.first_side);
        const std::size_t side_count = static_cast<std::size_t>(brush.side_count);
        if (first_side + side_count > brush_sides.size())
        {
            continue;
        }

        for (std::size_t side_offset = 0; side_offset < side_count; ++side_offset)
        {
            const BspBrushSide& side = brush_sides[first_side + side_offset];
            if (side.bevel != 0 || side.plane >= planes.size())
            {
                continue;
            }

            const BspPlane& plane = planes[side.plane];
            std::optional<std::vector<Vec3>> polygon = polygon_for_brush_side(brush, side_offset, brush_sides, planes);
            if (!polygon || polygon->size() < 3)
            {
                continue;
            }

            const Vec3 normal = normalize(plane.normal);
            const std::uint32_t surface_flags =
                side.texinfo >= 0 && static_cast<std::size_t>(side.texinfo) < texinfo.size() ? texinfo[static_cast<std::size_t>(side.texinfo)].flags : 0;
            for (std::size_t vertex = 1; vertex + 1 < polygon->size(); ++vertex)
            {
                Vec3 a = (*polygon)[0];
                Vec3 b = (*polygon)[vertex];
                Vec3 c = (*polygon)[vertex + 1];
                const Vec3 area = cross(b - a, c - a);
                if (length(area) <= 0.001F)
                {
                    continue;
                }

                if (dot(area, normal) < 0.0F)
                {
                    std::swap(b, c);
                }

                append_collision_triangle(mesh, a, b, c, normal, surface_flags, brush.contents);
            }
        }
    }

    return mesh.collision_triangles.size() - before;
}

void flatten_render_chunks(WorldMesh& mesh, const std::vector<RenderMeshChunk>& chunks)
{
    for (std::uint32_t material_index = 0; material_index < chunks.size(); ++material_index)
    {
        const RenderMeshChunk& chunk = chunks[material_index];
        if (chunk.indices.empty())
        {
            continue;
        }

        const std::uint32_t first_index = static_cast<std::uint32_t>(mesh.indices.size());
        const std::uint32_t first_vertex = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), chunk.vertices.begin(), chunk.vertices.end());
        for (const std::uint32_t index : chunk.indices)
        {
            mesh.indices.push_back(first_vertex + index);
        }

        mesh.batches.push_back(WorldMeshBatch{
            .material_index = material_index,
            .first_index = first_index,
            .index_count = static_cast<std::uint32_t>(chunk.indices.size()),
        });
    }
}

using SourceModelInfoCache = std::unordered_map<std::string, std::optional<SourceModelInfo>>;

const SourceModelInfo* cached_source_model_info(SourceModelInfoCache& cache, const SourceAssetStore& assets, const std::string& model_path)
{
    const std::string key = lower_copy(model_path);
    auto [it, inserted] = cache.try_emplace(key);
    if (inserted)
    {
        it->second = load_source_model_info(assets, model_path);
    }
    return it->second ? &*it->second : nullptr;
}

void resolve_prop_model_info(std::vector<WorldProp>& props, const SourceAssetStore& assets, SourceModelInfoCache& model_cache)
{
    for (WorldProp& prop : props)
    {
        if (prop.model_path.empty())
        {
            continue;
        }

        if (const SourceModelInfo* model = cached_source_model_info(model_cache, assets, prop.model_path))
        {
            prop.bounds_min = model->bounds.mins;
            prop.bounds_max = model->bounds.maxs;
            prop.model_bounds_loaded = true;
            prop.material_name = source_model_material_for_skin(*model, prop.skin, 0);
            prop.model_material_loaded = !prop.material_name.empty();
            prop.model_mesh_loaded = !model->meshes.empty();
        }
    }
}

Vec3 transform_prop_point(const WorldProp& prop, Vec3 point)
{
    point = point * std::max(prop.scale, 0.0F);

    const float pitch = prop.angles.x * (3.14159265358979323846F / 180.0F);
    const float yaw = prop.angles.y * (3.14159265358979323846F / 180.0F);
    const float roll = prop.angles.z * (3.14159265358979323846F / 180.0F);
    const float sp = std::sin(pitch);
    const float cp = std::cos(pitch);
    const float sy = std::sin(yaw);
    const float cy = std::cos(yaw);
    const float sr = std::sin(roll);
    const float cr = std::cos(roll);

    const Vec3 forward{cp * cy, cp * sy, -sp};
    const Vec3 right{(-sr * sp * cy) + (cr * sy), (-sr * sp * sy) - (cr * cy), -sr * cp};
    const Vec3 up{(cr * sp * cy) + (sr * sy), (cr * sp * sy) - (sr * cy), cr * cp};
    return {
        prop.origin.x + (forward.x * point.x) - (right.x * point.y) + (up.x * point.z),
        prop.origin.y + (forward.y * point.x) - (right.y * point.y) + (up.y * point.z),
        prop.origin.z + (forward.z * point.x) - (right.z * point.y) + (up.z * point.z),
    };
}

Vec3 rotate_prop_vector(const WorldProp& prop, Vec3 vector)
{
    const float pitch = prop.angles.x * (3.14159265358979323846F / 180.0F);
    const float yaw = prop.angles.y * (3.14159265358979323846F / 180.0F);
    const float roll = prop.angles.z * (3.14159265358979323846F / 180.0F);
    const float sp = std::sin(pitch);
    const float cp = std::cos(pitch);
    const float sy = std::sin(yaw);
    const float cy = std::cos(yaw);
    const float sr = std::sin(roll);
    const float cr = std::cos(roll);

    const Vec3 forward{cp * cy, cp * sy, -sp};
    const Vec3 right{(-sr * sp * cy) + (cr * sy), (-sr * sp * sy) - (cr * cy), -sr * cp};
    const Vec3 up{(cr * sp * cy) + (sr * sy), (cr * sp * sy) - (sr * cy), cr * cp};
    return normalize({
        (forward.x * vector.x) - (right.x * vector.y) + (up.x * vector.z),
        (forward.y * vector.x) - (right.y * vector.y) + (up.y * vector.z),
        (forward.z * vector.x) - (right.z * vector.y) + (up.z * vector.z),
    });
}

std::uint32_t material_index_for_prop(
    WorldMesh& mesh,
    std::vector<RenderMeshChunk>& chunks,
    std::unordered_map<std::string, std::uint32_t>& material_to_index,
    std::string material_name)
{
    material_name = lower_copy(material_name.empty() ? "__prop_missing" : material_name);
    if (const auto it = material_to_index.find(material_name); it != material_to_index.end())
    {
        return it->second;
    }

    const std::uint32_t material_index = static_cast<std::uint32_t>(mesh.materials.size());
    mesh.materials.push_back(WorldMaterial{
        .name = material_name,
        .width = 1,
        .height = 1,
    });
    chunks.emplace_back();
    material_to_index.emplace(std::move(material_name), material_index);
    return material_index;
}

void append_prop_triangle(RenderMeshChunk& chunk, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 normal = normalize(cross(b - a, c - a));
    const std::uint32_t first_index = static_cast<std::uint32_t>(chunk.vertices.size());
    chunk.vertices.push_back(WorldMeshVertex{a, normal, {0.0F, 0.0F}});
    chunk.vertices.push_back(WorldMeshVertex{b, normal, {1.0F, 0.0F}});
    chunk.vertices.push_back(WorldMeshVertex{c, normal, {1.0F, 1.0F}});
    chunk.indices.push_back(first_index);
    chunk.indices.push_back(first_index + 1);
    chunk.indices.push_back(first_index + 2);
}

void append_prop_box(WorldMesh& mesh, RenderMeshChunk& chunk, const WorldProp& prop)
{
    const Vec3 mins = prop.bounds_min;
    const Vec3 maxs = prop.bounds_max;
    std::array<Vec3, 8> corners{{
        {mins.x, mins.y, mins.z},
        {maxs.x, mins.y, mins.z},
        {maxs.x, maxs.y, mins.z},
        {mins.x, maxs.y, mins.z},
        {mins.x, mins.y, maxs.z},
        {maxs.x, mins.y, maxs.z},
        {maxs.x, maxs.y, maxs.z},
        {mins.x, maxs.y, maxs.z},
    }};

    for (Vec3& corner : corners)
    {
        corner = transform_prop_point(prop, corner);
        include_bounds(mesh, corner);
    }

    constexpr std::array<std::array<std::uint8_t, 4>, 6> faces{{
        {{0, 1, 2, 3}},
        {{4, 7, 6, 5}},
        {{0, 4, 5, 1}},
        {{1, 5, 6, 2}},
        {{2, 6, 7, 3}},
        {{3, 7, 4, 0}},
    }};

    for (const auto& face : faces)
    {
        append_prop_triangle(chunk, corners[face[0]], corners[face[1]], corners[face[2]]);
        append_prop_triangle(chunk, corners[face[0]], corners[face[2]], corners[face[3]]);
    }
}

bool append_prop_collision_triangle(WorldMesh& mesh, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 area = cross(b - a, c - a);
    if (length(area) <= 0.001F)
    {
        return false;
    }

    WorldTriangle triangle;
    triangle.points[0] = a;
    triangle.points[1] = b;
    triangle.points[2] = c;
    triangle.normal = normalize(area);
    triangle.contents = kContentsSolid;
    mesh.collision_triangles.push_back(triangle);
    return true;
}

void append_prop_collision_box(WorldMesh& mesh, const WorldProp& prop)
{
    const Vec3 mins = prop.bounds_min;
    const Vec3 maxs = prop.bounds_max;
    std::array<Vec3, 8> corners{{
        {mins.x, mins.y, mins.z},
        {maxs.x, mins.y, mins.z},
        {maxs.x, maxs.y, mins.z},
        {mins.x, maxs.y, mins.z},
        {mins.x, mins.y, maxs.z},
        {maxs.x, mins.y, maxs.z},
        {maxs.x, maxs.y, maxs.z},
        {mins.x, maxs.y, maxs.z},
    }};

    for (Vec3& corner : corners)
    {
        corner = transform_prop_point(prop, corner);
    }

    constexpr std::array<std::array<std::uint8_t, 4>, 6> faces{{
        {{0, 1, 2, 3}},
        {{4, 7, 6, 5}},
        {{0, 4, 5, 1}},
        {{1, 5, 6, 2}},
        {{2, 6, 7, 3}},
        {{3, 7, 4, 0}},
    }};

    for (const auto& face : faces)
    {
        append_prop_collision_triangle(mesh, corners[face[0]], corners[face[1]], corners[face[2]]);
        append_prop_collision_triangle(mesh, corners[face[0]], corners[face[2]], corners[face[3]]);
    }
}

bool append_prop_collision_model_meshes(WorldMesh& mesh, const WorldProp& prop, const SourceModelInfo& model)
{
    bool appended = false;
    for (const SourceModelMesh& source_mesh : model.meshes)
    {
        for (std::size_t index = 0; index + 2 < source_mesh.indices.size(); index += 3)
        {
            const std::uint32_t ia = source_mesh.indices[index];
            const std::uint32_t ib = source_mesh.indices[index + 1];
            const std::uint32_t ic = source_mesh.indices[index + 2];
            if (ia >= source_mesh.vertices.size() || ib >= source_mesh.vertices.size() || ic >= source_mesh.vertices.size())
            {
                continue;
            }

            appended = append_prop_collision_triangle(mesh,
                transform_prop_point(prop, source_mesh.vertices[ia].position),
                transform_prop_point(prop, source_mesh.vertices[ib].position),
                transform_prop_point(prop, source_mesh.vertices[ic].position)) || appended;
        }
    }
    return appended;
}

void append_prop_collision_meshes(WorldMesh& mesh, const std::vector<WorldProp>& props, const SourceModelInfoCache& model_cache)
{
    for (const WorldProp& prop : props)
    {
        if (prop.solid == kSolidNone || prop.model_path.empty())
        {
            continue;
        }

        if (prop.solid == kSolidBbox)
        {
            if (prop.model_bounds_loaded)
            {
                append_prop_collision_box(mesh, prop);
            }
            continue;
        }

        if (prop.solid == kSolidVPhysics)
        {
            const auto model = model_cache.find(lower_copy(prop.model_path));
            if (model != model_cache.end() && model->second && append_prop_collision_model_meshes(mesh, prop, *model->second))
            {
                continue;
            }

            if (prop.model_bounds_loaded)
            {
                append_prop_collision_box(mesh, prop);
            }
        }
    }
}

bool append_prop_model_meshes(
    WorldMesh& world_mesh,
    std::vector<RenderMeshChunk>& chunks,
    std::unordered_map<std::string, std::uint32_t>& material_to_index,
    const WorldProp& prop,
    const SourceModelInfo& model)
{
    bool appended = false;
    for (const SourceModelMesh& source_mesh : model.meshes)
    {
        if (source_mesh.indices.empty())
        {
            continue;
        }

        std::string material_name = source_model_material_for_skin(model, prop.skin, source_mesh.material);
        if (material_name.empty())
        {
            material_name = prop.material_name.empty() ? prop.model_path : prop.material_name;
        }

        const std::uint32_t material_index = material_index_for_prop(world_mesh, chunks, material_to_index, std::move(material_name));
        RenderMeshChunk& chunk = chunks[material_index];
        const std::size_t first_vertex = chunk.vertices.size();
        if (first_vertex > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            source_mesh.vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - first_vertex)
        {
            continue;
        }
        const std::uint32_t first_vertex_index = static_cast<std::uint32_t>(first_vertex);

        chunk.vertices.reserve(chunk.vertices.size() + source_mesh.vertices.size());
        for (const SourceModelVertex& source_vertex : source_mesh.vertices)
        {
            const Vec3 position = transform_prop_point(prop, source_vertex.position);
            chunk.vertices.push_back(WorldMeshVertex{
                .position = position,
                .normal = rotate_prop_vector(prop, source_vertex.normal),
                .texcoord = source_vertex.texcoord,
            });
            include_bounds(world_mesh, position);
        }

        const std::size_t first_index = chunk.indices.size();
        for (const std::uint32_t source_index : source_mesh.indices)
        {
            if (source_index < source_mesh.vertices.size())
            {
                chunk.indices.push_back(first_vertex_index + source_index);
            }
        }

        if (chunk.indices.size() == first_index)
        {
            chunk.vertices.resize(first_vertex);
            continue;
        }
        appended = true;
    }
    return appended;
}

void append_prop_render_meshes(WorldMesh& mesh, const std::vector<WorldProp>& props, const SourceModelInfoCache& model_cache)
{
    if (props.empty())
    {
        return;
    }

    std::vector<RenderMeshChunk> chunks(mesh.materials.size());
    std::unordered_map<std::string, std::uint32_t> material_to_index;
    for (std::uint32_t index = 0; index < mesh.materials.size(); ++index)
    {
        material_to_index.emplace(lower_copy(mesh.materials[index].name), index);
    }

    std::size_t skipped_models = 0;
    for (const WorldProp& prop : props)
    {
        const auto model = model_cache.find(lower_copy(prop.model_path));
        if (model != model_cache.end() && model->second && append_prop_model_meshes(mesh, chunks, material_to_index, prop, *model->second))
        {
            continue;
        }

        ++skipped_models;
    }

    if (skipped_models > 0)
    {
        log_warning("skipped {} prop model(s) without loadable Source studio meshes", skipped_models);
    }
    flatten_render_chunks(mesh, chunks);
}

WorldMesh build_bsp_world_mesh(const std::vector<unsigned char>& bytes)
{
    const std::vector<Vec3> vertices = read_bsp_vertices(bytes);
    const std::vector<BspEdge> edges = read_bsp_edges(bytes);
    const std::vector<std::int32_t> surfedges = read_bsp_surfedges(bytes);
    const std::vector<BspTexData> texdata = read_bsp_texdata(bytes);
    const std::vector<BspTexInfo> texinfo = read_bsp_texinfo(bytes);
    const BspLightmapData lightmap_data = read_bsp_lightmap_data(bytes);
    const std::vector<BspFace> faces = read_bsp_faces(bytes, lightmap_data.prefer_hdr_faces);
    const std::vector<SourceBspDisplacement> displacements = read_source_bsp_displacements(bytes);
    BspLightmapBuildResult lightmaps = build_lightmap_atlas(faces, texinfo, lightmap_data);
    const bool has_brush_collision_lumps =
        read_lump(bytes, kPlanesLump).length > 0 && read_lump(bytes, kBrushesLump).length > 0 && read_lump(bytes, kBrushSidesLump).length > 0;

    WorldMesh mesh;
    mesh.lightmap_atlas = std::move(lightmaps.atlas);
    std::vector<RenderMeshChunk> render_chunks;
    std::unordered_map<std::int32_t, std::uint32_t> texdata_to_material;
    std::size_t skipped_faces = 0;
    for (std::size_t face_index = 0; face_index < faces.size(); ++face_index)
    {
        const BspFace& face = faces[face_index];
        const std::uint32_t surface_flags = surface_flags_for_face(face, texinfo);
        if (has_any_surface_flag(surface_flags, kSurfaceSky2D | kSurfaceSky))
        {
            mesh.has_sky_surfaces = true;
        }

        const BspTexInfo* face_texinfo = texinfo_for_face(face, texinfo);
        const bool render_surface = should_render_surface(surface_flags);
        const bool collide_surface = (!has_brush_collision_lumps || face.dispinfo >= 0) && should_collide_surface(surface_flags);
        if (!render_surface && !collide_surface)
        {
            continue;
        }

        std::optional<std::vector<Vec3>> polygon = polygon_for_face(face, vertices, edges, surfedges);
        if (!polygon || polygon->size() < 3)
        {
            ++skipped_faces;
            continue;
        }

        if (const SourceBspDisplacement* displacement = source_displacement_by_index(displacements, face.dispinfo))
        {
            if (polygon->size() == 4)
            {
                if (std::optional<SourceDisplacementSurface> surface = build_source_displacement_surface(*displacement, *polygon))
                {
                    const BspTexInfo fallback_texinfo{};
                    const BspTexInfo& resolved_texinfo = face_texinfo != nullptr ? *face_texinfo : fallback_texinfo;
                    RenderMeshChunk* render_chunk = nullptr;
                    const WorldMaterial* material = nullptr;
                    if (render_surface)
                    {
                        const std::uint32_t material_index = material_index_for_texinfo(mesh, render_chunks, texdata_to_material, face_texinfo, texdata);
                        render_chunk = &render_chunks[material_index];
                        material = &mesh.materials[material_index];
                    }
                    const FaceLightmapPlacement* placement =
                        face_index < lightmaps.placements.size() && lightmaps.placements[face_index].valid ? &lightmaps.placements[face_index] : nullptr;
                    append_displacement_surface(mesh,
                        render_chunk,
                        *surface,
                        resolved_texinfo,
                        material,
                        face,
                        placement,
                        mesh.lightmap_atlas,
                        surface_flags,
                        collide_surface);
                    continue;
                }
            }
            ++skipped_faces;
            continue;
        }

        const Vec3 normal = polygon_normal(*polygon);
        for (const Vec3 point : *polygon)
        {
            include_bounds(mesh, point);
        }

        for (std::size_t vertex = 1; vertex + 1 < polygon->size(); ++vertex)
        {
            const Vec3 a = (*polygon)[0];
            const Vec3 b = (*polygon)[vertex];
            const Vec3 c = (*polygon)[vertex + 1];
            if (length(cross(b - a, c - a)) <= 0.001F)
            {
                continue;
            }

            if (render_surface)
            {
                const std::uint32_t material_index = material_index_for_texinfo(mesh, render_chunks, texdata_to_material, face_texinfo, texdata);
                const BspTexInfo fallback_texinfo{};
                const BspTexInfo& resolved_texinfo = face_texinfo != nullptr ? *face_texinfo : fallback_texinfo;
                const FaceLightmapPlacement* placement =
                    face_index < lightmaps.placements.size() && lightmaps.placements[face_index].valid ? &lightmaps.placements[face_index] : nullptr;
                append_render_triangle(render_chunks[material_index],
                    a,
                    b,
                    c,
                    normal,
                    resolved_texinfo,
                    mesh.materials[material_index],
                    face,
                    placement,
                    mesh.lightmap_atlas);
            }

            if (collide_surface)
            {
                append_collision_triangle(mesh, a, b, c, normal, surface_flags);
            }
        }
    }

    if (has_brush_collision_lumps)
    {
        const std::vector<BspPlane> planes = read_bsp_planes(bytes);
        const std::vector<BspBrushSide> brush_sides = read_bsp_brush_sides(bytes);
        const std::vector<BspBrush> brushes = read_bsp_brushes(bytes);
        const std::vector<BspModel> models = read_bsp_models(bytes);
        const std::vector<BspNode> nodes = read_bsp_nodes(bytes);
        const std::vector<BspLeaf> leaves = read_bsp_leaves(bytes);
        const std::vector<std::uint16_t> leaf_brushes = read_bsp_leaf_brushes(bytes);
        const std::vector<bool> referenced_brushes = collect_world_model_brushes(brushes, models, nodes, leaves, leaf_brushes);
        const std::size_t brush_collision_count =
            append_bsp_brush_collision(mesh, planes, texinfo, brush_sides, brushes, referenced_brushes);
        if (brush_collision_count == 0 && std::any_of(brushes.begin(), brushes.end(), [](const BspBrush& brush) {
                return should_collide_brush(brush.contents);
            }))
        {
            log_warning("BSP contains Source collision brush contents but no brush collision triangles were generated");
        }
    }

    if (skipped_faces > 0)
    {
        log_warning("skipped {} malformed BSP face(s)", skipped_faces);
    }

    flatten_render_chunks(mesh, render_chunks);
    return mesh;
}

LoadedWorld load_source_bsp(
    const ContentFileSystem& filesystem,
    std::string name,
    std::filesystem::path relative_path,
    std::filesystem::path resolved_path)
{
    std::vector<unsigned char> bytes = read_binary_file(resolved_path);
    if (bytes.size() < kBspHeaderSize)
    {
        throw std::runtime_error("BSP header is truncated");
    }

    const std::uint32_t ident = read_u32_le(bytes, 0);
    if (ident != kBspIdent)
    {
        throw std::runtime_error("not a Source BSP file");
    }

    LoadedWorld world;
    world.name = std::move(name);
    world.relative_path = std::move(relative_path);
    world.resolved_path = std::move(resolved_path);
    world.kind = WorldAssetKind::SourceBsp;
    world.asset_version = read_u32_le(bytes, 4);
    world.map_revision = read_u32_le(bytes, 8 + (kBspLumpCount * 16));
    world.byte_size = bytes.size();
    world.checksum = fnv1a32(bytes);

    const BspLump entities_lump = read_lump(bytes, kEntitiesLump);
    const std::string entities_text = entity_lump_text(bytes, entities_lump);
    const std::vector<ParsedEntity> entities = parse_entity_lump(entities_text);
    world.entity_count = entities.size();
    world.entities = collect_world_entities(entities);

    for (const ParsedEntity& entity : entities)
    {
        const std::optional<std::string> class_name = find_property(entity, "classname");
        if (class_name && *class_name == "worldspawn")
        {
            world.worldspawn = entity.properties;
            break;
        }
    }

    world.spawn_points = collect_spawn_points(entities);
    world.lights = collect_lights(entities);
    world.embedded_assets = read_bsp_pakfile_assets(bytes);
    world.props = read_source_static_props(bytes);
    std::vector<WorldProp> entity_props = collect_entity_props(entities);
    world.props.insert(world.props.end(), std::make_move_iterator(entity_props.begin()), std::make_move_iterator(entity_props.end()));
    SourceAssetStore source_assets(filesystem, &world.embedded_assets);
    SourceModelInfoCache source_model_cache;
    resolve_prop_model_info(world.props, source_assets, source_model_cache);
    world.mesh = build_bsp_world_mesh(bytes);
    append_prop_collision_meshes(world.mesh, world.props, source_model_cache);
    append_prop_render_meshes(world.mesh, world.props, source_model_cache);
    return world;
}

std::optional<std::string> extract_json_string(std::string_view text, std::string_view key)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t key_pos = text.find(needle);
    if (key_pos == std::string_view::npos)
    {
        return std::nullopt;
    }

    const std::size_t colon = text.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos)
    {
        return std::nullopt;
    }

    std::size_t cursor = colon + 1;
    skip_whitespace(text, cursor);
    if (cursor >= text.size() || text[cursor] != '"')
    {
        return std::nullopt;
    }

    return parse_quoted(text, cursor);
}

std::optional<Vec3> extract_json_position(std::string_view text, std::size_t search_begin = 0)
{
    const std::size_t position = text.find("\"position\"", search_begin);
    if (position == std::string_view::npos)
    {
        return std::nullopt;
    }

    const std::size_t object_begin = text.find('{', position);
    const std::size_t object_end = text.find('}', object_begin);
    if (object_begin == std::string_view::npos || object_end == std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto find_component = [&](std::string_view component) -> std::optional<float> {
        const std::string needle = "\"" + std::string(component) + "\"";
        const std::size_t key = text.find(needle, object_begin);
        if (key == std::string_view::npos || key > object_end)
        {
            return std::nullopt;
        }

        const std::size_t colon = text.find(':', key + needle.size());
        if (colon == std::string_view::npos || colon > object_end)
        {
            return std::nullopt;
        }

        std::size_t cursor = colon + 1;
        skip_whitespace(text, cursor);
        const std::size_t value_begin = cursor;
        while (cursor < object_end && (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0 || text[cursor] == '-' ||
                                         text[cursor] == '+' || text[cursor] == '.'))
        {
            ++cursor;
        }

        return parse_float(text.substr(value_begin, cursor - value_begin));
    };

    const std::optional<float> x = find_component("x");
    const std::optional<float> y = find_component("y");
    const std::optional<float> z = find_component("z");
    if (!x || !y || !z)
    {
        return std::nullopt;
    }

    return Vec3{*x, *y, *z};
}

std::size_t count_level_entities(std::string_view text)
{
    std::size_t count = 0;
    std::size_t cursor = 0;
    while ((cursor = text.find("\"type\"", cursor)) != std::string_view::npos)
    {
        ++count;
        cursor += 6;
    }
    return count;
}

LoadedWorld load_openstrike_level(std::string name, std::filesystem::path relative_path, std::filesystem::path resolved_path)
{
    const std::string text = read_text_file(resolved_path);

    LoadedWorld world;
    world.name = extract_json_string(text, "level_name").value_or(std::move(name));
    world.relative_path = std::move(relative_path);
    world.resolved_path = std::move(resolved_path);
    world.kind = WorldAssetKind::OpenStrikeLevel;
    world.asset_version = 1;
    world.byte_size = text.size();
    world.entity_count = count_level_entities(text);

    WorldEntity worldspawn;
    worldspawn.class_name = "worldspawn";
    worldspawn.properties["classname"] = worldspawn.class_name;
    world.entities.push_back(std::move(worldspawn));

    const std::size_t player_entity = text.find("\"name\": \"player\"");
    if (const std::optional<Vec3> origin = extract_json_position(text, player_entity == std::string_view::npos ? 0 : player_entity))
    {
        world.spawn_points.push_back(WorldSpawnPoint{
            .class_name = "openstrike_spawn",
            .origin = *origin,
        });
        WorldEntity spawn_entity;
        spawn_entity.class_name = "openstrike_spawn";
        spawn_entity.properties["classname"] = spawn_entity.class_name;
        spawn_entity.properties["origin"] =
            std::to_string(origin->x) + " " + std::to_string(origin->y) + " " + std::to_string(origin->z);
        world.entities.push_back(std::move(spawn_entity));
    }

    return world;
}

std::optional<LoadedWorld> try_load_candidate(
    const ContentFileSystem& filesystem, std::string_view requested_name, const std::filesystem::path& relative_path)
{
    const std::optional<std::filesystem::path> resolved = filesystem.resolve(relative_path, "GAME");
    if (!resolved)
    {
        return std::nullopt;
    }

    const std::string relative = relative_path.generic_string();
    if (has_suffix(relative, ".bsp"))
    {
        return load_source_bsp(filesystem, canonical_map_name(requested_name), relative_path, *resolved);
    }

    if (has_suffix(relative, ".level.json"))
    {
        return load_openstrike_level(canonical_map_name(requested_name), relative_path, *resolved);
    }

    return std::nullopt;
}

void add_maps_from_root(
    std::set<std::string>& maps, const std::filesystem::path& root, const std::filesystem::path& relative_dir, std::string_view suffix)
{
    std::error_code error;
    const std::filesystem::path map_root = root / relative_dir;
    if (!std::filesystem::is_directory(map_root, error))
    {
        return;
    }

    for (std::filesystem::recursive_directory_iterator it(map_root, error), end; it != end && !error; it.increment(error))
    {
        if (!it->is_regular_file(error))
        {
            continue;
        }

        std::string filename = it->path().filename().generic_string();
        if (!has_suffix(filename, suffix))
        {
            continue;
        }

        std::filesystem::path relative = std::filesystem::relative(it->path(), map_root, error);
        if (error)
        {
            error.clear();
            continue;
        }

        std::string name = relative.generic_string();
        name = remove_suffix(std::move(name), suffix);
        maps.insert(name);
    }
}

bool contains_case_insensitive(std::string_view value, std::string_view filter)
{
    if (filter.empty() || filter == "*")
    {
        return true;
    }

    return lower_copy(value).find(lower_copy(filter)) != std::string::npos;
}
}

bool WorldManager::load_map(std::string_view map_name, ContentFileSystem& filesystem)
{
    const std::string canonical = canonical_map_name(map_name);
    if (canonical.empty())
    {
        log_warning("map load failed: no map specified");
        return false;
    }

    for (const std::filesystem::path& candidate : map_candidates(map_name))
    {
        try
        {
            std::optional<LoadedWorld> loaded = try_load_candidate(filesystem, canonical, candidate);
            if (!loaded)
            {
                continue;
            }

            current_world_ = std::move(*loaded);
            ++generation_;

            log_info("loaded {} world '{}' from '{}' entities={} spawns={} props={} lights={} embedded_assets={} render_tris={} collision_tris={} bytes={}",
                to_string(current_world_->kind),
                current_world_->name,
                current_world_->resolved_path.string(),
                current_world_->entity_count,
                current_world_->spawn_points.size(),
                current_world_->props.size(),
                current_world_->lights.size(),
                current_world_->embedded_assets.size(),
                current_world_->mesh.indices.size() / 3,
                current_world_->mesh.collision_triangles.size(),
                current_world_->byte_size);
            return true;
        }
        catch (const std::exception& error)
        {
            log_warning("map load failed for '{}': {}", candidate.generic_string(), error.what());
            return false;
        }
    }

    log_warning("map load failed: '{}' not found under GAME search paths", canonical);
    return false;
}

void WorldManager::unload()
{
    if (current_world_)
    {
        log_info("unloaded world '{}'", current_world_->name);
    }

    current_world_.reset();
    ++generation_;
}

const LoadedWorld* WorldManager::current_world() const
{
    return current_world_ ? &*current_world_ : nullptr;
}

std::uint64_t WorldManager::generation() const
{
    return generation_;
}

std::vector<std::string> WorldManager::list_maps(const ContentFileSystem& filesystem, std::string_view filter) const
{
    std::set<std::string> unique_maps;
    for (const SearchPath& path : filesystem.search_paths("GAME"))
    {
        add_maps_from_root(unique_maps, path.root, "maps", ".bsp");
        add_maps_from_root(unique_maps, path.root, "assets/levels", ".level.json");
    }

    std::vector<std::string> maps;
    for (const std::string& map : unique_maps)
    {
        if (contains_case_insensitive(map, filter))
        {
            maps.push_back(map);
        }
    }

    return maps;
}

std::optional<float> find_floor_z(const LoadedWorld& world, Vec3 origin, float max_drop)
{
    std::optional<float> best_z;
    const float min_z = origin.z - std::max(0.0F, max_drop);
    constexpr float kEpsilon = 0.0001F;
    constexpr float kWalkableNormalZ = 0.35F;

    for (const WorldTriangle& triangle : world.mesh.collision_triangles)
    {
        if (!triangle_blocks_player_movement(triangle))
        {
            continue;
        }

        if (std::fabs(triangle.normal.z) < kWalkableNormalZ)
        {
            continue;
        }

        const Vec3 a = triangle.points[0];
        const Vec3 b = triangle.points[1];
        const Vec3 c = triangle.points[2];
        const float denominator = ((b.y - c.y) * (a.x - c.x)) + ((c.x - b.x) * (a.y - c.y));
        if (std::fabs(denominator) <= kEpsilon)
        {
            continue;
        }

        const float weight_a = (((b.y - c.y) * (origin.x - c.x)) + ((c.x - b.x) * (origin.y - c.y))) / denominator;
        const float weight_b = (((c.y - a.y) * (origin.x - c.x)) + ((a.x - c.x) * (origin.y - c.y))) / denominator;
        const float weight_c = 1.0F - weight_a - weight_b;
        if (weight_a < -kEpsilon || weight_b < -kEpsilon || weight_c < -kEpsilon)
        {
            continue;
        }

        const float z = (weight_a * a.z) + (weight_b * b.z) + (weight_c * c.z);
        if (z > origin.z + 0.5F || z < min_z)
        {
            continue;
        }

        if (!best_z || z > *best_z)
        {
            best_z = z;
        }
    }

    return best_z;
}

std::string_view to_string(WorldAssetKind kind)
{
    switch (kind)
    {
    case WorldAssetKind::SourceBsp:
        return "source-bsp";
    case WorldAssetKind::OpenStrikeLevel:
        return "openstrike-level";
    }

    return "unknown";
}
}
