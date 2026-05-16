#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openstrike
{
class ContentFileSystem;

class SourceAssetStore
{
public:
    explicit SourceAssetStore(const ContentFileSystem& filesystem,
        const std::unordered_map<std::string, std::vector<unsigned char>>* embedded_assets = nullptr);

    [[nodiscard]] std::optional<std::vector<unsigned char>> read_binary(
        const std::filesystem::path& relative_path, std::string_view path_id = "GAME") const;
    [[nodiscard]] std::optional<std::string> read_text(
        const std::filesystem::path& relative_path, std::string_view path_id = "GAME") const;

private:
    struct VpkEntry
    {
        std::filesystem::path dir_path;
        std::filesystem::path archive_prefix;
        std::uint64_t dir_data_offset = 0;
        std::uint16_t archive_index = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
        std::vector<unsigned char> preload;
    };

    struct VpkDirectoryMount;

    void mount_vpks(const ContentFileSystem& filesystem);
    void mount_vpk_directory(const std::filesystem::path& dir_path);
    [[nodiscard]] static std::shared_ptr<const VpkDirectoryMount> load_vpk_directory_mount(
        const std::filesystem::path& dir_path);
    [[nodiscard]] std::optional<std::vector<unsigned char>> read_vpk_entry(const VpkEntry& entry) const;

    const ContentFileSystem& filesystem_;
    const std::unordered_map<std::string, std::vector<unsigned char>>* embedded_assets_ = nullptr;
    std::vector<std::shared_ptr<const VpkDirectoryMount>> vpk_mounts_;
    std::unordered_map<std::string, const VpkEntry*> vpk_entries_;
};
}
