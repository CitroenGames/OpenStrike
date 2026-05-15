#include "openstrike/audio/source_sound_library.hpp"

#include "openstrike/audio/audio_system.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/source/source_asset_store.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace openstrike
{
namespace
{
std::string to_lower(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

std::string normalize_script_path(std::string_view path)
{
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front())))
    {
        out.erase(out.begin());
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
    {
        out.pop_back();
    }
    return out;
}

std::optional<float> parse_float(std::string_view value)
{
    std::string text(value);
    char* end = nullptr;
    const float result = std::strtof(text.c_str(), &end);
    if (end == text.c_str())
    {
        return std::nullopt;
    }
    return result;
}

std::optional<int> parse_int(std::string_view value)
{
    int result = 0;
    const char* first = value.data();
    const char* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, result);
    if (ec == std::errc())
    {
        return result;
    }

    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (std::isdigit(static_cast<unsigned char>(value[i])))
        {
            std::size_t end = i;
            while (end < value.size() && std::isdigit(static_cast<unsigned char>(value[end])))
            {
                ++end;
            }
            return parse_int(value.substr(i, end - i));
        }
    }

    return std::nullopt;
}

int parse_channel(std::string_view value)
{
    const std::string key = to_lower(value);
    if (key == "chan_static")
    {
        return ChanStatic;
    }
    if (key == "chan_stream")
    {
        return ChanStream;
    }
    if (key == "chan_music")
    {
        return ChanStatic;
    }
    if (key == "chan_voice")
    {
        return ChanVoice;
    }
    if (key == "chan_weapon")
    {
        return ChanWeapon;
    }
    if (key == "chan_item")
    {
        return ChanItem;
    }
    if (key == "chan_body")
    {
        return ChanBody;
    }
    if (const std::optional<int> parsed = parse_int(value))
    {
        return *parsed;
    }
    return ChanAuto;
}

int parse_sound_level(std::string_view value)
{
    const std::string key = to_lower(value);
    if (key == "sndlvl_none")
    {
        return 0;
    }
    if (key == "sndlvl_idle")
    {
        return 60;
    }
    if (key == "sndlvl_static")
    {
        return 66;
    }
    if (key == "sndlvl_norm")
    {
        return 75;
    }
    if (key == "sndlvl_talking")
    {
        return 80;
    }
    if (const std::optional<int> parsed = parse_int(value))
    {
        return *parsed;
    }
    return 75;
}

int parse_pitch(std::string_view value)
{
    const std::string key = to_lower(value);
    if (key == "pitch_norm")
    {
        return 100;
    }
    if (key == "pitch_low")
    {
        return 95;
    }
    if (key == "pitch_high")
    {
        return 120;
    }
    if (const std::optional<int> parsed = parse_int(value))
    {
        return *parsed;
    }
    return 100;
}

std::vector<std::string> tokenize_keyvalues(std::string_view text)
{
    std::vector<std::string> tokens;
    std::string token;
    bool in_quote = false;

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (!in_quote && ch == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n')
            {
                ++i;
            }
            continue;
        }

        if (in_quote)
        {
            if (ch == '\\' && i + 1 < text.size())
            {
                token.push_back(text[++i]);
                continue;
            }
            if (ch == '"')
            {
                tokens.push_back(token);
                token.clear();
                in_quote = false;
                continue;
            }
            token.push_back(ch);
            continue;
        }

        if (ch == '"')
        {
            if (!token.empty())
            {
                tokens.push_back(token);
                token.clear();
            }
            in_quote = true;
            continue;
        }

        if (ch == '{' || ch == '}')
        {
            if (!token.empty())
            {
                tokens.push_back(token);
                token.clear();
            }
            tokens.emplace_back(1, ch);
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            if (!token.empty())
            {
                tokens.push_back(token);
                token.clear();
            }
            continue;
        }

        token.push_back(ch);
    }

    if (!token.empty())
    {
        tokens.push_back(token);
    }

    return tokens;
}

