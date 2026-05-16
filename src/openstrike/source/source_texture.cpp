#include "openstrike/source/source_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kVtfHighResImageResource = 0x30U;
constexpr std::uint32_t kVtfResourceFlagMask = 0xFF000000U;
constexpr std::uint32_t kVtfResourceHasNoDataChunk = 0x02000000U;
constexpr std::uint32_t kTextureFlagsEnvmap = 0x00004000U;

enum SourceImageFormat : std::int32_t
{
    ImageFormatRgba8888 = 0,
    ImageFormatAbgr8888 = 1,
    ImageFormatRgb888 = 2,
    ImageFormatBgr888 = 3,
    ImageFormatRgb565 = 4,
    ImageFormatI8 = 5,
    ImageFormatIa88 = 6,
    ImageFormatA8 = 8,
    ImageFormatRgb888Bluescreen = 9,
    ImageFormatBgr888Bluescreen = 10,
    ImageFormatArgb8888 = 11,
    ImageFormatBgra8888 = 12,
    ImageFormatDxt1 = 13,
    ImageFormatDxt3 = 14,
    ImageFormatDxt5 = 15,
    ImageFormatBgrx8888 = 16,
    ImageFormatBgr565 = 17,
    ImageFormatBgrx5551 = 18,
    ImageFormatBgra4444 = 19,
    ImageFormatDxt1OneBitAlpha = 20,
    ImageFormatBgra5551 = 21,
    ImageFormatUv88 = 22,
    ImageFormatRgbx8888 = 31,
    ImageFormatAti2n = 34,
    ImageFormatAti1n = 35,
};

