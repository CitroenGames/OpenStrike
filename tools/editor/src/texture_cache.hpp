#pragma once

#include "rhi/rhi_types.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/source/source_asset_store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class Rhi;

struct CachedTexture
{
    RhiTexture rhiTexture = RHI_NULL_TEXTURE;
    int width = 64;
    int height = 64;
};

class TextureCache
{
public:
    void Init(Rhi* rhi);
    void Shutdown();

    void SetGameDir(const char* dir);
    const CachedTexture& Get(const std::string& materialName);
    void Clear();

    void SetLogFunc(std::function<void(const char*)> fn) { m_logFunc = std::move(fn); }

private:
    CachedTexture LoadVTF(const std::string& materialName);
    void RebuildAssetStore();
    void Log(const char* fmt, ...);

    Rhi* m_rhi = nullptr;
    std::unordered_map<std::string, CachedTexture> m_cache;
    CachedTexture m_fallback;
    std::string m_gameDir;

    std::unique_ptr<openstrike::ContentFileSystem> m_filesystem;
    std::unique_ptr<openstrike::SourceAssetStore> m_assets;

    std::function<void(const char*)> m_logFunc;
};
