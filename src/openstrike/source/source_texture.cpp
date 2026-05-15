#include "openstrike/source/source_texture.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kVtfHighResImageResource = 0x30U;
constexpr std::uint32_t kTextureFlagsEnvmap = 0x00004000U;

enum SourceImageFormat : std::int32_t
{
    ImageFormatRgba8888 = 0,
    ImageFormatAbgr8888 = 1,
    ImageFormatRgb888 = 2,
    ImageFormatBgr888 = 3,
    ImageFormatArgb8888 = 11,
    ImageFormatBgra8888 = 12,
    ImageFormatDxt1 = 13,
    ImageFormatDxt3 = 14,
    ImageFormatDxt5 = 15,
    ImageFormatBgrx8888 = 16,
    ImageFormatDxt1OneBitAlpha = 20,
    ImageFormatRgbx8888 = 31,
};

std::uint32_t read_u32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
    {
        throw std::runtime_error("unexpected end of VTF");
    }

    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint16_t read_u16_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size())
    {
        throw std::runtime_error("unexpected end of VTF");
    }

    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::int32_t read_s32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(read_u32_le(bytes, offset));
}

std::uint32_t mip_dimension(std::uint32_t dimension, std::uint32_t mip)
{
    return std::max<std::uint32_t>(dimension >> mip, 1);
}

bool source_format_for_vtf(std::int32_t image_format, SourceTextureFormat& output)
{
    switch (image_format)
    {
    case ImageFormatDxt1:
    case ImageFormatDxt1OneBitAlpha:
        output = SourceTextureFormat::Bc1;
        return true;
    case ImageFormatDxt3:
        output = SourceTextureFormat::Bc2;
        return true;
    case ImageFormatDxt5:
        output = SourceTextureFormat::Bc3;
        return true;
    case ImageFormatRgba8888:
    case ImageFormatAbgr8888:
    case ImageFormatRgb888:
    case ImageFormatBgr888:
    case ImageFormatArgb8888:
    case ImageFormatBgra8888:
    case ImageFormatBgrx8888:
    case ImageFormatRgbx8888:
        output = SourceTextureFormat::Rgba8;
        return true;
    default:
        return false;
    }
}

bool is_uncompressed_rgba_output(std::int32_t image_format)
{
    switch (image_format)
    {
    case ImageFormatRgba8888:
    case ImageFormatAbgr8888:
    case ImageFormatRgb888:
    case ImageFormatBgr888:
    case ImageFormatArgb8888:
    case ImageFormatBgra8888:
    case ImageFormatBgrx8888:
    case ImageFormatRgbx8888:
        return true;
    default:
        return false;
    }
}

std::uint32_t vtf_image_size(std::int32_t image_format, std::uint32_t width, std::uint32_t height)
{
    SourceTextureFormat source_format = SourceTextureFormat::Rgba8;
    if (!source_format_for_vtf(image_format, source_format))
    {
        return 0;
    }

    if (is_uncompressed_rgba_output(image_format))
    {
        switch (image_format)
        {
        case ImageFormatRgb888:
        case ImageFormatBgr888:
            return width * height * 3;
        default:
            return width * height * 4;
        }
    }

    return source_texture_row_bytes(source_format, width) * source_texture_row_count(source_format, height);
}

std::uint32_t low_res_image_size(std::int32_t image_format, std::uint32_t width, std::uint32_t height)
{
    if (image_format < 0 || width == 0 || height == 0)
    {
        return 0;
    }

    return vtf_image_size(image_format, width, height);
}

std::optional<std::uint32_t> high_res_image_offset(std::span<const unsigned char> bytes, std::uint32_t major, std::uint32_t minor, std::uint32_t header_size)
{
    if (major == 7 && minor >= 3 && bytes.size() >= 80)
    {
        const std::uint32_t resource_count = read_u32_le(bytes, 68);
        const std::size_t resource_begin = 80;
        const std::size_t resource_end = resource_begin + (static_cast<std::size_t>(resource_count) * 8);
        if (resource_end <= bytes.size())
        {
            for (std::size_t index = 0; index < resource_count; ++index)
            {
                const std::size_t offset = resource_begin + (index * 8);
                const std::uint32_t type = read_u32_le(bytes, offset) & 0x00FFFFFFU;
                const std::uint32_t data_offset = read_u32_le(bytes, offset + 4);
                if (type == kVtfHighResImageResource)
                {
                    return data_offset;
                }
            }
        }
    }

    if (bytes.size() < header_size || header_size < 63)
    {
        return std::nullopt;
    }

    const std::int32_t low_format = read_s32_le(bytes, 57);
    const std::uint32_t low_width = bytes[61];
    const std::uint32_t low_height = bytes[62];
    return header_size + low_res_image_size(low_format, low_width, low_height);
}

std::uint64_t disk_offset_for_mip(
    std::int32_t image_format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t mip_count,
    std::uint32_t frame_count,
    std::uint32_t face_count,
    std::uint32_t mip_level)
{
    std::uint64_t offset = 0;
    for (std::uint32_t stored_mip = mip_count; stored_mip-- > mip_level + 1;)
    {
        offset += static_cast<std::uint64_t>(vtf_image_size(image_format, mip_dimension(width, stored_mip), mip_dimension(height, stored_mip))) *
                  frame_count * face_count;
    }
    return offset;
}

