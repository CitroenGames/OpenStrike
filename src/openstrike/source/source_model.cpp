#include "openstrike/source/source_model.hpp"

#include "openstrike/source/source_asset_store.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kStudioIdent = 0x54534449U; // "IDST"
constexpr std::uint32_t kStudioVertexIdent = 0x56534449U; // "IDSV"
constexpr std::int32_t kStudioVertexVersion = 4;
constexpr std::int32_t kOptimizedModelVersion = 7;
constexpr std::size_t kStudioLengthOffset = 76;
constexpr std::size_t kStudioHullMinOffset = 104;
constexpr std::size_t kStudioHullMaxOffset = 116;
constexpr std::size_t kStudioViewMinOffset = 128;
constexpr std::size_t kStudioViewMaxOffset = 140;
constexpr std::size_t kStudioChecksumOffset = 8;
constexpr std::size_t kStudioNumTexturesOffset = 204;
constexpr std::size_t kStudioTextureIndexOffset = 208;
constexpr std::size_t kStudioNumCdTexturesOffset = 212;
constexpr std::size_t kStudioCdTextureIndexOffset = 216;
constexpr std::size_t kStudioNumSkinRefOffset = 220;
constexpr std::size_t kStudioNumSkinFamiliesOffset = 224;
constexpr std::size_t kStudioSkinIndexOffset = 228;
constexpr std::size_t kStudioNumBodyPartsOffset = 232;
constexpr std::size_t kStudioBodyPartIndexOffset = 236;
constexpr std::size_t kStudioTextureSize = 64;
constexpr std::size_t kStudioBodyPartSize = 16;
constexpr std::size_t kStudioModelSize = 148;
constexpr std::size_t kStudioMeshSize = 116;
constexpr std::size_t kStudioVertexFileHeaderSize = 64;
constexpr std::size_t kStudioVertexSize = 48;
constexpr std::size_t kStudioVertexPositionOffset = 16;
constexpr std::size_t kStudioVertexNormalOffset = 28;
constexpr std::size_t kStudioVertexTexcoordOffset = 40;
constexpr std::size_t kVtxFileHeaderSize = 36;
constexpr std::size_t kVtxBodyPartSize = 8;
constexpr std::size_t kVtxModelSize = 8;
constexpr std::size_t kVtxLodSize = 12;
constexpr std::size_t kVtxMeshSize = 9;
constexpr std::size_t kVtxStripGroupSize = 33;
constexpr std::size_t kVtxStripSize = 35;
constexpr std::size_t kVtxVertexSize = 9;
constexpr std::uint8_t kVtxStripIsTriList = 0x01U;
constexpr std::int32_t kMaxStudioCount = 4096;

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
    {
        return 0;
    }

    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::int32_t read_i32_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(read_u32_le(bytes, offset));
}

std::uint16_t read_u16_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size())
    {
        return 0;
    }

    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::int16_t read_i16_le(const std::vector<unsigned char>& bytes, std::size_t offset)
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

Vec3 read_vec3(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return {
        read_f32_le(bytes, offset),
        read_f32_le(bytes, offset + 4),
        read_f32_le(bytes, offset + 8),
    };
}

Vec2 read_vec2(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return {
        read_f32_le(bytes, offset),
        read_f32_le(bytes, offset + 4),
    };
}

bool valid_bounds(Vec3 mins, Vec3 maxs)
{
    return std::isfinite(mins.x) && std::isfinite(mins.y) && std::isfinite(mins.z) && std::isfinite(maxs.x) &&
           std::isfinite(maxs.y) && std::isfinite(maxs.z) && maxs.x > mins.x + 0.001F && maxs.y > mins.y + 0.001F &&
           maxs.z > mins.z + 0.001F;
}

std::filesystem::path normalize_model_path(std::filesystem::path path)
{
    std::string normalized = path.generic_string();
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.size() < 4 || normalized.substr(normalized.size() - 4) != ".mdl")
    {
        normalized += ".mdl";
    }
    return normalized;
}

