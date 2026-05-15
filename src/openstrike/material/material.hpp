#pragma once

#include "openstrike/source/source_texture.hpp"

#include <cstdint>
#include <string>

namespace openstrike
{
namespace MaterialFlags
{
inline constexpr std::uint32_t None = 0;
inline constexpr std::uint32_t Translucent = 1U << 0U;
inline constexpr std::uint32_t AlphaTest = 1U << 1U;
inline constexpr std::uint32_t Unlit = 1U << 2U;
inline constexpr std::uint32_t NoDraw = 1U << 3U;
inline constexpr std::uint32_t Tool = 1U << 4U;
inline constexpr std::uint32_t Sky = 1U << 5U;
inline constexpr std::uint32_t NormalMap = 1U << 6U;
inline constexpr std::uint32_t Missing = 1U << 7U;
inline constexpr std::uint32_t FallbackTexture = 1U << 8U;
}

struct MaterialGpuConstants
{
    float base_color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t flags = MaterialFlags::None;
    float alpha_cutoff = 0.5F;
    float padding[2] = {0.0F, 0.0F};
};

static_assert(sizeof(MaterialGpuConstants) == 32);

struct MaterialDefinition
{
    std::string name;
    std::string shader;
    std::string base_texture;
    std::string normal_texture;
    MaterialGpuConstants constants;
    bool found_source_material = false;
};

struct LoadedMaterial
{
    MaterialDefinition definition;
    SourceTexture base_texture;
    bool base_texture_loaded = false;
    bool using_fallback_texture = false;
};
}