bool token_is_block_marker(std::string_view token)
{
    return token == "{" || token == "}";
}

void parse_sound_script(std::string_view script, std::unordered_map<std::string, SourceSoundEntry>& entries)
{
    const std::vector<std::string> tokens = tokenize_keyvalues(script);
    std::size_t i = 0;
    while (i + 1 < tokens.size())
    {
        if (token_is_block_marker(tokens[i]) || tokens[i + 1] != "{")
        {
            ++i;
            continue;
        }

        SourceSoundEntry entry;
        entry.name = tokens[i];
        bool has_wave = false;
        i += 2;
        int depth = 1;

        while (i < tokens.size() && depth > 0)
        {
            const std::string& token = tokens[i];
            if (token == "{")
            {
                ++depth;
                ++i;
                continue;
            }
            if (token == "}")
            {
                --depth;
                ++i;
                continue;
            }

            if (depth == 1 && i + 1 < tokens.size() && !token_is_block_marker(tokens[i + 1]))
            {
                const std::string key = to_lower(token);
                const std::string& value = tokens[i + 1];
                if (key == "wave")
                {
                    entry.wave = value;
                    has_wave = true;
                }
                else if (key == "channel")
                {
                    entry.channel = parse_channel(value);
                }
                else if (key == "soundlevel")
                {
                    entry.sound_level = parse_sound_level(value);
                }
                else if (key == "pitch")
                {
                    entry.pitch = parse_pitch(value);
                }
                else if (key == "volume")
                {
                    if (const std::optional<float> parsed = parse_float(value))
                    {
                        entry.volume = *parsed;
                    }
                }
                else if (key == "loop")
                {
                    const std::string loop_value = to_lower(value);
                    entry.loop = loop_value == "1" || loop_value == "true" || loop_value == "yes";
                }

                i += 2;
                continue;
            }

            ++i;
        }

        if (has_wave && !entry.wave.empty())
        {
            entries[to_lower(entry.name)] = std::move(entry);
        }
    }
}

void collect_manifest_scripts(std::string_view manifest, std::vector<std::string>& scripts)
{
    const std::vector<std::string> tokens = tokenize_keyvalues(manifest);
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        const std::string key = to_lower(tokens[i]);
        if (key == "precache_file" || key == "autocache_file")
        {
            scripts.push_back(normalize_script_path(tokens[i + 1]));
            ++i;
        }
    }
}
}

SourceSoundLibrary::SourceSoundLibrary(const SourceAssetStore& assets)
{
    load_manifest(assets);
    load_script(assets, "scripts/game_sounds_music2.txt");
    load_script(assets, "sound/music/valve_csgo_01/game_sounds_music.txt");
    log_info("loaded {} Source sound script entries", entries_.size());
}

const SourceSoundEntry* SourceSoundLibrary::find(std::string_view name) const
{
    const auto it = entries_.find(to_lower(name));
    if (it == entries_.end())
    {
        return nullptr;
    }
    return &it->second;
}

void SourceSoundLibrary::load_manifest(const SourceAssetStore& assets)
{
    std::vector<std::string> scripts;
    if (const auto manifest = assets.read_text("scripts/game_sounds_manifest.txt", "GAME"))
    {
        collect_manifest_scripts(*manifest, scripts);
    }

    std::unordered_set<std::string> loaded;
    for (const std::string& script : scripts)
    {
        if (script.empty() || !loaded.insert(to_lower(script)).second)
        {
            continue;
        }
        load_script(assets, script);
    }
}

void SourceSoundLibrary::load_script(const SourceAssetStore& assets, std::string_view path)
{
    const std::string normalized = normalize_script_path(path);
    if (normalized.empty())
    {
        return;
    }
    if (const auto script = assets.read_text(normalized, "GAME"))
    {
        parse_sound_script(*script, entries_);
    }
}
}
