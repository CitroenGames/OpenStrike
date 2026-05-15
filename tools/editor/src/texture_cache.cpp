#include "texture_cache.hpp"

#include "editor_profiler.hpp"
#include "rhi/rhi.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_texture.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

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

std::string ParseBaseTextureFromVMT(const std::string& content)
{
    std::string lower = content;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    constexpr const char* key = "$basetexture";
    std::size_t search_from = 0;
    while (search_from < lower.size())
    {
        const std::size_t pos = lower.find(key, search_from);
        if (pos == std::string::npos)
            return {};

        std::size_t value_pos = pos + 12;
        if (value_pos < lower.size() && lower[value_pos] == '"')
            ++value_pos;
        else if (value_pos < lower.size() && std::isalnum(static_cast<unsigned char>(lower[value_pos])) != 0)
        {
            search_from = value_pos;
            continue;
        }

        while (value_pos < content.size() && std::isspace(static_cast<unsigned char>(content[value_pos])) != 0)
            ++value_pos;

        std::string value;
        if (value_pos < content.size() && content[value_pos] == '"')
        {
            const std::size_t end = content.find('"', value_pos + 1);
            if (end != std::string::npos)
                value = content.substr(value_pos + 1, end - value_pos - 1);
        }
        else
        {
            std::size_t end = value_pos;
            while (end < content.size() && std::isspace(static_cast<unsigned char>(content[end])) == 0 && content[end] != '}')
                ++end;
            value = content.substr(value_pos, end - value_pos);
        }

        value = StripMaterialExtension(std::move(value));
        if (!value.empty())
            return value;

        search_from = value_pos + 1;
    }

    return {};
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

    CachedTexture tex = LoadVTF(key);
    auto result = m_cache.emplace(key, tex);
    return result.first->second;
}

CachedTexture TextureCache::LoadVTF(const std::string& materialName)
{
    if (!m_assets)
        return m_fallback;

    std::string vtf_name = materialName;
    if (std::optional<std::string> vmt = m_assets->read_text("materials/" + materialName + ".vmt"))
    {
        std::string base_texture = ParseBaseTextureFromVMT(*vmt);
        if (!base_texture.empty())
            vtf_name = std::move(base_texture);
    }

    const std::string path = "materials/" + vtf_name + ".vtf";
    std::optional<std::vector<unsigned char>> bytes = m_assets->read_binary(path);
    if (!bytes)
    {
        Log("Texture not found: %s", path.c_str());
        return m_fallback;
    }

    std::optional<openstrike::SourceTexture> texture = openstrike::load_vtf_texture(*bytes);
    if (!texture || texture->format != openstrike::SourceTextureFormat::Rgba8 || texture->mips.empty())
    {
        Log("Unsupported or invalid VTF: %s", path.c_str());
        return m_fallback;
    }

    const openstrike::SourceTextureMip& mip = texture->mips.front();
    CachedTexture result;
    result.width = static_cast<int>(mip.width);
    result.height = static_cast<int>(mip.height);
    result.rhiTexture = m_rhi->CreateTexture2D(result.width, result.height, mip.bytes.data(), true);
    return result;
}
