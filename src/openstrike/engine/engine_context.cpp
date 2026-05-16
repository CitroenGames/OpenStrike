#include "openstrike/engine/engine_context.hpp"

#include "openstrike/core/log.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

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

std::optional<std::uint16_t> parse_port(std::string_view text)
{
    unsigned parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0 || parsed > 65535U)
    {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parsed);
}

std::string lower_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string trim_copy(std::string_view text)
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

void normalize_slashes(std::string& path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
}

bool has_suffix(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && lower_copy(text.substr(text.size() - suffix.size())) == suffix;
}

std::string remove_suffix(std::string text, std::string_view suffix)
{
    if (has_suffix(text, suffix))
    {
        text.resize(text.size() - suffix.size());
    }
    return text;
}

std::string canonical_map_argument(std::string_view map_name)
{
    std::string name = trim_copy(map_name);
    normalize_slashes(name);
    if (name.rfind("maps/", 0) == 0)
    {
        name.erase(0, 5);
    }
    if (name.rfind("assets/levels/", 0) == 0)
    {
        name.erase(0, 14);
    }
    name = remove_suffix(std::move(name), ".bsp");
    name = remove_suffix(std::move(name), ".level.json");
    return name;
}

void disconnect_client_connection(ConsoleCommandContext& context)
{
    if (context.network == nullptr)
    {
        return;
    }

    context.network->disconnect_client(0);
    context.variables.set("net_status", std::string(to_string(context.network->client().state())));
}

void stop_network_session(ConsoleCommandContext& context)
{
    if (context.network == nullptr)
    {
        return;
    }

    disconnect_client_connection(context);
    context.network->stop_server();
}

void clear_loaded_world_variables(ConsoleVariables& variables)
{
    variables.set("host_map", "");
    variables.set("mapname", "");
}

void update_loaded_world_variables(ConsoleVariables& variables, const LoadedWorld& world)
{
    variables.set("host_map", world.name);
    variables.set("mapname", world.name);
}

std::optional<std::string> resolve_map_argument(const CommandInvocation& invocation, ConsoleCommandContext& context, std::string_view failure_prefix)
{
    std::string requested = canonical_map_argument(invocation.args[0]);
    if (requested.empty())
    {
        log_warning("{} failed: no map specified", failure_prefix);
        return std::nullopt;
    }

    const std::vector<std::string> all_maps = context.world->list_maps(*context.filesystem, "*");
    const std::string requested_lower = lower_copy(requested);
    for (const std::string& map : all_maps)
    {
        if (lower_copy(map) == requested_lower)
        {
            return map;
        }
    }

    const std::vector<std::string> fuzzy_matches = context.world->list_maps(*context.filesystem, requested);
    if (!fuzzy_matches.empty())
    {
        return fuzzy_matches.front();
    }

    (void)failure_prefix;
    return requested;
}

void disconnect_host_session(ConsoleCommandContext& context)
{
    if (context.audio != nullptr)
    {
        context.audio->stop_all_sounds();
    }

    stop_network_session(context);

    if (context.world != nullptr)
    {
        context.world->unload();
    }

    clear_loaded_world_variables(context.variables);
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
    variables.register_variable("net_port", std::to_string(config.network_port), "UDP port for dedicated/listen server networking.");
    variables.register_variable("net_status", "disconnected", "Current client network connection state.");
    variables.register_variable("game_content_root", config.content_root.string(), "Root directory for mounted game content.");
    variables.register_variable("game_ui_document", config.rml_document.generic_string(), "Initial RmlUi document.");
    AudioSystem::register_variables(variables);
}

bool load_host_map(const std::string& map_name, ConsoleCommandContext& context)
{
    if (!context.world->load_map(map_name, *context.filesystem))
    {
        return false;
    }

    if (const LoadedWorld* world = context.world->current_world())
    {
        update_loaded_world_variables(context.variables, *world);
    }

    if (context.audio != nullptr)
    {
        context.audio->stop_all_sounds();
    }

    return true;
}

bool run_changelevel_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (invocation.args.empty())
    {
        log_info("changelevel <levelname> : continue game on a new level");
        return false;
    }

    if (context.filesystem == nullptr || context.world == nullptr)
    {
        log_warning("changelevel failed: world loading services are not available");
        return false;
    }

    if (context.world->current_world() == nullptr)
    {
        log_info("Can't changelevel, not running server");
        return false;
    }

    const std::optional<std::string> map_name = resolve_map_argument(invocation, context, "changelevel");
    if (!map_name)
    {
        return false;
    }

    return load_host_map(*map_name, context);
}

bool run_map_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (invocation.args.empty())
    {
        log_warning("No map specified");
        return false;
    }

    if (context.filesystem == nullptr || context.world == nullptr)
    {
        log_warning("map failed: world loading services are not available");
        return false;
    }

    if (context.world->current_world() != nullptr && context.network != nullptr && context.network->server().is_running())
    {
        return run_changelevel_command(invocation, context);
    }

    const std::optional<std::string> map_name = resolve_map_argument(invocation, context, "map load");
    if (!map_name)
    {
        return false;
    }

    if (!load_host_map(*map_name, context))
    {
        return false;
    }

    disconnect_client_connection(context);

    return true;
}

void list_maps_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (context.filesystem == nullptr || context.world == nullptr)
    {
        log_warning("maps failed: world loading services are not available");
        return;
    }

    if (invocation.args.empty() || invocation.args.size() > 2)
    {
        log_info("Usage:  maps <substring>");
        log_info("maps * for full listing");
        return;
    }

    const std::string filter = invocation.args[0] == "*" ? "*" : invocation.args[0];
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

