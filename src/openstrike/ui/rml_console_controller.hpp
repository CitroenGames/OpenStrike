#pragma once

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <vector>

namespace Rml
{
class Context;
class Element;
class ElementDocument;
}

namespace openstrike
{
class EngineContext;

class RmlConsoleController final : public Rml::EventListener
{
public:
    RmlConsoleController();
    ~RmlConsoleController() override;

    RmlConsoleController(const RmlConsoleController&) = delete;
    RmlConsoleController& operator=(const RmlConsoleController&) = delete;

    [[nodiscard]] bool initialize(Rml::Context& rml_context, EngineContext& engine_context);
    void shutdown();
    void update();

    void show();
    void hide();
    void toggle();
    [[nodiscard]] bool visible() const;

    void ProcessEvent(Rml::Event& event) override;

private:
    void submit_command(const Rml::String& command);
    void refresh_output();
    void clear_input();
    [[nodiscard]] Rml::Element* element(const char* id) const;

    EngineContext* engine_context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    bool visible_ = false;
    std::vector<std::string> rendered_lines_;
};
}
