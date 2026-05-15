#include "openstrike/world/world.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_model.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
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
constexpr std::size_t kTexInfoLump = 6;
constexpr std::size_t kFacesLump = 7;
constexpr std::size_t kEdgesLump = 12;
constexpr std::size_t kSurfEdgesLump = 13;
constexpr std::size_t kGameLump = 35;
constexpr std::size_t kPakFileLump = 40;
constexpr std::size_t kTexDataStringDataLump = 43;
constexpr std::size_t kTexDataStringTableLump = 44;
constexpr std::uint32_t kSurfaceSky2D = 0x0002;
constexpr std::uint32_t kSurfaceSky = 0x0004;
constexpr std::uint32_t kSurfaceNoDraw = 0x0080;
constexpr std::uint32_t kSurfaceSkip = 0x0200;

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

struct BspTexInfo
{
    std::array<std::array<float, 4>, 2> texture_vecs{};
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
};

struct RenderMeshChunk
{
    std::vector<WorldMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
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

std::optional<std::string> find_property(const ParsedEntity& entity, std::string_view name)
{
    const auto it = entity.properties.find(std::string(name));
    if (it == entity.properties.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool is_spawn_class(std::string_view class_name)
{
    return class_name == "info_player_start" || class_name == "info_player_deathmatch" ||
           class_name == "info_player_terrorist" || class_name == "info_player_counterterrorist" ||
           class_name == "info_player_teamspawn";
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

bool is_entity_prop_class(std::string_view class_name)
{
    const std::string lower = lower_copy(class_name);
    return lower == "prop_static" || lower == "prop_dynamic" || lower == "prop_dynamic_override" ||
           lower == "prop_physics" || lower == "prop_physics_multiplayer" || lower == "prop_detail";
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
    constexpr std::uint32_t skip_flags = kSurfaceSky2D | kSurfaceSky | kSurfaceSkip;
    return !has_any_surface_flag(flags, skip_flags);
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

std::vector<BspEdge> read_bsp_edges(const std::vector<unsigned char>& bytes)
{
    return read_lump_array<BspEdge>(bytes, read_lump(bytes, kEdgesLump), 4, "edges", [&](std::size_t offset) {
        return BspEdge{{
            read_u16_le(bytes, offset),
            read_u16_le(bytes, offset + 2),
        }};
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

struct GameLumpData
{
    std::span<const unsigned char> bytes;
    std::uint16_t version = 0;
};

std::uint32_t fourcc(std::string_view text)
{
    if (text.size() != 4)
    {
        return 0;
    }

    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[3])) << 24U);
}

bool is_static_prop_game_lump_id(std::uint32_t id)
{
    return id == fourcc("sprp") || id == fourcc("prps");
}

std::optional<GameLumpData> find_static_prop_game_lump(const std::vector<unsigned char>& bytes)
{
    const BspLump game_lump = read_lump(bytes, kGameLump);
    if (game_lump.length == 0)
    {
        return std::nullopt;
    }

    validate_lump_range(bytes, game_lump, "game");
    std::span<const unsigned char> data(bytes.data() + game_lump.offset, game_lump.length);
    if (data.size() < 4)
    {
        return std::nullopt;
    }

    const std::int32_t count = read_s32_le(data, 0);
    if (count <= 0 || count > 4096)
    {
        return std::nullopt;
    }

    std::size_t cursor = 4;
    for (std::int32_t index = 0; index < count; ++index)
    {
        if (cursor + 16 > data.size())
        {
            break;
        }

        const std::uint32_t id = read_u32_le(data, cursor);
        const std::uint16_t flags = read_u16_le(data, cursor + 4);
        const std::uint16_t version = read_u16_le(data, cursor + 6);
        const std::uint32_t file_offset = read_u32_le(data, cursor + 8);
        const std::uint32_t file_length = read_u32_le(data, cursor + 12);
        cursor += 16;
        if (!is_static_prop_game_lump_id(id))
        {
            continue;
        }

        if ((flags & 0x0001U) != 0)
        {
            log_warning("static prop game lump is compressed; skipping static props");
            return std::nullopt;
        }

        const std::uint64_t begin = file_offset;
        const std::uint64_t end = begin + file_length;
        if (end > bytes.size() || end < begin)
        {
            log_warning("static prop game lump points outside the BSP file");
            return std::nullopt;
        }

        return GameLumpData{
            .bytes = std::span<const unsigned char>(bytes.data() + begin, file_length),
            .version = version,
        };
    }

    return std::nullopt;
}

std::string read_fixed_string(std::span<const unsigned char> bytes, std::size_t offset, std::size_t length)
{
    if (offset + length > bytes.size())
    {
        throw std::runtime_error("unexpected end of static prop lump");
    }

    const char* begin = reinterpret_cast<const char*>(bytes.data() + offset);
    const char* end = begin + length;
    const char* zero = std::find(begin, end, '\0');
    std::string value(begin, zero);
    normalize_slashes(value);
    return value;
}

std::size_t static_prop_record_size(std::uint16_t version)
{
    switch (version)
    {
    case 4:
        return 56;
    case 5:
        return 60;
    case 6:
        return 64;
    case 7:
    case 8:
        return 68;
    case 9:
        return 72;
    default:
        return version >= 10 ? 76 : 0;
    }
}

Vec3 read_vec3(std::span<const unsigned char> bytes, std::size_t offset)
{
    return Vec3{
        read_f32_le(bytes, offset),
        read_f32_le(bytes, offset + 4),
        read_f32_le(bytes, offset + 8),
    };
}

WorldProp read_static_prop_record(
    std::span<const unsigned char> record,
    std::uint16_t version,
    const std::vector<std::string>& dictionary)
{
    WorldProp prop;
    prop.kind = WorldPropKind::StaticProp;
    prop.class_name = "static_prop";
    prop.origin = read_vec3(record, 0);
    prop.angles = read_vec3(record, 12);
    const std::uint16_t prop_type = read_u16_le(record, 24);
    if (prop_type < dictionary.size())
    {
        prop.model_path = dictionary[prop_type];
    }
    prop.solid = record[30];
    prop.flags = record[31];
    prop.skin = read_s32_le(record, 32);
    prop.lighting_origin = read_vec3(record, 44);
    if (version >= 7 && record.size() >= 68)
    {
        prop.color[0] = static_cast<float>(record[64]) / 255.0F;
        prop.color[1] = static_cast<float>(record[65]) / 255.0F;
        prop.color[2] = static_cast<float>(record[66]) / 255.0F;
        prop.color[3] = static_cast<float>(record[67]) / 255.0F;
    }
    if (version >= 10 && record.size() >= 76)
    {
        prop.flags_ex = read_u32_le(record, 72);
    }
    return prop;
}

std::vector<WorldProp> read_static_props(const std::vector<unsigned char>& bytes)
{
    const std::optional<GameLumpData> lump = find_static_prop_game_lump(bytes);
    if (!lump)
    {
        return {};
    }

    std::vector<WorldProp> props;
    try
    {
        std::size_t cursor = 0;
        const auto read_count = [&](std::string_view label) -> std::int32_t {
            if (cursor + 4 > lump->bytes.size())
            {
                throw std::runtime_error("static prop " + std::string(label) + " count is truncated");
            }
            const std::int32_t count = read_s32_le(lump->bytes, cursor);
            cursor += 4;
            if (count < 0 || count > 1'000'000)
            {
                throw std::runtime_error("static prop " + std::string(label) + " count is invalid");
            }
            return count;
        };

        const std::int32_t dictionary_count = read_count("dictionary");
        std::vector<std::string> dictionary;
        dictionary.reserve(static_cast<std::size_t>(dictionary_count));
        for (std::int32_t index = 0; index < dictionary_count; ++index)
        {
            dictionary.push_back(read_fixed_string(lump->bytes, cursor, 128));
            cursor += 128;
        }

        const std::int32_t leaf_count = read_count("leaf");
        const std::uint64_t leaf_bytes = static_cast<std::uint64_t>(leaf_count) * 2U;
        if (cursor + leaf_bytes > lump->bytes.size())
        {
            throw std::runtime_error("static prop leaf list is truncated");
        }
        cursor += static_cast<std::size_t>(leaf_bytes);

        const std::int32_t prop_count = read_count("instance");
        const std::size_t record_size = static_prop_record_size(lump->version);
        if (record_size == 0)
        {
            log_warning("unsupported static prop game lump version {}", lump->version);
            return {};
        }

        props.reserve(static_cast<std::size_t>(prop_count));
        for (std::int32_t index = 0; index < prop_count; ++index)
        {
            if (cursor + record_size > lump->bytes.size())
            {
                throw std::runtime_error("static prop instance list is truncated");
            }
            WorldProp prop = read_static_prop_record(lump->bytes.subspan(cursor, record_size), lump->version, dictionary);
            cursor += record_size;
            if (!prop.model_path.empty())
            {
                props.push_back(std::move(prop));
            }
        }
    }
    catch (const std::exception& error)
    {
        log_warning("failed to parse static props: {}", error.what());
        props.clear();
    }

    return props;
}

std::vector<BspFace> read_bsp_faces(const std::vector<unsigned char>& bytes)
{
    BspLump lump = read_lump(bytes, kFacesLump);
    if (const BspLump hdr_faces = read_lump(bytes, 58); hdr_faces.length > 0)
    {
        lump = hdr_faces;
    }

    return read_lump_array<BspFace>(bytes, lump, 56, "faces", [&](std::size_t offset) {
        return BspFace{
            .plane = read_u16_le(bytes, offset),
            .side = bytes[offset + 2],
            .first_edge = read_s32_le(bytes, offset + 4),
            .edge_count = read_s16_le(bytes, offset + 8),
            .texinfo = read_s16_le(bytes, offset + 10),
        };
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

void append_render_triangle(RenderMeshChunk& chunk, Vec3 a, Vec3 b, Vec3 c, Vec3 normal, const BspTexInfo& texinfo, const WorldMaterial& material)
{
    const std::uint32_t first_index = static_cast<std::uint32_t>(chunk.vertices.size());
    chunk.vertices.push_back(WorldMeshVertex{a, normal, texture_coordinate(a, texinfo, material)});
    chunk.vertices.push_back(WorldMeshVertex{b, normal, texture_coordinate(b, texinfo, material)});
    chunk.vertices.push_back(WorldMeshVertex{c, normal, texture_coordinate(c, texinfo, material)});
    chunk.indices.push_back(first_index);
    chunk.indices.push_back(first_index + 1);
    chunk.indices.push_back(first_index + 2);
}

void append_collision_triangle(WorldMesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 normal, std::uint32_t surface_flags)
{
    WorldTriangle triangle;
    triangle.points[0] = a;
    triangle.points[1] = b;
    triangle.points[2] = c;
    triangle.normal = normal;
    triangle.surface_flags = surface_flags;
    mesh.collision_triangles.push_back(triangle);
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
        const std::uint32_t first_vertex = static_cast<std::uint32_t>(chunk.vertices.size());
        for (const std::uint32_t source_index : source_mesh.indices)
        {
            if (source_index >= source_mesh.vertices.size())
            {
                continue;
            }

            const SourceModelVertex& source_vertex = source_mesh.vertices[source_index];
            const Vec3 position = transform_prop_point(prop, source_vertex.position);
            chunk.vertices.push_back(WorldMeshVertex{
                .position = position,
                .normal = rotate_prop_vector(prop, source_vertex.normal),
                .texcoord = source_vertex.texcoord,
            });
            chunk.indices.push_back(first_vertex + static_cast<std::uint32_t>(chunk.vertices.size() - first_vertex - 1));
            include_bounds(world_mesh, position);
            appended = true;
        }
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

    for (const WorldProp& prop : props)
    {
        const auto model = model_cache.find(lower_copy(prop.model_path));
        if (model != model_cache.end() && model->second && append_prop_model_meshes(mesh, chunks, material_to_index, prop, *model->second))
        {
            continue;
        }

        const std::string material_name = prop.material_name.empty() ? prop.model_path : prop.material_name;
        const std::uint32_t material_index = material_index_for_prop(mesh, chunks, material_to_index, material_name);
        append_prop_box(mesh, chunks[material_index], prop);
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
    const std::vector<BspFace> faces = read_bsp_faces(bytes);

    WorldMesh mesh;
    std::vector<RenderMeshChunk> render_chunks;
    std::unordered_map<std::int32_t, std::uint32_t> texdata_to_material;
    std::size_t skipped_faces = 0;
    for (const BspFace& face : faces)
    {
        const std::uint32_t surface_flags = surface_flags_for_face(face, texinfo);
        if (has_any_surface_flag(surface_flags, kSurfaceSky2D | kSurfaceSky))
        {
            mesh.has_sky_surfaces = true;
        }

        const BspTexInfo* face_texinfo = texinfo_for_face(face, texinfo);
        const bool render_surface = should_render_surface(surface_flags);
        const bool collide_surface = should_collide_surface(surface_flags);
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
                append_render_triangle(render_chunks[material_index],
                    a,
                    b,
                    c,
                    normal,
                    face_texinfo != nullptr ? *face_texinfo : BspTexInfo{},
                    mesh.materials[material_index]);
            }

            if (collide_surface)
            {
                append_collision_triangle(mesh, a, b, c, normal, surface_flags);
            }
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
    world.embedded_assets = read_bsp_pakfile_assets(bytes);
    world.props = read_static_props(bytes);
    std::vector<WorldProp> entity_props = collect_entity_props(entities);
    world.props.insert(world.props.end(), std::make_move_iterator(entity_props.begin()), std::make_move_iterator(entity_props.end()));
    SourceAssetStore source_assets(filesystem, &world.embedded_assets);
    SourceModelInfoCache source_model_cache;
    resolve_prop_model_info(world.props, source_assets, source_model_cache);
    world.mesh = build_bsp_world_mesh(bytes);
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

    const std::size_t player_entity = text.find("\"name\": \"player\"");
    if (const std::optional<Vec3> origin = extract_json_position(text, player_entity == std::string_view::npos ? 0 : player_entity))
    {
        world.spawn_points.push_back(WorldSpawnPoint{
            .class_name = "openstrike_spawn",
            .origin = *origin,
        });
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

            log_info("loaded {} world '{}' from '{}' entities={} spawns={} props={} embedded_assets={} render_tris={} collision_tris={} bytes={}",
                to_string(current_world_->kind),
                current_world_->name,
                current_world_->resolved_path.string(),
                current_world_->entity_count,
                current_world_->spawn_points.size(),
                current_world_->props.size(),
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