struct Rgba8
{
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    unsigned char a = 255;
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

float read_f32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    const std::uint32_t raw = read_u32_le(bytes, offset);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
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
    case ImageFormatAti1n:
        output = SourceTextureFormat::Bc4;
        return true;
    case ImageFormatAti2n:
        output = SourceTextureFormat::Bc5;
        return true;
    case ImageFormatRgba8888:
    case ImageFormatAbgr8888:
    case ImageFormatRgb888:
    case ImageFormatBgr888:
    case ImageFormatRgb565:
    case ImageFormatI8:
    case ImageFormatIa88:
    case ImageFormatA8:
    case ImageFormatRgb888Bluescreen:
    case ImageFormatBgr888Bluescreen:
    case ImageFormatArgb8888:
    case ImageFormatBgra8888:
    case ImageFormatBgrx8888:
    case ImageFormatBgr565:
    case ImageFormatBgrx5551:
    case ImageFormatBgra4444:
    case ImageFormatBgra5551:
    case ImageFormatUv88:
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
    case ImageFormatRgb565:
    case ImageFormatI8:
    case ImageFormatIa88:
    case ImageFormatA8:
    case ImageFormatRgb888Bluescreen:
    case ImageFormatBgr888Bluescreen:
    case ImageFormatArgb8888:
    case ImageFormatBgra8888:
    case ImageFormatBgrx8888:
    case ImageFormatBgr565:
    case ImageFormatBgrx5551:
    case ImageFormatBgra4444:
    case ImageFormatBgra5551:
    case ImageFormatUv88:
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
        case ImageFormatRgb888Bluescreen:
        case ImageFormatBgr888Bluescreen:
            return width * height * 3;
        case ImageFormatRgb565:
        case ImageFormatBgr565:
        case ImageFormatBgrx5551:
        case ImageFormatBgra4444:
        case ImageFormatBgra5551:
        case ImageFormatIa88:
        case ImageFormatUv88:
            return width * height * 2;
        case ImageFormatI8:
        case ImageFormatA8:
            return width * height;
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

std::vector<SourceTextureResource> read_resource_table(std::span<const unsigned char> bytes, std::uint32_t major, std::uint32_t minor)
{
    std::vector<SourceTextureResource> resources;
    if (major == 7 && minor >= 3 && bytes.size() >= 80)
    {
        const std::uint32_t resource_count = read_u32_le(bytes, 68);
        const std::size_t resource_begin = 80;
        const std::size_t resource_end = resource_begin + (static_cast<std::size_t>(resource_count) * 8);
        if (resource_end <= bytes.size())
        {
            resources.reserve(resource_count);
            for (std::size_t index = 0; index < resource_count; ++index)
            {
                const std::size_t offset = resource_begin + (index * 8);
                const std::uint32_t raw_type = read_u32_le(bytes, offset);
                const std::uint32_t data_offset = read_u32_le(bytes, offset + 4);
                resources.push_back(SourceTextureResource{
                    .type = raw_type & ~kVtfResourceFlagMask,
                    .data = data_offset,
                    .has_no_data_chunk = (raw_type & kVtfResourceHasNoDataChunk) != 0,
                });
            }
        }
    }
    return resources;
}

std::optional<std::uint32_t> high_res_image_offset(
    std::span<const unsigned char> bytes,
    const SourceTextureInfo& info)
{
    for (const SourceTextureResource& resource : info.resources)
    {
        if (resource.type == kVtfHighResImageResource && !resource.has_no_data_chunk)
        {
            return resource.data;
        }
    }

    if (bytes.size() < info.header_size || info.header_size < 63)
    {
        return std::nullopt;
    }

    const std::int32_t low_format = read_s32_le(bytes, 57);
    const std::uint32_t low_width = bytes[61];
    const std::uint32_t low_height = bytes[62];
    return info.header_size + low_res_image_size(low_format, low_width, low_height);
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

std::uint16_t read_block_u16_le(const unsigned char* bytes);

unsigned char expand_bits(std::uint32_t value, unsigned bits)
{
    const std::uint32_t max_value = (1U << bits) - 1U;
    return static_cast<unsigned char>((value * 255U + (max_value / 2U)) / max_value);
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
        case ImageFormatRgb888Bluescreen:
            src += pixel * 3;
            r = src[0];
            g = src[1];
            b = src[2];
            if (r == 0 && g == 0 && b == 255)
            {
                a = 0;
            }
            break;
        case ImageFormatBgr888Bluescreen:
            src += pixel * 3;
            b = src[0];
            g = src[1];
            r = src[2];
            if (r == 0 && g == 0 && b == 255)
            {
                a = 0;
            }
            break;
        case ImageFormatRgb565:
        {
            src += pixel * 2;
            const std::uint16_t raw = read_block_u16_le(src);
            r = expand_bits(raw & 0x1FU, 5);
            g = expand_bits((raw >> 5U) & 0x3FU, 6);
            b = expand_bits((raw >> 11U) & 0x1FU, 5);
            break;
        }
        case ImageFormatBgr565:
        {
            src += pixel * 2;
            const std::uint16_t raw = read_block_u16_le(src);
            b = expand_bits(raw & 0x1FU, 5);
            g = expand_bits((raw >> 5U) & 0x3FU, 6);
            r = expand_bits((raw >> 11U) & 0x1FU, 5);
            break;
        }
        case ImageFormatBgrx5551:
        case ImageFormatBgra5551:
        {
            src += pixel * 2;
            const std::uint16_t raw = read_block_u16_le(src);
            b = expand_bits(raw & 0x1FU, 5);
            g = expand_bits((raw >> 5U) & 0x1FU, 5);
            r = expand_bits((raw >> 10U) & 0x1FU, 5);
            if (image_format == ImageFormatBgra5551)
            {
                a = (raw & 0x8000U) != 0 ? 255 : 0;
            }
            break;
        }
        case ImageFormatBgra4444:
        {
            src += pixel * 2;
            const std::uint16_t raw = read_block_u16_le(src);
            b = expand_bits(raw & 0xFU, 4);
            g = expand_bits((raw >> 4U) & 0xFU, 4);
            r = expand_bits((raw >> 8U) & 0xFU, 4);
            a = expand_bits((raw >> 12U) & 0xFU, 4);
            break;
        }
        case ImageFormatI8:
            src += pixel;
            r = src[0];
            g = src[0];
            b = src[0];
            break;
        case ImageFormatIa88:
            src += pixel * 2;
            r = src[0];
            g = src[0];
            b = src[0];
            a = src[1];
            break;
        case ImageFormatA8:
            src += pixel;
            a = src[0];
            break;
        case ImageFormatUv88:
            src += pixel * 2;
            r = src[0];
            g = src[1];
            b = 255;
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

std::uint16_t read_block_u16_le(const unsigned char* bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) | static_cast<std::uint16_t>(bytes[1] << 8U);
}

std::uint32_t read_block_u32_le(const unsigned char* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

unsigned char expand_565_channel(std::uint16_t value, int shift, int bits)
{
    const std::uint16_t mask = static_cast<std::uint16_t>((1U << bits) - 1U);
    const std::uint16_t channel = static_cast<std::uint16_t>((value >> shift) & mask);
    return static_cast<unsigned char>((channel * 255U + (mask / 2U)) / mask);
}

Rgba8 rgb565_to_rgba(std::uint16_t color)
{
    return Rgba8{
        .r = expand_565_channel(color, 11, 5),
        .g = expand_565_channel(color, 5, 6),
        .b = expand_565_channel(color, 0, 5),
        .a = 255,
    };
}

Rgba8 interpolate_color(Rgba8 lhs, Rgba8 rhs, unsigned lhs_weight, unsigned rhs_weight, unsigned divisor)
{
    return Rgba8{
        .r = static_cast<unsigned char>(((lhs.r * lhs_weight) + (rhs.r * rhs_weight) + (divisor / 2U)) / divisor),
        .g = static_cast<unsigned char>(((lhs.g * lhs_weight) + (rhs.g * rhs_weight) + (divisor / 2U)) / divisor),
        .b = static_cast<unsigned char>(((lhs.b * lhs_weight) + (rhs.b * rhs_weight) + (divisor / 2U)) / divisor),
        .a = static_cast<unsigned char>(((lhs.a * lhs_weight) + (rhs.a * rhs_weight) + (divisor / 2U)) / divisor),
    };
}

std::array<Rgba8, 4> decode_bc_color_table(const unsigned char* block, bool force_four_color)
{
    const std::uint16_t color0 = read_block_u16_le(block);
    const std::uint16_t color1 = read_block_u16_le(block + 2);

    std::array<Rgba8, 4> colors{};
    colors[0] = rgb565_to_rgba(color0);
    colors[1] = rgb565_to_rgba(color1);
    if (force_four_color || color0 > color1)
    {
        colors[2] = interpolate_color(colors[0], colors[1], 2, 1, 3);
        colors[3] = interpolate_color(colors[0], colors[1], 1, 2, 3);
    }
    else
    {
        colors[2] = interpolate_color(colors[0], colors[1], 1, 1, 2);
        colors[3] = Rgba8{.r = 0, .g = 0, .b = 0, .a = 0};
    }

    return colors;
}

void write_decoded_pixel(std::vector<unsigned char>& output, std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y, Rgba8 color)
{
    if (x >= width || y >= height)
    {
        return;
    }

    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
    output[offset + 0] = color.r;
    output[offset + 1] = color.g;
    output[offset + 2] = color.b;
    output[offset + 3] = color.a;
}

void decode_bc_color_block(
    const unsigned char* block,
    std::vector<unsigned char>& output,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t block_x,
    std::uint32_t block_y,
    bool force_four_color,
    const std::array<unsigned char, 16>* alpha_values = nullptr)
{
    const std::array<Rgba8, 4> colors = decode_bc_color_table(block, force_four_color);
    const std::uint32_t selectors = read_block_u32_le(block + 4);
    for (std::uint32_t y = 0; y < 4; ++y)
    {
        for (std::uint32_t x = 0; x < 4; ++x)
        {
            const std::uint32_t pixel = (y * 4U) + x;
            Rgba8 color = colors[(selectors >> (pixel * 2U)) & 0x3U];
            if (alpha_values != nullptr)
            {
                color.a = (*alpha_values)[pixel];
            }
            write_decoded_pixel(output, width, height, (block_x * 4U) + x, (block_y * 4U) + y, color);
        }
    }
}

std::vector<unsigned char> decode_bc1(std::span<const unsigned char> source, std::uint32_t width, std::uint32_t height)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4U);
    const std::uint32_t block_cols = std::max<std::uint32_t>((width + 3U) / 4U, 1U);
    const std::uint32_t block_rows = std::max<std::uint32_t>((height + 3U) / 4U, 1U);
    for (std::uint32_t by = 0; by < block_rows; ++by)
    {
        for (std::uint32_t bx = 0; bx < block_cols; ++bx)
        {
            const std::size_t block_offset = (static_cast<std::size_t>(by) * block_cols + bx) * 8U;
            decode_bc_color_block(source.data() + block_offset, rgba, width, height, bx, by, false);
        }
    }
    return rgba;
}

std::vector<unsigned char> decode_bc2(std::span<const unsigned char> source, std::uint32_t width, std::uint32_t height)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4U);
    const std::uint32_t block_cols = std::max<std::uint32_t>((width + 3U) / 4U, 1U);
    const std::uint32_t block_rows = std::max<std::uint32_t>((height + 3U) / 4U, 1U);
    for (std::uint32_t by = 0; by < block_rows; ++by)
    {
        for (std::uint32_t bx = 0; bx < block_cols; ++bx)
        {
            const std::size_t block_offset = (static_cast<std::size_t>(by) * block_cols + bx) * 16U;
            std::array<unsigned char, 16> alpha_values{};
            const std::uint64_t alpha_bits =
                static_cast<std::uint64_t>(read_block_u32_le(source.data() + block_offset)) |
                (static_cast<std::uint64_t>(read_block_u32_le(source.data() + block_offset + 4)) << 32U);
            for (std::uint32_t pixel = 0; pixel < 16; ++pixel)
            {
                const unsigned char alpha = static_cast<unsigned char>((alpha_bits >> (pixel * 4U)) & 0xFU);
                alpha_values[pixel] = static_cast<unsigned char>((alpha << 4U) | alpha);
            }
            decode_bc_color_block(source.data() + block_offset + 8, rgba, width, height, bx, by, true, &alpha_values);
        }
    }
    return rgba;
}

