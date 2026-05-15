#include "openstrike/ui/main_menu_controller.hpp"

#include "openstrike/core/log.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <utility>

namespace openstrike
{
MainMenuController::MainMenuController() = default;

MainMenuController::~MainMenuController()
{
    detach();
}

void MainMenuController::attach(Rml::ElementDocument& document)
{
    detach();
    document_ = &document;
    should_quit_ = false;
    prime_only_ = true;
    settings_visible_ = false;
    play_page_visible_ = false;
    play_mode_ = "matchmaking";
    active_play_tab_ = "competitive";
    selected_map_.clear();

    attach_buttons();
    show_home_page();
    set_play_mode(play_mode_);
    set_play_tab(active_play_tab_);
    update_go_button_state();
}

void MainMenuController::detach()
{
    if (document_ == nullptr)
    {
        return;
    }

    Rml::ElementList buttons;
    document_->GetElementsByClassName(buttons, "menu-btn");
    for (Rml::Element* button : buttons)
    {
        if (button != nullptr)
        {
            button->RemoveEventListener("click", this);
        }
    }

    document_ = nullptr;
}

bool MainMenuController::should_quit() const
{
    return should_quit_;
}

void MainMenuController::set_open_console_callback(std::function<void()> callback)
{
    open_console_callback_ = std::move(callback);
}

void MainMenuController::ProcessEvent(Rml::Event& event)
{
    Rml::Element* element = event.GetCurrentElement();
    if (element == nullptr)
    {
        return;
    }

    const Rml::String command = element->GetAttribute<Rml::String>("data-command", "");
    if (command.empty())
    {
        return;
    }

    log_info("RmlUi menu command: {}", command);

    if (element->IsClassSet("nav-btn") && command != "ShowPlayPage" && command != "ShowHomePage" && command != "OpenOptionsDialog")
    {
        show_home_page();
    }

    if (command == "ShowPlayPage")
    {
        show_play_page(true);
    }
    else if (command == "ShowHomePage")
    {
        show_home_page();
    }
    else if (command == "PlayMode")
    {
        const Rml::String mode = element->GetAttribute<Rml::String>("data-tab", "");
        if (!mode.empty())
        {
            set_play_mode(mode);
        }
    }
    else if (command == "PlayTab")
    {
        const Rml::String tab = element->GetAttribute<Rml::String>("data-tab", "");
        if (!tab.empty())
        {
            set_play_tab(tab);
        }
    }
    else if (command == "ToggleMap")
    {
        const Rml::String map_name = element->GetAttribute<Rml::String>("data-map", "");
        if (!map_name.empty())
        {
            toggle_map_selection(map_name);
        }
    }
    else if (command == "TogglePrime")
    {
        toggle_prime();
    }
    else if (command == "PlayGo")
    {
        execute_go();
    }
    else if (command == "OpenOptionsDialog" || command == "OpenSettings")
    {
        show_settings(true);
    }
    else if (command == "CloseSettings")
    {
        show_settings(false);
    }
    else if (command == "SettingsTab")
    {
        const Rml::String tab = element->GetAttribute<Rml::String>("data-tab", "");
        if (!tab.empty())
        {
            update_class_for_elements("settings-tab", "settings-tab-active", "data-tab", tab);
        }
    }
    else if (command == "SettingsReset")
    {
        reset_settings();
    }
    else if (command == "OpenServerBrowser")
    {
        show_home_page();
        set_status("Server browser is not wired yet.", true);
    }
    else if (command == "OpenInventory")
    {
        show_home_page();
        set_status("Inventory is not wired yet.", true);
    }
    else if (command == "OpenConsole")
    {
        if (open_console_callback_)
        {
            open_console_callback_();
        }
    }
    else if (command == "QuitNoConfirm")
    {
        should_quit_ = true;
    }
}

void MainMenuController::attach_buttons()
{
    if (document_ == nullptr)
    {
        return;
    }

    Rml::ElementList buttons;
    document_->GetElementsByClassName(buttons, "menu-btn");
    for (Rml::Element* button : buttons)
    {
        if (button != nullptr)
        {
            button->AddEventListener("click", this);
        }
    }

    log_info("attached {} RmlUi main menu button(s)", buttons.size());
}

void MainMenuController::show_home_page()
{
    settings_visible_ = false;
    play_page_visible_ = false;
    set_display("content-panel", "flex");
    set_display("content-fade", "block");
    set_display("play-page", "none");
    set_display("settings-page", "none");
    update_nav_active(nullptr);
}

void MainMenuController::show_play_page(bool show)
{
    if (!show)
    {
        show_home_page();
        return;
    }

    settings_visible_ = false;
    play_page_visible_ = true;
    set_display("content-panel", "none");
    set_display("content-fade", "none");
    set_display("settings-page", "none");
    set_display("play-page", "flex");
    update_nav_active("nav-play");
    set_play_mode(play_mode_);
    update_go_button_state();
}

void MainMenuController::show_settings(bool show)
{
    if (!show)
    {
        show_home_page();
        return;
    }

    settings_visible_ = true;
    play_page_visible_ = false;
    set_display("content-panel", "none");
    set_display("content-fade", "none");
    set_display("play-page", "none");
    set_display("settings-page", "flex");
    update_nav_active("nav-settings");
}

void MainMenuController::set_play_mode(const Rml::String& mode)
{
    play_mode_ = mode;
    const bool bots = mode == "bots";
    set_class("mode-matchmaking", "play-mode-tab-active", !bots);
    set_class("mode-bots", "play-mode-tab-active", bots);
    set_display("play-matchmaking-content", bots ? "none" : "flex");
    set_display("play-bots-content", bots ? "flex" : "none");
    update_go_button_state();
}

void MainMenuController::set_play_tab(const Rml::String& tab)
{
    active_play_tab_ = tab;
    update_class_for_elements("play-tab", "play-tab-active", "data-tab", tab);
}

void MainMenuController::toggle_map_selection(const Rml::String& map_name)
{
    const bool already_selected = selected_map_ == map_name;

    Rml::ElementList cards;
    if (document_ != nullptr)
    {
        document_->GetElementsByClassName(cards, "map-card");
    }

    for (Rml::Element* card : cards)
    {
        if (card != nullptr)
        {
            card->SetClass("map-selected", false);
        }
    }

    selected_map_.clear();
    if (!already_selected)
    {
        selected_map_ = map_name;
        set_class(("map-" + map_name).c_str(), "map-selected", true);
        set_class(("bots-map-" + map_name).c_str(), "map-selected", true);
    }

    update_go_button_state();
    set_status("", false);
}

void MainMenuController::execute_go()
{
    if (selected_map_.empty())
    {
        set_status("Select a map first.", true);
        return;
    }

    const std::string mode = play_mode_ == "bots" ? "Bot game" : "Matchmaking";
    set_status(mode + " launch for " + selected_map_ + " is not wired yet.", true);
    log_info("{} requested for map '{}'", mode, selected_map_);
}

void MainMenuController::toggle_prime()
{
    prime_only_ = !prime_only_;
    set_class("play-prime-check", "checked", prime_only_);
}

void MainMenuController::reset_settings()
{
    if (Rml::Element* reset = element("settings-reset-btn"))
    {
        reset->SetInnerRML("Reset complete");
    }
}

void MainMenuController::set_status(const std::string& text, bool visible)
{
    Rml::Element* status = element("mm-status");
    if (status == nullptr)
    {
        return;
    }

    status->SetProperty("display", visible ? "block" : "none");
    status->SetInnerRML(text);
}

void MainMenuController::update_go_button_state()
{
    const bool disabled = selected_map_.empty();
    set_class("play-go-btn", "go-disabled", disabled);
    set_class("bots-go-btn", "go-disabled", disabled);
}

void MainMenuController::update_nav_active(const char* active_id)
{
    if (document_ == nullptr)
    {
        return;
    }

    Rml::ElementList nav_buttons;
    document_->GetElementsByClassName(nav_buttons, "nav-btn");
    for (Rml::Element* button : nav_buttons)
    {
        if (button != nullptr)
        {
            button->SetClass("active", false);
        }
    }

    if (active_id != nullptr)
    {
        set_class(active_id, "active", true);
    }
}

void MainMenuController::update_class_for_elements(
    const char* class_name, const char* active_class, const char* attribute_name, const Rml::String& active_value)
{
    if (document_ == nullptr)
    {
        return;
    }

    Rml::ElementList elements;
    document_->GetElementsByClassName(elements, class_name);
    for (Rml::Element* item : elements)
    {
        if (item == nullptr)
        {
            continue;
        }

        const Rml::String value = item->GetAttribute<Rml::String>(attribute_name, "");
        item->SetClass(active_class, value == active_value);
    }
}

void MainMenuController::set_display(const char* id, const char* value)
{
    if (Rml::Element* item = element(id))
    {
        item->SetProperty("display", value);
    }
}

void MainMenuController::set_class(const char* id, const char* class_name, bool enabled)
{
    if (Rml::Element* item = element(id))
    {
        item->SetClass(class_name, enabled);
    }
}

Rml::Element* MainMenuController::element(const char* id) const
{
    if (document_ == nullptr)
    {
        return nullptr;
    }

    return document_->GetElementById(id);
}
}
