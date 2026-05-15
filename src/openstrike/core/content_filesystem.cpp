#include "openstrike/core/content_filesystem.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace openstrike
{
namespace
{
std::string normalize_path_id(std::string_view path_id)
{
    std::string normalized(path_id);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return normalized;
}
}

void ContentFileSystem::clear()
{
    search_paths_.clear();
}

void ContentFileSystem::add_search_path(std::filesystem::path root, std::string path_id, SearchPathPosition position)
{
    SearchPath entry{
        .root = std::move(root),
        .path_id = normalize_path_id(path_id),
    };

    if (position == SearchPathPosition::Head)
    {
        search_paths_.insert(search_paths_.begin(), std::move(entry));
    }
    else
    {
        search_paths_.push_back(std::move(entry));
    }
}

bool ContentFileSystem::remove_search_path(const std::filesystem::path& root, std::string_view path_id)
{
    const std::string normalized_id = normalize_path_id(path_id);
    const auto previous_size = search_paths_.size();
    search_paths_.erase(std::remove_if(search_paths_.begin(), search_paths_.end(), [&](const SearchPath& entry) {
                            return entry.root == root && (normalized_id.empty() || entry.path_id == normalized_id);
                        }),
        search_paths_.end());

    return search_paths_.size() != previous_size;
}

std::optional<std::filesystem::path> ContentFileSystem::resolve(const std::filesystem::path& relative_path, std::string_view path_id) const
{
    if (relative_path.empty())
    {
        return std::nullopt;
    }

    if (relative_path.is_absolute())
    {
        std::error_code error;
        if (std::filesystem::exists(relative_path, error))
        {
            return relative_path.lexically_normal();
        }
        return std::nullopt;
    }

    const std::string normalized_id = normalize_path_id(path_id);
    for (const SearchPath& entry : search_paths_)
    {
        if (!normalized_id.empty() && entry.path_id != normalized_id)
        {
            continue;
        }

        const std::filesystem::path candidate = (entry.root / relative_path).lexically_normal();
        std::error_code error;
        if (std::filesystem::exists(candidate, error))
        {
            return candidate;
        }
    }

    return std::nullopt;
}

bool ContentFileSystem::exists(const std::filesystem::path& relative_path, std::string_view path_id) const
{
    return resolve(relative_path, path_id).has_value();
}

std::string ContentFileSystem::read_text(const std::filesystem::path& relative_path, std::string_view path_id) const
{
    const std::optional<std::filesystem::path> resolved = resolve(relative_path, path_id);
    if (!resolved)
    {
        throw std::runtime_error("file not found: " + relative_path.string());
    }

    std::ifstream file(*resolved, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to open file: " + resolved->string());
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::vector<SearchPath> ContentFileSystem::search_paths(std::string_view path_id) const
{
    const std::string normalized_id = normalize_path_id(path_id);
    std::vector<SearchPath> paths;
    for (const SearchPath& entry : search_paths_)
    {
        if (normalized_id.empty() || entry.path_id == normalized_id)
        {
            paths.push_back(entry);
        }
    }
    return paths;
}

std::string ContentFileSystem::search_path_string(std::string_view path_id) const
{
    std::string result;
    for (const SearchPath& entry : search_paths(path_id))
    {
        if (!result.empty())
        {
            result += ';';
        }
        result += entry.root.string();
    }
    return result;
}
}
