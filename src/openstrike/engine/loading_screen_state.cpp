#include "openstrike/engine/loading_screen_state.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace openstrike
{
namespace
{
bool ends_with_case_insensitive(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size())
    {
        return false;
    }

    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (std::size_t index = 0; index < suffix.size(); ++index)
    {
        const auto a = static_cast<unsigned char>(tail[index]);
        const auto b = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(a) != std::tolower(b))
        {
            return false;
        }
    }
    return true;
}

void strip_suffix(std::string& text, std::string_view suffix)
{
    if (ends_with_case_insensitive(text, suffix))
    {
        text.resize(text.size() - suffix.size());
    }
}
}

std::string loading_screen_map_name(std::string_view map_name)
{
    std::string clean(map_name);
    std::replace(clean.begin(), clean.end(), '\\', '/');

    const std::size_t slash = clean.find_last_of('/');
    if (slash != std::string::npos)
    {
        clean.erase(0, slash + 1);
    }

    strip_suffix(clean, ".level.json");
    strip_suffix(clean, ".bsp");
    return clean;
}

std::string_view default_loading_description()
{
    return "Settings:\n- Friendly fire is OFF\n- Team collision is OFF";
}

std::string_view default_loading_tip()
{
    return "You are more accurate when you stand still. Crouching makes you the most accurate.";
}

void LoadingScreenState::open_for_map(std::string_view map_name)
{
    open_for_map(map_name, "Competitive", default_loading_description(), default_loading_tip());
}

void LoadingScreenState::open_for_map(
    std::string_view map_name, std::string_view game_mode, std::string_view description, std::string_view tip)
{
    snapshot_.visible = true;
    snapshot_.auto_close = false;
    snapshot_.progress = 0.0F;
    snapshot_.map_name = loading_screen_map_name(map_name);
    snapshot_.game_mode = std::string(game_mode);
    snapshot_.description = std::string(description);
    snapshot_.status = "Retrieving game data...";
    snapshot_.tip = std::string(tip);
    bump_revision();
}

void LoadingScreenState::set_progress(float fraction, std::string_view status)
{
    snapshot_.visible = true;
    snapshot_.auto_close = false;
    snapshot_.progress = std::clamp(fraction, 0.0F, 1.0F);
    snapshot_.status = std::string(status);
    bump_revision();
}

void LoadingScreenState::set_status(std::string_view status)
{
    snapshot_.visible = true;
    snapshot_.status = std::string(status);
    bump_revision();
}

void LoadingScreenState::complete(std::string_view status)
{
    snapshot_.visible = true;
    snapshot_.auto_close = true;
    snapshot_.progress = 1.0F;
    snapshot_.status = std::string(status);
    bump_revision();
}

void LoadingScreenState::close()
{
    if (!snapshot_.visible && !snapshot_.auto_close)
    {
        return;
    }

    snapshot_.visible = false;
    snapshot_.auto_close = false;
    bump_revision();
}

bool LoadingScreenState::visible() const
{
    return snapshot_.visible;
}

const LoadingScreenSnapshot& LoadingScreenState::snapshot() const
{
    return snapshot_;
}

void LoadingScreenState::bump_revision()
{
    ++snapshot_.revision;
}
}
