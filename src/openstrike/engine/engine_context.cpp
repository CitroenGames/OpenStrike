#include "openstrike/engine/engine_context.hpp"

#include "openstrike/core/log.hpp"

#include <filesystem>
#include <string>

namespace openstrike
{
namespace
{
std::string join_args(const std::vector<std::string>& args)
{
    std::string text;
    for (const std::string& arg : args)
    {
        if (!text.empty())
        {
            text += ' ';
        }
        text += arg;
    }
    return text;
}

void configure_filesystem(ContentFileSystem& filesystem, const RuntimeConfig& config)
{
    filesystem.clear();
    filesystem.add_search_path(config.content_root, "MOD");
    filesystem.add_search_path(config.content_root, "GAME");

    const std::filesystem::path csgo_root = config.content_root / "csgo";
    filesystem.add_search_path(csgo_root, "GAME");
    filesystem.add_search_path(config.content_root / "platform", "PLATFORM");
}

void register_default_variables(ConsoleVariables& variables, const RuntimeConfig& config)
{
    variables.register_variable("host_tickrate", std::to_string(config.tick_rate), "Fixed simulation ticks per second.");
    variables.register_variable("host_max_ticks_per_frame", std::to_string(config.max_ticks_per_frame), "Maximum catch-up ticks per host frame.");
    variables.register_variable("host_framerate", config.deterministic_frames ? "60" : "0", "Forced deterministic frame rate. Zero means realtime.");
    variables.register_variable("r_backend", std::string(to_string(config.renderer_backend)), "Selected renderer backend.");
    variables.register_variable("r_vsync", config.vsync ? "1" : "0", "Vertical synchronization toggle.");
    variables.register_variable("r_width", std::to_string(config.window_width), "Requested render width.");
    variables.register_variable("r_height", std::to_string(config.window_height), "Requested render height.");
    variables.register_variable("con_enable", "1", "Allows opening the RmlUi console.");
    variables.register_variable("host_map", "", "Current loaded world name.");
    variables.register_variable("mapname", "", "Current loaded world name.");
    variables.register_variable("game_content_root", config.content_root.string(), "Root directory for mounted game content.");
    variables.register_variable("game_ui_document", config.rml_document.generic_string(), "Initial RmlUi document.");
}

bool run_map_command(const CommandInvocation& invocation, ConsoleCommandContext& context, std::string_view command_name)
{
    if (invocation.args.empty())
    {
        log_warning("usage: {} <mapname>", command_name);
        return false;
    }

    if (context.filesystem == nullptr || context.world == nullptr)
    {
        log_warning("{} failed: world loading services are not available", command_name);
        return false;
    }

    if (!context.world->load_map(invocation.args[0], *context.filesystem))
    {
        return false;
    }

    if (const LoadedWorld* world = context.world->current_world())
    {
        context.variables.set("host_map", world->name);
        context.variables.set("mapname", world->name);
    }

    return true;
}

void list_maps_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (context.filesystem == nullptr || context.world == nullptr)
    {
        log_warning("maps failed: world loading services are not available");
        return;
    }

    const std::string filter = invocation.args.empty() ? "*" : invocation.args[0];
    const std::vector<std::string> maps = context.world->list_maps(*context.filesystem, filter);
    if (maps.empty())
    {
        log_info("no maps found for '{}'", filter);
        return;
    }

    for (const std::string& map : maps)
    {
        log_info("{}", map);
    }
}

void register_default_commands(CommandRegistry& commands)
{
    commands.register_command("quit", "Requests engine shutdown.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.request_quit)
        {
            context.request_quit();
        }
    });

    commands.register_command("echo", "Writes text to the engine log.", [](const CommandInvocation& invocation, ConsoleCommandContext&) {
        log_info("{}", join_args(invocation.args));
    });

    commands.register_command("set", "Sets a console variable.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (invocation.args.size() < 2)
        {
            log_warning("usage: set <cvar> <value>");
            return;
        }

        const std::string value = join_args(std::vector<std::string>(invocation.args.begin() + 1, invocation.args.end()));
        if (!context.variables.set(invocation.args[0], value))
        {
            log_warning("unknown cvar '{}'", invocation.args[0]);
        }
    });

    commands.register_command("cvarlist", "Lists registered console variables.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        for (const ConsoleVariable& variable : context.variables.variables())
        {
            log_info("{} = \"{}\" - {}", variable.name, variable.value, variable.description);
        }
    });

    commands.register_command("cmdlist", "Lists registered console commands.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.registry == nullptr)
        {
            return;
        }

        for (const ConsoleCommand& command : context.registry->commands())
        {
            log_info("{} - {}", command.name, command.description);
        }
    });

    commands.register_command("path", "Lists mounted content search paths.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.filesystem == nullptr)
        {
            return;
        }

        for (const SearchPath& path : context.filesystem->search_paths())
        {
            log_info("{}: {}", path.path_id, path.root.string());
        }
    });

    commands.register_command("map", "Start playing on the specified map.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        run_map_command(invocation, context, "map");
    });

    commands.register_command("changelevel", "Change the current world to the specified map.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        run_map_command(invocation, context, "changelevel");
    });

    commands.register_command("maps", "Lists available Source BSP and OpenStrike level maps.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        list_maps_command(invocation, context);
    });

    commands.register_command("exec", "Executes a cfg file through the command buffer.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (invocation.args.empty())
        {
            log_warning("usage: exec <file>");
            return;
        }

        if (context.filesystem == nullptr)
        {
            log_warning("no filesystem mounted for exec");
            return;
        }

        try
        {
            context.command_buffer.insert_text(context.filesystem->read_text(invocation.args[0], "GAME"));
            log_info("exec '{}'", invocation.args[0]);
        }
        catch (const std::exception& error)
        {
            log_warning("exec failed for '{}': {}", invocation.args[0], error.what());
        }
    });
}
}

ConsoleCommandContext EngineContext::console_context()
{
    return ConsoleCommandContext{
        .variables = variables,
        .command_buffer = command_buffer,
        .registry = &commands,
        .filesystem = &filesystem,
        .world = &world,
        .request_quit = [this] {
            request_quit();
        },
    };
}

void EngineContext::request_quit()
{
    quit_requested = true;
}

void configure_engine_context(EngineContext& context, const RuntimeConfig& config)
{
    context = EngineContext{};
    configure_filesystem(context.filesystem, config);
    register_default_variables(context.variables, config);
    register_default_commands(context.commands);

    for (const std::string& command : config.startup_commands)
    {
        context.command_buffer.add_text(command);
    }
}
}
