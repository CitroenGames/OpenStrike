#include "openstrike/source/source_asset_store.hpp"

#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/source/source_paths.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kVpkSignature = 0x55AA1234U;
constexpr std::uint16_t kVpkDirArchiveIndex = 0x7FFFU;

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
    {
        throw std::runtime_error("unexpected end of VPK");
    }

    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint16_t read_u16_le(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size())
    {
        throw std::runtime_error("unexpected end of VPK");
    }

    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::vector<unsigned char> read_binary_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0)
    {
        throw std::runtime_error("failed to query file size: " + path.string());
    }

    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
    {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    if (!file && !file.eof())
    {
        throw std::runtime_error("failed to read file: " + path.string());
    }

    return bytes;
}

std::string read_zero_string(const std::vector<unsigned char>& bytes, std::size_t& cursor, std::size_t limit)
{
    if (cursor > limit)
    {
        throw std::runtime_error("VPK directory cursor is outside the tree");
    }

    const std::size_t begin = cursor;
    while (cursor < limit && bytes[cursor] != 0)
    {
        ++cursor;
    }

    if (cursor >= limit)
    {
        throw std::runtime_error("unterminated VPK directory string");
    }

    std::string text(reinterpret_cast<const char*>(bytes.data() + begin), cursor - begin);
    ++cursor;
    return text;
}

std::string vpk_entry_path(std::string_view directory, std::string_view filename, std::string_view extension)
{
    std::string result;
    if (!directory.empty() && directory != " ")
    {
        result += directory;
        result += '/';
    }
    result += filename;
    if (!extension.empty())
    {
        result += '.';
        result += extension;
    }
    source_normalize_slashes(result);
    return source_lower_copy(result);
}

}

SourceAssetStore::SourceAssetStore(
    const ContentFileSystem& filesystem,
    const std::unordered_map<std::string, std::vector<unsigned char>>* embedded_assets)
    : filesystem_(filesystem)
    , embedded_assets_(embedded_assets)
{
    mount_vpks(filesystem);
}

std::optional<std::vector<unsigned char>> SourceAssetStore::read_binary(const std::filesystem::path& relative_path, std::string_view path_id) const
{
    const std::string normalized = normalize_source_asset_path(relative_path);
    if (normalized.empty())
    {
        return std::nullopt;
    }

    if (embedded_assets_ != nullptr)
    {
        if (const auto embedded = embedded_assets_->find(normalized); embedded != embedded_assets_->end())
        {
            return embedded->second;
        }
    }

    if (const std::optional<std::filesystem::path> loose = filesystem_.resolve(normalized, path_id))
    {
        try
        {
            return read_binary_file(*loose);
        }
        catch (const std::exception& error)
        {
            log_warning("failed to read loose Source asset '{}': {}", loose->string(), error.what());
        }
    }

    const auto it = vpk_entries_.find(normalized);
    if (it == vpk_entries_.end())
    {
        return std::nullopt;
    }

    return read_vpk_entry(it->second);
}

