#pragma once

#include <RmlUi/Core/EventListener.h>

#include <cstdint>
#include <filesystem>

namespace Rml
{
class Context;
class Element;
class ElementDocument;
}

namespace openstrike
{
class EngineContext;
struct RuntimeConfig;

class RmlTeamMenuController final : public Rml::EventListener
{
public:
    RmlTeamMenuController();
    ~RmlTeamMenuController() override;

    RmlTeamMenuController(const RmlTeamMenuController&) = delete;
    RmlTeamMenuController& operator=(const RmlTeamMenuController&) = delete;

    bool initialize(Rml::Context& rml_context, EngineContext& engine_context, const RuntimeConfig& config);
    void shutdown();
    void update(bool main_menu_visible, bool loading_screen_visible);

    void show();
    void hide();
    [[nodiscard]] bool visible() const;

    void ProcessEvent(Rml::Event& event) override;

private:
    bool load_document();
    void attach_buttons();
    void update_timer();
    void update_failure();
    void submit_command(const char* command);
    void set_text(const char* id, const std::string& value);
    void set_display(const char* id, const char* value);

    [[nodiscard]] Rml::Element* element(const char* id) const;

    Rml::Context* rml_context_ = nullptr;
    EngineContext* engine_context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    std::filesystem::path document_path_;
    bool visible_ = false;
    bool queued_join_game_ = false;
    std::uint64_t last_failure_revision_ = 0;
};
}
