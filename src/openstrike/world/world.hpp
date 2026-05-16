#pragma once

#include "openstrike/core/math.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openstrike
{
class ContentFileSystem;

enum class WorldAssetKind
{
    SourceBsp,
    OpenStrikeLevel
};

struct WorldSpawnPoint
{
    std::string class_name;
    Vec3 origin;
    Vec3 angles;
};

enum class WorldPropKind
{
    StaticProp,
    EntityProp
};

struct WorldProp
{
    WorldPropKind kind = WorldPropKind::StaticProp;
    std::string class_name;
    std::string model_path;
    Vec3 origin;
    Vec3 angles;
    Vec3 bounds_min{-16.0F, -16.0F, 0.0F};
    Vec3 bounds_max{16.0F, 16.0F, 32.0F};
    Vec3 lighting_origin;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t flags = 0;
    std::uint32_t flags_ex = 0;
    std::int32_t skin = 0;
    std::uint8_t solid = 0;
    std::string material_name;
    bool model_bounds_loaded = false;
    bool model_material_loaded = false;
    bool model_mesh_loaded = false;
};

enum class WorldLightKind
{
    Point
};

struct WorldLight
{
    WorldLightKind kind = WorldLightKind::Point;
    Vec3 position;
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    float intensity = 1.0F;
    float radius = 384.0F;
};

struct WorldMeshVertex
{
    Vec3 position;
    Vec3 normal;
    Vec2 texcoord;
};

struct WorldTriangle
{
    Vec3 points[3];
    Vec3 normal;
    std::uint32_t surface_flags = 0;
};

struct WorldMaterial
{
    std::string name;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
};

struct WorldMeshBatch
{
    std::uint32_t material_index = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct WorldMesh
{
    std::vector<WorldMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<WorldMaterial> materials;
    std::vector<WorldMeshBatch> batches;
    std::vector<WorldTriangle> collision_triangles;
    Vec3 bounds_min;
    Vec3 bounds_max;
    bool has_bounds = false;
    bool has_sky_surfaces = false;
};

struct LoadedWorld
{
    std::string name;
    std::filesystem::path relative_path;
    std::filesystem::path resolved_path;
    WorldAssetKind kind = WorldAssetKind::SourceBsp;
    std::uint32_t asset_version = 0;
    std::uint32_t map_revision = 0;
    std::uintmax_t byte_size = 0;
    std::uint32_t checksum = 0;
    std::size_t entity_count = 0;
    std::unordered_map<std::string, std::string> worldspawn;
    std::unordered_map<std::string, std::vector<unsigned char>> embedded_assets;
    std::vector<WorldSpawnPoint> spawn_points;
    std::vector<WorldProp> props;
    std::vector<WorldLight> lights;
    WorldMesh mesh;
};

class WorldManager
{
public:
    bool load_map(std::string_view map_name, ContentFileSystem& filesystem);
    void unload();

    [[nodiscard]] const LoadedWorld* current_world() const;
    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] std::vector<std::string> list_maps(const ContentFileSystem& filesystem, std::string_view filter = {}) const;

private:
    std::optional<LoadedWorld> current_world_;
    std::uint64_t generation_ = 0;
};

[[nodiscard]] std::optional<float> find_floor_z(const LoadedWorld& world, Vec3 origin, float max_drop = 128.0F);
[[nodiscard]] std::string_view to_string(WorldAssetKind kind);
}
