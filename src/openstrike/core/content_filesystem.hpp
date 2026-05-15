#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
enum class SearchPathPosition
{
    Head,
    Tail
};

struct SearchPath
{
    std::filesystem::path root;
    std::string path_id;
};

class ContentFileSystem
{
public:
    void clear();
    void add_search_path(std::filesystem::path root, std::string path_id = "GAME", SearchPathPosition position = SearchPathPosition::Tail);
    bool remove_search_path(const std::filesystem::path& root, std::string_view path_id = {});

    [[nodiscard]] std::optional<std::filesystem::path> resolve(
        const std::filesystem::path& relative_path, std::string_view path_id = {}) const;
    [[nodiscard]] bool exists(const std::filesystem::path& relative_path, std::string_view path_id = {}) const;
    [[nodiscard]] std::string read_text(const std::filesystem::path& relative_path, std::string_view path_id = {}) const;
    [[nodiscard]] std::vector<SearchPath> search_paths(std::string_view path_id = {}) const;
    [[nodiscard]] std::string search_path_string(std::string_view path_id = {}) const;

private:
    std::vector<SearchPath> search_paths_;
};
}
