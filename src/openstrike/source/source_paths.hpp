#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace openstrike
{
[[nodiscard]] std::string source_lower_copy(std::string_view text);
[[nodiscard]] std::string source_trim_copy(std::string_view text);
void source_normalize_slashes(std::string& path);

[[nodiscard]] std::string normalize_source_asset_path(const std::filesystem::path& path);
[[nodiscard]] std::string normalize_source_material_name(std::string_view material_name);
[[nodiscard]] std::string normalize_source_texture_name(std::string_view texture_name);
[[nodiscard]] std::string normalize_source_material_asset_path(std::string_view material_name);
}
