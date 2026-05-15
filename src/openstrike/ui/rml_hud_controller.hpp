#pragma once

#include <cstdint>
#include <string>

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
struct HudState;

class RmlHudController final
{
public:
    RmlHudController();
    ~RmlHudController();

    RmlHudController(const RmlHudController&) = delete;
    RmlHudController& operator=(const RmlHudController&) = delete;

    bool initialize(Rml::Context& rml_context, EngineContext& engine_context, const RuntimeConfig& config);
    void shutdown();
    void update(bool main_menu_visible);

    [[nodiscard]] bool visible() const;

private:
    void refresh(const HudState& state);
    void set_text(const char* id, const std::string& value);
    void set_display(const char* id, const char* value);
    void set_class(const char* id, const char* class_name, bool enabled);
    void set_percent_width(const char* id, float percent);

    [[nodiscard]] Rml::Element* element(const char* id) const;

    EngineContext* engine_context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    std::uint64_t rendered_revision_ = UINT64_MAX;
    bool visible_ = false;
};
}
