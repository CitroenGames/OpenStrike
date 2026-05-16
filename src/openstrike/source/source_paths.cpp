#include "openstrike/source/source_paths.hpp"

#include <algorithm>
#include <cctype>

namespace openstrike
{
std::string source_lower_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string source_trim_copy(std::string_view text)
{
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0)
    {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0)
    {
        --last;
    }

    return std::string(text.substr(first, last - first));
}

void source_normalize_slashes(std::string& path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
}

std::string normalize_source_asset_path(const std::filesystem::path& path)
{
    std::string normalized = path.generic_string();
    source_normalize_slashes(normalized);
    normalized = source_trim_copy(normalized);
    while (!normalized.empty() && normalized.front() == '/')
    {
        normalized.erase(normalized.begin());
    }
    return source_lower_copy(normalized);
}

std::string normalize_source_material_name(std::string_view material_name)
{
    std::string normalized = source_trim_copy(material_name);
    source_normalize_slashes(normalized);
    normalized = source_lower_copy(normalized);
    if (normalized.rfind("materials/", 0) == 0)
    {
        normalized.erase(0, 10);
    }
    if (normalized.size() >= 4 && normalized.ends_with(".vmt"))
    {
        normalized.resize(normalized.size() - 4);
    }
    return normalized;
}

std::string normalize_source_texture_name(std::string_view texture_name)
{
    std::string normalized = source_trim_copy(texture_name);
    source_normalize_slashes(normalized);
    normalized = source_lower_copy(normalized);
    if (normalized.rfind("materials/", 0) == 0)
    {
        normalized.erase(0, 10);
    }
    if (normalized.size() >= 4 && normalized.ends_with(".vtf"))
    {
        normalized.resize(normalized.size() - 4);
    }
    return normalized;
}

std::string normalize_source_material_asset_path(std::string_view material_name)
{
    std::string path = normalize_source_material_name(material_name);
    if (path.empty())
    {
        return {};
    }
    return "materials/" + path + ".vmt";
}
}
