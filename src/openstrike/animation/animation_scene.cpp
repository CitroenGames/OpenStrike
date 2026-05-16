#include "openstrike/animation/animation_scene.hpp"

#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_paths.hpp"

#include <algorithm>

namespace openstrike
{
namespace
{
std::string normalized_model_key(std::filesystem::path path)
{
    std::string key = path.generic_string();
    std::replace(key.begin(), key.end(), '\\', '/');
    key = source_lower_copy(key);
    if (key.size() < 4 || key.substr(key.size() - 4) != ".mdl")
    {
        key += ".mdl";
    }
    return key;
}
}

const StudioModel* AnimationModelCache::load(const SourceAssetStore& assets, const std::filesystem::path& model_path)
{
    const std::string key = normalized_model_key(model_path);
    auto it = models_.find(key);
    if (it == models_.end())
    {
        it = models_.emplace(key, load_source_studio_model(assets, key)).first;
    }

    return it->second ? &*it->second : nullptr;
}

const StudioModel* AnimationModelCache::find(std::string_view model_path) const
{
    const std::string key = normalized_model_key(std::filesystem::path(std::string(model_path)));
    const auto it = models_.find(key);
    return it != models_.end() && it->second ? &*it->second : nullptr;
}

void AnimationModelCache::clear()
{
    models_.clear();
}

AnimationEntity& AnimationScene::upsert_entity(std::uint32_t entity_id, std::string_view model_path, const SourceAssetStore& assets)
{
    AnimationEntity& entity = entities_[entity_id];
    const std::string key = normalized_model_key(std::filesystem::path(std::string(model_path)));
    if (entity.entity_id != entity_id || entity.model_path != key)
    {
        entity = {};
        entity.entity_id = entity_id;
        entity.model_path = key;
        if (const StudioModel* model = model_cache_.load(assets, key))
        {
            reset_animation_state(entity.playback, *model, 0);
            entity.pose = evaluate_studio_pose(*model, entity.playback);
            entity.model_loaded = true;
        }
    }
    return entity;
}

void AnimationScene::remove_entity(std::uint32_t entity_id)
{
    entities_.erase(entity_id);
}

void AnimationScene::clear()
{
    entities_.clear();
    model_cache_.clear();
}

void AnimationScene::advance(const SourceAssetStore& assets, float delta_seconds)
{
    for (auto& [entity_id, entity] : entities_)
    {
        (void)entity_id;
        const StudioModel* model = model_cache_.load(assets, entity.model_path);
        entity.model_loaded = model != nullptr;
        if (model == nullptr)
        {
            continue;
        }

        advance_animation_state(entity.playback, *model, delta_seconds);
        entity.pose = evaluate_studio_pose(*model, entity.playback);
    }
}

AnimationEntity* AnimationScene::find_entity(std::uint32_t entity_id)
{
    const auto it = entities_.find(entity_id);
    return it != entities_.end() ? &it->second : nullptr;
}

const AnimationEntity* AnimationScene::find_entity(std::uint32_t entity_id) const
{
    const auto it = entities_.find(entity_id);
    return it != entities_.end() ? &it->second : nullptr;
}

const StudioModel* AnimationScene::find_model(std::string_view model_path) const
{
    return model_cache_.find(model_path);
}

const std::unordered_map<std::uint32_t, AnimationEntity>& AnimationScene::entities() const
{
    return entities_;
}
}