std::array<unsigned char, 8> decode_bc3_alpha_table(const unsigned char* block)
{
    std::array<unsigned char, 8> alpha{};
    alpha[0] = block[0];
    alpha[1] = block[1];
    if (alpha[0] > alpha[1])
    {
        alpha[2] = static_cast<unsigned char>((6U * alpha[0] + 1U * alpha[1] + 3U) / 7U);
        alpha[3] = static_cast<unsigned char>((5U * alpha[0] + 2U * alpha[1] + 3U) / 7U);
        alpha[4] = static_cast<unsigned char>((4U * alpha[0] + 3U * alpha[1] + 3U) / 7U);
        alpha[5] = static_cast<unsigned char>((3U * alpha[0] + 4U * alpha[1] + 3U) / 7U);
        alpha[6] = static_cast<unsigned char>((2U * alpha[0] + 5U * alpha[1] + 3U) / 7U);
        alpha[7] = static_cast<unsigned char>((1U * alpha[0] + 6U * alpha[1] + 3U) / 7U);
    }
    else
    {
        alpha[2] = static_cast<unsigned char>((4U * alpha[0] + 1U * alpha[1] + 2U) / 5U);
        alpha[3] = static_cast<unsigned char>((3U * alpha[0] + 2U * alpha[1] + 2U) / 5U);
        alpha[4] = static_cast<unsigned char>((2U * alpha[0] + 3U * alpha[1] + 2U) / 5U);
        alpha[5] = static_cast<unsigned char>((1U * alpha[0] + 4U * alpha[1] + 2U) / 5U);
        alpha[6] = 0;
        alpha[7] = 255;
    }
    return alpha;
}

