#pragma once

#include "openstrike/animation/source_studio.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace openstrike
{
class SourceAssetStore;

struct AnimationEntity
{
    std::uint32_t entity_id = 0;
    std::string model_path;
    Vec3 origin;
    Vec3 angles;
    std::int32_t body = 0;
    std::int32_t skin = 0;
    std::int32_t hitbox_set = 0;
    AnimationPlaybackState playback;
    StudioPose pose;
    bool model_loaded = false;
};

class AnimationModelCache
{
public:
    [[nodiscard]] const StudioModel* load(const SourceAssetStore& assets, const std::filesystem::path& model_path);
    [[nodiscard]] const StudioModel* find(std::string_view model_path) const;
    void clear();

private:
    std::unordered_map<std::string, std::optional<StudioModel>> models_;
};

class AnimationScene
{
public:
    [[nodiscard]] AnimationEntity& upsert_entity(
        std::uint32_t entity_id,
        std::string_view model_path,
        const SourceAssetStore& assets);
    void remove_entity(std::uint32_t entity_id);
    void clear();
    void advance(const SourceAssetStore& assets, float delta_seconds);

    [[nodiscard]] AnimationEntity* find_entity(std::uint32_t entity_id);
    [[nodiscard]] const AnimationEntity* find_entity(std::uint32_t entity_id) const;
    [[nodiscard]] const StudioModel* find_model(std::string_view model_path) const;
    [[nodiscard]] const std::unordered_map<std::uint32_t, AnimationEntity>& entities() const;

private:
    AnimationModelCache model_cache_;
    std::unordered_map<std::uint32_t, AnimationEntity> entities_;
};
}
