#pragma once

#include "openstrike/core/command_line.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

namespace openstrike
{
enum class AppMode
{
    Client,
    DedicatedServer,
    Editor
};

enum class RendererBackend
{
    Auto,
    Null,
    D3D12
};

struct RuntimeConfig
{
    AppMode mode = AppMode::Client;
    RendererBackend renderer_backend = RendererBackend::Auto;
    std::filesystem::path content_root;
    std::filesystem::path rml_document = "assets/ui/mainmenu.rml";
    std::uint64_t max_frames = 0;
    int max_ticks_per_frame = 8;
    std::uint32_t window_width = 1280;
    std::uint32_t window_height = 720;
    std::uint16_t network_port = 27015;
    double tick_rate = 64.0;
    bool vsync = true;
    bool deterministic_frames = false;
    bool quiet = false;
    std::vector<std::string> startup_commands;

    [[nodiscard]] double tick_interval_seconds() const;

    static RuntimeConfig from_command_line(const CommandLine& command_line);
};

[[nodiscard]] std::string_view to_string(AppMode mode);
[[nodiscard]] std::string_view to_string(RendererBackend backend);
}