std::vector<unsigned char> decode_bc3(std::span<const unsigned char> source, std::uint32_t width, std::uint32_t height)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4U);
    const std::uint32_t block_cols = std::max<std::uint32_t>((width + 3U) / 4U, 1U);
    const std::uint32_t block_rows = std::max<std::uint32_t>((height + 3U) / 4U, 1U);
    for (std::uint32_t by = 0; by < block_rows; ++by)
    {
        for (std::uint32_t bx = 0; bx < block_cols; ++bx)
        {
            const std::size_t block_offset = (static_cast<std::size_t>(by) * block_cols + bx) * 16U;
            const std::array<unsigned char, 8> alpha_table = decode_bc3_alpha_table(source.data() + block_offset);
            std::array<unsigned char, 16> alpha_values{};
            std::uint64_t alpha_selectors = 0;
            for (std::uint32_t byte = 0; byte < 6; ++byte)
            {
                alpha_selectors |= static_cast<std::uint64_t>(source[block_offset + 2 + byte]) << (byte * 8U);
            }
            for (std::uint32_t pixel = 0; pixel < 16; ++pixel)
            {
                alpha_values[pixel] = alpha_table[(alpha_selectors >> (pixel * 3U)) & 0x7U];
            }
            decode_bc_color_block(source.data() + block_offset + 8, rgba, width, height, bx, by, true, &alpha_values);
        }
    }
    return rgba;
}