std::filesystem::path companion_path(const std::filesystem::path& model_path, std::string_view extension)
{
    std::filesystem::path path = normalize_model_path(model_path);
    path.replace_extension(std::string(extension));
    return path;
}

bool valid_range(const std::vector<unsigned char>& bytes, std::size_t offset, std::size_t size)
{
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

bool valid_count(std::int32_t count)
{
    return count >= 0 && count <= kMaxStudioCount;
}

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

std::string normalize_material_fragment(std::string text)
{
    text = lower_copy(trim_copy(text));
    std::replace(text.begin(), text.end(), '\\', '/');
    while (!text.empty() && text.front() == '/')
    {
        text.erase(text.begin());
    }
    if (text.rfind("materials/", 0) == 0)
    {
        text.erase(0, 10);
    }
    if (text.size() >= 4 && (text.ends_with(".vmt") || text.ends_with(".vtf")))
    {
        text.resize(text.size() - 4);
    }
    return text;
}

std::string read_zero_string_at(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset >= bytes.size())
    {
        return {};
    }

    const std::size_t begin = offset;
    while (offset < bytes.size() && bytes[offset] != 0)
    {
        ++offset;
    }
    return std::string(reinterpret_cast<const char*>(bytes.data() + begin), offset - begin);
}

std::string join_material_path(std::string cd_path, std::string texture_name)
{
    cd_path = normalize_material_fragment(std::move(cd_path));
    texture_name = normalize_material_fragment(std::move(texture_name));
    if (texture_name.empty())
    {
        return {};
    }

    if (cd_path.empty() || texture_name.find('/') != std::string::npos)
    {
        return texture_name;
    }

    if (!cd_path.ends_with('/'))
    {
        cd_path.push_back('/');
    }
    return normalize_material_fragment(cd_path + texture_name);
}

bool material_asset_exists(const SourceAssetStore& assets, const std::string& material)
{
    if (material.empty())
    {
        return false;
    }

    return assets.read_binary("materials/" + material + ".vmt").has_value() ||
           assets.read_binary("materials/" + material + ".vtf").has_value();
}

