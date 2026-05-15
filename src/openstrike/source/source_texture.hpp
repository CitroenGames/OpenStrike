#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openstrike
{
enum class SourceTextureFormat
{
    Rgba8,
    Bc1,
    Bc2,
    Bc3
};

struct SourceTextureMip
{
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::vector<unsigned char> bytes;
};

struct SourceTexture
{
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    SourceTextureFormat format = SourceTextureFormat::Rgba8;
    std::vector<SourceTextureMip> mips;
};

[[nodiscard]] std::optional<SourceTexture> load_vtf_texture(std::span<const unsigned char> bytes);
[[nodiscard]] std::uint32_t source_texture_row_bytes(SourceTextureFormat format, std::uint32_t width);
[[nodiscard]] std::uint32_t source_texture_row_count(SourceTextureFormat format, std::uint32_t height);
}