void decode_bc4_values(
    const unsigned char* block,
    std::array<unsigned char, 16>& values)
{
    const std::array<unsigned char, 8> table = decode_bc3_alpha_table(block);
    std::uint64_t selectors = 0;
    for (std::uint32_t byte = 0; byte < 6; ++byte)
    {
        selectors |= static_cast<std::uint64_t>(block[2 + byte]) << (byte * 8U);
    }

    for (std::uint32_t pixel = 0; pixel < 16; ++pixel)
    {
        values[pixel] = table[(selectors >> (pixel * 3U)) & 0x7U];
    }
}

std::vector<unsigned char> decode_bc4(std::span<const unsigned char> source, std::uint32_t width, std::uint32_t height)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4U);
    const std::uint32_t block_cols = std::max<std::uint32_t>((width + 3U) / 4U, 1U);
    const std::uint32_t block_rows = std::max<std::uint32_t>((height + 3U) / 4U, 1U);
    for (std::uint32_t by = 0; by < block_rows; ++by)
    {
        for (std::uint32_t bx = 0; bx < block_cols; ++bx)
        {
            const std::size_t block_offset = (static_cast<std::size_t>(by) * block_cols + bx) * 8U;
            std::array<unsigned char, 16> values{};
            decode_bc4_values(source.data() + block_offset, values);
            for (std::uint32_t y = 0; y < 4; ++y)
            {
                for (std::uint32_t x = 0; x < 4; ++x)
                {
                    const unsigned char value = values[(y * 4U) + x];
                    write_decoded_pixel(rgba, width, height, (bx * 4U) + x, (by * 4U) + y, Rgba8{value, value, value, 255});
                }
            }
        }
    }
    return rgba;
}

std::vector<unsigned char> decode_bc5(std::span<const unsigned char> source, std::uint32_t width, std::uint32_t height)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4U);
    const std::uint32_t block_cols = std::max<std::uint32_t>((width + 3U) / 4U, 1U);
    const std::uint32_t block_rows = std::max<std::uint32_t>((height + 3U) / 4U, 1U);
    for (std::uint32_t by = 0; by < block_rows; ++by)
    {
        for (std::uint32_t bx = 0; bx < block_cols; ++bx)
        {
            const std::size_t block_offset = (static_cast<std::size_t>(by) * block_cols + bx) * 16U;
            std::array<unsigned char, 16> red{};
            std::array<unsigned char, 16> green{};
            decode_bc4_values(source.data() + block_offset, red);
            decode_bc4_values(source.data() + block_offset + 8, green);
            for (std::uint32_t y = 0; y < 4; ++y)
            {
                for (std::uint32_t x = 0; x < 4; ++x)
                {
                    const std::uint32_t pixel = (y * 4U) + x;
                    const float nx = (static_cast<float>(red[pixel]) / 127.5F) - 1.0F;
                    const float ny = (static_cast<float>(green[pixel]) / 127.5F) - 1.0F;
                    const float nz = std::sqrt(std::max(0.0F, 1.0F - (nx * nx) - (ny * ny)));
                    const unsigned char blue = static_cast<unsigned char>(std::clamp((nz * 0.5F + 0.5F) * 255.0F, 0.0F, 255.0F));
                    write_decoded_pixel(rgba, width, height, (bx * 4U) + x, (by * 4U) + y, Rgba8{red[pixel], green[pixel], blue, 255});
                }
            }
        }
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
        const std::uint16_t start_frame = read_u16_le(bytes, 26);
        const std::int32_t image_format = read_s32_le(bytes, 52);
        const std::uint32_t mip_count = std::max<std::uint32_t>(bytes[56], 1);
        const std::int32_t low_res_image_format = read_s32_le(bytes, 57);
        const std::uint32_t low_res_width = bytes[61];
        const std::uint32_t low_res_height = bytes[62];
        const std::uint32_t depth = (major == 7 && minor >= 2 && bytes.size() >= 65) ? std::max<std::uint32_t>(read_u16_le(bytes, 63), 1) : 1;
        if (width == 0 || height == 0 || depth != 1)
        {
            return std::nullopt;
        }

        SourceTextureFormat texture_format = SourceTextureFormat::Rgba8;
        if (!source_format_for_vtf(image_format, texture_format))
        {
            return std::nullopt;
        }

        const std::uint32_t face_count = (flags & kTextureFlagsEnvmap) != 0 ? 6U : 1U;
        SourceTextureInfo info;
        info.major_version = major;
        info.minor_version = minor;
        info.header_size = header_size;
        info.depth = depth;
        info.flags = flags;
        info.frame_count = frame_count;
        info.face_count = face_count;
        info.mip_count = mip_count;
        info.image_format = image_format;
        info.low_res_image_format = low_res_image_format;
        info.low_res_width = low_res_width;
        info.low_res_height = low_res_height;
        info.start_frame = start_frame;
        if (bytes.size() >= 52)
        {
            info.reflectivity[0] = read_f32_le(bytes, 32);
            info.reflectivity[1] = read_f32_le(bytes, 36);
            info.reflectivity[2] = read_f32_le(bytes, 40);
            info.bump_scale = read_f32_le(bytes, 48);
        }
        info.resources = read_resource_table(bytes, major, minor);

        const std::optional<std::uint32_t> image_offset = high_res_image_offset(bytes, info);
        if (!image_offset || *image_offset >= bytes.size())
        {
            return std::nullopt;
        }

        SourceTexture texture;
        texture.width = width;
        texture.height = height;
        texture.format = texture_format;
        texture.info = std::move(info);
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
    case SourceTextureFormat::Bc4:
        return std::max<std::uint32_t>((width + 3U) / 4U, 1U) * 8U;
    case SourceTextureFormat::Bc2:
    case SourceTextureFormat::Bc3:
    case SourceTextureFormat::Bc5:
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
    case SourceTextureFormat::Bc4:
    case SourceTextureFormat::Bc5:
        return std::max<std::uint32_t>((height + 3U) / 4U, 1U);
    case SourceTextureFormat::Rgba8:
        return height;
    }

    return height;
}