std::string choose_source_material(
    const SourceAssetStore& assets,
    const std::vector<std::string>& cd_texture_paths,
    std::string texture_name)
{
    texture_name = normalize_material_fragment(std::move(texture_name));
    if (texture_name.empty())
    {
        return {};
    }

    std::vector<std::string> candidates;
    auto push_candidate = [&candidates](std::string candidate) {
        candidate = normalize_material_fragment(std::move(candidate));
        if (!candidate.empty() && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        {
            candidates.push_back(std::move(candidate));
        }
    };

    if (texture_name.find('/') != std::string::npos)
    {
        push_candidate(texture_name);
    }
    for (const std::string& cd_path : cd_texture_paths)
    {
        push_candidate(join_material_path(cd_path, texture_name));
    }
    push_candidate(texture_name);

    for (const std::string& candidate : candidates)
    {
        if (material_asset_exists(assets, candidate))
        {
            return candidate;
        }
    }

    return candidates.empty() ? std::string{} : candidates.front();
}

SourceModelBounds read_model_bounds(const std::vector<unsigned char>& bytes)
{
    SourceModelBounds bounds{
        .mins = read_vec3(bytes, kStudioViewMinOffset),
        .maxs = read_vec3(bytes, kStudioViewMaxOffset),
    };
    if (valid_bounds(bounds.mins, bounds.maxs))
    {
        return bounds;
    }

    bounds.mins = read_vec3(bytes, kStudioHullMinOffset);
    bounds.maxs = read_vec3(bytes, kStudioHullMaxOffset);
    return bounds;
}

void read_model_materials(const SourceAssetStore& assets, const std::vector<unsigned char>& bytes, SourceModelInfo& info)
{
    const std::int32_t texture_count = read_i32_le(bytes, kStudioNumTexturesOffset);
    const std::int32_t texture_index = read_i32_le(bytes, kStudioTextureIndexOffset);
    const std::int32_t cd_texture_count = read_i32_le(bytes, kStudioNumCdTexturesOffset);
    const std::int32_t cd_texture_index = read_i32_le(bytes, kStudioCdTextureIndexOffset);
    if (!valid_count(texture_count) || !valid_count(cd_texture_count) || texture_index < 0 || cd_texture_index < 0)
    {
        return;
    }

    std::vector<std::string> cd_texture_paths;
    if (valid_range(bytes, static_cast<std::size_t>(cd_texture_index), static_cast<std::size_t>(cd_texture_count) * 4U))
    {
        cd_texture_paths.reserve(static_cast<std::size_t>(cd_texture_count));
        for (std::int32_t index = 0; index < cd_texture_count; ++index)
        {
            const std::int32_t string_offset = read_i32_le(bytes, static_cast<std::size_t>(cd_texture_index) + (static_cast<std::size_t>(index) * 4U));
            if (string_offset >= 0)
            {
                cd_texture_paths.push_back(read_zero_string_at(bytes, static_cast<std::size_t>(string_offset)));
            }
        }
    }

    if (!valid_range(bytes, static_cast<std::size_t>(texture_index), static_cast<std::size_t>(texture_count) * kStudioTextureSize))
    {
        return;
    }

    info.materials.reserve(static_cast<std::size_t>(texture_count));
    for (std::int32_t index = 0; index < texture_count; ++index)
    {
        const std::size_t texture_offset = static_cast<std::size_t>(texture_index) + (static_cast<std::size_t>(index) * kStudioTextureSize);
        const std::int32_t name_index = read_i32_le(bytes, texture_offset);
        if (name_index < 0)
        {
            info.materials.emplace_back();
            continue;
        }

        const std::string texture_name = read_zero_string_at(bytes, texture_offset + static_cast<std::size_t>(name_index));
        info.materials.push_back(choose_source_material(assets, cd_texture_paths, texture_name));
    }
}

void read_model_skin_table(const std::vector<unsigned char>& bytes, SourceModelInfo& info)
{
    info.num_skin_refs = read_i32_le(bytes, kStudioNumSkinRefOffset);
    info.num_skin_families = read_i32_le(bytes, kStudioNumSkinFamiliesOffset);
    const std::int32_t skin_index = read_i32_le(bytes, kStudioSkinIndexOffset);
    if (!valid_count(info.num_skin_refs) || !valid_count(info.num_skin_families) || skin_index < 0 || info.num_skin_refs == 0 ||
        info.num_skin_families == 0)
    {
        info.num_skin_refs = 0;
        info.num_skin_families = 0;
        return;
    }

    const std::size_t skin_count = static_cast<std::size_t>(info.num_skin_refs) * static_cast<std::size_t>(info.num_skin_families);
    if (!valid_range(bytes, static_cast<std::size_t>(skin_index), skin_count * 2U))
    {
        info.num_skin_refs = 0;
        info.num_skin_families = 0;
        return;
    }

    info.skin_families.reserve(skin_count);
    for (std::size_t index = 0; index < skin_count; ++index)
    {
        info.skin_families.push_back(read_i16_le(bytes, static_cast<std::size_t>(skin_index) + (index * 2U)));
    }
}

std::optional<std::vector<SourceModelVertex>> load_vvd_vertices(
    const SourceAssetStore& assets,
    const std::filesystem::path& model_path,
    std::uint32_t checksum)
{
    const std::optional<std::vector<unsigned char>> bytes = assets.read_binary(companion_path(model_path, ".vvd"));
    if (!bytes || bytes->size() < kStudioVertexFileHeaderSize || read_u32_le(*bytes, 0) != kStudioVertexIdent ||
        read_i32_le(*bytes, 4) != kStudioVertexVersion)
    {
        return std::nullopt;
    }

    const std::uint32_t vertex_checksum = read_u32_le(*bytes, 8);
    if (checksum != 0 && vertex_checksum != 0 && vertex_checksum != checksum)
    {
        return std::nullopt;
    }

    const std::int32_t lod_vertex_count = read_i32_le(*bytes, 16);
    const std::int32_t fixup_count = read_i32_le(*bytes, 48);
    const std::int32_t fixup_table_start = read_i32_le(*bytes, 52);
    const std::int32_t vertex_data_start = read_i32_le(*bytes, 56);
    if (lod_vertex_count <= 0 || !valid_count(lod_vertex_count) || fixup_count < 0 || !valid_count(fixup_count) || vertex_data_start < 0)
    {
        return std::nullopt;
    }

    std::vector<SourceModelVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(lod_vertex_count));

    auto append_vertex = [&](std::size_t vertex_offset) {
        SourceModelVertex vertex;
        vertex.position = read_vec3(*bytes, vertex_offset + kStudioVertexPositionOffset);
        vertex.normal = read_vec3(*bytes, vertex_offset + kStudioVertexNormalOffset);
        vertex.texcoord = read_vec2(*bytes, vertex_offset + kStudioVertexTexcoordOffset);
        vertices.push_back(vertex);
    };

    if (fixup_count == 0)
    {
        const std::size_t vertex_offset = static_cast<std::size_t>(vertex_data_start);
        const std::size_t vertex_bytes = static_cast<std::size_t>(lod_vertex_count) * kStudioVertexSize;
        if (!valid_range(*bytes, vertex_offset, vertex_bytes))
        {
            return std::nullopt;
        }

        for (std::int32_t index = 0; index < lod_vertex_count; ++index)
        {
            append_vertex(vertex_offset + (static_cast<std::size_t>(index) * kStudioVertexSize));
        }
        return vertices;
    }

    if (fixup_table_start < 0 || !valid_range(*bytes, static_cast<std::size_t>(fixup_table_start), static_cast<std::size_t>(fixup_count) * 12U))
    {
        return std::nullopt;
    }

    for (std::int32_t fixup = 0; fixup < fixup_count; ++fixup)
    {
        const std::size_t fixup_offset = static_cast<std::size_t>(fixup_table_start) + (static_cast<std::size_t>(fixup) * 12U);
        const std::int32_t lod = read_i32_le(*bytes, fixup_offset);
        const std::int32_t source_vertex = read_i32_le(*bytes, fixup_offset + 4);
        const std::int32_t count = read_i32_le(*bytes, fixup_offset + 8);
        if (lod < 0 || source_vertex < 0 || count <= 0 || !valid_count(count))
        {
            continue;
        }

        const std::size_t vertex_offset = static_cast<std::size_t>(vertex_data_start) + (static_cast<std::size_t>(source_vertex) * kStudioVertexSize);
        if (!valid_range(*bytes, vertex_offset, static_cast<std::size_t>(count) * kStudioVertexSize))
        {
            return std::nullopt;
        }

        for (std::int32_t index = 0; index < count; ++index)
        {
            append_vertex(vertex_offset + (static_cast<std::size_t>(index) * kStudioVertexSize));
        }
        if (vertices.size() >= static_cast<std::size_t>(lod_vertex_count))
        {
            break;
        }
    }

    if (vertices.empty())
    {
        return std::nullopt;
    }
    return vertices;
}

