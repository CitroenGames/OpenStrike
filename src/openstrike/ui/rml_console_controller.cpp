#include "openstrike/ui/rml_console_controller.hpp"

#include "openstrike/core/console.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <string>

namespace openstrike
{
namespace
{
constexpr const char* kConsoleDocument = R"(
<rml>
<head>
    <style>
        body {
            width: 100%;
            height: 100%;
            margin: 0;
            font-family: LatoLatin;
            color: #d9e2ef;
        }
        #console-root {
            position: absolute;
            left: 0;
            top: 0;
            right: 0;
            height: 46%;
            display: flex;
            flex-direction: column;
            background-color: rgba(5, 7, 10, 238);
            border-bottom-width: 2dp;
            border-bottom-color: rgba(222, 159, 55, 210);
        }
        #console-header {
            display: flex;
            flex-direction: row;
            align-items: center;
            height: 28dp;
            padding: 0 12dp;
            background-color: rgba(18, 24, 32, 245);
            color: #ffffff;
            font-size: 13dp;
            font-weight: bold;
        }
        #console-title {
            flex: 1;
        }
        #console-close {
            width: 30dp;
            height: 22dp;
            line-height: 22dp;
            text-align: center;
            color: #b8c4d3;
            background-color: rgba(255, 255, 255, 10);
        }
        #console-close:hover {
            color: #ffffff;
            background-color: rgba(222, 79, 67, 210);
        }
        #console-output {
            flex: 1;
            overflow-y: auto;
            padding: 8dp 12dp 6dp 12dp;
            font-size: 12dp;
            line-height: 17dp;
            color: #d8e0ea;
        }
        .console-line {
            white-space: pre;
        }
        #console-form {
            display: flex;
            flex-direction: row;
            align-items: center;
            height: 36dp;
            padding: 0 12dp;
            background-color: rgba(10, 14, 20, 248);
            border-top-width: 1dp;
            border-top-color: rgba(255, 255, 255, 22);
        }
        #console-prompt {
            width: 18dp;
            color: #de9f37;
            font-size: 15dp;
            font-weight: bold;
        }
        #console-input {
            flex: 1;
            height: 26dp;
            padding: 2dp 6dp;
            color: #ffffff;
            background-color: rgba(255, 255, 255, 14);
            border-width: 1dp;
            border-color: rgba(255, 255, 255, 40);
            font-size: 13dp;
        }
    </style>
</head>
<body>
    <div id="console-root">
        <div id="console-header">
            <div id="console-title">OpenStrike Console</div>
            <button id="console-close">X</button>
        </div>
        <div id="console-output"></div>
        <form id="console-form">
            <div id="console-prompt">&gt;</div>
            <input id="console-input" name="command" type="text" />
        </form>
    </div>
</body>
</rml>
)";

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
}

RmlConsoleController::RmlConsoleController() = default;

RmlConsoleController::~RmlConsoleController()
{
    shutdown();
}

bool RmlConsoleController::initialize(Rml::Context& rml_context, EngineContext& engine_context)
{
    shutdown();
    engine_context_ = &engine_context;

    document_ = rml_context.LoadDocumentFromMemory(kConsoleDocument, "openstrike:console");
    if (document_ == nullptr)
    {
        log_error("failed to create RmlUi console document");
        return false;
    }

    if (Rml::Element* form = element("console-form"))
    {
        form->AddEventListener("submit", this);
    }

    if (Rml::Element* close = element("console-close"))
    {
        close->AddEventListener("click", this);
    }

    refresh_output();
    document_->Hide();
    visible_ = false;
    return true;
}

void RmlConsoleController::shutdown()
{
    if (document_ != nullptr)
    {
        if (Rml::Element* form = element("console-form"))
        {
            form->RemoveEventListener("submit", this);
        }

        if (Rml::Element* close = element("console-close"))
        {
            close->RemoveEventListener("click", this);
        }

        document_->Close();
    }

    document_ = nullptr;
    engine_context_ = nullptr;
    visible_ = false;
    rendered_lines_.clear();
}

void RmlConsoleController::update()
{
    if (visible_)
    {
        refresh_output();
    }
}

void RmlConsoleController::show()
{
    if (document_ == nullptr || engine_context_ == nullptr || !engine_context_->variables.get_bool("con_enable", true))
    {
        return;
    }

    visible_ = true;
    refresh_output();
    document_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);

    if (Rml::Element* input = element("console-input"))
    {
        input->Focus();
    }
}

void RmlConsoleController::hide()
{
    if (document_ == nullptr)
    {
        return;
    }

    document_->Hide();
    visible_ = false;
}

void RmlConsoleController::toggle()
{
    if (visible_)
    {
        hide();
    }
    else
    {
        show();
    }
}

bool RmlConsoleController::visible() const
{
    return visible_;
}

void RmlConsoleController::ProcessEvent(Rml::Event& event)
{
    if (event == "submit")
    {
        event.StopPropagation();
        submit_command(event.GetParameter<Rml::String>("command", ""));
        return;
    }

    if (event == "click" && event.GetCurrentElement() == element("console-close"))
    {
        event.StopPropagation();
        hide();
    }
}

void RmlConsoleController::submit_command(const Rml::String& command)
{
    if (engine_context_ == nullptr)
    {
        return;
    }

    if (command.empty())
    {
        clear_input();
        return;
    }

    log_info("> {}", command);
    engine_context_->command_buffer.add_text(command);
    clear_input();
    refresh_output();
}

void RmlConsoleController::refresh_output()
{
    if (document_ == nullptr)
    {
        return;
    }

    std::vector<std::string> lines = Logger::instance().recent_lines(120);
    if (lines == rendered_lines_)
    {
        return;
    }

    std::string rml;
    for (const std::string& line : lines)
    {
        rml += "<div class=\"console-line\">";
        rml += escape_rml(line);
        rml += "</div>";
    }

    if (Rml::Element* output = element("console-output"))
    {
        output->SetInnerRML(rml);
        output->SetScrollTop(1000000.0f);
    }

    rendered_lines_ = std::move(lines);
}

void RmlConsoleController::clear_input()
{
    Rml::Element* input_element = element("console-input");
    auto* input = dynamic_cast<Rml::ElementFormControlInput*>(input_element);
    if (input == nullptr)
    {
        return;
    }

    input->SetValue("");
    input->Focus();
}

Rml::Element* RmlConsoleController::element(const char* id) const
{
    if (document_ == nullptr)
    {
        return nullptr;
    }

    return document_->GetElementById(id);
}
}