std::optional<SourceTexture> source_texture_to_rgba8(const SourceTexture& texture)
{
    if (texture.mips.empty())
    {
        return std::nullopt;
    }

    if (texture.format == SourceTextureFormat::Rgba8)
    {
        return texture;
    }

    SourceTexture converted;
    converted.width = texture.width;
    converted.height = texture.height;
    converted.format = SourceTextureFormat::Rgba8;
    converted.info = texture.info;
    converted.mips.reserve(texture.mips.size());

    for (const SourceTextureMip& source_mip : texture.mips)
    {
        const std::uint32_t source_row_bytes = source_texture_row_bytes(texture.format, source_mip.width);
        const std::uint32_t source_row_count = source_texture_row_count(texture.format, source_mip.height);
        const std::uint64_t required_bytes = static_cast<std::uint64_t>(source_row_bytes) * source_row_count;
        if (source_mip.bytes.size() < required_bytes)
        {
            return std::nullopt;
        }

        SourceTextureMip decoded_mip;
        decoded_mip.width = source_mip.width;
        decoded_mip.height = source_mip.height;
        const auto source = std::span<const unsigned char>(source_mip.bytes.data(), static_cast<std::size_t>(required_bytes));
        switch (texture.format)
        {
        case SourceTextureFormat::Bc1:
            decoded_mip.bytes = decode_bc1(source, source_mip.width, source_mip.height);
            break;
        case SourceTextureFormat::Bc2:
            decoded_mip.bytes = decode_bc2(source, source_mip.width, source_mip.height);
            break;
        case SourceTextureFormat::Bc3:
            decoded_mip.bytes = decode_bc3(source, source_mip.width, source_mip.height);
            break;
        case SourceTextureFormat::Bc4:
            decoded_mip.bytes = decode_bc4(source, source_mip.width, source_mip.height);
            break;
        case SourceTextureFormat::Bc5:
            decoded_mip.bytes = decode_bc5(source, source_mip.width, source_mip.height);
            break;
        case SourceTextureFormat::Rgba8:
            decoded_mip.bytes = source_mip.bytes;
            break;
        }
        converted.mips.push_back(std::move(decoded_mip));
    }

    return converted;
}
}