std::optional<std::string> SourceAssetStore::read_text(const std::filesystem::path& relative_path, std::string_view path_id) const
{
    const std::optional<std::vector<unsigned char>> bytes = read_binary(relative_path, path_id);
    if (!bytes)
    {
        return std::nullopt;
    }

    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

void SourceAssetStore::mount_vpks(const ContentFileSystem& filesystem)
{
    for (const SearchPath& path : filesystem.search_paths("GAME"))
    {
        std::error_code error;
        if (!std::filesystem::is_directory(path.root, error))
        {
            continue;
        }

        std::vector<std::filesystem::path> candidates;
        for (std::filesystem::directory_iterator it(path.root, error), end; it != end && !error; it.increment(error))
        {
            if (!it->is_regular_file(error))
            {
                continue;
            }

            const std::string filename = source_lower_copy(it->path().filename().string());
            if (filename.size() >= 8 && filename.ends_with("_dir.vpk"))
            {
                candidates.push_back(it->path());
            }
        }

        std::sort(candidates.begin(), candidates.end());
        for (const std::filesystem::path& candidate : candidates)
        {
            mount_vpk_directory(candidate);
        }
    }
}

void SourceAssetStore::mount_vpk_directory(const std::filesystem::path& dir_path)
{
    std::vector<unsigned char> bytes;
    try
    {
        bytes = read_binary_file(dir_path);
    }
    catch (const std::exception& error)
    {
        log_warning("failed to read VPK directory '{}': {}", dir_path.string(), error.what());
        return;
    }

    try
    {
        if (bytes.size() < 12 || read_u32_le(bytes, 0) != kVpkSignature)
        {
            return;
        }

        const std::uint32_t version = read_u32_le(bytes, 4);
        std::size_t header_size = 0;
        std::uint32_t tree_size = 0;
        if (version == 1)
        {
            header_size = 12;
            tree_size = read_u32_le(bytes, 8);
        }
        else if (version == 2)
        {
            header_size = 28;
            tree_size = read_u32_le(bytes, 8);
        }
        else
        {
            log_warning("unsupported VPK version {} in '{}'", version, dir_path.string());
            return;
        }

        const std::size_t tree_begin = header_size;
        const std::size_t tree_end = tree_begin + tree_size;
        if (tree_end > bytes.size() || tree_end < tree_begin)
        {
            log_warning("VPK directory tree is invalid in '{}'", dir_path.string());
            return;
        }

        std::filesystem::path archive_prefix = dir_path.parent_path() / dir_path.stem();
        std::string prefix = archive_prefix.string();
        const std::string suffix = "_dir";
        if (prefix.size() >= suffix.size() && source_lower_copy(prefix.substr(prefix.size() - suffix.size())) == suffix)
        {
            prefix.resize(prefix.size() - suffix.size());
            archive_prefix = prefix;
        }

        std::size_t cursor = tree_begin;
        while (cursor < tree_end)
        {
            const std::string extension = read_zero_string(bytes, cursor, tree_end);
            if (extension.empty())
            {
                break;
            }

            while (cursor < tree_end)
            {
                const std::string directory = read_zero_string(bytes, cursor, tree_end);
                if (directory.empty())
                {
                    break;
                }

                while (cursor < tree_end)
                {
                    const std::string filename = read_zero_string(bytes, cursor, tree_end);
                    if (filename.empty())
                    {
                        break;
                    }

                    if (cursor + 18 > tree_end)
                    {
                        throw std::runtime_error("VPK file record is truncated");
                    }

                    cursor += 4; // crc
                    const std::uint16_t preload_size = read_u16_le(bytes, cursor);
                    cursor += 2;
                    const std::uint16_t archive_index = read_u16_le(bytes, cursor);
                    cursor += 2;
                    const std::uint32_t entry_offset = read_u32_le(bytes, cursor);
                    cursor += 4;
                    const std::uint32_t entry_size = read_u32_le(bytes, cursor);
                    cursor += 4;
                    const std::uint16_t terminator = read_u16_le(bytes, cursor);
                    cursor += 2;
                    if (terminator != 0xFFFFU)
                    {
                        throw std::runtime_error("VPK file record terminator is invalid");
                    }

                    if (cursor + preload_size > tree_end)
                    {
                        throw std::runtime_error("VPK preload data is truncated");
                    }

                    VpkEntry entry;
                    entry.dir_path = dir_path;
                    entry.archive_prefix = archive_prefix;
                    entry.dir_data_offset = tree_end;
                    entry.archive_index = archive_index;
                    entry.offset = entry_offset;
                    entry.size = entry_size;
                    entry.preload.assign(bytes.begin() + cursor, bytes.begin() + cursor + preload_size);
                    cursor += preload_size;

                    vpk_entries_.try_emplace(vpk_entry_path(directory, filename, extension), std::move(entry));
                }
            }
        }

        log_info("mounted Source VPK '{}' entries={}", dir_path.string(), vpk_entries_.size());
    }
    catch (const std::exception& error)
    {
        log_warning("failed to parse VPK directory '{}': {}", dir_path.string(), error.what());
    }
}

std::optional<std::vector<unsigned char>> SourceAssetStore::read_vpk_entry(const VpkEntry& entry) const
{
    std::vector<unsigned char> bytes = entry.preload;
    if (entry.size == 0)
    {
        return bytes;
    }

    std::filesystem::path archive_path = entry.dir_path;
    if (entry.archive_index != kVpkDirArchiveIndex)
    {
        const std::string filename = entry.archive_prefix.filename().string() + "_" + std::format("{:03}", entry.archive_index) + ".vpk";
        archive_path = entry.archive_prefix.parent_path() / filename;
    }
    std::ifstream file(archive_path, std::ios::binary);
    if (!file)
    {
        log_warning("failed to open VPK archive '{}'", archive_path.string());
        return std::nullopt;
    }

    const std::uint64_t file_offset = (entry.archive_index == kVpkDirArchiveIndex ? entry.dir_data_offset : 0) + entry.offset;
    file.seekg(static_cast<std::streamoff>(file_offset), std::ios::beg);
    const std::size_t previous_size = bytes.size();
    bytes.resize(previous_size + entry.size);
    file.read(reinterpret_cast<char*>(bytes.data() + previous_size), static_cast<std::streamsize>(entry.size));
    if (!file)
    {
        log_warning("failed to read VPK asset from '{}'", archive_path.string());
        return std::nullopt;
    }

    return bytes;
}
}