void net_status_command(const CommandInvocation&, ConsoleCommandContext& context)
{
    if (context.network == nullptr)
    {
        log_warning("network services are not available");
        return;
    }

    const NetworkServer& server = context.network->server();
    const NetworkClient& client = context.network->client();
    log_info("network client state={} remote={} id={} local_port={}",
        to_string(client.state()),
        client.remote_address().to_string(),
        client.connection_id(),
        client.local_port());
    log_info("network server running={} port={} clients={}",
        server.is_running() ? "yes" : "no",
        server.local_port(),
        server.clients().size());
}

void connect_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (context.network == nullptr)
    {
        log_warning("connect failed: network services are not available");
        return;
    }
    if (invocation.args.empty())
    {
        log_warning("usage: connect <ipv4[:port]>");
        return;
    }

    const int default_port = context.variables.get_int("net_port", 27015);
    const std::optional<NetworkAddress> address = parse_network_address(invocation.args[0], static_cast<std::uint16_t>(std::clamp(default_port, 1, 65535)));
    if (!address)
    {
        log_warning("connect failed: invalid address '{}'", invocation.args[0]);
        return;
    }

    if (context.network->connect_client(*address, 0))
    {
        log_info("connecting to {}", address->to_string());
    }
}

void disconnect_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (!invocation.args.empty())
    {
        log_info("disconnect reason: {}", invocation.args[0]);
    }
    disconnect_host_session(context);
    log_info("disconnected from active game");
}

void quit_command(const CommandInvocation&, ConsoleCommandContext& context)
{
    disconnect_host_session(context);
    if (context.request_quit)
    {
        context.request_quit();
    }
}

void net_listen_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (context.network == nullptr)
    {
        log_warning("net_listen failed: network services are not available");
        return;
    }

    std::uint16_t port = static_cast<std::uint16_t>(std::clamp(context.variables.get_int("net_port", 27015), 1, 65535));
    if (!invocation.args.empty())
    {
        const std::optional<std::uint16_t> parsed = parse_port(invocation.args[0]);
        if (!parsed)
        {
            log_warning("usage: net_listen [port]");
            return;
        }
        port = *parsed;
        context.variables.set("net_port", std::to_string(port));
    }

    if (context.network->start_server(port))
    {
        log_info("network server listening on UDP port {}", context.network->server().local_port());
    }
}

void net_say_command(const CommandInvocation& invocation, ConsoleCommandContext& context)
{
    if (context.network == nullptr)
    {
        log_warning("net_say failed: network services are not available");
        return;
    }
    if (invocation.args.empty())
    {
        log_warning("usage: net_say <message>");
        return;
    }

    const std::string text = join_args(invocation.args);
    if (context.network->client().is_connected())
    {
        context.network->send_client_text(text, 0);
    }
    if (context.network->server().is_running())
    {
        context.network->broadcast_server_text(text, 0);
    }
}

void register_default_commands(CommandRegistry& commands)
{
    commands.register_command("quit", "Disconnects and requests engine shutdown.", quit_command);
    commands.register_command("exit", "Exit the engine.", quit_command);

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

    commands.register_command("map", "Start playing on the specified map.", run_map_command);

    commands.register_command("changelevel", "Change server to the specified map.", run_changelevel_command);

    commands.register_command("maps", "Lists available Source BSP and OpenStrike level maps.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        list_maps_command(invocation, context);
    });

    commands.register_command("connect", "Connect to an OpenStrike UDP server.", connect_command);
    commands.register_command("disconnect", "Disconnect from the active game and unload the current world.", disconnect_command);
    commands.register_command("net_listen", "Start a UDP listen server.", net_listen_command);
    commands.register_command("net_status", "Print client/server network state.", net_status_command);
    commands.register_command("net_say", "Send a text message through the active network session.", net_say_command);
    AudioSystem::register_commands(commands);

    commands.register_command("clear", "Clears the console output.", [](const CommandInvocation&, ConsoleCommandContext&) {
        Logger::instance().clear_history();
    });

    commands.register_command("condump", "Dumps the console output to a text file under the active mod path.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        std::filesystem::path output_dir;
        if (context.filesystem != nullptr)
        {
            const std::vector<SearchPath> mod_paths = context.filesystem->search_paths("MOD");
            if (!mod_paths.empty())
            {
                output_dir = mod_paths.front().root;
            }
        }
        if (output_dir.empty())
        {
            output_dir = std::filesystem::current_path();
        }

        std::filesystem::path target;
        if (!invocation.args.empty())
        {
            target = output_dir / invocation.args[0];
        }
        else
        {
            for (int index = 0; index < 1000; ++index)
            {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "condump%03d.txt", index);
                std::filesystem::path candidate = output_dir / buffer;
                std::error_code ec;
                if (!std::filesystem::exists(candidate, ec))
                {
                    target = std::move(candidate);
                    break;
                }
            }
            if (target.empty())
            {
                log_warning("condump failed: too many existing dump files");
                return;
            }
        }

        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);

        std::ofstream stream(target, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            log_warning("condump failed: cannot open '{}'", target.string());
            return;
        }

        const std::vector<std::string> lines = Logger::instance().recent_lines(4096);
        for (const std::string& line : lines)
        {
            stream.write(line.data(), static_cast<std::streamsize>(line.size()));
            stream.put('\n');
        }

        log_info("console dumped to {}", target.string());
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
        .network = &network,
        .audio = &audio,
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
