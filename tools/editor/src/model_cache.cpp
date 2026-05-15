#include "model_cache.hpp"

#include "rhi/rhi.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <system_error>

namespace
{
std::string NormalizeModelPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (path.rfind("models/", 0) != 0)
        path = "models/" + path;
    if (path.size() < 4 || path.substr(path.size() - 4) != ".mdl")
        path += ".mdl";
    return path;
}

void IncludeBounds(Vec3& mins, Vec3& maxs, const Vec3& value)
{
    mins.x = std::min(mins.x, value.x);
    mins.y = std::min(mins.y, value.y);
    mins.z = std::min(mins.z, value.z);
    maxs.x = std::max(maxs.x, value.x);
    maxs.y = std::max(maxs.y, value.y);
    maxs.z = std::max(maxs.z, value.z);
}
}

void ModelCache::Log(const char* fmt, ...)
{
    if (!m_logFunc)
        return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    m_logFunc(buf);
}

void ModelCache::Init(Rhi* rhi)
{
    m_rhi = rhi;
}

void ModelCache::Shutdown()
{
    Clear();
    m_assets.reset();
    m_filesystem.reset();
}

void ModelCache::Clear()
{
    for (auto& [name, model] : m_cache)
    {
        if (model.vl)
            m_rhi->DestroyVertexLayout(model.vl);
        if (model.vbo)
            m_rhi->DestroyBuffer(model.vbo);
        if (model.ibo)
            m_rhi->DestroyBuffer(model.ibo);
    }
    m_cache.clear();
}

void ModelCache::SetGameDir(const char* dir)
{
    m_gameDir = dir ? dir : "";
    Clear();
    RebuildAssetStore();
}

void ModelCache::RebuildAssetStore()
{
    m_assets.reset();
    m_filesystem = std::make_unique<openstrike::ContentFileSystem>();
    if (m_gameDir.empty())
        return;

    namespace fs = std::filesystem;
    m_filesystem->add_search_path(m_gameDir, "GAME", openstrike::SearchPathPosition::Head);

    std::error_code ec;
    const fs::path root = fs::path(m_gameDir);
    const fs::path parent = root.parent_path();
    auto addSharedDir = [&](const fs::path& base, const char* name) {
        if (base.empty())
            return;

        fs::path shared_dir = base / name;
        if (fs::is_directory(shared_dir, ec))
            m_filesystem->add_search_path(shared_dir, "GAME", openstrike::SearchPathPosition::Tail);
    };

    for (const char* shared : {"hl2", "platform"})
    {
        addSharedDir(root, shared);
        addSharedDir(parent, shared);
    }

    m_assets = std::make_unique<openstrike::SourceAssetStore>(*m_filesystem);
    Log("Model search path: %s", m_filesystem->search_path_string("GAME").c_str());
}

const CachedModel& ModelCache::Get(const std::string& modelPath)
{
    const std::string normalized = NormalizeModelPath(modelPath);
    auto it = m_cache.find(normalized);
    if (it != m_cache.end())
        return it->second;

    CachedModel model = LoadMDL(normalized);
    auto result = m_cache.emplace(normalized, std::move(model));
    return result.first->second;
}

CachedModel ModelCache::LoadMDL(const std::string& modelPath)
{
    if (!m_assets)
        return m_fallback;

    std::optional<openstrike::SourceModelInfo> info = openstrike::load_source_model_info(*m_assets, modelPath);
    if (!info)
    {
        Log("Model not found or unsupported: %s", modelPath.c_str());
        return m_fallback;
    }

    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    Vec3 mins{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    Vec3 maxs{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
    };

    for (const openstrike::SourceModelMesh& mesh : info->meshes)
    {
        const std::uint32_t base_vertex = static_cast<std::uint32_t>(vertices.size() / 8);
        for (const openstrike::SourceModelVertex& source_vertex : mesh.vertices)
        {
            const Vec3 pos = SourceToGL(source_vertex.position.x, source_vertex.position.y, source_vertex.position.z);
            const Vec3 normal = SourceToGL(source_vertex.normal.x, source_vertex.normal.y, source_vertex.normal.z);
            vertices.insert(vertices.end(), {pos.x, pos.y, pos.z, normal.x, normal.y, normal.z, source_vertex.texcoord.x, source_vertex.texcoord.y});
            IncludeBounds(mins, maxs, pos);
        }

        for (const std::uint32_t index : mesh.indices)
            indices.push_back(base_vertex + index);
    }

    if (vertices.empty() || indices.empty())
        return m_fallback;

    CachedModel model;
    model.mins = mins;
    model.maxs = maxs;
    if (!info->materials.empty())
        model.materialName = openstrike::source_model_material_for_skin(*info, 0, 0);

    CreateMesh(model, vertices, indices);
    model.valid = model.vl != RHI_NULL_VERTEX_LAYOUT;
    Log("Loaded model: %s (%d triangles)", modelPath.c_str(), model.indexCount / 3);
    return model;
}

void ModelCache::CreateMesh(CachedModel& model, const std::vector<float>& vertices, const std::vector<std::uint32_t>& indices)
{
    model.vbo = m_rhi->CreateBuffer(RhiBufferUsage::Vertex, vertices.data(), vertices.size() * sizeof(float));
    model.ibo = m_rhi->CreateBuffer(RhiBufferUsage::Index, indices.data(), indices.size() * sizeof(std::uint32_t));
    model.indexCount = static_cast<int>(indices.size());

    std::vector<RhiVertexAttrib> attribs = {
        {0, 3, 8 * static_cast<int>(sizeof(float)), 0},
        {1, 3, 8 * static_cast<int>(sizeof(float)), 3 * static_cast<int>(sizeof(float))},
        {2, 2, 8 * static_cast<int>(sizeof(float)), 6 * static_cast<int>(sizeof(float))},
    };
    model.vl = m_rhi->CreateVertexLayout(model.vbo, attribs, model.ibo);
}
