#include "openstrike/core/command_line.hpp"
#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/engine/engine.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/fixed_timestep.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/game/movement.hpp"
#include "openstrike/renderer/null_renderer.hpp"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const char* expression, const char* file, int line)
{
    if (!condition)
    {
        std::cerr << file << ':' << line << ": requirement failed: " << expression << '\n';
        std::exit(1);
    }
}

#define REQUIRE(expression) require((expression), #expression, __FILE__, __LINE__)

void test_command_line_config()
{
    openstrike::CommandLine command_line({
        "--dedicated",
        "--frames=12",
        "--tickrate=128",
        "--content-root=game",
        "--ui-document=ui/mainmenu.rml",
        "--renderer=dx12",
        "--width=1920",
        "--height=1080",
        "--no-vsync",
        "--deterministic",
        "--exec=autoexec.cfg",
        "+map",
        "de_dust2",
        "+sv_cheats",
        "1",
    });
    const openstrike::RuntimeConfig config = openstrike::RuntimeConfig::from_command_line(command_line);

    REQUIRE(config.mode == openstrike::AppMode::DedicatedServer);
    REQUIRE(config.renderer_backend == openstrike::RendererBackend::D3D12);
    REQUIRE(config.max_frames == 12);
    REQUIRE(config.window_width == 1920);
    REQUIRE(config.window_height == 1080);
    REQUIRE(config.rml_document == std::filesystem::path("ui/mainmenu.rml"));
    REQUIRE(!config.vsync);
    REQUIRE(config.deterministic_frames);
    REQUIRE(std::abs(config.tick_rate - 128.0) < 0.001);
    REQUIRE(config.content_root == std::filesystem::path("game"));
    REQUIRE(config.startup_commands.size() == 3);
    REQUIRE(config.startup_commands[0] == "exec autoexec.cfg");
    REQUIRE(config.startup_commands[1] == "map de_dust2");
    REQUIRE(config.startup_commands[2] == "sv_cheats 1");
}

void test_renderer_aliases()
{
    {
        openstrike::CommandLine command_line({"--dx12"});
        const openstrike::RuntimeConfig config = openstrike::RuntimeConfig::from_command_line(command_line);
        REQUIRE(config.renderer_backend == openstrike::RendererBackend::D3D12);
    }

    {
        openstrike::CommandLine command_line({"--null-renderer"});
        const openstrike::RuntimeConfig config = openstrike::RuntimeConfig::from_command_line(command_line);
        REQUIRE(config.renderer_backend == openstrike::RendererBackend::Null);
    }
}

void test_fixed_step_accumulates_ticks()
{
    openstrike::FixedStepAccumulator fixed_step(1.0 / 64.0);

    REQUIRE(fixed_step.consume(1.0 / 128.0, 8) == 0);
    REQUIRE(fixed_step.consume(1.0 / 128.0, 8) == 1);
    REQUIRE(fixed_step.consume(1.0, 128) == 64);
}

void test_fixed_step_clamps_runaway_frames()
{
    openstrike::FixedStepAccumulator fixed_step(1.0 / 64.0);

    REQUIRE(fixed_step.consume(10.0, 4) == 4);
    REQUIRE(fixed_step.interpolation_alpha() >= 0.0);
    REQUIRE(fixed_step.interpolation_alpha() <= 1.0);
}

void test_content_filesystem_path_ids()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_content_fs";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "mod/cfg");
    std::filesystem::create_directories(root / "csgo/cfg");

    {
        std::ofstream(root / "mod/cfg/game.cfg") << "echo mod";
        std::ofstream(root / "csgo/cfg/game.cfg") << "echo game";
    }

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "mod", "GAME");
    filesystem.add_search_path(root / "csgo", "GAME");
    filesystem.add_search_path(root / "platform", "PLATFORM");

    const std::optional<std::filesystem::path> resolved = filesystem.resolve("cfg/game.cfg", "GAME");
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->parent_path().filename() == "cfg");
    REQUIRE(resolved->parent_path().parent_path().filename() == "mod");
    REQUIRE(filesystem.read_text("cfg/game.cfg", "GAME") == "echo mod");
    REQUIRE(filesystem.search_paths("GAME").size() == 2);

    std::filesystem::remove_all(root);
}

void test_command_buffer_cvars_and_quit()
{
    openstrike::RuntimeConfig config;
    config.content_root = std::filesystem::current_path();

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    context.variables.register_variable("sv_cheats", "0", "Cheat toggle.");

    context.command_buffer.add_text("set sv_cheats 1; echo command-buffer; quit");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    const std::size_t executed = context.command_buffer.execute(context.commands, console_context);

    REQUIRE(executed == 3);
    REQUIRE(context.variables.get_bool("sv_cheats"));
    REQUIRE(context.quit_requested);
}

void test_engine_startup_quit_command()
{
    openstrike::RuntimeConfig config;
    config.renderer_backend = openstrike::RendererBackend::Null;
    config.content_root = std::filesystem::current_path();
    config.startup_commands.push_back("quit");

    openstrike::Engine engine(std::make_unique<openstrike::NullRenderer>());
    const openstrike::EngineStats stats = engine.run(config);

    REQUIRE(stats.frame_count == 0);
    REQUIRE(stats.tick_count == 0);
}

void test_ground_movement_accelerates_and_friction_slows()
{
    openstrike::PlayerState player;
    openstrike::MovementTuning tuning;
    openstrike::InputCommand forward;
    forward.move_y = 1.0F;

    for (int tick = 0; tick < 64; ++tick)
    {
        openstrike::simulate_player_move(player, forward, tuning);
    }

    REQUIRE(player.velocity.length_2d() > 200.0F);
    REQUIRE(player.velocity.length_2d() <= tuning.max_ground_speed + 0.01F);

    const float speed_before_friction = player.velocity.length_2d();
    openstrike::simulate_player_move(player, {}, tuning);

    REQUIRE(player.velocity.length_2d() < speed_before_friction);
}

void test_jump_returns_to_ground()
{
    openstrike::PlayerState player;
    openstrike::MovementTuning tuning;
    openstrike::InputCommand jump;
    jump.jump = true;

    openstrike::simulate_player_move(player, jump, tuning);
    REQUIRE(!player.on_ground);
    REQUIRE(player.velocity.z > 0.0F);

    for (int tick = 0; tick < 256; ++tick)
    {
        openstrike::simulate_player_move(player, {}, tuning);
    }

    REQUIRE(player.on_ground);
    REQUIRE(player.origin.z == 0.0F);
}
}

int main()
{
    try
    {
        test_command_line_config();
        test_renderer_aliases();
        test_fixed_step_accumulates_ticks();
        test_fixed_step_clamps_runaway_frames();
        test_content_filesystem_path_ids();
        test_command_buffer_cvars_and_quit();
        test_engine_startup_quit_command();
        test_ground_movement_accelerates_and_friction_slows();
        test_jump_returns_to_ground();
    }
    catch (const std::exception& error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }

    std::cout << "openstrike tests passed\n";
    return 0;
}
