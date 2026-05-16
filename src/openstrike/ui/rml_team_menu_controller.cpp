#include "openstrike/ui/rml_team_menu_controller.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/game/team_system.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace openstrike
{
namespace
{
std::string rml_path_string(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::string escape_rml(std::string_view text)
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
}

RmlTeamMenuController::RmlTeamMenuController() = default;

RmlTeamMenuController::~RmlTeamMenuController()
{
    shutdown();
}

bool RmlTeamMenuController::initialize(Rml::Context& rml_context, EngineContext& engine_context, const RuntimeConfig& config)
{
    shutdown();
    rml_context_ = &rml_context;
    engine_context_ = &engine_context;

    const std::filesystem::path candidates[] = {
        config.content_root / "csgo/resource/ui/teammenu.rml",
        config.content_root / "resource/ui/teammenu.rml",
        config.content_root / "assets/ui/teammenu.rml",
    };
    for (const std::filesystem::path& candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            document_path_ = candidate;
            return true;
        }
    }

    log_warning("RmlUi team menu document was not found under '{}'", rml_path_string(config.content_root));
    return true;
}

void RmlTeamMenuController::shutdown()
{
    if (document_ != nullptr)
    {
        Rml::ElementList buttons;
        document_->GetElementsByClassName(buttons, "team-btn");
        for (Rml::Element* button : buttons)
        {
            if (button != nullptr)
            {
                button->RemoveEventListener("click", this);
            }
        }
        document_->Close();
    }

    rml_context_ = nullptr;
    engine_context_ = nullptr;
    document_ = nullptr;
    visible_ = false;
    queued_join_game_ = false;
    last_failure_revision_ = 0;
}

void RmlTeamMenuController::update(bool main_menu_visible, bool loading_screen_visible)
{
    if (engine_context_ == nullptr)
    {
        return;
    }

    const bool should_show = !main_menu_visible && !loading_screen_visible &&
                             engine_context_->teams.should_show_team_menu(
                                 local_team_connection_id(engine_context_->network),
                                 engine_context_->world.current_world() != nullptr);
    if (should_show && !visible_)
    {
        show();
    }
    else if (!should_show && visible_)
    {
        hide();
    }

    if (visible_)
    {
        update_timer();
        update_failure();
    }
}

void RmlTeamMenuController::show()
{
    if (visible_)
    {
        return;
    }
    if (!load_document())
    {
        return;
    }

    visible_ = true;
    queued_join_game_ = true;
    document_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
    if (engine_context_ != nullptr)
    {
        engine_context_->command_buffer.add_text("joingame");
    }
    update_timer();
    update_failure();
}

void RmlTeamMenuController::hide()
{
    visible_ = false;
    queued_join_game_ = false;
    if (document_ != nullptr)
    {
        document_->Hide();
    }
}

bool RmlTeamMenuController::visible() const
{
    return visible_;
}

void RmlTeamMenuController::ProcessEvent(Rml::Event& event)
{
    Rml::Element* item = event.GetCurrentElement();
    if (item == nullptr || engine_context_ == nullptr)
    {
        return;
    }

    const Rml::String command = item->GetAttribute<Rml::String>("data-command", "");
    if (command == "JoinT")
    {
        submit_command("jointeam 2 1");
    }
    else if (command == "JoinCT")
    {
        submit_command("jointeam 3 1");
    }
    else if (command == "AutoAssign")
    {
        submit_command("jointeam 0 1");
    }
    else if (command == "Spectate")
    {
        submit_command("jointeam 1");
    }
}

bool RmlTeamMenuController::load_document()
{
    if (document_ != nullptr)
    {
        return true;
    }
    if (rml_context_ == nullptr || document_path_.empty())
    {
        return false;
    }

    document_ = rml_context_->LoadDocument(rml_path_string(document_path_));
    if (document_ == nullptr)
    {
        log_warning("failed to load RmlUi team menu document '{}'", rml_path_string(document_path_));
        return false;
    }

    attach_buttons();
    document_->Hide();
    log_info("loaded RmlUi team menu document '{}'", rml_path_string(document_path_));
    return true;
}

void RmlTeamMenuController::attach_buttons()
{
    if (document_ == nullptr)
    {
        return;
    }

    Rml::ElementList buttons;
    document_->GetElementsByClassName(buttons, "team-btn");
    for (Rml::Element* button : buttons)
    {
        if (button != nullptr)
        {
            button->AddEventListener("click", this);
        }
    }
    log_info("attached {} RmlUi team menu button(s)", buttons.size());
}

void RmlTeamMenuController::update_timer()
{
    if (engine_context_ == nullptr)
    {
        return;
    }
    const int seconds = std::max(0, engine_context_->variables.get_int("mp_force_pick_time", 15));
    set_text("team-timer", std::format("Auto-assign in {}s", seconds));
}

void RmlTeamMenuController::update_failure()
{
    if (engine_context_ == nullptr)
    {
        return;
    }
    const TeamPlayerState* player = engine_context_->teams.find_player(local_team_connection_id(engine_context_->network));
    if (player == nullptr || player->last_join_failure == TeamJoinFailedReason::None)
    {
        set_display("team-error", "none");
        set_text("team-error", "");
        last_failure_revision_ = engine_context_->teams.revision();
        return;
    }
    if (last_failure_revision_ == engine_context_->teams.revision())
    {
        return;
    }
    set_display("team-error", "block");
    set_text("team-error", std::format("Cannot join: {}", to_string(player->last_join_failure)));
    last_failure_revision_ = engine_context_->teams.revision();
}

void RmlTeamMenuController::submit_command(const char* command)
{
    if (engine_context_ == nullptr)
    {
        return;
    }
    engine_context_->command_buffer.add_text(command);
    hide();
}

void RmlTeamMenuController::set_text(const char* id, const std::string& value)
{
    if (Rml::Element* item = element(id))
    {
        item->SetInnerRML(escape_rml(value));
    }
}

void RmlTeamMenuController::set_display(const char* id, const char* value)
{
    if (Rml::Element* item = element(id))
    {
        item->SetProperty("display", value);
    }
}

Rml::Element* RmlTeamMenuController::element(const char* id) const
{
    if (document_ == nullptr)
    {
        return nullptr;
    }
    return document_->GetElementById(id);
}
}
