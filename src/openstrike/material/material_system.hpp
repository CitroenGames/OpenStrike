#pragma once

#include "openstrike/material/material.hpp"

#include <optional>
#include <string_view>

namespace openstrike
{
class SourceAssetStore;

class MaterialSystem
{
public:
    explicit MaterialSystem(const SourceAssetStore& assets);

    [[nodiscard]] LoadedMaterial load_world_material(std::string_view material_name) const;
    [[nodiscard]] std::optional<MaterialDefinition> resolve_source_material(std::string_view material_name) const;
    [[nodiscard]] SourceTexture fallback_texture(std::string_view material_name) const;

private:
    const SourceAssetStore& assets_;
};
}
