#include "openstrike/ui/rml_hud_controller.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/hud_state.hpp"
#include "openstrike/engine/runtime_config.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>

namespace openstrike
{
namespace
{
std::string rml_path_string(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::string escape_rml(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string format_timer(double seconds)
{
    const int total_seconds = std::max(0, static_cast<int>(std::floor(seconds + 0.5)));
    return std::format("{}:{:02}", total_seconds / 60, total_seconds % 60);
}

std::string format_percent(float percent)
{
    return std::format("{:.0f}%", std::clamp(percent, 0.0F, 100.0F));
}
}

RmlHudController::RmlHudController() = default;

RmlHudController::~RmlHudController()
{
    shutdown();
}

bool RmlHudController::initialize(Rml::Context& rml_context, EngineContext& engine_context, const RuntimeConfig& config)
{
    shutdown();
    engine_context_ = &engine_context;

    const std::filesystem::path candidates[] = {
        config.content_root / "csgo/resource/ui/hud.rml",
        config.content_root / "resource/ui/hud.rml",
        config.content_root / "assets/ui/hud.rml",
    };

    for (const std::filesystem::path& candidate : candidates)
    {
        if (!std::filesystem::exists(candidate))
        {
            continue;
        }

        document_ = rml_context.LoadDocument(rml_path_string(candidate));
        if (document_ != nullptr)
        {
            log_info("loaded RmlUi HUD document '{}'", rml_path_string(candidate));
            break;
        }

        log_warning("failed to load RmlUi HUD document '{}'", rml_path_string(candidate));
    }

    if (document_ == nullptr)
    {
        log_warning("RmlUi HUD document was not found under '{}'", rml_path_string(config.content_root));
        engine_context_ = nullptr;
        return true;
    }

    document_->Hide();
    rendered_revision_ = UINT64_MAX;
    visible_ = false;
    return true;
}

void RmlHudController::shutdown()
{
    if (document_ != nullptr)
    {
        document_->Close();
    }

    document_ = nullptr;
    engine_context_ = nullptr;
    rendered_revision_ = UINT64_MAX;
    visible_ = false;
}

void RmlHudController::update(bool main_menu_visible)
{
    if (document_ == nullptr || engine_context_ == nullptr)
    {
        visible_ = false;
        return;
    }

    const HudState& state = engine_context_->hud;
    const bool should_show = state.visible && !main_menu_visible;
    if (should_show != visible_)
    {
        visible_ = should_show;
        if (visible_)
        {
            document_->Show();
        }
        else
        {
            document_->Hide();
        }
    }

    if (visible_ && state.revision != rendered_revision_)
    {
        refresh(state);
        rendered_revision_ = state.revision;
    }
}

bool RmlHudController::visible() const
{
    return visible_;
}

void RmlHudController::refresh(const HudState& state)
{
    const int max_health = std::max(1, state.max_health);
    const int health = std::clamp(state.health, 0, max_health);
    const int armor = std::clamp(state.armor, 0, 100);
    const int clip = std::max(0, state.ammo_in_clip);
    const int reserve = std::max(0, state.reserve_ammo);

    set_text("hud-ct-score", std::to_string(state.counter_terrorist_score));
    set_text("hud-t-score", std::to_string(state.terrorist_score));
    set_text("hud-timer", format_timer(state.round_time_seconds));
    set_class("hud-timer", "timer-warning", state.round_time_seconds <= 10.0);

    set_text("hud-money", std::format("${}", state.money));
    set_text("hud-health-value", std::to_string(health));
    set_text("hud-armor-value", std::to_string(armor));
    set_percent_width("hud-health-bar", (static_cast<float>(health) / static_cast<float>(max_health)) * 100.0F);
    set_percent_width("hud-armor-bar", static_cast<float>(armor));
    set_class("hud-health-icon", "health-low", health <= 25);
    set_class("hud-health-bar", "health-low", health <= 25);

    set_display("hud-ammo", state.alive ? "flex" : "none");
    set_text("hud-ammo-clip", std::to_string(clip));
    set_text("hud-ammo-reserve", std::to_string(reserve));
    set_class("hud-ammo-clip", "ammo-low", clip <= 5);

    if (state.kill_feed.empty())
    {
        set_text("hud-killfeed", "");
    }
    else if (Rml::Element* killfeed = element("hud-killfeed"))
    {
        killfeed->SetInnerRML("<div class=\"killfeed-entry local-involved\"><span class=\"killfeed-killer team-ct\">"
            + escape_rml(state.kill_feed) + "</span></div>");
    }
}

void RmlHudController::set_text(const char* id, const std::string& value)
{
    if (Rml::Element* item = element(id))
    {
        item->SetInnerRML(escape_rml(value));
    }
}

void RmlHudController::set_display(const char* id, const char* value)
{
    if (Rml::Element* item = element(id))
    {
        item->SetProperty("display", value);
    }
}

void RmlHudController::set_class(const char* id, const char* class_name, bool enabled)
{
    if (Rml::Element* item = element(id))
    {
        item->SetClass(class_name, enabled);
    }
}

void RmlHudController::set_percent_width(const char* id, float percent)
{
    if (Rml::Element* item = element(id))
    {
        item->SetProperty("width", format_percent(percent));
    }
}

Rml::Element* RmlHudController::element(const char* id) const
{
    if (document_ == nullptr)
    {
        return nullptr;
    }

    return document_->GetElementById(id);
}
}
