#include "texture_cache.hpp"

#include "editor_profiler.hpp"
#include "rhi/rhi.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/material/material.hpp"
#include "openstrike/material/material_system.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_texture.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>

namespace
{
std::string LowerAssetPath(std::string text)
{
    std::replace(text.begin(), text.end(), '\\', '/');
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string StripMaterialExtension(std::string text)
{
    text = LowerAssetPath(std::move(text));
    if (text.rfind("materials/", 0) == 0)
        text.erase(0, 10);
    if (text.size() > 4)
    {
        const std::string ext = text.substr(text.size() - 4);
        if (ext == ".vmt" || ext == ".vtf")
            text.resize(text.size() - 4);
    }
    return text;
}
}

void TextureCache::Log(const char* fmt, ...)
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

void TextureCache::Init(Rhi* rhi)
{
    m_rhi = rhi;

    constexpr int texSize = 64;
    constexpr int checkerSize = 8;
    unsigned char pixels[texSize * texSize * 4];
    for (int y = 0; y < texSize; ++y)
    {
        for (int x = 0; x < texSize; ++x)
        {
            const bool light = ((x / checkerSize) + (y / checkerSize)) % 2 == 0;
            const unsigned char c = light ? 200 : 140;
            const int idx = (y * texSize + x) * 4;
            pixels[idx + 0] = c;
            pixels[idx + 1] = c;
            pixels[idx + 2] = c;
            pixels[idx + 3] = 255;
        }
    }

    m_fallback.rhiTexture = m_rhi->CreateTexture2D(texSize, texSize, pixels, true);
    m_fallback.width = texSize;
    m_fallback.height = texSize;
}

void TextureCache::Shutdown()
{
    Clear();
    if (m_fallback.rhiTexture)
    {
        m_rhi->DestroyTexture(m_fallback.rhiTexture);
        m_fallback.rhiTexture = RHI_NULL_TEXTURE;
    }
    m_materials.reset();
    m_assets.reset();
    m_filesystem.reset();
}

void TextureCache::Clear()
{
    for (auto& [name, tex] : m_cache)
    {
        if (tex.rhiTexture && tex.rhiTexture != m_fallback.rhiTexture)
            m_rhi->DestroyTexture(tex.rhiTexture);
    }
    m_cache.clear();
}

void TextureCache::SetGameDir(const char* dir)
{
    EDITOR_PROFILE_SCOPE("TextureCache_SetGameDir");
    m_gameDir = dir ? dir : "";
    Clear();
    RebuildAssetStore();
}

void TextureCache::RebuildAssetStore()
{
    m_materials.reset();
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
    m_materials = std::make_unique<openstrike::MaterialSystem>(*m_assets);
    Log("Texture search path: %s", m_filesystem->search_path_string("GAME").c_str());
}

const CachedTexture& TextureCache::Get(const std::string& materialName)
{
    const std::string key = StripMaterialExtension(materialName);
    if (key.empty())
        return m_fallback;

    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return it->second;

    CachedTexture tex = LoadMaterialTexture(key);
    auto result = m_cache.emplace(key, tex);
    return result.first->second;
}

CachedTexture TextureCache::LoadMaterialTexture(const std::string& materialName)
{
    if (!m_materials)
        return m_fallback;

    openstrike::LoadedMaterial material = m_materials->load_world_material(materialName);
    std::optional<openstrike::SourceTexture> texture = openstrike::source_texture_to_rgba8(material.base_texture);

    if (!texture || texture->format != openstrike::SourceTextureFormat::Rgba8 || texture->mips.empty())
    {
        Log("Unsupported or invalid material texture: %s", materialName.c_str());
        return m_fallback;
    }

    const openstrike::SourceTextureMip& mip = texture->mips.front();
    if (mip.width == 0 || mip.height == 0 || mip.bytes.empty())
    {
        Log("Empty material texture: %s", materialName.c_str());
        return m_fallback;
    }

    CachedTexture result;
    result.width = static_cast<int>(mip.width);
    result.height = static_cast<int>(mip.height);
    result.rhiTexture = m_rhi->CreateTexture2D(result.width, result.height, mip.bytes.data(), true);
    return result;
}
