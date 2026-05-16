#include "openstrike/material/material_system.hpp"

#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_keyvalues.hpp"
#include "openstrike/source/source_paths.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace openstrike
{
namespace
{
std::uint32_t hash_string(std::string_view text)
{
    std::uint32_t hash = 2166136261U;
    for (const char ch : text)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619U;
    }
    return hash;
}

bool parse_bool_token(std::string_view token)
{
    const std::string value = source_lower_copy(source_trim_copy(token));
    return !(value.empty() || value == "0" || value == "false" || value == "no");
}

std::optional<float> parse_float_token(std::string_view token)
{
    const std::string value = source_trim_copy(token);
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    if (end == value.c_str())
    {
        return std::nullopt;
    }
    return parsed;
}

bool parse_color(std::string value, float (&output)[4])
{
    for (char& ch : value)
    {
        if (ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == ',')
        {
            ch = ' ';
        }
    }

    std::istringstream stream(value);
    std::array<float, 4> parsed{output[0], output[1], output[2], output[3]};
    int count = 0;
    while (count < 4 && stream >> parsed[static_cast<std::size_t>(count)])
    {
        ++count;
    }

    if (count < 3)
    {
        return false;
    }

    const bool color255 = parsed[0] > 1.0F || parsed[1] > 1.0F || parsed[2] > 1.0F;
    const float scale = color255 ? (1.0F / 255.0F) : 1.0F;
    output[0] = std::clamp(parsed[0] * scale, 0.0F, 1.0F);
    output[1] = std::clamp(parsed[1] * scale, 0.0F, 1.0F);
    output[2] = std::clamp(parsed[2] * scale, 0.0F, 1.0F);
    if (count >= 4)
    {
        output[3] = std::clamp(parsed[3], 0.0F, 1.0F);
    }
    return true;
}

void mark_name_flags(MaterialDefinition& definition)
{
    const std::string material_name = source_lower_copy(definition.name);
    const std::string shader = source_lower_copy(definition.shader);
    if (material_name.rfind("tools/", 0) == 0)
    {
        definition.constants.flags |= MaterialFlags::Tool;
    }
    if (material_name.rfind("tools/toolsnodraw", 0) == 0 || material_name.rfind("tools/toolsskip", 0) == 0 ||
        material_name.rfind("tools/toolstrigger", 0) == 0 || material_name.rfind("tools/toolsclip", 0) == 0 ||
        material_name.rfind("tools/toolsplayerclip", 0) == 0)
    {
        definition.constants.flags |= MaterialFlags::NoDraw;
    }
    if (material_name.rfind("skybox/", 0) == 0 || material_name.rfind("tools/toolsskybox", 0) == 0)
    {
        definition.constants.flags |= MaterialFlags::Sky | MaterialFlags::Unlit;
    }
    if (shader == "unlitgeneric" || shader == "sprite" || shader == "vertexlitgeneric_dx6")
    {
        definition.constants.flags |= MaterialFlags::Unlit;
    }
}

void apply_vmt_property(MaterialDefinition& definition, std::string_view property_key, std::string_view property_value)
{
    const std::string key = source_lower_copy(property_key);
    if (key == "$basetexture" || key == "%tooltexture")
    {
        definition.base_texture = normalize_source_texture_name(property_value);
    }
    else if (key == "$bumpmap" || key == "$normalmap")
    {
        definition.normal_texture = normalize_source_texture_name(property_value);
        if (!definition.normal_texture.empty())
        {
            definition.constants.flags |= MaterialFlags::NormalMap;
        }
    }
    else if (key == "$translucent" || key == "$vertexalpha")
    {
        if (parse_bool_token(property_value))
        {
            definition.constants.flags |= MaterialFlags::Translucent;
        }
    }
    else if (key == "$alphatest")
    {
        if (parse_bool_token(property_value))
        {
            definition.constants.flags |= MaterialFlags::AlphaTest;
        }
    }
    else if (key == "$selfillum")
    {
        if (parse_bool_token(property_value))
        {
            definition.constants.flags |= MaterialFlags::Unlit;
        }
    }
    else if (key == "$nocull")
    {
        if (parse_bool_token(property_value))
        {
            definition.constants.flags |= MaterialFlags::Translucent;
        }
    }
    else if (key == "$alpha")
    {
        if (const std::optional<float> alpha = parse_float_token(property_value))
        {
            definition.constants.base_color[3] = std::clamp(*alpha, 0.0F, 1.0F);
            if (definition.constants.base_color[3] < 1.0F)
            {
                definition.constants.flags |= MaterialFlags::Translucent;
            }
        }
    }
    else if (key == "$alphatestreference")
    {
        if (const std::optional<float> cutoff = parse_float_token(property_value))
        {
            definition.constants.alpha_cutoff = std::clamp(*cutoff, 0.0F, 1.0F);
        }
    }
    else if (key == "$color" || key == "$color2")
    {
        parse_color(std::string(property_value), definition.constants.base_color);
    }
}

void apply_vmt_node(MaterialDefinition& definition, const SourceKeyValueNode& node)
{
    for (const auto& child : node.children)
    {
        if (child->is_block())
        {
            apply_vmt_node(definition, *child);
        }
        else
        {
            apply_vmt_property(definition, child->key, child->value);
        }
    }
}

std::optional<std::string> node_value(const SourceKeyValueNode& node, std::string_view wanted_token)
{
    if (const std::optional<std::string_view> value = source_kv_find_value_ci(node, wanted_token))
    {
        return std::string(*value);
    }
    return std::nullopt;
}

std::optional<MaterialDefinition> load_source_material_recursive(const SourceAssetStore& assets, std::string material_name, int depth)
{
    if (depth > 8)
    {
        return std::nullopt;
    }

    const std::string normalized_name = normalize_source_material_name(material_name);
    const std::optional<std::string> vmt = assets.read_text(normalize_source_material_asset_path(normalized_name));
    if (!vmt)
    {
        return std::nullopt;
    }

    SourceKeyValueParseResult parsed = parse_source_keyvalues(*vmt);
    if (!parsed.ok || parsed.roots.empty())
    {
        return std::nullopt;
    }

    const SourceKeyValueNode& root = *parsed.roots.front();
    const std::string shader = source_lower_copy(root.key);
    MaterialDefinition definition;
    if (shader == "patch")
    {
        if (const std::optional<std::string> include = node_value(root, "include"))
        {
            if (std::optional<MaterialDefinition> included = load_source_material_recursive(assets, *include, depth + 1))
            {
                definition = std::move(*included);
            }
        }
    }

    definition.name = normalized_name;
    if (definition.shader.empty())
    {
        definition.shader = shader;
    }
    definition.found_source_material = true;
    apply_vmt_node(definition, root);
    mark_name_flags(definition);
    return definition;
}
}