std::optional<std::vector<unsigned char>> load_vtx_bytes(const SourceAssetStore& assets, const std::filesystem::path& model_path)
{
    for (std::string_view extension : {".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx"})
    {
        if (std::optional<std::vector<unsigned char>> bytes = assets.read_binary(companion_path(model_path, extension)))
        {
            return bytes;
        }
    }
    return std::nullopt;
}

bool append_vtx_vertex(
    SourceModelMesh& mesh,
    const std::vector<SourceModelVertex>& vertices,
    std::int32_t model_vertex_base,
    std::int32_t mesh_vertex_offset,
    const std::vector<unsigned char>& vtx_bytes,
    std::size_t strip_group_offset,
    std::size_t vtx_vertex_index)
{
    const std::size_t vtx_vertex_offset = strip_group_offset + vtx_vertex_index;
    if (!valid_range(vtx_bytes, vtx_vertex_offset, kVtxVertexSize))
    {
        return false;
    }

    const std::int32_t original_mesh_vertex = read_u16_le(vtx_bytes, vtx_vertex_offset + 4);
    const std::int32_t vertex_index = model_vertex_base + mesh_vertex_offset + original_mesh_vertex;
    if (vertex_index < 0 || static_cast<std::size_t>(vertex_index) >= vertices.size())
    {
        return false;
    }

    const std::uint32_t index = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(vertices[static_cast<std::size_t>(vertex_index)]);
    mesh.indices.push_back(index);
    return true;
}

