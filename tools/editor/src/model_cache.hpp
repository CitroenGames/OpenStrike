#pragma once

#include "ed_math.hpp"
#include "rhi/rhi_types.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/source/source_asset_store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Rhi;

struct CachedModel
{
    RhiVertexLayout vl = RHI_NULL_VERTEX_LAYOUT;
    RhiBuffer vbo = RHI_NULL_BUFFER;
    RhiBuffer ibo = RHI_NULL_BUFFER;
    int indexCount = 0;

    Vec3 mins;
    Vec3 maxs;
    std::string materialName;
    bool valid = false;
};

class ModelCache
{
public:
    void Init(Rhi* rhi);
    void Shutdown();
    void Clear();

    void SetGameDir(const char* dir);
    const CachedModel& Get(const std::string& modelPath);

    const CachedModel* Find(const std::string& modelPath) const
    {
        auto it = m_cache.find(modelPath);
        return (it != m_cache.end() && it->second.valid) ? &it->second : nullptr;
    }

    void SetLogFunc(std::function<void(const char*)> fn) { m_logFunc = std::move(fn); }

private:
    CachedModel LoadMDL(const std::string& modelPath);
    void CreateMesh(CachedModel& model, const std::vector<float>& vertices, const std::vector<std::uint32_t>& indices);
    void RebuildAssetStore();
    void Log(const char* fmt, ...);

    Rhi* m_rhi = nullptr;
    std::unordered_map<std::string, CachedModel> m_cache;
    CachedModel m_fallback;
    std::string m_gameDir;

    std::unique_ptr<openstrike::ContentFileSystem> m_filesystem;
    std::unique_ptr<openstrike::SourceAssetStore> m_assets;

    std::function<void(const char*)> m_logFunc;
};