std::vector<unsigned char> convert_to_rgba(std::int32_t image_format, std::span<const unsigned char> source, std::uint32_t width, std::uint32_t height)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4);
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        const unsigned char* src = source.data();
        unsigned char r = 255;
        unsigned char g = 255;
        unsigned char b = 255;
        unsigned char a = 255;

        switch (image_format)
        {
        case ImageFormatRgba8888:
            src += pixel * 4;
            r = src[0];
            g = src[1];
            b = src[2];
            a = src[3];
            break;
        case ImageFormatAbgr8888:
            src += pixel * 4;
            a = src[0];
            b = src[1];
            g = src[2];
            r = src[3];
            break;
        case ImageFormatArgb8888:
            src += pixel * 4;
            a = src[0];
            r = src[1];
            g = src[2];
            b = src[3];
            break;
        case ImageFormatBgra8888:
            src += pixel * 4;
            b = src[0];
            g = src[1];
            r = src[2];
            a = src[3];
            break;
        case ImageFormatBgrx8888:
            src += pixel * 4;
            b = src[0];
            g = src[1];
            r = src[2];
            break;
        case ImageFormatRgbx8888:
            src += pixel * 4;
            r = src[0];
            g = src[1];
            b = src[2];
            break;
        case ImageFormatRgb888:
            src += pixel * 3;
            r = src[0];
            g = src[1];
            b = src[2];
            break;
        case ImageFormatBgr888:
            src += pixel * 3;
            b = src[0];
            g = src[1];
            r = src[2];
            break;
        default:
            break;
        }

        rgba[(pixel * 4) + 0] = r;
        rgba[(pixel * 4) + 1] = g;
        rgba[(pixel * 4) + 2] = b;
        rgba[(pixel * 4) + 3] = a;
    }
    return rgba;
}
}

std::optional<SourceTexture> load_vtf_texture(std::span<const unsigned char> bytes)
{
    try
    {
        if (bytes.size() < 64 || std::memcmp(bytes.data(), "VTF\0", 4) != 0)
        {
            return std::nullopt;
        }

        const std::uint32_t major = read_u32_le(bytes, 4);
        const std::uint32_t minor = read_u32_le(bytes, 8);
        const std::uint32_t header_size = read_u32_le(bytes, 12);
        const std::uint32_t width = read_u16_le(bytes, 16);
        const std::uint32_t height = read_u16_le(bytes, 18);
        const std::uint32_t flags = read_u32_le(bytes, 20);
        const std::uint32_t frame_count = std::max<std::uint32_t>(read_u16_le(bytes, 24), 1);
        const std::int32_t image_format = read_s32_le(bytes, 52);
        const std::uint32_t mip_count = std::max<std::uint32_t>(bytes[56], 1);
        if (width == 0 || height == 0)
        {
            return std::nullopt;
        }

        SourceTextureFormat texture_format = SourceTextureFormat::Rgba8;
        if (!source_format_for_vtf(image_format, texture_format))
        {
            return std::nullopt;
        }

        const std::optional<std::uint32_t> image_offset = high_res_image_offset(bytes, major, minor, header_size);
        if (!image_offset || *image_offset >= bytes.size())
        {
            return std::nullopt;
        }

        const std::uint32_t face_count = (flags & kTextureFlagsEnvmap) != 0 ? 6U : 1U;
        SourceTexture texture;
        texture.width = width;
        texture.height = height;
        texture.format = texture_format;
        texture.mips.reserve(mip_count);

        for (std::uint32_t mip = 0; mip < mip_count; ++mip)
        {
            const std::uint32_t mip_width = mip_dimension(width, mip);
            const std::uint32_t mip_height = mip_dimension(height, mip);
            const std::uint32_t source_size = vtf_image_size(image_format, mip_width, mip_height);
            if (source_size == 0)
            {
                return std::nullopt;
            }

            const std::uint64_t offset = static_cast<std::uint64_t>(*image_offset) +
                                         disk_offset_for_mip(image_format, width, height, mip_count, frame_count, face_count, mip);
            if (offset + source_size > bytes.size())
            {
                return std::nullopt;
            }

            SourceTextureMip texture_mip;
            texture_mip.width = mip_width;
            texture_mip.height = mip_height;
            const auto source = bytes.subspan(static_cast<std::size_t>(offset), source_size);
            if (is_uncompressed_rgba_output(image_format))
            {
                texture_mip.bytes = convert_to_rgba(image_format, source, mip_width, mip_height);
            }
            else
            {
                texture_mip.bytes.assign(source.begin(), source.end());
            }
            texture.mips.push_back(std::move(texture_mip));
        }

        return texture;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::uint32_t source_texture_row_bytes(SourceTextureFormat format, std::uint32_t width)
{
    switch (format)
    {
    case SourceTextureFormat::Bc1:
        return std::max<std::uint32_t>((width + 3U) / 4U, 1U) * 8U;
    case SourceTextureFormat::Bc2:
    case SourceTextureFormat::Bc3:
        return std::max<std::uint32_t>((width + 3U) / 4U, 1U) * 16U;
    case SourceTextureFormat::Rgba8:
        return width * 4U;
    }

    return width * 4U;
}

std::uint32_t source_texture_row_count(SourceTextureFormat format, std::uint32_t height)
{
    switch (format)
    {
    case SourceTextureFormat::Bc1:
    case SourceTextureFormat::Bc2:
    case SourceTextureFormat::Bc3:
        return std::max<std::uint32_t>((height + 3U) / 4U, 1U);
    case SourceTextureFormat::Rgba8:
        return height;
    }

    return height;
}
}
