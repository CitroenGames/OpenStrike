#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace openstrike
{
struct LoadingScreenSnapshot
{
    bool visible = false;
    bool auto_close = false;
    float progress = 0.0F;
    std::uint64_t revision = 0;
    std::string map_name;
    std::string game_mode;
    std::string description;
    std::string status;
    std::string tip;
};

class LoadingScreenState
{
public:
    void open_for_map(std::string_view map_name);
    void open_for_map(std::string_view map_name, std::string_view game_mode, std::string_view description, std::string_view tip);
    void set_progress(float fraction, std::string_view status);
    void set_status(std::string_view status);
    void complete(std::string_view status);
    void close();

    [[nodiscard]] bool visible() const;
    [[nodiscard]] const LoadingScreenSnapshot& snapshot() const;

private:
    void bump_revision();

    LoadingScreenSnapshot snapshot_;
};

[[nodiscard]] std::string loading_screen_map_name(std::string_view map_name);
[[nodiscard]] std::string_view default_loading_description();
[[nodiscard]] std::string_view default_loading_tip();
}
