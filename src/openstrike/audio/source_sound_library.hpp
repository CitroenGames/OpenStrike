#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace openstrike
{
class SourceAssetStore;

struct SourceSoundEntry
{
    std::string name;
    std::string wave;
    int channel = 0;
    int sound_level = 75;
    int pitch = 100;
    float volume = 1.0f;
    bool loop = false;
};

class SourceSoundLibrary
{
public:
    explicit SourceSoundLibrary(const SourceAssetStore& assets);

    const SourceSoundEntry* find(std::string_view name) const;
    [[nodiscard]] std::size_t entry_count() const { return entries_.size(); }

private:
    void load_manifest(const SourceAssetStore& assets);
    void load_script(const SourceAssetStore& assets, std::string_view path);

    std::unordered_map<std::string, SourceSoundEntry> entries_;
};
}