void append_vtx_triangle(
    SourceModelMesh& mesh,
    const std::vector<SourceModelVertex>& vertices,
    std::int32_t model_vertex_base,
    std::int32_t mesh_vertex_offset,
    const std::vector<unsigned char>& vtx_bytes,
    std::size_t strip_group_offset,
    std::size_t vertex_table_offset,
    std::uint16_t a,
    std::uint16_t b,
    std::uint16_t c)
{
    const std::size_t first_index = mesh.indices.size();
    if (!append_vtx_vertex(mesh, vertices, model_vertex_base, mesh_vertex_offset, vtx_bytes, strip_group_offset, vertex_table_offset + (static_cast<std::size_t>(a) * kVtxVertexSize)) ||
        !append_vtx_vertex(mesh, vertices, model_vertex_base, mesh_vertex_offset, vtx_bytes, strip_group_offset, vertex_table_offset + (static_cast<std::size_t>(b) * kVtxVertexSize)) ||
        !append_vtx_vertex(mesh, vertices, model_vertex_base, mesh_vertex_offset, vtx_bytes, strip_group_offset, vertex_table_offset + (static_cast<std::size_t>(c) * kVtxVertexSize)))
    {
        mesh.indices.resize(first_index);
        mesh.vertices.resize(first_index);
    }
}

void append_strip_indices(
    SourceModelMesh& mesh,
    const std::vector<SourceModelVertex>& vertices,
    std::int32_t model_vertex_base,
    std::int32_t mesh_vertex_offset,
    const std::vector<unsigned char>& vtx_bytes,
    std::size_t strip_group_offset,
    std::size_t vertex_table_offset,
    std::size_t index_table_offset,
    std::size_t first_index,
    std::int32_t index_count)
{
    if (index_count <= 0 || (index_count % 3) != 0)
    {
        return;
    }

    for (std::int32_t offset = 0; offset + 2 < index_count; offset += 3)
    {
        const std::size_t index_offset = strip_group_offset + index_table_offset + ((first_index + static_cast<std::size_t>(offset)) * 2U);
        if (!valid_range(vtx_bytes, index_offset, 6))
        {
            return;
        }

        append_vtx_triangle(
            mesh,
            vertices,
            model_vertex_base,
            mesh_vertex_offset,
            vtx_bytes,
            strip_group_offset,
            vertex_table_offset,
            read_u16_le(vtx_bytes, index_offset),
            read_u16_le(vtx_bytes, index_offset + 2),
            read_u16_le(vtx_bytes, index_offset + 4));
    }
}