MaterialSystem::MaterialSystem(const SourceAssetStore& assets)
    : assets_(assets)
{
}

std::optional<MaterialDefinition> MaterialSystem::resolve_source_material(std::string_view material_name) const
{
    return load_source_material_recursive(assets_, std::string(material_name), 0);
}

SourceTexture MaterialSystem::fallback_texture(std::string_view material_name) const
{
    constexpr std::uint32_t size = 8;
    const std::uint32_t hash = hash_string(material_name);
    const std::array<unsigned char, 3> base{
        static_cast<unsigned char>(72 + (hash & 0x7FU)),
        static_cast<unsigned char>(72 + ((hash >> 8U) & 0x7FU)),
        static_cast<unsigned char>(72 + ((hash >> 16U) & 0x7FU)),
    };

    SourceTexture texture;
    texture.width = size;
    texture.height = size;
    texture.format = SourceTextureFormat::Rgba8;
    SourceTextureMip mip;
    mip.width = size;
    mip.height = size;
    mip.bytes.resize(size * size * 4);
    for (std::uint32_t y = 0; y < size; ++y)
    {
        for (std::uint32_t x = 0; x < size; ++x)
        {
            const bool checker = ((x / 2U) + (y / 2U)) % 2U == 0U;
            const float scale = checker ? 1.0F : 0.55F;
            const std::size_t offset = ((y * size) + x) * 4U;
            mip.bytes[offset + 0] = static_cast<unsigned char>(std::clamp(base[0] * scale, 0.0F, 255.0F));
            mip.bytes[offset + 1] = static_cast<unsigned char>(std::clamp(base[1] * scale, 0.0F, 255.0F));
            mip.bytes[offset + 2] = static_cast<unsigned char>(std::clamp(base[2] * scale, 0.0F, 255.0F));
            mip.bytes[offset + 3] = 255;
        }
    }
    texture.mips.push_back(std::move(mip));
    return texture;
}

LoadedMaterial MaterialSystem::load_world_material(std::string_view material_name) const
{
    LoadedMaterial loaded;
    const std::string normalized_name = normalize_source_material_name(material_name);
    if (std::optional<MaterialDefinition> resolved = resolve_source_material(normalized_name))
    {
        loaded.definition = std::move(*resolved);
    }
    else
    {
        loaded.definition.name = normalized_name.empty() ? "__missing" : normalized_name;
        loaded.definition.base_texture = normalized_name;
        loaded.definition.shader = "missing";
        loaded.definition.constants.flags |= MaterialFlags::Missing;
        mark_name_flags(loaded.definition);
    }

    std::string texture_name = loaded.definition.base_texture.empty() ? loaded.definition.name : loaded.definition.base_texture;
    texture_name = normalize_source_texture_name(texture_name);
    if (!texture_name.empty() && (loaded.definition.constants.flags & MaterialFlags::NoDraw) == 0)
    {
        if (const std::optional<std::vector<unsigned char>> vtf = assets_.read_binary("materials/" + texture_name + ".vtf"))
        {
            if (std::optional<SourceTexture> texture = load_vtf_texture(*vtf))
            {
                loaded.base_texture = std::move(*texture);
                loaded.base_texture_loaded = true;
                return loaded;
            }
        }
    }

    loaded.base_texture = fallback_texture(loaded.definition.name);
    loaded.using_fallback_texture = true;
    loaded.definition.constants.flags |= MaterialFlags::FallbackTexture;
    return loaded;
}
}
