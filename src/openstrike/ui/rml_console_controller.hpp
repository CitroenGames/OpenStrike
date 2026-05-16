#pragma once

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <cstddef>
#include <deque>
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
    struct CompletionEntry
    {
        std::string name;
        std::string display;
    };

    void on_submit(Rml::Event& event);
    void on_input_keydown(Rml::Event& event);
    void on_input_change();
    void on_completion_click(Rml::Event& event);

    void submit_command(std::string command);
    void refresh_output(bool force = false);
    void rebuild_completions();
    void render_completions();
    void cycle_completion(int direction);
    void apply_common_prefix();
    void apply_completion(std::size_t index);
    void set_selected_completion(std::size_t index);
    void clear_completions();
    void clear_input();
    void push_history(const std::string& command);
    void echo_submitted(const std::string& command);
    [[nodiscard]] std::string current_input_text() const;
    void set_input_text(const std::string& text);

    [[nodiscard]] Rml::Element* element(const char* id) const;

    EngineContext* engine_context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    bool visible_ = false;

    std::size_t last_entry_count_ = 0;

    std::deque<std::string> command_history_;
    int history_cursor_ = -1;

    std::vector<CompletionEntry> completions_;
    std::size_t completion_index_ = 0;
    std::string partial_text_;
    bool autocomplete_mode_ = false;
    bool suppress_input_change_ = false;
};
}
