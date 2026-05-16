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
    Bc3,
    Bc4,
    Bc5
};

struct SourceTextureResource
{
    std::uint32_t type = 0;
    std::uint32_t data = 0;
    bool has_no_data_chunk = false;
};

struct SourceTextureInfo
{
    std::uint32_t major_version = 0;
    std::uint32_t minor_version = 0;
    std::uint32_t header_size = 0;
    std::uint32_t depth = 1;
    std::uint32_t flags = 0;
    std::uint32_t frame_count = 1;
    std::uint32_t face_count = 1;
    std::uint32_t mip_count = 1;
    std::int32_t image_format = -1;
    std::int32_t low_res_image_format = -1;
    std::uint32_t low_res_width = 0;
    std::uint32_t low_res_height = 0;
    std::uint16_t start_frame = 0;
    float reflectivity[3] = {0.0F, 0.0F, 0.0F};
    float bump_scale = 1.0F;
    std::vector<SourceTextureResource> resources;
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
    SourceTextureInfo info;
    std::vector<SourceTextureMip> mips;
};

[[nodiscard]] std::optional<SourceTexture> load_vtf_texture(std::span<const unsigned char> bytes);
[[nodiscard]] std::optional<SourceTexture> source_texture_to_rgba8(const SourceTexture& texture);
[[nodiscard]] std::uint32_t source_texture_row_bytes(SourceTextureFormat format, std::uint32_t width);
[[nodiscard]] std::uint32_t source_texture_row_count(SourceTextureFormat format, std::uint32_t height);
}
