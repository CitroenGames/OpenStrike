#include "openstrike/ui/rml_loading_screen_controller.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/loading_screen_state.hpp"
#include "openstrike/engine/runtime_config.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

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
        case '\n':
            escaped += "<br/>";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}
}

RmlLoadingScreenController::RmlLoadingScreenController() = default;

RmlLoadingScreenController::~RmlLoadingScreenController()
{
    shutdown();
}

bool RmlLoadingScreenController::initialize(Rml::Context& rml_context, const RuntimeConfig& config)
{
    shutdown();

    const std::filesystem::path candidates[] = {
        config.content_root / "csgo/resource/ui/loadingscreen.rml",
        config.content_root / "resource/ui/loadingscreen.rml",
        config.content_root / "assets/ui/loadingscreen.rml",
    };

    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error))
        {
            continue;
        }

        document_ = rml_context.LoadDocument(rml_path_string(candidate));
        if (document_ != nullptr)
        {
            log_info("loaded RmlUi loading screen document '{}'", rml_path_string(candidate));
            break;
        }

        log_warning("failed to load RmlUi loading screen document '{}'", rml_path_string(candidate));
    }

    if (document_ == nullptr)
    {
        log_warning("RmlUi loading screen document was not found under '{}'", rml_path_string(config.content_root));
        return true;
    }

    document_->Hide();
    rendered_revision_ = UINT64_MAX;
    auto_close_presented_revision_ = UINT64_MAX;
    visible_ = false;
    return true;
}

void RmlLoadingScreenController::shutdown()
{
    if (document_ != nullptr)
    {
        document_->Close();
    }

    document_ = nullptr;
    rendered_revision_ = UINT64_MAX;
    auto_close_presented_revision_ = UINT64_MAX;
    visible_ = false;
}

void RmlLoadingScreenController::update(LoadingScreenState& state)
{
    if (document_ == nullptr)
    {
        visible_ = false;
        return;
    }

    const LoadingScreenSnapshot& snapshot = state.snapshot();
    if (snapshot.visible != visible_)
    {
        visible_ = snapshot.visible;
        if (visible_)
        {
            document_->Show();
        }
        else
        {
            document_->Hide();
        }
    }

    if (visible_ && snapshot.revision != rendered_revision_)
    {
        refresh(snapshot);
        rendered_revision_ = snapshot.revision;
    }

    if (!visible_ || !snapshot.auto_close)
    {
        return;
    }

    if (auto_close_presented_revision_ == snapshot.revision)
    {
        state.close();
        return;
    }

    auto_close_presented_revision_ = snapshot.revision;
}

bool RmlLoadingScreenController::visible() const
{
    return visible_;
}

void RmlLoadingScreenController::refresh(const LoadingScreenSnapshot& snapshot)
{
    set_text("loading-map-name", snapshot.map_name.empty() ? "Loading" : snapshot.map_name);
    set_text("loading-game-mode", snapshot.game_mode);
    set_inner_rml("loading-description", escape_rml(snapshot.description));
    set_text("loading-tip-text", snapshot.tip);
    set_text("loading-status", snapshot.status);
    set_text("loading-progress-percent", std::format("{:.0f}%", std::clamp(snapshot.progress, 0.0F, 1.0F) * 100.0F));
    set_text("loading-map-area-name", snapshot.map_name.empty() ? "MAP" : snapshot.map_name);
    set_percent_width("loading-progress-fill", snapshot.progress * 100.0F);
    set_opacity("loading-overlay", 0.82F - (std::clamp(snapshot.progress, 0.0F, 1.0F) * 0.34F));
}

void RmlLoadingScreenController::set_text(const char* id, const std::string& value)
{
    if (Rml::Element* item = element(id))
    {
        item->SetInnerRML(escape_rml(value));
    }
}

void RmlLoadingScreenController::set_inner_rml(const char* id, const std::string& value)
{
    if (Rml::Element* item = element(id))
    {
        item->SetInnerRML(value);
    }
}

void RmlLoadingScreenController::set_percent_width(const char* id, float percent)
{
    if (Rml::Element* item = element(id))
    {
        item->SetProperty("width", std::format("{:.0f}%", std::clamp(percent, 0.0F, 100.0F)));
    }
}

void RmlLoadingScreenController::set_opacity(const char* id, float opacity)
{
    if (Rml::Element* item = element(id))
    {
        item->SetProperty("opacity", std::format("{:.2f}", std::clamp(opacity, 0.0F, 1.0F)));
    }
}

Rml::Element* RmlLoadingScreenController::element(const char* id) const
{
    if (document_ == nullptr)
    {
        return nullptr;
    }

    return document_->GetElementById(id);
}
}