void read_model_meshes(
    const SourceAssetStore& assets,
    const std::filesystem::path& model_path,
    const std::vector<unsigned char>& mdl_bytes,
    SourceModelInfo& info)
{
    const std::uint32_t checksum = read_u32_le(mdl_bytes, kStudioChecksumOffset);
    std::optional<std::vector<SourceModelVertex>> vertices = load_vvd_vertices(assets, model_path, checksum);
    std::optional<std::vector<unsigned char>> vtx_bytes = load_vtx_bytes(assets, model_path);
    if (!vertices || !vtx_bytes || vtx_bytes->size() < kVtxFileHeaderSize || read_i32_le(*vtx_bytes, 0) != kOptimizedModelVersion)
    {
        return;
    }

    const std::uint32_t vtx_checksum = read_u32_le(*vtx_bytes, 16);
    if (checksum != 0 && vtx_checksum != 0 && vtx_checksum != checksum)
    {
        return;
    }

    const std::int32_t body_part_count = read_i32_le(mdl_bytes, kStudioNumBodyPartsOffset);
    const std::int32_t body_part_index = read_i32_le(mdl_bytes, kStudioBodyPartIndexOffset);
    const std::int32_t vtx_body_part_count = read_i32_le(*vtx_bytes, 28);
    const std::int32_t vtx_body_part_index = read_i32_le(*vtx_bytes, 32);
    if (!valid_count(body_part_count) || !valid_count(vtx_body_part_count) || body_part_index < 0 || vtx_body_part_index < 0)
    {
        return;
    }

    const std::int32_t iter_body_parts = std::min(body_part_count, vtx_body_part_count);
    for (std::int32_t body_part = 0; body_part < iter_body_parts; ++body_part)
    {
        const std::size_t mdl_body_part_offset =
            static_cast<std::size_t>(body_part_index) + (static_cast<std::size_t>(body_part) * kStudioBodyPartSize);
        const std::size_t vtx_body_part_offset =
            static_cast<std::size_t>(vtx_body_part_index) + (static_cast<std::size_t>(body_part) * kVtxBodyPartSize);
        if (!valid_range(mdl_bytes, mdl_body_part_offset, kStudioBodyPartSize) || !valid_range(*vtx_bytes, vtx_body_part_offset, kVtxBodyPartSize))
        {
            return;
        }

        const std::int32_t model_count = read_i32_le(mdl_bytes, mdl_body_part_offset + 4);
        const std::int32_t model_index = read_i32_le(mdl_bytes, mdl_body_part_offset + 12);
        const std::int32_t vtx_model_count = read_i32_le(*vtx_bytes, vtx_body_part_offset);
        const std::int32_t vtx_model_index = read_i32_le(*vtx_bytes, vtx_body_part_offset + 4);
        if (!valid_count(model_count) || !valid_count(vtx_model_count) || model_index < 0 || vtx_model_index < 0)
        {
            continue;
        }

        const std::int32_t iter_models = std::min(model_count, vtx_model_count);
        for (std::int32_t model = 0; model < iter_models; ++model)
        {
            const std::size_t mdl_model_offset = mdl_body_part_offset + static_cast<std::size_t>(model_index) + (static_cast<std::size_t>(model) * kStudioModelSize);
            const std::size_t vtx_model_offset = vtx_body_part_offset + static_cast<std::size_t>(vtx_model_index) + (static_cast<std::size_t>(model) * kVtxModelSize);
            if (!valid_range(mdl_bytes, mdl_model_offset, kStudioModelSize) || !valid_range(*vtx_bytes, vtx_model_offset, kVtxModelSize))
            {
                continue;
            }

            const std::int32_t mesh_count = read_i32_le(mdl_bytes, mdl_model_offset + 72);
            const std::int32_t mesh_index = read_i32_le(mdl_bytes, mdl_model_offset + 76);
            const std::int32_t model_vertex_index = read_i32_le(mdl_bytes, mdl_model_offset + 84);
            const std::int32_t vtx_lod_count = read_i32_le(*vtx_bytes, vtx_model_offset);
            const std::int32_t vtx_lod_index = read_i32_le(*vtx_bytes, vtx_model_offset + 4);
            if (!valid_count(mesh_count) || mesh_index < 0 || model_vertex_index < 0 || vtx_lod_count <= 0 || !valid_count(vtx_lod_count) ||
                vtx_lod_index < 0)
            {
                continue;
            }

            const std::size_t vtx_lod_offset = vtx_model_offset + static_cast<std::size_t>(vtx_lod_index);
            if (!valid_range(*vtx_bytes, vtx_lod_offset, kVtxLodSize))
            {
                continue;
            }

            const std::int32_t vtx_mesh_count = read_i32_le(*vtx_bytes, vtx_lod_offset);
            const std::int32_t vtx_mesh_index = read_i32_le(*vtx_bytes, vtx_lod_offset + 4);
            if (!valid_count(vtx_mesh_count) || vtx_mesh_index < 0)
            {
                continue;
            }

            const std::int32_t iter_meshes = std::min(mesh_count, vtx_mesh_count);
            for (std::int32_t mesh_index_in_model = 0; mesh_index_in_model < iter_meshes; ++mesh_index_in_model)
            {
                const std::size_t mdl_mesh_offset =
                    mdl_model_offset + static_cast<std::size_t>(mesh_index) + (static_cast<std::size_t>(mesh_index_in_model) * kStudioMeshSize);
                const std::size_t vtx_mesh_offset =
                    vtx_lod_offset + static_cast<std::size_t>(vtx_mesh_index) + (static_cast<std::size_t>(mesh_index_in_model) * kVtxMeshSize);
                if (!valid_range(mdl_bytes, mdl_mesh_offset, kStudioMeshSize) || !valid_range(*vtx_bytes, vtx_mesh_offset, kVtxMeshSize))
                {
                    continue;
                }

                SourceModelMesh mesh;
                mesh.material = read_i32_le(mdl_bytes, mdl_mesh_offset);
                const std::int32_t mesh_vertex_offset = read_i32_le(mdl_bytes, mdl_mesh_offset + 12);
                const std::int32_t strip_group_count = read_i32_le(*vtx_bytes, vtx_mesh_offset);
                const std::int32_t strip_group_index = read_i32_le(*vtx_bytes, vtx_mesh_offset + 4);
                if (!valid_count(strip_group_count) || strip_group_index < 0 || mesh_vertex_offset < 0)
                {
                    continue;
                }

                for (std::int32_t strip_group = 0; strip_group < strip_group_count; ++strip_group)
                {
                    const std::size_t strip_group_offset =
                        vtx_mesh_offset + static_cast<std::size_t>(strip_group_index) + (static_cast<std::size_t>(strip_group) * kVtxStripGroupSize);
                    if (!valid_range(*vtx_bytes, strip_group_offset, kVtxStripGroupSize))
                    {
                        continue;
                    }

                    const std::int32_t vertex_count = read_i32_le(*vtx_bytes, strip_group_offset);
                    const std::int32_t vertex_table_offset = read_i32_le(*vtx_bytes, strip_group_offset + 4);
                    const std::int32_t index_count = read_i32_le(*vtx_bytes, strip_group_offset + 8);
                    const std::int32_t index_table_offset = read_i32_le(*vtx_bytes, strip_group_offset + 12);
                    const std::int32_t strip_count = read_i32_le(*vtx_bytes, strip_group_offset + 16);
                    const std::int32_t strip_table_offset = read_i32_le(*vtx_bytes, strip_group_offset + 20);
                    if (!valid_count(vertex_count) || !valid_count(index_count) || !valid_count(strip_count) || vertex_table_offset < 0 ||
                        index_table_offset < 0 || strip_table_offset < 0)
                    {
                        continue;
                    }

                    if (strip_count == 0)
                    {
                        append_strip_indices(
                            mesh,
                            *vertices,
                            model_vertex_index / static_cast<std::int32_t>(kStudioVertexSize),
                            mesh_vertex_offset,
                            *vtx_bytes,
                            strip_group_offset,
                            static_cast<std::size_t>(vertex_table_offset),
                            static_cast<std::size_t>(index_table_offset),
                            0,
                            index_count);
                        continue;
                    }

                    for (std::int32_t strip = 0; strip < strip_count; ++strip)
                    {
                        const std::size_t strip_offset =
                            strip_group_offset + static_cast<std::size_t>(strip_table_offset) + (static_cast<std::size_t>(strip) * kVtxStripSize);
                        if (!valid_range(*vtx_bytes, strip_offset, kVtxStripSize))
                        {
                            continue;
                        }

                        const std::int32_t strip_index_count = read_i32_le(*vtx_bytes, strip_offset);
                        const std::int32_t strip_first_index = read_i32_le(*vtx_bytes, strip_offset + 4);
                        const std::uint8_t flags = (*vtx_bytes)[strip_offset + 18];
                        if ((flags & kVtxStripIsTriList) == 0 || strip_first_index < 0)
                        {
                            continue;
                        }

                        append_strip_indices(
                            mesh,
                            *vertices,
                            model_vertex_index / static_cast<std::int32_t>(kStudioVertexSize),
                            mesh_vertex_offset,
                            *vtx_bytes,
                            strip_group_offset,
                            static_cast<std::size_t>(vertex_table_offset),
                            static_cast<std::size_t>(index_table_offset),
                            static_cast<std::size_t>(strip_first_index),
                            strip_index_count);
                    }
                }

                if (!mesh.indices.empty())
                {
                    info.meshes.push_back(std::move(mesh));
                }
            }
        }
    }
}
}

