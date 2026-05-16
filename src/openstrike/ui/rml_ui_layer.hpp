#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Rml
{
class Context;
class RenderInterface;
}

class SystemInterface_SDL;
struct SDL_Window;
union SDL_Event;

namespace openstrike
{
class EngineContext;
class MainMenuController;
class RmlConsoleController;
class RmlHudController;
class RmlLoadingScreenController;
class RmlTeamMenuController;
struct RuntimeConfig;

class RmlUiLayer final
{
public:
    RmlUiLayer();
    ~RmlUiLayer();

    RmlUiLayer(const RmlUiLayer&) = delete;
    RmlUiLayer& operator=(const RmlUiLayer&) = delete;

    [[nodiscard]] bool initialize(
        SDL_Window& window,
        Rml::RenderInterface& render_interface,
        EngineContext* engine_context,
        const RuntimeConfig& config,
        std::uint32_t width,
        std::uint32_t height);
    void shutdown();

    void update(std::uint32_t width, std::uint32_t height);
    void render();

    [[nodiscard]] bool handle_hotkey_event(const SDL_Event& event);
    void dispatch_sdl_event(SDL_Window& window, SDL_Event& event);

    [[nodiscard]] bool gameplay_ui_visible() const;
    [[nodiscard]] bool wants_mouse_capture() const;
    [[nodiscard]] bool should_quit() const;

private:
    void sync_main_menu_visibility();
    bool load_fonts(const RuntimeConfig& config);
    void launch_map(std::string map_name, std::string game_mode_alias);

    std::unique_ptr<SystemInterface_SDL> system_interface_;
    std::unique_ptr<MainMenuController> main_menu_controller_;
    std::unique_ptr<RmlLoadingScreenController> loading_screen_controller_;
    std::unique_ptr<RmlHudController> hud_controller_;
    std::unique_ptr<RmlTeamMenuController> team_menu_controller_;
    std::unique_ptr<RmlConsoleController> console_controller_;
    EngineContext* engine_context_ = nullptr;
    Rml::Context* context_ = nullptr;
    std::uint64_t main_menu_world_generation_ = 0;
    bool main_menu_loading_active_ = false;
    bool initialized_ = false;
};
}
