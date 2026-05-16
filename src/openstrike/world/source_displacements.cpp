#include "openstrike/world/source_displacements.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/world/source_bsp_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace openstrike
{
namespace
{
constexpr std::size_t kDispInfoLump = 26;
constexpr std::size_t kDispVertsLump = 33;
constexpr std::size_t kDispTrisLump = 48;
constexpr std::size_t kDispInfoStride = 176;
constexpr std::size_t kDispVertStride = 20;
constexpr std::size_t kDispTriStride = 2;
constexpr std::size_t kAllowedVertsOffset = 136;
constexpr std::int32_t kMinMapDispPower = 2;
constexpr std::int32_t kMaxMapDispPower = 4;

struct SourceDispVert
{
    Vec3 vector;
    float distance = 0.0F;
    float alpha = 0.0F;
};

float dot3(Vec3 lhs, Vec3 rhs)
{
    return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

Vec3 cross3(Vec3 lhs, Vec3 rhs)
{
    return {
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

float length3(Vec3 value)
{
    return std::sqrt(dot3(value, value));
}

Vec3 normalize3(Vec3 value)
{
    const float value_length = length3(value);
    if (value_length <= 0.00001F)
    {
        return {0.0F, 0.0F, 1.0F};
    }

    return value * (1.0F / value_length);
}

std::int32_t displacement_grid_size(std::int32_t power)
{
    return (1 << power) + 1;
}

std::int32_t displacement_vertex_count(std::int32_t power)
{
    const std::int32_t grid_size = displacement_grid_size(power);
    return grid_size * grid_size;
}

std::int32_t displacement_triangle_count(std::int32_t power)
{
    const std::int32_t cells = 1 << power;
    return cells * cells * 2;
}

std::vector<SourceDispVert> read_disp_verts(std::span<const unsigned char> bsp_bytes)
{
    return source_bsp::read_lump_array<SourceDispVert>(bsp_bytes, kDispVertsLump, kDispVertStride, "disp verts", [](std::span<const unsigned char> data, std::size_t offset) {
        return SourceDispVert{
            .vector = source_bsp::read_vec3(data, offset),
            .distance = source_bsp::read_f32(data, offset + 12),
            .alpha = source_bsp::read_f32(data, offset + 16),
        };
    });
}

std::vector<std::uint16_t> read_disp_tris(std::span<const unsigned char> bsp_bytes)
{
    return source_bsp::read_lump_array<std::uint16_t>(bsp_bytes, kDispTrisLump, kDispTriStride, "disp tris", [](std::span<const unsigned char> data, std::size_t offset) {
        return source_bsp::read_u16(data, offset);
    });
}

std::array<Vec3, 4> order_displacement_corners(const SourceBspDisplacement& displacement, std::span<const Vec3> face_polygon)
{
    std::size_t start_corner = 0;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < 4; ++index)
    {
        const Vec3 delta = face_polygon[index] - displacement.start_position;
        const float distance = dot3(delta, delta);
        if (distance < best_distance)
        {
            best_distance = distance;
            start_corner = index;
        }
    }

    std::array<Vec3, 4> corners{};
    for (std::size_t index = 0; index < corners.size(); ++index)
    {
        corners[index] = face_polygon[(start_corner + index) % 4U];
    }
    return corners;
}

Vec3 bilerp(Vec3 c0, Vec3 c1, Vec3 c2, Vec3 c3, float row, float column)
{
    return (c0 * (1.0F - row) * (1.0F - column)) + (c1 * row * (1.0F - column)) + (c2 * row * column) +
           (c3 * (1.0F - row) * column);
}

void append_source_cell_indices(std::vector<std::uint32_t>& indices, std::uint32_t ndx, std::uint32_t grid_size)
{
    if ((ndx % 2U) == 1U)
    {
        indices.push_back(ndx);
        indices.push_back(ndx + grid_size);
        indices.push_back(ndx + 1U);
        indices.push_back(ndx + 1U);
        indices.push_back(ndx + grid_size);
        indices.push_back(ndx + grid_size + 1U);
        return;
    }

    indices.push_back(ndx);
    indices.push_back(ndx + grid_size);
    indices.push_back(ndx + grid_size + 1U);
    indices.push_back(ndx);
    indices.push_back(ndx + grid_size + 1U);
    indices.push_back(ndx + 1U);
}
}

std::vector<SourceBspDisplacement> read_source_bsp_displacements(std::span<const unsigned char> bsp_bytes)
{
    const source_bsp::Lump disp_info_lump = source_bsp::read_lump(bsp_bytes, kDispInfoLump);
    if (disp_info_lump.length == 0)
    {
        return {};
    }

    try
    {
        const std::span<const unsigned char> disp_infos = source_bsp::lump_bytes(bsp_bytes, disp_info_lump, "dispinfo");
        if ((disp_infos.size() % kDispInfoStride) != 0)
        {
            throw std::runtime_error("BSP dispinfo lump has an invalid size");
        }

        const std::vector<SourceDispVert> disp_verts = read_disp_verts(bsp_bytes);
        const std::vector<std::uint16_t> disp_tris = read_disp_tris(bsp_bytes);

        std::vector<SourceBspDisplacement> displacements;
        displacements.reserve(disp_infos.size() / kDispInfoStride);
        for (std::size_t offset = 0; offset < disp_infos.size(); offset += kDispInfoStride)
        {
            SourceBspDisplacement displacement;
            displacement.start_position = source_bsp::read_vec3(disp_infos, offset);
            const std::int32_t vert_start = source_bsp::read_s32(disp_infos, offset + 12);
            const std::int32_t tri_start = source_bsp::read_s32(disp_infos, offset + 16);
            displacement.power = source_bsp::read_s32(disp_infos, offset + 20);
            displacement.min_tess = source_bsp::read_s32(disp_infos, offset + 24);
            displacement.smoothing_angle = source_bsp::read_f32(disp_infos, offset + 28);
            displacement.contents = source_bsp::read_u32(disp_infos, offset + 32);
            displacement.map_face = source_bsp::read_u16(disp_infos, offset + 36);
            for (std::size_t index = 0; index < std::size(displacement.allowed_verts); ++index)
            {
                displacement.allowed_verts[index] = source_bsp::read_u32(disp_infos, offset + kAllowedVertsOffset + (index * 4U));
            }

            if (displacement.power < kMinMapDispPower || displacement.power > kMaxMapDispPower)
            {
                log_warning("skipped BSP displacement with invalid power {}", displacement.power);
                displacements.push_back(std::move(displacement));
                continue;
            }

            const std::int32_t vertex_count = displacement_vertex_count(displacement.power);
            const std::int32_t triangle_count = displacement_triangle_count(displacement.power);
            if (vert_start < 0 || tri_start < 0 || static_cast<std::uint64_t>(vert_start) + static_cast<std::uint64_t>(vertex_count) > disp_verts.size() ||
                static_cast<std::uint64_t>(tri_start) + static_cast<std::uint64_t>(triangle_count) > disp_tris.size())
            {
                log_warning("skipped malformed BSP displacement vertex/triangle ranges");
                displacements.push_back(std::move(displacement));
                continue;
            }

            displacement.vectors.reserve(static_cast<std::size_t>(vertex_count));
            displacement.distances.reserve(static_cast<std::size_t>(vertex_count));
            displacement.alphas.reserve(static_cast<std::size_t>(vertex_count));
            for (std::int32_t index = 0; index < vertex_count; ++index)
            {
                const SourceDispVert& vertex = disp_verts[static_cast<std::size_t>(vert_start + index)];
                displacement.vectors.push_back(vertex.vector);
                displacement.distances.push_back(vertex.distance);
                displacement.alphas.push_back(vertex.alpha);
            }

            displacement.triangle_tags.reserve(static_cast<std::size_t>(triangle_count));
            for (std::int32_t index = 0; index < triangle_count; ++index)
            {
                displacement.triangle_tags.push_back(disp_tris[static_cast<std::size_t>(tri_start + index)]);
            }
            displacements.push_back(std::move(displacement));
        }
        return displacements;
    }
    catch (const std::exception& error)
    {
        log_warning("failed to parse BSP displacements: {}", error.what());
        return {};
    }
}

const SourceBspDisplacement* source_displacement_by_index(
    const std::vector<SourceBspDisplacement>& displacements,
    std::int16_t index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= displacements.size())
    {
        return nullptr;
    }

    const SourceBspDisplacement& displacement = displacements[static_cast<std::size_t>(index)];
    if (displacement.power < kMinMapDispPower || displacement.power > kMaxMapDispPower || displacement.vectors.empty())
    {
        return nullptr;
    }

    return &displacement;
}

std::optional<SourceDisplacementSurface> build_source_displacement_surface(
    const SourceBspDisplacement& displacement,
    std::span<const Vec3> face_polygon)
{
    if (face_polygon.size() != 4 || displacement.power < kMinMapDispPower || displacement.power > kMaxMapDispPower)
    {
        return std::nullopt;
    }

    const std::int32_t grid_size_s32 = displacement_grid_size(displacement.power);
    const std::int32_t vertex_count = displacement_vertex_count(displacement.power);
    if (static_cast<std::int32_t>(displacement.vectors.size()) < vertex_count ||
        static_cast<std::int32_t>(displacement.distances.size()) < vertex_count)
    {
        return std::nullopt;
    }

    const std::uint32_t grid_size = static_cast<std::uint32_t>(grid_size_s32);
    const std::uint32_t cells = grid_size - 1U;
    const float inv_size = 1.0F / static_cast<float>(cells);
    const std::array<Vec3, 4> corners = order_displacement_corners(displacement, face_polygon);

    SourceDisplacementSurface surface;
    surface.corners = corners;
    surface.vertices.resize(static_cast<std::size_t>(vertex_count));
    surface.contents = displacement.contents;
    for (std::uint32_t row = 0; row < grid_size; ++row)
    {
        const float row_fraction = static_cast<float>(row) * inv_size;
        for (std::uint32_t column = 0; column < grid_size; ++column)
        {
            const float column_fraction = static_cast<float>(column) * inv_size;
            const std::uint32_t index = (row * grid_size) + column;
            const Vec3 flat_position = bilerp(corners[0], corners[1], corners[2], corners[3], row_fraction, column_fraction);
            surface.vertices[index] = SourceDisplacementVertex{
                .position = flat_position + (displacement.vectors[index] * displacement.distances[index]),
                .normal = {0.0F, 0.0F, 0.0F},
                .row_fraction = row_fraction,
                .column_fraction = column_fraction,
                .alpha = index < displacement.alphas.size() ? displacement.alphas[index] : 0.0F,
            };
        }
    }

    surface.indices.reserve(static_cast<std::size_t>(cells) * cells * 6U);
    for (std::uint32_t row = 0; row < cells; ++row)
    {
        for (std::uint32_t column = 0; column < cells; ++column)
        {
            append_source_cell_indices(surface.indices, (row * grid_size) + column, grid_size);
        }
    }

    for (std::size_t index = 0; index + 2 < surface.indices.size(); index += 3)
    {
        const std::uint32_t ia = surface.indices[index + 0];
        const std::uint32_t ib = surface.indices[index + 1];
        const std::uint32_t ic = surface.indices[index + 2];
        Vec3 normal = normalize3(cross3(
            surface.vertices[ib].position - surface.vertices[ia].position,
            surface.vertices[ic].position - surface.vertices[ia].position));
        surface.vertices[ia].normal += normal;
        surface.vertices[ib].normal += normal;
        surface.vertices[ic].normal += normal;
    }

    for (SourceDisplacementVertex& vertex : surface.vertices)
    {
        vertex.normal = normalize3(vertex.normal);
    }

    surface.triangle_tags = displacement.triangle_tags;
    return surface;
}
}
