#include "openstrike/network/network_stream.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace openstrike
{
void NetworkByteWriter::write_u8(std::uint8_t value)
{
    bytes_.push_back(value);
}

void NetworkByteWriter::write_u16(std::uint16_t value)
{
    bytes_.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes_.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

void NetworkByteWriter::write_u32(std::uint32_t value)
{
    write_u16(static_cast<std::uint16_t>(value & 0xFFFFU));
    write_u16(static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU));
}

void NetworkByteWriter::write_u64(std::uint64_t value)
{
    write_u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
    write_u32(static_cast<std::uint32_t>((value >> 32ULL) & 0xFFFFFFFFULL));
}

bool NetworkByteWriter::write_string(std::string_view value)
{
    if (value.size() > std::numeric_limits<std::uint16_t>::max())
    {
        return false;
    }

    write_u16(static_cast<std::uint16_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
}

void NetworkByteWriter::write_bytes(std::span<const unsigned char> bytes)
{
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

const std::vector<unsigned char>& NetworkByteWriter::bytes() const
{
    return bytes_;
}

std::vector<unsigned char> NetworkByteWriter::take_bytes()
{
    return std::move(bytes_);
}

NetworkByteReader::NetworkByteReader(std::span<const unsigned char> bytes)
    : bytes_(bytes)
{
}

bool NetworkByteReader::read_u8(std::uint8_t& value)
{
    if (cursor_ + 1 > bytes_.size())
    {
        failed_ = true;
        return false;
    }

    value = bytes_[cursor_++];
    return true;
}

bool NetworkByteReader::read_u16(std::uint16_t& value)
{
    std::uint8_t lo = 0;
    std::uint8_t hi = 0;
    if (!read_u8(lo) || !read_u8(hi))
    {
        return false;
    }

    value = static_cast<std::uint16_t>(lo) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(hi) << 8U);
    return true;
}

bool NetworkByteReader::read_u32(std::uint32_t& value)
{
    std::uint16_t lo = 0;
    std::uint16_t hi = 0;
    if (!read_u16(lo) || !read_u16(hi))
    {
        return false;
    }

    value = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16U);
    return true;
}

bool NetworkByteReader::read_u64(std::uint64_t& value)
{
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (!read_u32(lo) || !read_u32(hi))
    {
        return false;
    }

    value = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32ULL);
    return true;
}

bool NetworkByteReader::read_string(std::string& value)
{
    std::uint16_t size = 0;
    if (!read_u16(size) || cursor_ + size > bytes_.size())
    {
        failed_ = true;
        return false;
    }

    value.assign(reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
    cursor_ += size;
    return true;
}

bool NetworkByteReader::read_bytes(std::span<unsigned char> output)
{
    if (cursor_ + output.size() > bytes_.size())
    {
        failed_ = true;
        return false;
    }

    std::copy(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_),
        bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_ + output.size()),
        output.begin());
    cursor_ += output.size();
    return true;
}

std::span<const unsigned char> NetworkByteReader::remaining_bytes() const
{
    return bytes_.subspan(cursor_);
}

bool NetworkByteReader::empty() const
{
    return cursor_ == bytes_.size();
}

bool NetworkByteReader::failed() const
{
    return failed_;
}
}
