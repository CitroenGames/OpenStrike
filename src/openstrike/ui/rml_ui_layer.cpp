#include "openstrike/ui/rml_ui_layer.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/game/game_mode.hpp"
#include "openstrike/ui/main_menu_controller.hpp"
#include "openstrike/ui/rml_console_controller.hpp"
#include "openstrike/ui/rml_hud_controller.hpp"
#include "openstrike/ui/rml_loading_screen_controller.hpp"
#include "openstrike/ui/rml_team_menu_controller.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace openstrike
{
namespace
{
std::string rml_path_string(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

bool controller_visible(const MainMenuController* controller)
{
    return controller != nullptr && controller->visible();
}

bool controller_visible(const RmlLoadingScreenController* controller)
{
    return controller != nullptr && controller->visible();
}

bool controller_visible(const RmlHudController* controller)
{
    return controller != nullptr && controller->visible();
}

bool controller_visible(const RmlTeamMenuController* controller)
{
    return controller != nullptr && controller->visible();
}

bool controller_visible(const RmlConsoleController* controller)
{
    return controller != nullptr && controller->visible();
}
}

RmlUiLayer::RmlUiLayer() = default;

RmlUiLayer::~RmlUiLayer()
{
    shutdown();
}

bool RmlUiLayer::initialize(
    SDL_Window& window,
    Rml::RenderInterface& render_interface,
    EngineContext* engine_context,
    const RuntimeConfig& config,
    std::uint32_t width,
    std::uint32_t height)
{
    shutdown();

    engine_context_ = engine_context;
    system_interface_ = std::make_unique<SystemInterface_SDL>();
    system_interface_->SetWindow(&window);

    Rml::SetSystemInterface(system_interface_.get());
    Rml::SetRenderInterface(&render_interface);

    if (!Rml::Initialise())
    {
        log_error("Rml::Initialise failed");
        shutdown();
        return false;
    }
    initialized_ = true;

    context_ = Rml::CreateContext("main", Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    if (context_ == nullptr)
    {
        log_error("Rml::CreateContext failed");
        shutdown();
        return false;
    }

    Rml::Debugger::Initialise(context_);
    Rml::Debugger::SetVisible(false);

    if (!load_fonts(config))
    {
        shutdown();
        return false;
    }

    main_menu_controller_ = std::make_unique<MainMenuController>();
    main_menu_controller_->set_open_console_callback([this] {
        if (console_controller_ != nullptr)
        {
            console_controller_->show();
        }
    });
    main_menu_controller_->set_launch_map_callback([this](std::string map_name, std::string game_mode_alias) {
        launch_map(std::move(map_name), std::move(game_mode_alias));
    });
    if (!main_menu_controller_->initialize(*context_, config))
    {
        shutdown();
        return false;
    }

    if (engine_context_ != nullptr)
    {
        loading_screen_controller_ = std::make_unique<RmlLoadingScreenController>();
        if (!loading_screen_controller_->initialize(*context_, config))
        {
            shutdown();
            return false;
        }

        hud_controller_ = std::make_unique<RmlHudController>();
        if (!hud_controller_->initialize(*context_, *engine_context_, config))
        {
            shutdown();
            return false;
        }

        team_menu_controller_ = std::make_unique<RmlTeamMenuController>();
        if (!team_menu_controller_->initialize(*context_, *engine_context_, config))
        {
            shutdown();
            return false;
        }

        console_controller_ = std::make_unique<RmlConsoleController>();
        if (!console_controller_->initialize(*context_, *engine_context_))
        {
            shutdown();
            return false;
        }
    }

    return true;
}

void RmlUiLayer::shutdown()
{
    console_controller_.reset();
    team_menu_controller_.reset();
    hud_controller_.reset();
    loading_screen_controller_.reset();
    main_menu_controller_.reset();

    if (initialized_)
    {
        Rml::Shutdown();
        initialized_ = false;
    }

    context_ = nullptr;
    engine_context_ = nullptr;
    system_interface_.reset();
    main_menu_world_generation_ = 0;
    main_menu_loading_active_ = false;
}

void RmlUiLayer::update(std::uint32_t width, std::uint32_t height)
{
    if (context_ == nullptr)
    {
        return;
    }

    context_->SetDimensions(Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));

    if (console_controller_ != nullptr)
    {
        console_controller_->update();
    }

    if (loading_screen_controller_ != nullptr && engine_context_ != nullptr)
    {
        loading_screen_controller_->update(engine_context_->loading_screen);
    }

    sync_main_menu_visibility();
    const bool main_menu_visible = controller_visible(main_menu_controller_.get());
    const bool loading_screen_visible = controller_visible(loading_screen_controller_.get());

    if (team_menu_controller_ != nullptr)
    {
        team_menu_controller_->update(main_menu_visible, loading_screen_visible);
    }

    if (hud_controller_ != nullptr)
    {
        const bool team_menu_visible = controller_visible(team_menu_controller_.get());
        hud_controller_->update(main_menu_visible || loading_screen_visible || team_menu_visible);
    }

    context_->Update();
}

void RmlUiLayer::render()
{
    if (context_ != nullptr)
    {
        context_->Render();
    }
}

bool RmlUiLayer::handle_hotkey_event(const SDL_Event& event)
{
    if (event.type != SDL_EVENT_KEY_DOWN)
    {
        return false;
    }

    if (event.key.key == SDLK_F8)
    {
        Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
        return true;
    }

    if (console_controller_ != nullptr)
    {
        if (event.key.key == SDLK_GRAVE || event.key.key == SDLK_TILDE)
        {
            console_controller_->toggle();
            return true;
        }

        if (event.key.key == SDLK_ESCAPE && console_controller_->visible())
        {
            console_controller_->hide();
            return true;
        }
    }

    if (event.key.key == SDLK_ESCAPE && main_menu_controller_ != nullptr && engine_context_ != nullptr &&
        engine_context_->world.current_world() != nullptr)
    {
        main_menu_controller_->set_visible(!main_menu_controller_->visible());
        return true;
    }

    return false;
}

void RmlUiLayer::dispatch_sdl_event(SDL_Window& window, SDL_Event& event)
{
    if (context_ != nullptr)
    {
        RmlSDL::InputEventHandler(context_, &window, event);
    }
}

bool RmlUiLayer::gameplay_ui_visible() const
{
    return controller_visible(console_controller_.get()) || controller_visible(main_menu_controller_.get()) ||
           controller_visible(loading_screen_controller_.get()) || controller_visible(team_menu_controller_.get());
}

bool RmlUiLayer::wants_mouse_capture() const
{
    return engine_context_ != nullptr && engine_context_->world.current_world() != nullptr && !gameplay_ui_visible();
}

bool RmlUiLayer::should_quit() const
{
    return main_menu_controller_ != nullptr && main_menu_controller_->should_quit();
}

void RmlUiLayer::sync_main_menu_visibility()
{
    if (engine_context_ == nullptr || main_menu_controller_ == nullptr)
    {
        return;
    }

    const std::uint64_t world_generation = engine_context_->world.generation();
    const bool loading_screen_active = engine_context_->loading_screen.visible();
    if (world_generation == main_menu_world_generation_ && loading_screen_active == main_menu_loading_active_)
    {
        return;
    }

    main_menu_world_generation_ = world_generation;
    main_menu_loading_active_ = loading_screen_active;
    if (loading_screen_active)
    {
        if (main_menu_controller_->visible())
        {
            main_menu_controller_->set_visible(false);
            log_info("hid RmlUi main menu for loading screen");
        }
    }
    else if (engine_context_->world.current_world() != nullptr)
    {
        if (main_menu_controller_->visible())
        {
            main_menu_controller_->set_visible(false);
            log_info("hid RmlUi main menu for active world");
        }
    }
    else if (!main_menu_controller_->visible())
    {
        main_menu_controller_->set_visible(true);
        log_info("showed RmlUi main menu because no world is active");
    }
}

bool RmlUiLayer::load_fonts(const RuntimeConfig& config)
{
    const std::vector<std::filesystem::path> font_roots{
        config.content_root / "csgo/resource/ui/fonts",
        config.content_root / "assets/ui/fonts",
        config.content_root / "resource/ui/fonts",
    };

    int loaded_fonts = 0;
    for (const std::filesystem::path& font_root : font_roots)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(font_root, error))
        {
            continue;
        }

        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(font_root, error))
        {
            if (error)
            {
                break;
            }

            if (!entry.is_regular_file(error))
            {
                continue;
            }

            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (extension != ".ttf" && extension != ".otf")
            {
                continue;
            }

            if (Rml::LoadFontFace(rml_path_string(entry.path()), loaded_fonts == 0))
            {
                ++loaded_fonts;
            }
        }
    }

    log_info("loaded {} RmlUi font face(s) from '{}'", loaded_fonts, rml_path_string(config.content_root));
    return true;
}

void RmlUiLayer::launch_map(std::string map_name, std::string game_mode_alias)
{
    if (engine_context_ == nullptr)
    {
        return;
    }

    if (!apply_game_mode_alias(engine_context_->variables, game_mode_alias, engine_context_->filesystem, map_name))
    {
        log_warning("menu selected unknown game mode alias '{}'", game_mode_alias);
    }

    engine_context_->loading_screen.open_for_map(
        map_name, current_game_mode_display_name(engine_context_->variables), default_loading_description(), default_loading_tip());
    engine_context_->loading_screen.set_progress(0.05F, "Retrieving game data...");
    engine_context_->command_buffer.add_text("map " + map_name);
}
}
