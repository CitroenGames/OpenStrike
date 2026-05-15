#include "openstrike/core/command_line.hpp"
#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/engine/engine.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/fixed_timestep.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/game/movement.hpp"
#include "openstrike/renderer/null_renderer.hpp"
#include "openstrike/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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

void write_u32_le(std::vector<unsigned char>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<unsigned char>(value & 0xFFU);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    bytes[offset + 2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
    bytes[offset + 3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
}

void write_u16_le(std::vector<unsigned char>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<unsigned char>(value & 0xFFU);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
}

void write_s16_le(std::vector<unsigned char>& bytes, std::size_t offset, std::int16_t value)
{
    write_u16_le(bytes, offset, static_cast<std::uint16_t>(value));
}

void write_f32_le(std::vector<unsigned char>& bytes, std::size_t offset, float value)
{
    std::uint32_t raw = 0;
    static_assert(sizeof(raw) == sizeof(value));
    std::memcpy(&raw, &value, sizeof(raw));
    write_u32_le(bytes, offset, raw);
}

void write_lump(std::vector<unsigned char>& bytes, std::size_t lump, std::size_t offset, std::size_t length)
{
    write_u32_le(bytes, 8 + (lump * 16), static_cast<std::uint32_t>(offset));
    write_u32_le(bytes, 12 + (lump * 16), static_cast<std::uint32_t>(length));
}

void write_minimal_bsp(const std::filesystem::path& path, const std::string& entities)
{
    constexpr std::size_t lump_count = 64;
    constexpr std::size_t header_size = 8 + (lump_count * 16) + 4;
    std::vector<unsigned char> bytes(header_size + entities.size(), 0);

    write_u32_le(bytes, 0, 0x50534256U);
    write_u32_le(bytes, 4, 21);
    write_u32_le(bytes, 8, static_cast<std::uint32_t>(header_size));
    write_u32_le(bytes, 12, static_cast<std::uint32_t>(entities.size()));
    write_u32_le(bytes, 8 + (lump_count * 16), 3);

    std::copy(entities.begin(), entities.end(), bytes.begin() + header_size);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_square_bsp(const std::filesystem::path& path, const std::string& entities, std::uint32_t surface_flags = 0)
{
    constexpr std::size_t lump_count = 64;
    constexpr std::size_t header_size = 8 + (lump_count * 16) + 4;
    std::vector<unsigned char> bytes(header_size, 0);

    write_u32_le(bytes, 0, 0x50534256U);
    write_u32_le(bytes, 4, 21);
    write_u32_le(bytes, 8 + (lump_count * 16), 4);

    auto append_lump = [&](std::size_t lump, const std::vector<unsigned char>& data) {
        const std::size_t offset = bytes.size();
        bytes.insert(bytes.end(), data.begin(), data.end());
        write_lump(bytes, lump, offset, data.size());
    };

    append_lump(0, std::vector<unsigned char>(entities.begin(), entities.end()));

    std::vector<unsigned char> vertices(4 * 12, 0);
    const std::array<openstrike::Vec3, 4> points{{
        {-64.0F, -64.0F, 32.0F},
        {64.0F, -64.0F, 32.0F},
        {64.0F, 64.0F, 32.0F},
        {-64.0F, 64.0F, 32.0F},
    }};
    for (std::size_t index = 0; index < points.size(); ++index)
    {
        write_f32_le(vertices, (index * 12) + 0, points[index].x);
        write_f32_le(vertices, (index * 12) + 4, points[index].y);
        write_f32_le(vertices, (index * 12) + 8, points[index].z);
    }
    append_lump(3, vertices);

    std::vector<unsigned char> texinfo(72, 0);
    write_u32_le(texinfo, 64, surface_flags);
    append_lump(6, texinfo);

    std::vector<unsigned char> edges(5 * 4, 0);
    const std::array<std::array<std::uint16_t, 2>, 5> edge_indices{{
        {0, 0},
        {0, 1},
        {1, 2},
        {2, 3},
        {3, 0},
    }};
    for (std::size_t index = 0; index < edge_indices.size(); ++index)
    {
        write_u16_le(edges, (index * 4) + 0, edge_indices[index][0]);
        write_u16_le(edges, (index * 4) + 2, edge_indices[index][1]);
    }
    append_lump(12, edges);

    std::vector<unsigned char> surfedges(4 * 4, 0);
    write_u32_le(surfedges, 0, 1);
    write_u32_le(surfedges, 4, 2);
    write_u32_le(surfedges, 8, 3);
    write_u32_le(surfedges, 12, 4);
    append_lump(13, surfedges);

    std::vector<unsigned char> faces(56, 0);
    faces[2] = 0;
    faces[3] = 1;
    write_u32_le(faces, 4, 0);
    write_s16_le(faces, 8, 4);
    write_s16_le(faces, 10, 0);
    write_s16_le(faces, 12, -1);
    std::fill(faces.begin() + 16, faces.begin() + 20, static_cast<unsigned char>(0xFF));
    write_f32_le(faces, 24, 128.0F * 128.0F);
    append_lump(7, faces);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

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

void test_world_manager_loads_source_bsp()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_world_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "\"mapversion\" \"7\"\n"
        "}\n"
        "{\n"
        "\"classname\" \"info_player_terrorist\"\n"
        "\"origin\" \"10 20 30\"\n"
        "\"angles\" \"0 90 0\"\n"
        "}\n";
    write_minimal_bsp(root / "game/maps/test_world.bsp", entities);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::WorldManager world;
    REQUIRE(world.load_map("test_world", filesystem));

    const openstrike::LoadedWorld* loaded = world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->kind == openstrike::WorldAssetKind::SourceBsp);
    REQUIRE(loaded->name == "test_world");
    REQUIRE(loaded->asset_version == 21);
    REQUIRE(loaded->map_revision == 3);
    REQUIRE(loaded->entity_count == 2);
    REQUIRE(loaded->worldspawn.at("mapversion") == "7");
    REQUIRE(loaded->spawn_points.size() == 1);
    REQUIRE(loaded->spawn_points[0].origin.x == 10.0F);
    REQUIRE(loaded->spawn_points[0].origin.y == 20.0F);
    REQUIRE(loaded->spawn_points[0].origin.z == 30.0F);

    const std::vector<std::string> maps = world.list_maps(filesystem, "test");
    REQUIRE(std::find(maps.begin(), maps.end(), "test_world") != maps.end());

    std::filesystem::remove_all(root);
}

void test_world_manager_builds_bsp_mesh_and_floor()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_world_mesh_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n"
        "{\n"
        "\"classname\" \"info_player_start\"\n"
        "\"origin\" \"0 0 96\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/mesh_world.bsp", entities);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::WorldManager world;
    REQUIRE(world.load_map("mesh_world", filesystem));

    const openstrike::LoadedWorld* loaded = world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->mesh.vertices.size() == 6);
    REQUIRE(loaded->mesh.indices.size() == 6);
    REQUIRE(loaded->mesh.collision_triangles.size() == 2);
    REQUIRE(loaded->mesh.has_bounds);
    REQUIRE(loaded->mesh.bounds_min.z == 32.0F);
    REQUIRE(loaded->mesh.bounds_max.z == 32.0F);

    const std::optional<float> floor_z = openstrike::find_floor_z(*loaded, openstrike::Vec3{0.0F, 0.0F, 96.0F}, 128.0F);
    REQUIRE(floor_z.has_value());
    REQUIRE(*floor_z == 32.0F);

    std::filesystem::remove_all(root);
}

void test_map_command_loads_current_world()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_map_command_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n"
        "{\n"
        "\"classname\" \"info_player_counterterrorist\"\n"
        "\"origin\" \"4 5 6\"\n"
        "}\n";
    write_minimal_bsp(root / "game/maps/command_world.bsp", entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);

    context.command_buffer.add_text("map command_world");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    const openstrike::LoadedWorld* loaded = context.world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == "command_world");
    REQUIRE(context.variables.get_string("mapname") == "command_world");
    REQUIRE(context.variables.get_string("host_map") == "command_world");

    std::filesystem::remove_all(root);
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
        test_world_manager_loads_source_bsp();
        test_world_manager_builds_bsp_mesh_and_floor();
        test_map_command_loads_current_world();
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
