#pragma once

#include "openstrike/core/math.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openstrike
{
class SourceAssetStore;

struct SourceModelBounds
{
    Vec3 mins;
    Vec3 maxs;
};

struct SourceModelVertex
{
    Vec3 position;
    Vec3 normal;
    Vec2 texcoord;
};

struct SourceModelMesh
{
    std::int32_t material = 0;
    std::vector<SourceModelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct SourceModelInfo
{
    SourceModelBounds bounds;
    std::vector<std::string> materials;
    std::vector<std::int16_t> skin_families;
    std::int32_t num_skin_refs = 0;
    std::int32_t num_skin_families = 0;
    std::vector<SourceModelMesh> meshes;
};

[[nodiscard]] std::optional<SourceModelInfo> load_source_model_info(
    const SourceAssetStore& assets,
    const std::filesystem::path& model_path);

[[nodiscard]] std::optional<SourceModelBounds> load_source_model_bounds(
    const SourceAssetStore& assets,
    const std::filesystem::path& model_path);

[[nodiscard]] std::string source_model_material_for_skin(const SourceModelInfo& model, std::int32_t skin, std::int32_t material_index);
}