std::optional<SourceModelInfo> load_source_model_info(const SourceAssetStore& assets, const std::filesystem::path& model_path)
{
    const std::optional<std::vector<unsigned char>> bytes = assets.read_binary(normalize_model_path(model_path));
    if (!bytes || bytes->size() < kStudioViewMaxOffset + 12)
    {
        return std::nullopt;
    }

    const std::uint32_t ident = read_u32_le(*bytes, 0);
    const std::uint32_t declared_length = read_u32_le(*bytes, kStudioLengthOffset);
    if (ident != kStudioIdent || declared_length > bytes->size())
    {
        return std::nullopt;
    }

    SourceModelInfo info;
    info.bounds = read_model_bounds(*bytes);
    if (!valid_bounds(info.bounds.mins, info.bounds.maxs))
    {
        return std::nullopt;
    }

    read_model_materials(assets, *bytes, info);
    read_model_skin_table(*bytes, info);
    read_model_meshes(assets, model_path, *bytes, info);
    return info;
}

std::optional<SourceModelBounds> load_source_model_bounds(const SourceAssetStore& assets, const std::filesystem::path& model_path)
{
    if (std::optional<SourceModelInfo> info = load_source_model_info(assets, model_path))
    {
        return info->bounds;
    }
    return std::nullopt;
}

std::string source_model_material_for_skin(const SourceModelInfo& model, std::int32_t skin, std::int32_t material_index)
{
    if (model.materials.empty())
    {
        return {};
    }

    std::int32_t mapped_material = material_index;
    if (model.num_skin_refs > 0 && model.num_skin_families > 0 && !model.skin_families.empty())
    {
        const std::int32_t clamped_skin = std::clamp(skin, 0, model.num_skin_families - 1);
        const std::int32_t clamped_ref = std::clamp(material_index, 0, model.num_skin_refs - 1);
        const std::size_t skin_ref_index = (static_cast<std::size_t>(clamped_skin) * static_cast<std::size_t>(model.num_skin_refs)) +
                                           static_cast<std::size_t>(clamped_ref);
        if (skin_ref_index < model.skin_families.size())
        {
            mapped_material = model.skin_families[skin_ref_index];
        }
    }

    if (mapped_material < 0 || static_cast<std::size_t>(mapped_material) >= model.materials.size())
    {
        mapped_material = std::clamp(material_index, 0, static_cast<std::int32_t>(model.materials.size() - 1));
    }
    return model.materials[static_cast<std::size_t>(mapped_material)];
}
}
