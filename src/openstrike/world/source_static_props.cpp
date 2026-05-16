#include "openstrike/world/source_static_props.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/world/source_bsp_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace openstrike
{
namespace
{
constexpr std::size_t kGameLump = 35;
constexpr std::size_t kStaticPropNameLength = 128;
constexpr std::uint16_t kStaticPropMinVersion = 4;
constexpr std::uint16_t kStaticPropMaxVersion = 10;

struct GameLumpData
{
    std::span<const unsigned char> bytes;
    std::uint16_t version = 0;
};

void normalize_slashes(std::string& path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
}

bool is_static_prop_game_lump_id(std::uint32_t id)
{
    return id == source_bsp::fourcc("sprp") || id == source_bsp::fourcc("prps");
}

std::optional<GameLumpData> find_static_prop_game_lump(std::span<const unsigned char> bsp_bytes)
{
    const source_bsp::Lump game_lump = source_bsp::read_lump(bsp_bytes, kGameLump);
    if (game_lump.length == 0)
    {
        return std::nullopt;
    }

    const std::span<const unsigned char> data = source_bsp::lump_bytes(bsp_bytes, game_lump, "game");
    if (data.size() < 4)
    {
        return std::nullopt;
    }

    const std::int32_t count = source_bsp::read_s32(data, 0);
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

        const std::uint32_t id = source_bsp::read_u32(data, cursor);
        const std::uint16_t flags = source_bsp::read_u16(data, cursor + 4);
        const std::uint16_t version = source_bsp::read_u16(data, cursor + 6);
        const std::uint32_t file_offset = source_bsp::read_u32(data, cursor + 8);
        const std::uint32_t file_length = source_bsp::read_u32(data, cursor + 12);
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

        if (version < kStaticPropMinVersion || version > kStaticPropMaxVersion)
        {
            log_warning("unsupported static prop game lump version {}", version);
            return std::nullopt;
        }

        const std::uint64_t begin = file_offset;
        const std::uint64_t end = begin + file_length;
        if (end > bsp_bytes.size() || end < begin)
        {
            log_warning("static prop game lump points outside the BSP file");
            return std::nullopt;
        }

        return GameLumpData{
            .bytes = bsp_bytes.subspan(static_cast<std::size_t>(begin), file_length),
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
    case 10:
        return 76;
    default:
        return 0;
    }
}

WorldProp read_static_prop_record(
    std::span<const unsigned char> record,
    std::uint16_t version,
    const std::vector<std::string>& dictionary)
{
    WorldProp prop;
    prop.kind = WorldPropKind::StaticProp;
    prop.class_name = "static_prop";
    prop.origin = source_bsp::read_vec3(record, 0);
    prop.angles = source_bsp::read_vec3(record, 12);
    const std::uint16_t prop_type = source_bsp::read_u16(record, 24);
    if (prop_type < dictionary.size())
    {
        prop.model_path = dictionary[prop_type];
    }
    prop.first_leaf = source_bsp::read_u16(record, 26);
    prop.leaf_count = source_bsp::read_u16(record, 28);
    prop.solid = record[30];
    prop.flags = record[31];
    prop.skin = source_bsp::read_s32(record, 32);
    prop.fade_min_dist = source_bsp::read_f32(record, 36);
    prop.fade_max_dist = source_bsp::read_f32(record, 40);
    prop.lighting_origin = source_bsp::read_vec3(record, 44);

    if (version >= 5)
    {
        prop.forced_fade_scale = source_bsp::read_f32(record, 56);
    }

    if (version == 6 || version == 7)
    {
        prop.min_dx_level = source_bsp::read_u16(record, 60);
        prop.max_dx_level = source_bsp::read_u16(record, 62);
    }

    if (version >= 8)
    {
        prop.min_cpu_level = record[60];
        prop.max_cpu_level = record[61];
        prop.min_gpu_level = record[62];
        prop.max_gpu_level = record[63];
    }

    if (version >= 7)
    {
        prop.color[0] = static_cast<float>(record[64]) / 255.0F;
        prop.color[1] = static_cast<float>(record[65]) / 255.0F;
        prop.color[2] = static_cast<float>(record[66]) / 255.0F;
        prop.color[3] = static_cast<float>(record[67]) / 255.0F;
    }

    if (version >= 9)
    {
        prop.disable_x360 = record[68] != 0;
    }

    if (version >= 10)
    {
        prop.flags_ex = source_bsp::read_u32(record, 72);
    }

    return prop;
}
}

std::vector<WorldProp> read_source_static_props(std::span<const unsigned char> bsp_bytes)
{
    const std::optional<GameLumpData> lump = find_static_prop_game_lump(bsp_bytes);
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
            const std::int32_t count = source_bsp::read_s32(lump->bytes, cursor);
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
            dictionary.push_back(read_fixed_string(lump->bytes, cursor, kStaticPropNameLength));
            cursor += kStaticPropNameLength;
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
}
