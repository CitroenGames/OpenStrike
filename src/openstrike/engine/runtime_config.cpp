#include "openstrike/engine/runtime_config.hpp"

#include <charconv>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

    if (value == "metal")
    {
        return RendererBackend::Metal;
    }

    throw std::invalid_argument("invalid renderer backend '" + value + "'");
}

std::optional<std::filesystem::path> environment_path(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr)
    {
        return std::nullopt;
    }

    std::string result(value);
    std::free(value);
    if (result.empty())
    {
        return std::nullopt;
    }

    return result;
#else
    if (const char* value = std::getenv(name))
    {
        if (*value != '\0')
        {
            return value;
        }
    }

    return std::nullopt;
#endif
}

bool has_content_marker(const std::filesystem::path& root)
{
    std::error_code error;
    return std::filesystem::is_regular_file(root / "assets/ui/mainmenu.rml", error) ||
           std::filesystem::is_regular_file(root / "csgo/resource/ui/mainmenu.rml", error);
}

std::optional<std::filesystem::path> executable_directory()
{
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size == 0)
    {
        return std::nullopt;
    }

    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        return std::nullopt;
    }

    std::error_code error;
    std::filesystem::path executable = std::filesystem::weakly_canonical(buffer.data(), error);
    if (error)
    {
        executable = std::filesystem::path(buffer.data());
    }

    return executable.parent_path();
#else
    return std::nullopt;
#endif
}

std::filesystem::path default_content_root()
{
    if (const auto env_content_root = environment_path("OPENSTRIKE_CONTENT_ROOT"))
    {
        return *env_content_root;
    }

    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    if (!error)
    {
        if (has_content_marker(current))
        {
            return current;
        }

        if (has_content_marker(current / "content"))
        {
            return current / "content";
        }
    }

    if (const auto executable_root = executable_directory())
    {
        if (has_content_marker(*executable_root))
        {
            return *executable_root;
        }

        if (has_content_marker(*executable_root / "content"))
        {
            return *executable_root / "content";
        }
    }

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

    if (command_line.has_flag("metal"))
    {
        config.renderer_backend = RendererBackend::Metal;
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

    if (const auto port = command_line.option("net-port"))
    {
        const std::uint64_t parsed = parse_u64(*port, "net-port");
        if (parsed == 0 || parsed > 65535)
        {
            throw std::invalid_argument("--net-port must be between 1 and 65535");
        }
        config.network_port = static_cast<std::uint16_t>(parsed);
    }

    if (const auto port = command_line.option("port"))
    {
        const std::uint64_t parsed = parse_u64(*port, "port");
        if (parsed == 0 || parsed > 65535)
        {
            throw std::invalid_argument("--port must be between 1 and 65535");
        }
        config.network_port = static_cast<std::uint16_t>(parsed);
    }

    if (const auto connect = command_line.option("connect"))
    {
        config.startup_commands.push_back("connect " + *connect);
    }

    config.vsync = !command_line.has_flag("no-vsync");
    config.dx12_profile = command_line.has_flag("dx12-profile");
    config.dx12_async_recording = !command_line.has_flag("dx12-no-async-recording");
    if (const auto profile_path = command_line.option("dx12-profile-path"))
    {
        config.dx12_profile = true;
        config.dx12_profile_path = *profile_path;
    }
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
    case RendererBackend::Metal:
        return "metal";
    }

    return "unknown";
}
}
