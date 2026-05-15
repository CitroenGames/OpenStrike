#include "openstrike/engine/runtime_config.hpp"

#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace openstrike
{
namespace
{
std::uint64_t parse_u64(const std::string& value, std::string_view option_name)
{
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        throw std::invalid_argument("invalid integer value for --" + std::string(option_name));
    }

    return parsed;
}

double parse_double(const std::string& value, std::string_view option_name)
{
    double parsed = 0.0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        throw std::invalid_argument("invalid number value for --" + std::string(option_name));
    }

    return parsed;
}

RendererBackend parse_renderer_backend(const std::string& value)
{
    if (value == "auto")
    {
        return RendererBackend::Auto;
    }

    if (value == "null")
    {
        return RendererBackend::Null;
    }

    if (value == "dx12" || value == "d3d12")
    {
        return RendererBackend::D3D12;
    }

    throw std::invalid_argument("invalid renderer backend '" + value + "'");
}

std::filesystem::path default_content_root()
{
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    if (!error)
    {
        if (std::filesystem::is_regular_file(current / "assets/ui/mainmenu.rml", error) ||
            std::filesystem::is_regular_file(current / "csgo/resource/ui/mainmenu.rml", error))
        {
            return current;
        }

        if (std::filesystem::is_regular_file(current / "content/assets/ui/mainmenu.rml", error))
        {
            return current / "content";
        }
    }

#if defined(_WIN32)
    const std::filesystem::path local_content = "I:/content";
    if (std::filesystem::is_directory(local_content, error))
    {
        return local_content;
    }
#endif

    return "content";
}
}

double RuntimeConfig::tick_interval_seconds() const
{
    return 1.0 / tick_rate;
}

RuntimeConfig RuntimeConfig::from_command_line(const CommandLine& command_line)
{
    RuntimeConfig config;
    config.content_root = default_content_root();

    if (command_line.has_flag("dedicated"))
    {
        config.mode = AppMode::DedicatedServer;
    }

    if (command_line.has_flag("editor"))
    {
        config.mode = AppMode::Editor;
    }

    if (const auto frames = command_line.option("frames"))
    {
        config.max_frames = parse_u64(*frames, "frames");
    }

    if (const auto renderer = command_line.option("renderer"))
    {
        config.renderer_backend = parse_renderer_backend(*renderer);
    }

    if (command_line.has_flag("dx12") || command_line.has_flag("d3d12"))
    {
        config.renderer_backend = RendererBackend::D3D12;
    }

    if (command_line.has_flag("null-renderer"))
    {
        config.renderer_backend = RendererBackend::Null;
    }

    if (const auto tick_rate = command_line.option("tickrate"))
    {
        config.tick_rate = parse_double(*tick_rate, "tickrate");
        if (config.tick_rate <= 0.0)
        {
            throw std::invalid_argument("--tickrate must be positive");
        }
    }

    if (const auto max_ticks = command_line.option("max-ticks-per-frame"))
    {
        config.max_ticks_per_frame = static_cast<int>(parse_u64(*max_ticks, "max-ticks-per-frame"));
        if (config.max_ticks_per_frame <= 0)
        {
            throw std::invalid_argument("--max-ticks-per-frame must be positive");
        }
    }

    if (const auto content_root = command_line.option("content-root"))
    {
        config.content_root = *content_root;
    }

    if (const auto rml_document = command_line.option("rml"))
    {
        config.rml_document = *rml_document;
    }

    if (const auto rml_document = command_line.option("ui-document"))
    {
        config.rml_document = *rml_document;
    }

    if (const auto exec_file = command_line.option("exec"))
    {
        config.startup_commands.push_back("exec " + *exec_file);
    }

    if (const auto width = command_line.option("width"))
    {
        config.window_width = static_cast<std::uint32_t>(parse_u64(*width, "width"));
        if (config.window_width == 0)
        {
            throw std::invalid_argument("--width must be positive");
        }
    }

    if (const auto height = command_line.option("height"))
    {
        config.window_height = static_cast<std::uint32_t>(parse_u64(*height, "height"));
        if (config.window_height == 0)
        {
            throw std::invalid_argument("--height must be positive");
        }
    }

    config.vsync = !command_line.has_flag("no-vsync");
    config.deterministic_frames = command_line.has_flag("deterministic");
    if (command_line.has_flag("realtime"))
    {
        config.deterministic_frames = false;
    }
    for (const std::string& command : command_line.startup_commands())
    {
        config.startup_commands.push_back(command);
    }
    config.quiet = command_line.has_flag("quiet");
    return config;
}

std::string_view to_string(AppMode mode)
{
    switch (mode)
    {
    case AppMode::Client:
        return "client";
    case AppMode::DedicatedServer:
        return "dedicated";
    case AppMode::Editor:
        return "editor";
    }

    return "unknown";
}

std::string_view to_string(RendererBackend backend)
{
    switch (backend)
    {
    case RendererBackend::Auto:
        return "auto";
    case RendererBackend::Null:
        return "null";
    case RendererBackend::D3D12:
        return "dx12";
    }

    return "unknown";
}
}
