#pragma once

#include "openstrike/core/math.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike::source_bsp
{
constexpr std::size_t kLumpCount = 64;
constexpr std::size_t kHeaderSize = 8 + (kLumpCount * 16) + 4;

struct Lump
{
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t version = 0;
};

inline std::uint16_t read_u16(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size())
    {
        throw std::runtime_error("unexpected end of BSP buffer");
    }

    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

inline std::int16_t read_s16(std::span<const unsigned char> bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(read_u16(bytes, offset));
}

inline std::uint32_t read_u32(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
    {
        throw std::runtime_error("unexpected end of BSP buffer");
    }

    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

inline std::int32_t read_s32(std::span<const unsigned char> bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(read_u32(bytes, offset));
}

inline float read_f32(std::span<const unsigned char> bytes, std::size_t offset)
{
    const std::uint32_t raw = read_u32(bytes, offset);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

inline Vec3 read_vec3(std::span<const unsigned char> bytes, std::size_t offset)
{
    return {
        read_f32(bytes, offset),
        read_f32(bytes, offset + 4),
        read_f32(bytes, offset + 8),
    };
}

inline std::uint32_t fourcc(std::string_view text)
{
    if (text.size() != 4)
    {
        return 0;
    }

    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[3])) << 24U);
}

inline Lump read_lump(std::span<const unsigned char> bytes, std::size_t lump_index)
{
    if (bytes.size() < kHeaderSize || lump_index >= kLumpCount)
    {
        throw std::runtime_error("BSP header is truncated");
    }

    const std::size_t offset = 8 + (lump_index * 16);
    return Lump{
        .offset = read_u32(bytes, offset),
        .length = read_u32(bytes, offset + 4),
        .version = read_u32(bytes, offset + 8),
    };
}

inline std::span<const unsigned char> lump_bytes(std::span<const unsigned char> bytes, const Lump& lump, std::string_view name)
{
    const std::uint64_t begin = lump.offset;
    const std::uint64_t end = begin + lump.length;
    if (end > bytes.size() || end < begin)
    {
        throw std::runtime_error("BSP " + std::string(name) + " lump points outside the file");
    }

    return bytes.subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(lump.length));
}

template <typename T, typename Reader>
std::vector<T> read_lump_array(std::span<const unsigned char> bytes, std::size_t lump_index, std::size_t stride, std::string_view name, Reader reader)
{
    const Lump lump = read_lump(bytes, lump_index);
    const std::span<const unsigned char> data = lump_bytes(bytes, lump, name);
    if (stride == 0 || (data.size() % stride) != 0)
    {
        throw std::runtime_error("BSP " + std::string(name) + " lump has an invalid size");
    }

    std::vector<T> values;
    values.reserve(data.size() / stride);
    for (std::size_t offset = 0; offset < data.size(); offset += stride)
    {
        values.push_back(reader(data, offset));
    }

    return values;
}
}
