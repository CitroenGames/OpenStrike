#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
class NetworkByteWriter
{
public:
    void write_u8(std::uint8_t value);
    void write_u16(std::uint16_t value);
    void write_u32(std::uint32_t value);
    void write_u64(std::uint64_t value);
    bool write_string(std::string_view value);
    void write_bytes(std::span<const unsigned char> bytes);

    [[nodiscard]] const std::vector<unsigned char>& bytes() const;
    [[nodiscard]] std::vector<unsigned char> take_bytes();

private:
    std::vector<unsigned char> bytes_;
};

class NetworkByteReader
{
public:
    explicit NetworkByteReader(std::span<const unsigned char> bytes);

    [[nodiscard]] bool read_u8(std::uint8_t& value);
    [[nodiscard]] bool read_u16(std::uint16_t& value);
    [[nodiscard]] bool read_u32(std::uint32_t& value);
    [[nodiscard]] bool read_u64(std::uint64_t& value);
    [[nodiscard]] bool read_string(std::string& value);
    [[nodiscard]] bool read_bytes(std::span<unsigned char> output);
    [[nodiscard]] std::span<const unsigned char> remaining_bytes() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] bool failed() const;

private:
    std::span<const unsigned char> bytes_;
    std::size_t cursor_ = 0;
    bool failed_ = false;
};
}
