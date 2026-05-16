#pragma once

#include "openstrike/core/math.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openstrike
{
struct SourceBspDisplacement
{
    Vec3 start_position;
    std::int32_t power = 0;
    std::int32_t min_tess = 0;
    float smoothing_angle = 0.0F;
    std::uint32_t contents = 0;
    std::uint16_t map_face = 0;
    std::vector<Vec3> vectors;
    std::vector<float> distances;
    std::vector<float> alphas;
    std::vector<std::uint16_t> triangle_tags;
    std::uint32_t allowed_verts[10]{};
};

struct SourceDisplacementVertex
{
    Vec3 position;
    Vec3 normal;
    float row_fraction = 0.0F;
    float column_fraction = 0.0F;
    float alpha = 0.0F;
};

struct SourceDisplacementSurface
{
    std::array<Vec3, 4> corners{};
    std::vector<SourceDisplacementVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint16_t> triangle_tags;
    std::uint32_t contents = 0;
};

[[nodiscard]] std::vector<SourceBspDisplacement> read_source_bsp_displacements(std::span<const unsigned char> bsp_bytes);
[[nodiscard]] const SourceBspDisplacement* source_displacement_by_index(
    const std::vector<SourceBspDisplacement>& displacements,
    std::int16_t index);
[[nodiscard]] std::optional<SourceDisplacementSurface> build_source_displacement_surface(
    const SourceBspDisplacement& displacement,
    std::span<const Vec3> face_polygon);
}
