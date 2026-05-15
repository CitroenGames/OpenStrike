#pragma once

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>

namespace Rml
{
class Context;
class Element;
class ElementDocument;
}

namespace openstrike
{
struct RuntimeConfig;

class MainMenuController final : public Rml::EventListener
{
public:
    MainMenuController();
    ~MainMenuController() override;

    MainMenuController(const MainMenuController&) = delete;
    MainMenuController& operator=(const MainMenuController&) = delete;

    [[nodiscard]] bool initialize(Rml::Context& rml_context, const RuntimeConfig& config);
    void shutdown();
    void attach(Rml::ElementDocument& document);
    void detach();
    void set_open_console_callback(std::function<void()> callback);
    void set_launch_map_callback(std::function<void(std::string)> callback);
    void set_visible(bool visible);

    [[nodiscard]] bool should_quit() const;
    [[nodiscard]] bool visible() const;

    void ProcessEvent(Rml::Event& event) override;

private:
    void attach_buttons();
    void show_home_page();
    void show_play_page(bool show);
    void show_settings(bool show);
    void set_play_mode(const Rml::String& mode);
    void set_play_tab(const Rml::String& tab);
    void toggle_map_selection(const Rml::String& map_name);
    void execute_go();
    void toggle_prime();
    void reset_settings();
    void set_status(const std::string& text, bool visible);
    void update_go_button_state();
    void update_nav_active(const char* active_id);
    void update_class_for_elements(const char* class_name, const char* active_class, const char* attribute_name, const Rml::String& active_value);
    void set_display(const char* id, const char* value);
    void set_class(const char* id, const char* class_name, bool enabled);

    [[nodiscard]] Rml::Element* element(const char* id) const;

    Rml::ElementDocument* document_ = nullptr;
    bool visible_ = false;
    bool should_quit_ = false;
    bool prime_only_ = true;
    bool settings_visible_ = false;
    bool play_page_visible_ = false;
    Rml::String play_mode_ = "matchmaking";
    Rml::String active_play_tab_ = "competitive";
    Rml::String selected_map_;
    std::function<void()> open_console_callback_;
    std::function<void(std::string)> launch_map_callback_;
};
}
