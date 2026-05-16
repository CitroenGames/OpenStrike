#include "openstrike/ui/rml_console_controller.hpp"

#include "openstrike/core/console.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace openstrike
{
namespace
{
constexpr std::size_t kMaxHistory = 64;
constexpr std::size_t kMaxCompletions = 24;
constexpr std::size_t kMaxOutputLines = 200;

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
            height: 52%;
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
        }
        .console-line {
            white-space: pre;
        }
        .log-info  { color: #d8e0ea; }
        .log-trace { color: #98a2b3; }
        .log-warn  { color: #f0c674; }
        .log-error { color: #ec7373; }
        .log-echo  { color: #ffd07a; }
        #console-completion {
            max-height: 170dp;
            overflow-y: auto;
            background-color: rgba(20, 26, 36, 245);
            border-top-width: 1dp;
            border-top-color: rgba(255, 255, 255, 30);
            display: block;
        }
        #console-completion.hidden {
            display: none;
        }
        .completion-item {
            padding: 3dp 12dp;
            font-size: 12dp;
            color: #d8e0ea;
            white-space: pre;
        }
        .completion-item:hover {
            background-color: rgba(222, 159, 55, 60);
            color: #ffffff;
        }
        .completion-item.selected {
            background-color: rgba(222, 159, 55, 110);
            color: #ffffff;
        }
        .completion-name  { color: #ffd07a; }
        .completion-value { color: #98a2b3; }
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
        #console-submit {
            margin-left: 8dp;
            width: 64dp;
            height: 26dp;
            background-color: rgba(222, 159, 55, 200);
            color: #16110a;
            font-size: 12dp;
            font-weight: bold;
            text-align: center;
            line-height: 22dp;
        }
        #console-submit:hover {
            background-color: rgba(255, 184, 70, 220);
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
        <div id="console-completion" class="hidden"></div>
        <form id="console-form">
            <div id="console-prompt">&gt;</div>
            <input id="console-input" name="command" type="text" />
            <input id="console-submit" type="submit" value="Submit" />
        </form>
    </div>
</body>
</rml>
)";

const char* level_class(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "log-trace";
    case LogLevel::Info:
        return "log-info";
    case LogLevel::Warning:
        return "log-warn";
    case LogLevel::Error:
        return "log-error";
    }
    return "log-info";
}

std::string escape_rml(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default:  escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool starts_with_ci(std::string_view text, std::string_view prefix)
{
    if (prefix.size() > text.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        {
            return false;
        }
    }
    return true;
}

std::string trim_view(std::string_view text)
{
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0)
    {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0)
    {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

std::string common_prefix(std::string_view a, std::string_view b)
{
    const std::size_t bound = std::min(a.size(), b.size());
    std::size_t i = 0;
    while (i < bound && std::tolower(static_cast<unsigned char>(a[i])) == std::tolower(static_cast<unsigned char>(b[i])))
    {
        ++i;
    }
    return std::string(a.substr(0, i));
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

    document_ = rml_context.LoadDocumentFromMemory(kConsoleDocument, "openstrike://console");
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
    if (Rml::Element* input = element("console-input"))
    {
        input->AddEventListener("keydown", this, true);
        input->AddEventListener("change", this);
    }
    if (Rml::Element* completion = element("console-completion"))
    {
        completion->AddEventListener("click", this);
    }

    refresh_output(true);
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
        if (Rml::Element* input = element("console-input"))
        {
            input->RemoveEventListener("keydown", this, true);
            input->RemoveEventListener("change", this);
        }
        if (Rml::Element* completion = element("console-completion"))
        {
            completion->RemoveEventListener("click", this);
        }
        document_->Close();
    }

    document_ = nullptr;
    engine_context_ = nullptr;
    visible_ = false;
    last_entry_count_ = 0;
    completions_.clear();
    partial_text_.clear();
    autocomplete_mode_ = false;
    suppress_input_change_ = false;
    history_cursor_ = -1;
}

void RmlConsoleController::update()
{
    if (visible_)
    {
        refresh_output(false);
    }
}

void RmlConsoleController::show()
{
    if (document_ == nullptr || engine_context_ == nullptr || !engine_context_->variables.get_bool("con_enable", true))
    {
        return;
    }

    visible_ = true;
    refresh_output(true);
    rebuild_completions();
    render_completions();
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
    const Rml::String& type = event.GetType();
    Rml::Element* current = event.GetCurrentElement();
    Rml::Element* target = event.GetTargetElement();

    if (type == "submit")
    {
        on_submit(event);
        return;
    }

    if (type == "keydown" && current != nullptr && current->GetId() == "console-input")
    {
        on_input_keydown(event);
        return;
    }

    if (type == "change" && current != nullptr && current->GetId() == "console-input")
    {
        on_input_change();
        return;
    }

    if (type == "click" && current != nullptr && current->GetId() == "console-close")
    {
        event.StopPropagation();
        hide();
        return;
    }

    if (type == "click" && current != nullptr && current->GetId() == "console-completion" && target != nullptr)
    {
        on_completion_click(event);
        return;
    }
}

void RmlConsoleController::on_submit(Rml::Event& event)
{
    event.StopPropagation();
    submit_command(event.GetParameter<Rml::String>("command", ""));
}

void RmlConsoleController::on_input_keydown(Rml::Event& event)
{
    const int identifier = event.GetParameter<int>("key_identifier", static_cast<int>(Rml::Input::KI_UNKNOWN));
    const auto key = static_cast<Rml::Input::KeyIdentifier>(identifier);

    if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER)
    {
        event.StopPropagation();
        submit_command(current_input_text());
        return;
    }

    if (key == Rml::Input::KI_TAB)
    {
        event.StopPropagation();
        if (!autocomplete_mode_ && completions_.size() > 1)
        {
            apply_common_prefix();
            autocomplete_mode_ = true;
            return;
        }
        cycle_completion(1);
        return;
    }

    if (key == Rml::Input::KI_DOWN)
    {
        event.StopPropagation();
        cycle_completion(1);
        return;
    }

    if (key == Rml::Input::KI_UP)
    {
        event.StopPropagation();
        cycle_completion(-1);
        return;
    }
}

void RmlConsoleController::on_input_change()
{
    if (suppress_input_change_)
    {
        return;
    }

    autocomplete_mode_ = false;
    history_cursor_ = -1;
    rebuild_completions();
    render_completions();
}

void RmlConsoleController::on_completion_click(Rml::Event& event)
{
    Rml::Element* target = event.GetTargetElement();
    while (target != nullptr && target->GetId() != "console-completion")
    {
        const Rml::String index_attr = target->GetAttribute<Rml::String>("data-index", "");
        if (!index_attr.empty())
        {
            event.StopPropagation();
            try
            {
                const std::size_t index = static_cast<std::size_t>(std::stoul(index_attr));
                apply_completion(index);
            }
            catch (...)
            {
            }
            return;
        }
        target = target->GetParentNode();
    }
}

void RmlConsoleController::submit_command(std::string command)
{
    if (engine_context_ == nullptr)
    {
        return;
    }

    command = trim_view(command);
    if (command.empty())
    {
        clear_input();
        return;
    }

    echo_submitted(command);
    push_history(command);

    engine_context_->command_buffer.add_text(command);

    clear_input();
    autocomplete_mode_ = false;
    history_cursor_ = -1;
    rebuild_completions();
    render_completions();
    refresh_output(true);
}

void RmlConsoleController::refresh_output(bool force)
{
    if (document_ == nullptr)
    {
        return;
    }

    const std::vector<LogEntry> entries = Logger::instance().recent_entries(kMaxOutputLines);
    if (!force && entries.size() == last_entry_count_)
    {
        return;
    }

    std::string rml;
    for (const LogEntry& entry : entries)
    {
        rml += "<div class=\"console-line ";
        rml += level_class(entry.level);
        rml += "\">";
        rml += escape_rml(entry.message);
        rml += "</div>";
    }

    if (Rml::Element* output = element("console-output"))
    {
        output->SetInnerRML(rml);
        output->SetScrollTop(1000000.0f);
    }

    last_entry_count_ = entries.size();
}

void RmlConsoleController::rebuild_completions()
{
    completions_.clear();
    completion_index_ = 0;
    partial_text_ = current_input_text();

    if (engine_context_ == nullptr)
    {
        return;
    }

    if (partial_text_.empty())
    {
        for (auto it = command_history_.rbegin(); it != command_history_.rend(); ++it)
        {
            completions_.push_back(CompletionEntry{*it, *it});
            if (completions_.size() >= kMaxCompletions)
            {
                break;
            }
        }
        return;
    }

    const std::vector<ConsoleCommand> commands = engine_context_->commands.commands();
    const std::vector<ConsoleVariable> variables = engine_context_->variables.variables();

    struct Candidate
    {
        std::string name;
        std::string display;
    };
    std::vector<Candidate> matches;

    for (const ConsoleCommand& command : commands)
    {
        if (starts_with_ci(command.name, partial_text_))
        {
            matches.push_back(Candidate{command.name, command.name});
        }
    }
    for (const ConsoleVariable& variable : variables)
    {
        if (starts_with_ci(variable.name, partial_text_))
        {
            std::string display = variable.name;
            display += " ";
            display += variable.value;
            matches.push_back(Candidate{variable.name, std::move(display)});
        }
    }

    std::sort(matches.begin(), matches.end(), [](const Candidate& a, const Candidate& b) {
        return a.name < b.name;
    });

    completions_.reserve(std::min(matches.size(), kMaxCompletions));
    for (Candidate& match : matches)
    {
        completions_.push_back(CompletionEntry{std::move(match.name), std::move(match.display)});
        if (completions_.size() >= kMaxCompletions)
        {
            break;
        }
    }
}

void RmlConsoleController::render_completions()
{
    Rml::Element* container = element("console-completion");
    if (container == nullptr)
    {
        return;
    }

    if (completions_.empty())
    {
        container->SetInnerRML("");
        container->SetClass("hidden", true);
        return;
    }

    container->SetClass("hidden", false);

    std::string rml;
    for (std::size_t i = 0; i < completions_.size(); ++i)
    {
        const bool is_selected = autocomplete_mode_ && i == completion_index_;
        rml += "<div class=\"completion-item";
        if (is_selected)
        {
            rml += " selected";
        }
        rml += "\" data-index=\"";
        rml += std::to_string(i);
        rml += "\">";
        rml += escape_rml(completions_[i].display);
        rml += "</div>";
    }
    container->SetInnerRML(rml);
}

void RmlConsoleController::cycle_completion(int direction)
{
    if (completions_.empty())
    {
        return;
    }

    if (!autocomplete_mode_)
    {
        autocomplete_mode_ = true;
        completion_index_ = direction >= 0 ? 0 : completions_.size() - 1;
    }
    else
    {
        const int count = static_cast<int>(completions_.size());
        int next = static_cast<int>(completion_index_) + direction;
        next = ((next % count) + count) % count;
        completion_index_ = static_cast<std::size_t>(next);
    }

    apply_completion(completion_index_);
}

void RmlConsoleController::apply_common_prefix()
{
    if (completions_.size() < 2)
    {
        if (!completions_.empty())
        {
            apply_completion(0);
        }
        return;
    }

    std::string prefix = completions_.front().name;
    for (std::size_t i = 1; i < completions_.size(); ++i)
    {
        prefix = common_prefix(prefix, completions_[i].name);
        if (prefix.empty())
        {
            break;
        }
    }

    if (prefix.size() > partial_text_.size())
    {
        suppress_input_change_ = true;
        set_input_text(prefix);
        suppress_input_change_ = false;
        partial_text_ = prefix;
        rebuild_completions();
    }

    render_completions();
}

void RmlConsoleController::apply_completion(std::size_t index)
{
    if (index >= completions_.size())
    {
        return;
    }

    completion_index_ = index;
    autocomplete_mode_ = true;

    std::string text = completions_[index].name;
    text += ' ';

    suppress_input_change_ = true;
    set_input_text(text);
    suppress_input_change_ = false;

    render_completions();
}

void RmlConsoleController::set_selected_completion(std::size_t index)
{
    completion_index_ = index;
    render_completions();
}

void RmlConsoleController::clear_completions()
{
    completions_.clear();
    completion_index_ = 0;
    autocomplete_mode_ = false;
    render_completions();
}

void RmlConsoleController::clear_input()
{
    suppress_input_change_ = true;
    set_input_text("");
    suppress_input_change_ = false;
    partial_text_.clear();

    if (Rml::Element* input_element = element("console-input"))
    {
        input_element->Focus();
    }
}

void RmlConsoleController::push_history(const std::string& command)
{
    if (command.empty())
    {
        return;
    }

    for (auto it = command_history_.begin(); it != command_history_.end();)
    {
        if (*it == command)
        {
            it = command_history_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    command_history_.push_back(command);
    while (command_history_.size() > kMaxHistory)
    {
        command_history_.pop_front();
    }
}

void RmlConsoleController::echo_submitted(const std::string& command)
{
    log_info("] {}", command);
}

std::string RmlConsoleController::current_input_text() const
{
    auto* input = dynamic_cast<Rml::ElementFormControlInput*>(element("console-input"));
    if (input == nullptr)
    {
        return {};
    }
    return std::string(input->GetValue());
}

void RmlConsoleController::set_input_text(const std::string& text)
{
    auto* input = dynamic_cast<Rml::ElementFormControlInput*>(element("console-input"));
    if (input == nullptr)
    {
        return;
    }
    input->SetValue(text);
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
