#include "openstrike/core/command_line.hpp"
#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/audio/audio_system.hpp"
#include "openstrike/engine/engine.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/fixed_timestep.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/engine/sdl_input.hpp"
#include "openstrike/game/fps_controller.hpp"
#include "openstrike/game/game_simulation.hpp"
#include "openstrike/game/movement.hpp"
#include "openstrike/material/material_system.hpp"
#include "openstrike/network/network_protocol.hpp"
#include "openstrike/network/network_session.hpp"
#include "openstrike/network/network_stream.hpp"
#include "openstrike/physics/physics_world.hpp"
#include "openstrike/renderer/null_renderer.hpp"
#include "openstrike/renderer/world_material_shader.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_fgd.hpp"
#include "openstrike/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

void append_u32_le(std::vector<unsigned char>& bytes, std::uint32_t value)
{
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 4);
    write_u32_le(bytes, offset, value);
}

void write_u16_le(std::vector<unsigned char>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<unsigned char>(value & 0xFFU);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
}

void append_u16_le(std::vector<unsigned char>& bytes, std::uint16_t value)
{
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 2);
    write_u16_le(bytes, offset, value);
}

void write_s16_le(std::vector<unsigned char>& bytes, std::size_t offset, std::int16_t value)
{
    write_u16_le(bytes, offset, static_cast<std::uint16_t>(value));
}

void write_s32_le(std::vector<unsigned char>& bytes, std::size_t offset, std::int32_t value)
{
    write_u32_le(bytes, offset, static_cast<std::uint32_t>(value));
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

void append_stored_zip_file(std::vector<unsigned char>& bytes, const std::string& name, const std::string& contents)
{
    append_u32_le(bytes, 0x04034B50U);
    append_u16_le(bytes, 20);
    append_u16_le(bytes, 0);
    append_u16_le(bytes, 0);
    append_u16_le(bytes, 0);
    append_u16_le(bytes, 0);
    append_u32_le(bytes, 0);
    append_u32_le(bytes, static_cast<std::uint32_t>(contents.size()));
    append_u32_le(bytes, static_cast<std::uint32_t>(contents.size()));
    append_u16_le(bytes, static_cast<std::uint16_t>(name.size()));
    append_u16_le(bytes, 0);
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.insert(bytes.end(), contents.begin(), contents.end());
}

std::vector<unsigned char> make_minimal_vtf(std::uint16_t width, std::uint16_t height, std::int32_t image_format, std::vector<unsigned char> image_bytes)
{
    constexpr std::size_t header_size = 80;
    std::vector<unsigned char> bytes(header_size, 0);
    bytes[0] = 'V';
    bytes[1] = 'T';
    bytes[2] = 'F';
    bytes[3] = '\0';
    write_u32_le(bytes, 4, 7);
    write_u32_le(bytes, 8, 2);
    write_u32_le(bytes, 12, static_cast<std::uint32_t>(header_size));
    write_u16_le(bytes, 16, width);
    write_u16_le(bytes, 18, height);
    write_u16_le(bytes, 24, 1);
    write_s32_le(bytes, 52, image_format);
    bytes[56] = 1;
    write_s32_le(bytes, 57, -1);
    bytes.insert(bytes.end(), image_bytes.begin(), image_bytes.end());
    return bytes;
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

std::uint32_t fourcc_test(std::string_view text)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[3])) << 24U);
}

void write_vec3_le(std::vector<unsigned char>& bytes, std::size_t offset, openstrike::Vec3 value)
{
    write_f32_le(bytes, offset, value.x);
    write_f32_le(bytes, offset + 4, value.y);
    write_f32_le(bytes, offset + 8, value.z);
}

void write_vec2_le(std::vector<unsigned char>& bytes, std::size_t offset, openstrike::Vec2 value)
{
    write_f32_le(bytes, offset, value.x);
    write_f32_le(bytes, offset + 4, value.y);
}

void write_zero_string(std::vector<unsigned char>& bytes, std::size_t offset, std::string_view value)
{
    if (offset + value.size() + 1 > bytes.size())
    {
        bytes.resize(offset + value.size() + 1, 0);
    }
    std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    bytes[offset + value.size()] = 0;
}

void write_minimal_source_model(const std::filesystem::path& path, openstrike::Vec3 mins, openstrike::Vec3 maxs)
{
    std::filesystem::create_directories(path.parent_path());
    constexpr std::uint32_t checksum = 0x12345678U;

    std::vector<unsigned char> bytes(1024, 0);
    bytes[0] = 'I';
    bytes[1] = 'D';
    bytes[2] = 'S';
    bytes[3] = 'T';
    write_u32_le(bytes, 4, 49);
    write_u32_le(bytes, 8, checksum);
    write_u32_le(bytes, 76, static_cast<std::uint32_t>(bytes.size()));
    write_vec3_le(bytes, 104, mins);
    write_vec3_le(bytes, 116, maxs);
    write_vec3_le(bytes, 128, mins);
    write_vec3_le(bytes, 140, maxs);
    write_u32_le(bytes, 204, 2);
    write_u32_le(bytes, 208, 256);
    write_u32_le(bytes, 212, 1);
    write_u32_le(bytes, 216, 440);
    write_u32_le(bytes, 220, 1);
    write_u32_le(bytes, 224, 3);
    write_u32_le(bytes, 228, 480);
    write_u32_le(bytes, 232, 1);
    write_u32_le(bytes, 236, 512);

    write_u32_le(bytes, 256, 128);
    write_u32_le(bytes, 320, 88);
    write_zero_string(bytes, 384, "crate_body");
    write_zero_string(bytes, 408, "crate_blue");
    write_u32_le(bytes, 440, 448);
    write_zero_string(bytes, 448, "models/test/props/");
    write_s16_le(bytes, 480, 0);
    write_s16_le(bytes, 482, 0);
    write_s16_le(bytes, 484, 1);

    write_u32_le(bytes, 512 + 4, 1);
    write_u32_le(bytes, 512 + 12, 16);
    const std::size_t model = 528;
    write_zero_string(bytes, model, "crate");
    write_f32_le(bytes, model + 68, 16.0F);
    write_u32_le(bytes, model + 72, 1);
    write_u32_le(bytes, model + 76, 148);
    write_u32_le(bytes, model + 80, 3);
    write_u32_le(bytes, model + 84, 0);
    const std::size_t mesh = model + 148;
    write_u32_le(bytes, mesh, 0);
    write_u32_le(bytes, mesh + 8, 3);
    write_u32_le(bytes, mesh + 12, 0);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    std::vector<unsigned char> vvd(64 + (3 * 48), 0);
    vvd[0] = 'I';
    vvd[1] = 'D';
    vvd[2] = 'S';
    vvd[3] = 'V';
    write_u32_le(vvd, 4, 4);
    write_u32_le(vvd, 8, checksum);
    write_u32_le(vvd, 12, 1);
    write_u32_le(vvd, 16, 3);
    write_u32_le(vvd, 56, 64);
    auto write_vertex = [&](std::size_t index, openstrike::Vec3 position, openstrike::Vec3 normal, openstrike::Vec2 texcoord) {
        const std::size_t vertex = 64 + (index * 48);
        vvd[vertex + 15] = 1;
        write_vec3_le(vvd, vertex + 16, position);
        write_vec3_le(vvd, vertex + 28, normal);
        write_vec2_le(vvd, vertex + 40, texcoord);
    };
    write_vertex(0, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F});
    write_vertex(1, {8.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F});
    write_vertex(2, {0.0F, 0.0F, 16.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F});
    std::ofstream vvd_file(path.parent_path() / (path.stem().string() + ".vvd"), std::ios::binary);
    vvd_file.write(reinterpret_cast<const char*>(vvd.data()), static_cast<std::streamsize>(vvd.size()));

    std::vector<unsigned char> vtx(174, 0);
    write_u32_le(vtx, 0, 7);
    write_u16_le(vtx, 8, 32);
    write_u16_le(vtx, 10, 3);
    write_u32_le(vtx, 12, 3);
    write_u32_le(vtx, 16, checksum);
    write_u32_le(vtx, 20, 1);
    write_u32_le(vtx, 28, 1);
    write_u32_le(vtx, 32, 36);
    write_u32_le(vtx, 36, 1);
    write_u32_le(vtx, 40, 8);
    write_u32_le(vtx, 44, 1);
    write_u32_le(vtx, 48, 8);
    write_u32_le(vtx, 52, 1);
    write_u32_le(vtx, 56, 12);
    write_u32_le(vtx, 64, 1);
    write_u32_le(vtx, 68, 9);
    const std::size_t strip_group = 73;
    write_u32_le(vtx, strip_group, 3);
    write_u32_le(vtx, strip_group + 4, 33);
    write_u32_le(vtx, strip_group + 8, 3);
    write_u32_le(vtx, strip_group + 12, 60);
    write_u32_le(vtx, strip_group + 16, 1);
    write_u32_le(vtx, strip_group + 20, 66);
    write_u16_le(vtx, strip_group + 33 + 4, 0);
    write_u16_le(vtx, strip_group + 42 + 4, 1);
    write_u16_le(vtx, strip_group + 51 + 4, 2);
    write_u16_le(vtx, strip_group + 60, 0);
    write_u16_le(vtx, strip_group + 62, 1);
    write_u16_le(vtx, strip_group + 64, 2);
    const std::size_t strip = strip_group + 66;
    write_u32_le(vtx, strip, 3);
    write_u32_le(vtx, strip + 8, 3);
    vtx[strip + 18] = 0x01;
    std::ofstream vtx_file(path.parent_path() / (path.stem().string() + ".dx90.vtx"), std::ios::binary);
    vtx_file.write(reinterpret_cast<const char*>(vtx.data()), static_cast<std::streamsize>(vtx.size()));
}

void write_static_prop_bsp(const std::filesystem::path& path, const std::string& entities)
{
    constexpr std::size_t lump_count = 64;
    constexpr std::size_t header_size = 8 + (lump_count * 16) + 4;
    std::vector<unsigned char> bytes(header_size, 0);

    write_u32_le(bytes, 0, 0x50534256U);
    write_u32_le(bytes, 4, 21);
    write_u32_le(bytes, 8 + (lump_count * 16), 5);

    auto append_lump = [&](std::size_t lump, const std::vector<unsigned char>& data) {
        const std::size_t offset = bytes.size();
        bytes.insert(bytes.end(), data.begin(), data.end());
        write_lump(bytes, lump, offset, data.size());
    };

    append_lump(0, std::vector<unsigned char>(entities.begin(), entities.end()));

    std::vector<unsigned char> static_prop_data;
    append_u32_le(static_prop_data, 1);
    const std::string model_name = "models/test/crate.mdl";
    const std::size_t name_offset = static_prop_data.size();
    static_prop_data.resize(name_offset + 128, 0);
    std::copy(model_name.begin(), model_name.end(), static_prop_data.begin() + name_offset);
    append_u32_le(static_prop_data, 0);
    append_u32_le(static_prop_data, 1);

    const std::size_t record = static_prop_data.size();
    static_prop_data.resize(record + 76, 0);
    write_vec3_le(static_prop_data, record + 0, {100.0F, 200.0F, 64.0F});
    write_vec3_le(static_prop_data, record + 12, {0.0F, 90.0F, 0.0F});
    write_u16_le(static_prop_data, record + 24, 0);
    static_prop_data[record + 30] = 6;
    static_prop_data[record + 31] = 0;
    write_u32_le(static_prop_data, record + 32, 2);
    write_vec3_le(static_prop_data, record + 44, {100.0F, 200.0F, 80.0F});
    write_f32_le(static_prop_data, record + 56, 1.0F);
    static_prop_data[record + 64] = 64;
    static_prop_data[record + 65] = 128;
    static_prop_data[record + 66] = 255;
    static_prop_data[record + 67] = 200;
    write_u32_le(static_prop_data, record + 72, 4);

    const std::size_t game_lump_offset = bytes.size();
    const std::size_t static_prop_offset = game_lump_offset + 20;
    std::vector<unsigned char> game_lump;
    append_u32_le(game_lump, 1);
    append_u32_le(game_lump, fourcc_test("sprp"));
    append_u16_le(game_lump, 0);
    append_u16_le(game_lump, 10);
    append_u32_le(game_lump, static_cast<std::uint32_t>(static_prop_offset));
    append_u32_le(game_lump, static_cast<std::uint32_t>(static_prop_data.size()));
    game_lump.insert(game_lump.end(), static_prop_data.begin(), static_prop_data.end());
    append_lump(35, game_lump);

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

    std::vector<unsigned char> texdata(32, 0);
    write_u32_le(texdata, 12, 0);
    write_u32_le(texdata, 16, 128);
    write_u32_le(texdata, 20, 128);
    write_u32_le(texdata, 24, 128);
    write_u32_le(texdata, 28, 128);
    append_lump(2, texdata);

    std::vector<unsigned char> texinfo(72, 0);
    write_f32_le(texinfo, 0, 1.0F);
    write_f32_le(texinfo, 12, 64.0F);
    write_f32_le(texinfo, 20, 1.0F);
    write_f32_le(texinfo, 28, 64.0F);
    write_u32_le(texinfo, 64, surface_flags);
    append_lump(6, texinfo);

    const std::string texture_name = "test/floor";
    append_lump(43, std::vector<unsigned char>(texture_name.begin(), texture_name.end()));
    bytes.push_back(0);
    write_lump(bytes, 43, bytes.size() - 1 - texture_name.size(), texture_name.size() + 1);

    std::vector<unsigned char> string_table(4, 0);
    append_lump(44, string_table);

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

    std::vector<unsigned char> pakfile;
    append_stored_zip_file(pakfile, "materials/test/floor.vmt", "\"LightmappedGeneric\" { \"$basetexture\" \"test/floor\" }");
    append_lump(40, pakfile);

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
        "--net-port=27016",
        "--connect=127.0.0.1:27016",
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
    REQUIRE(config.network_port == 27016);
    REQUIRE(config.rml_document == std::filesystem::path("ui/mainmenu.rml"));
    REQUIRE(!config.vsync);
    REQUIRE(config.deterministic_frames);
    REQUIRE(std::abs(config.tick_rate - 128.0) < 0.001);
    REQUIRE(config.content_root == std::filesystem::path("game"));
    REQUIRE(config.startup_commands.size() == 4);
    REQUIRE(config.startup_commands[0] == "exec autoexec.cfg");
    REQUIRE(config.startup_commands[1] == "connect 127.0.0.1:27016");
    REQUIRE(config.startup_commands[2] == "map de_dust2");
    REQUIRE(config.startup_commands[3] == "sv_cheats 1");
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

    {
        openstrike::CommandLine command_line({"--metal"});
        const openstrike::RuntimeConfig config = openstrike::RuntimeConfig::from_command_line(command_line);
        REQUIRE(config.renderer_backend == openstrike::RendererBackend::Metal);
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

void test_material_system_resolves_source_vmt_without_shader_combos()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_material_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "materials/base");
    std::filesystem::create_directories(root / "materials/maps");

    {
        std::ofstream(root / "materials/base/wall.vmt") <<
            "\"LightmappedGeneric\"\n"
            "{\n"
            "    \"$basetexture\" \"brick/wall01\"\n"
            "    \"$bumpmap\" \"brick/wall01_normal\"\n"
            "    \"$alphatest\" \"1\"\n"
            "    \"$color\" \"[0.25 0.5 1]\"\n"
            "}\n";

        std::ofstream(root / "materials/maps/wall_patch.vmt") <<
            "\"Patch\"\n"
            "{\n"
            "    \"include\" \"materials/base/wall.vmt\"\n"
            "    \"replace\"\n"
            "    {\n"
            "        \"$basetexture\" \"brick/wall02\"\n"
            "        \"$selfillum\" \"1\"\n"
            "        \"$alpha\" \"0.5\"\n"
            "    }\n"
            "}\n";
    }

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root, "GAME");
    openstrike::SourceAssetStore source_assets(filesystem);
    openstrike::MaterialSystem materials(source_assets);

    const openstrike::LoadedMaterial loaded = materials.load_world_material("maps/wall_patch");
    REQUIRE(loaded.definition.found_source_material);
    REQUIRE(loaded.definition.shader == "lightmappedgeneric");
    REQUIRE(loaded.definition.base_texture == "brick/wall02");
    REQUIRE(loaded.definition.normal_texture == "brick/wall01_normal");
    REQUIRE((loaded.definition.constants.flags & openstrike::MaterialFlags::AlphaTest) != 0);
    REQUIRE((loaded.definition.constants.flags & openstrike::MaterialFlags::Unlit) != 0);
    REQUIRE((loaded.definition.constants.flags & openstrike::MaterialFlags::NormalMap) != 0);
    REQUIRE((loaded.definition.constants.flags & openstrike::MaterialFlags::Translucent) != 0);
    REQUIRE(loaded.using_fallback_texture);
    REQUIRE(loaded.base_texture.width == 8);
    REQUIRE(std::abs(loaded.definition.constants.base_color[0] - 0.25F) < 0.001F);
    REQUIRE(std::abs(loaded.definition.constants.base_color[1] - 0.5F) < 0.001F);
    REQUIRE(std::abs(loaded.definition.constants.base_color[2] - 1.0F) < 0.001F);
    REQUIRE(std::abs(loaded.definition.constants.base_color[3] - 0.5F) < 0.001F);

    const openstrike::LoadedMaterial tool = materials.load_world_material("tools/toolsnodraw");
    REQUIRE((tool.definition.constants.flags & openstrike::MaterialFlags::Tool) != 0);
    REQUIRE((tool.definition.constants.flags & openstrike::MaterialFlags::NoDraw) != 0);

    const openstrike::LoadedMaterial sky = materials.load_world_material("skybox/sky_openstrike_testrt");
    REQUIRE((sky.definition.constants.flags & openstrike::MaterialFlags::Sky) != 0);
    REQUIRE((sky.definition.constants.flags & openstrike::MaterialFlags::Unlit) != 0);

    std::filesystem::remove_all(root);
}

void test_source_texture_decodes_compressed_vtf_to_rgba()
{
    constexpr std::int32_t image_format_dxt1 = 13;
    constexpr std::int32_t image_format_dxt5 = 15;

    std::vector<unsigned char> dxt1_block(8, 0);
    write_u16_le(dxt1_block, 0, 0xF800U);
    write_u16_le(dxt1_block, 2, 0x07E0U);
    write_u32_le(dxt1_block, 4, 0);

    const std::optional<openstrike::SourceTexture> bc1_texture = openstrike::load_vtf_texture(make_minimal_vtf(4, 4, image_format_dxt1, dxt1_block));
    REQUIRE(bc1_texture.has_value());
    REQUIRE(bc1_texture->format == openstrike::SourceTextureFormat::Bc1);

    const std::optional<openstrike::SourceTexture> bc1_rgba = openstrike::source_texture_to_rgba8(*bc1_texture);
    REQUIRE(bc1_rgba.has_value());
    REQUIRE(bc1_rgba->format == openstrike::SourceTextureFormat::Rgba8);
    REQUIRE(bc1_rgba->mips.size() == 1);
    REQUIRE(bc1_rgba->mips[0].bytes.size() == 4 * 4 * 4);
    REQUIRE(bc1_rgba->mips[0].bytes[0] == 255);
    REQUIRE(bc1_rgba->mips[0].bytes[1] == 0);
    REQUIRE(bc1_rgba->mips[0].bytes[2] == 0);
    REQUIRE(bc1_rgba->mips[0].bytes[3] == 255);

    std::vector<unsigned char> dxt5_block(16, 0);
    dxt5_block[0] = 128;
    dxt5_block[1] = 0;
    write_u16_le(dxt5_block, 8, 0xF800U);
    write_u16_le(dxt5_block, 10, 0x07E0U);
    write_u32_le(dxt5_block, 12, 0);

    const std::optional<openstrike::SourceTexture> bc3_texture = openstrike::load_vtf_texture(make_minimal_vtf(4, 4, image_format_dxt5, dxt5_block));
    REQUIRE(bc3_texture.has_value());
    REQUIRE(bc3_texture->format == openstrike::SourceTextureFormat::Bc3);

    const std::optional<openstrike::SourceTexture> bc3_rgba = openstrike::source_texture_to_rgba8(*bc3_texture);
    REQUIRE(bc3_rgba.has_value());
    REQUIRE(bc3_rgba->mips[0].bytes[0] == 255);
    REQUIRE(bc3_rgba->mips[0].bytes[1] == 0);
    REQUIRE(bc3_rgba->mips[0].bytes[2] == 0);
    REQUIRE(bc3_rgba->mips[0].bytes[3] == 128);
}

void test_renderer_shader_files_are_dedicated()
{
    const openstrike::Dx12ShaderFile world_vertex = openstrike::world_material_vertex_shader_file();
    const openstrike::Dx12ShaderFile world_pixel = openstrike::world_material_pixel_shader_file();
    REQUIRE(std::string_view(world_vertex.source_path) == "shaders/world_material.hlsl");
    REQUIRE(std::string_view(world_pixel.source_path) == "shaders/world_material.hlsl");
    REQUIRE(std::string_view(world_vertex.entry_point) == "VSMain");
    REQUIRE(std::string_view(world_pixel.entry_point) == "PSMain");
    REQUIRE(std::string_view(world_vertex.compiled_path).find("world_material.vs.cso") != std::string_view::npos);
    REQUIRE(std::string_view(world_pixel.compiled_path).find("world_material.ps.cso") != std::string_view::npos);

    const openstrike::Dx12ShaderFile skybox_vertex = openstrike::skybox_vertex_shader_file();
    const openstrike::Dx12ShaderFile skybox_pixel = openstrike::skybox_pixel_shader_file();
    REQUIRE(std::string_view(skybox_vertex.source_path) == "shaders/skybox.hlsl");
    REQUIRE(std::string_view(skybox_pixel.source_path) == "shaders/skybox.hlsl");
    REQUIRE(std::string_view(skybox_vertex.entry_point) == "VSMain");
    REQUIRE(std::string_view(skybox_pixel.entry_point) == "PSMain");
    REQUIRE(std::string_view(skybox_vertex.compiled_path).find("skybox.vs.cso") != std::string_view::npos);
    REQUIRE(std::string_view(skybox_pixel.compiled_path).find("skybox.ps.cso") != std::string_view::npos);
}

void test_network_stream_and_protocol_roundtrip()
{
    openstrike::NetworkByteWriter writer;
    writer.write_u8(7);
    writer.write_u16(27015);
    writer.write_u32(0x11223344U);
    writer.write_u64(0x0102030405060708ULL);
    REQUIRE(writer.write_string("payload"));

    openstrike::NetworkByteReader reader(writer.bytes());
    std::uint8_t small = 0;
    std::uint16_t port = 0;
    std::uint32_t id = 0;
    std::uint64_t tick = 0;
    std::string text;
    REQUIRE(reader.read_u8(small));
    REQUIRE(reader.read_u16(port));
    REQUIRE(reader.read_u32(id));
    REQUIRE(reader.read_u64(tick));
    REQUIRE(reader.read_string(text));
    REQUIRE(reader.empty());
    REQUIRE(small == 7);
    REQUIRE(port == 27015);
    REQUIRE(id == 0x11223344U);
    REQUIRE(tick == 0x0102030405060708ULL);
    REQUIRE(text == "payload");

    const std::vector<unsigned char> payload = openstrike::make_text_payload("hello");
    const std::vector<unsigned char> packet = openstrike::encode_network_packet(openstrike::NetworkMessageType::Text, 3, 2, 64, payload);
    const std::optional<openstrike::NetworkPacket> decoded = openstrike::decode_network_packet(packet);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->header.type == openstrike::NetworkMessageType::Text);
    REQUIRE(decoded->header.sequence == 3);
    REQUIRE(decoded->header.ack == 2);
    REQUIRE(decoded->header.tick == 64);
    REQUIRE(openstrike::read_text_payload(decoded->payload) == "hello");

    std::vector<unsigned char> corrupt = packet;
    corrupt[0] = 0;
    REQUIRE(!openstrike::decode_network_packet(corrupt).has_value());
}

void test_network_udp_loopback_connects_and_exchanges_text()
{
    openstrike::NetworkServer server;
    REQUIRE(server.start(0));
    const std::vector<openstrike::NetworkEvent> startup_events = server.drain_events();
    REQUIRE(!startup_events.empty());

    openstrike::NetworkClient client;
    REQUIRE(client.connect(openstrike::NetworkAddress::localhost(server.local_port()), 1));

    bool server_saw_client = false;
    bool client_connected = false;
    for (int attempt = 0; attempt < 64 && (!server_saw_client || !client_connected); ++attempt)
    {
        server.poll(static_cast<std::uint64_t>(attempt + 2));
        for (const openstrike::NetworkEvent& event : server.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::ClientConnected)
            {
                server_saw_client = true;
            }
        }

        client.poll(static_cast<std::uint64_t>(attempt + 2));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::ConnectedToServer)
            {
                client_connected = true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(server_saw_client);
    REQUIRE(client_connected);
    REQUIRE(client.is_connected());
    REQUIRE(server.clients().size() == 1);

    REQUIRE(client.send_text("client hello", 100));
    bool server_received_text = false;
    for (int attempt = 0; attempt < 64 && !server_received_text; ++attempt)
    {
        server.poll(static_cast<std::uint64_t>(100 + attempt));
        for (const openstrike::NetworkEvent& event : server.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::TextReceived && event.text == "client hello")
            {
                server_received_text = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(server_received_text);

    server.broadcast_text("server hello", 200);
    bool client_received_text = false;
    for (int attempt = 0; attempt < 64 && !client_received_text; ++attempt)
    {
        client.poll(static_cast<std::uint64_t>(200 + attempt));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::TextReceived && event.text == "server hello")
            {
                client_received_text = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client_received_text);

    client.disconnect(300);
    server.stop();
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

void test_audio_sound_chars_match_source_prefix_rules()
{
    REQUIRE(openstrike::AudioSystem::is_sound_char('*'));
    REQUIRE(openstrike::AudioSystem::is_sound_char('~'));
    REQUIRE(openstrike::AudioSystem::is_sound_char('+'));
    REQUIRE(!openstrike::AudioSystem::is_sound_char('w'));
    REQUIRE(openstrike::AudioSystem::skip_sound_chars("*~#weapons/ak47.wav") == "weapons/ak47.wav");
    REQUIRE(openstrike::AudioSystem::test_sound_char("*~#weapons/ak47.wav", '~'));
    REQUIRE(!openstrike::AudioSystem::test_sound_char("weapons/ak47.wav", '~'));
}

void test_audio_source_soundlevel_gain_falls_with_distance()
{
    const float near_gain = openstrike::AudioSystem::gain_from_sound_level(75, 64.0F);
    const float far_gain = openstrike::AudioSystem::gain_from_sound_level(75, 4096.0F);
    REQUIRE(near_gain > far_gain);
    REQUIRE(far_gain > 0.0F);
    REQUIRE(openstrike::AudioSystem::sound_level_to_distance_multiplier(0) == 0.0F);
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
        "\"skyname\" \"sky_openstrike_test\"\n"
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
    REQUIRE(loaded->worldspawn.at("skyname") == "sky_openstrike_test");
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
    REQUIRE(loaded->mesh.materials.size() == 1);
    REQUIRE(loaded->mesh.materials[0].name == "test/floor");
    REQUIRE(loaded->mesh.materials[0].width == 128);
    REQUIRE(loaded->mesh.materials[0].height == 128);
    REQUIRE(loaded->mesh.batches.size() == 1);
    REQUIRE(loaded->mesh.batches[0].index_count == 6);
    REQUIRE(!loaded->mesh.has_sky_surfaces);
    REQUIRE(loaded->embedded_assets.size() == 1);
    REQUIRE(loaded->embedded_assets.find("materials/test/floor.vmt") != loaded->embedded_assets.end());
    REQUIRE(loaded->mesh.collision_triangles.size() == 2);
    REQUIRE(loaded->mesh.has_bounds);
    REQUIRE(loaded->mesh.bounds_min.z == 32.0F);
    REQUIRE(loaded->mesh.bounds_max.z == 32.0F);

    float min_u = loaded->mesh.vertices[0].texcoord.x;
    float max_u = loaded->mesh.vertices[0].texcoord.x;
    float min_v = loaded->mesh.vertices[0].texcoord.y;
    float max_v = loaded->mesh.vertices[0].texcoord.y;
    for (const openstrike::WorldMeshVertex& vertex : loaded->mesh.vertices)
    {
        min_u = std::min(min_u, vertex.texcoord.x);
        max_u = std::max(max_u, vertex.texcoord.x);
        min_v = std::min(min_v, vertex.texcoord.y);
        max_v = std::max(max_v, vertex.texcoord.y);
    }
    REQUIRE(std::abs(min_u - 0.0F) < 0.001F);
    REQUIRE(std::abs(max_u - 1.0F) < 0.001F);
    REQUIRE(std::abs(min_v - 0.0F) < 0.001F);
    REQUIRE(std::abs(max_v - 1.0F) < 0.001F);

    const std::optional<float> floor_z = openstrike::find_floor_z(*loaded, openstrike::Vec3{0.0F, 0.0F, 96.0F}, 128.0F);
    REQUIRE(floor_z.has_value());
    REQUIRE(*floor_z == 32.0F);

    std::filesystem::remove_all(root);
}

void test_world_manager_tracks_source_sky_surfaces()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_world_sky_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "\"skyname\" \"sky_openstrike_test\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/sky_world.bsp", entities, 0x0004U);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::WorldManager world;
    REQUIRE(world.load_map("sky_world", filesystem));

    const openstrike::LoadedWorld* loaded = world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->mesh.has_sky_surfaces);
    REQUIRE(loaded->worldspawn.at("skyname") == "sky_openstrike_test");
    REQUIRE(loaded->mesh.vertices.empty());
    REQUIRE(loaded->mesh.indices.empty());
    REQUIRE(loaded->mesh.collision_triangles.empty());

    std::filesystem::remove_all(root);
}

void test_world_manager_loads_static_props_and_renders_bounds()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_static_prop_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_static_prop_bsp(root / "game/maps/static_props.bsp", entities);
    write_minimal_source_model(root / "game/models/test/crate.mdl", {-4.0F, -8.0F, 0.0F}, {4.0F, 8.0F, 16.0F});

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");
    openstrike::WorldManager manager;
    REQUIRE(manager.load_map("static_props", filesystem));

    const openstrike::LoadedWorld* loaded = manager.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->props.size() == 1);
    REQUIRE(loaded->props[0].kind == openstrike::WorldPropKind::StaticProp);
    REQUIRE(loaded->props[0].model_path == "models/test/crate.mdl");
    REQUIRE(loaded->props[0].skin == 2);
    REQUIRE(loaded->props[0].solid == 6);
    REQUIRE(loaded->props[0].flags_ex == 4);
    REQUIRE(loaded->props[0].model_bounds_loaded);
    REQUIRE(loaded->props[0].model_material_loaded);
    REQUIRE(loaded->props[0].model_mesh_loaded);
    REQUIRE(loaded->props[0].material_name == "models/test/props/crate_blue");
    REQUIRE(std::abs(loaded->props[0].color[0] - (64.0F / 255.0F)) < 0.001F);
    REQUIRE(std::abs(loaded->props[0].color[3] - (200.0F / 255.0F)) < 0.001F);
    REQUIRE(loaded->mesh.materials.size() == 1);
    REQUIRE(loaded->mesh.materials[0].name == "models/test/props/crate_blue");
    REQUIRE(loaded->mesh.vertices.size() == 3);
    REQUIRE(loaded->mesh.indices.size() == 3);
    REQUIRE(loaded->mesh.batches.size() == 1);
    REQUIRE(loaded->mesh.has_bounds);
    REQUIRE(loaded->mesh.bounds_max.z == 80.0F);

    std::filesystem::remove_all(root);
}

void test_source_fgd_loads_classes_and_metadata()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_fgd_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream(root / "base.fgd") <<
            "@BaseClass size(-8 -8 -8, 8 8 8) color(10 20 30) = Targetname\n"
            "[\n"
            "    targetname(target_source) : \"Name\" : : \"Entity name\"\n"
            "    spawnflags(flags) =\n"
            "    [\n"
            "        1 : \"Start enabled\" : 1\n"
            "        4 : \"Silent\" : 0\n"
            "    ]\n"
            "    input Enable(void) : \"Enable entity\"\n"
            "    output OnTrigger(void) : \"Entity fired\"\n"
            "]\n";

        std::ofstream(root / "game.fgd") <<
            "@include \"base.fgd\"\n"
            "@mapsize(-4096, 4096)\n"
            "@gridnav(64, 2, 3, 1000)\n"
            "@materialexclusion [ \"debug\" \"tools\" ]\n"
            "@AutoVisGroup = \"Gameplay\"\n"
            "[\n"
            "    \"Spawns\"\n"
            "    [\n"
            "        \"info_player_start\"\n"
            "        \"info_player_teamspawn\"\n"
            "    ]\n"
            "]\n"
            "@PointClass base(Targetname) halfgridsnap studio(\"models/editor/playerstart.mdl\") = info_player_start : \"Player start\"\n"
            "[\n"
            "    origin(origin) : \"Origin\" : \"0 0 64\" : \"Spawn position\"\n"
            "    team(choices) : \"Team\" : \"2\" =\n"
            "    [\n"
            "        2 : \"Terrorists\"\n"
            "        3 : \"Counter-Terrorists\"\n"
            "    ]\n"
            "    enabled(boolean) : \"Enabled\" : \"yes\"\n"
            "    input SetTeam(integer) : \"Set team\"\n"
            "    output OnSpawn(void) : \"Spawned\"\n"
            "]\n"
            "@SolidClass base(Targetname) = func_buyzone\n"
            "[\n"
            "]\n";
    }

    openstrike::SourceFgdGameData game_data;
    REQUIRE(game_data.load_file(root / "game.fgd"));
    REQUIRE(game_data.errors().empty());
    REQUIRE(game_data.classes().size() == 3);
    REQUIRE(game_data.min_map_coord() == -4096);
    REQUIRE(game_data.max_map_coord() == 4096);
    REQUIRE(game_data.grid_nav().has_value());
    REQUIRE(game_data.grid_nav()->edge_size == 64);
    REQUIRE(game_data.grid_nav()->offset_x == 2);
    REQUIRE(game_data.grid_nav()->offset_y == 3);
    REQUIRE(game_data.grid_nav()->trace_height == 1000);
    REQUIRE(game_data.material_exclusions().size() == 2);
    REQUIRE(game_data.auto_vis_groups().size() == 1);
    REQUIRE(game_data.auto_vis_groups()[0].classes.size() == 1);
    REQUIRE(game_data.auto_vis_groups()[0].classes[0].entities.size() == 2);

    const openstrike::SourceFgdEntityClass* player_start = game_data.find_class("INFO_PLAYER_START");
    REQUIRE(player_start != nullptr);
    REQUIRE(player_start->kind == openstrike::SourceFgdClassKind::Point);
    REQUIRE(player_start->description == "Player start");
    REQUIRE(player_start->half_grid_snap);
    REQUIRE(player_start->has_size);
    REQUIRE(player_start->mins.x == -8.0F);
    REQUIRE(player_start->maxs.z == 8.0F);
    REQUIRE(player_start->has_color);
    REQUIRE(player_start->color.r == 10);
    REQUIRE(player_start->color.g == 20);
    REQUIRE(player_start->color.b == 30);
    REQUIRE(player_start->bases.size() == 1);
    REQUIRE(player_start->helpers.size() == 1);
    REQUIRE(player_start->helpers[0].name == "studio");
    REQUIRE(player_start->helpers[0].parameters[0] == "models/editor/playerstart.mdl");
    REQUIRE(player_start->inputs.size() == 2);
    REQUIRE(player_start->outputs.size() == 2);

    const auto find_var = [&](std::string_view name) -> const openstrike::SourceFgdVariable* {
        const auto it = std::find_if(player_start->variables.begin(), player_start->variables.end(), [&](const openstrike::SourceFgdVariable& variable) {
            return variable.name == name;
        });
        return it == player_start->variables.end() ? nullptr : &*it;
    };

    const openstrike::SourceFgdVariable* targetname = find_var("targetname");
    REQUIRE(targetname != nullptr);
    REQUIRE(targetname->type == openstrike::SourceFgdValueType::TargetSource);
    REQUIRE(targetname->description == "Entity name");

    const openstrike::SourceFgdVariable* spawnflags = find_var("spawnflags");
    REQUIRE(spawnflags != nullptr);
    REQUIRE(spawnflags->type == openstrike::SourceFgdValueType::Flags);
    REQUIRE(spawnflags->default_value == "1");
    REQUIRE(spawnflags->choices.size() == 2);

    const openstrike::SourceFgdVariable* team = find_var("team");
    REQUIRE(team != nullptr);
    REQUIRE(team->type == openstrike::SourceFgdValueType::Choices);
    REQUIRE(team->default_value == "2");
    REQUIRE(team->choices.size() == 2);
    REQUIRE(team->choices[1].caption == "Counter-Terrorists");

    const openstrike::SourceFgdVariable* enabled = find_var("enabled");
    REQUIRE(enabled != nullptr);
    REQUIRE(enabled->type == openstrike::SourceFgdValueType::Choices);
    REQUIRE(enabled->default_value == "1");
    REQUIRE(enabled->choices.size() == 2);

    const openstrike::SourceFgdEntityClass* buyzone = game_data.find_class("func_buyzone");
    REQUIRE(buyzone != nullptr);
    REQUIRE(buyzone->kind == openstrike::SourceFgdClassKind::Solid);
    REQUIRE(buyzone->variables.size() == 2);

    std::filesystem::remove_all(root);
}

void test_source_fgd_accepts_concatenated_source_strings()
{
    openstrike::SourceFgdGameData game_data;
    const char* text =
        "@BaseClass = Targetname\n"
        "[\n"
        "    targetname(target_source) : \"Name\" : : \"First \" + \"second\"\n"
        "]\n"
        "@PointClass base(Targetname) studio(\"models/editor/playerstart.mdl\") = info_player_start : \"Player \" + \"start\"\n"
        "[\n"
        "    team(choices) : \"Team\" : 2 : \"Team \" + \"selector\" =\n"
        "    [\n"
        "        2 : \"Terrorist \" + \"spawn\"\n"
        "        3 : \"Counter-Terrorist\"\n"
        "    ]\n"
        "]\n";

    const bool loaded = game_data.load_text(text, "concat.fgd");
    if (!loaded)
    {
        for (const std::string& error : game_data.errors())
            std::cerr << error << '\n';
    }
    REQUIRE(loaded);
    REQUIRE(game_data.errors().empty());

    const openstrike::SourceFgdEntityClass* player_start = game_data.find_class("info_player_start");
    REQUIRE(player_start != nullptr);
    REQUIRE(player_start->description == "Player start");

    const auto find_var = [&](std::string_view name) -> const openstrike::SourceFgdVariable* {
        const auto it = std::find_if(player_start->variables.begin(), player_start->variables.end(), [&](const openstrike::SourceFgdVariable& variable) {
            return variable.name == name;
        });
        return it == player_start->variables.end() ? nullptr : &*it;
    };

    const openstrike::SourceFgdVariable* targetname = find_var("targetname");
    REQUIRE(targetname != nullptr);
    REQUIRE(targetname->description == "First second");

    const openstrike::SourceFgdVariable* team = find_var("team");
    REQUIRE(team != nullptr);
    REQUIRE(team->description == "Team selector");
    REQUIRE(team->choices.size() == 2);
    REQUIRE(team->choices[0].caption == "Terrorist spawn");
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
    REQUIRE(context.network.start_server(0));
    REQUIRE(context.network.connect_client(openstrike::NetworkAddress::localhost(27099), 0));

    context.command_buffer.add_text("map command_world.bsp");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    const openstrike::LoadedWorld* loaded = context.world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == "command_world");
    REQUIRE(context.variables.get_string("mapname") == "command_world");
    REQUIRE(context.variables.get_string("host_map") == "command_world");
    REQUIRE(context.network.client().state() == openstrike::NetworkConnectionState::Disconnected);
    REQUIRE(context.network.server().is_running());

    context.network.stop_server();
    std::filesystem::remove_all(root);
}

void test_map_and_changelevel_match_source_server_rules()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_source_map_command_rules";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string first_entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "\"mapversion\" \"1\"\n"
        "}\n";
    const std::string second_entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "\"mapversion\" \"2\"\n"
        "}\n";
    write_minimal_bsp(root / "game/maps/source_first.bsp", first_entities);
    write_minimal_bsp(root / "game/maps/source_second.bsp", second_entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext inactive_context;
    openstrike::configure_engine_context(inactive_context, config);
    inactive_context.command_buffer.add_text("changelevel source_second");
    openstrike::ConsoleCommandContext inactive_console_context = inactive_context.console_context();
    inactive_context.command_buffer.execute(inactive_context.commands, inactive_console_context);
    REQUIRE(inactive_context.world.current_world() == nullptr);

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    REQUIRE(context.world.load_map("source_first", context.filesystem));
    context.variables.set("host_map", "source_first");
    context.variables.set("mapname", "source_first");
    REQUIRE(context.network.start_server(0));
    REQUIRE(context.network.connect_client(openstrike::NetworkAddress::localhost(27099), 0));

    context.command_buffer.add_text("map second");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    const openstrike::LoadedWorld* loaded = context.world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == "source_second");
    REQUIRE(context.variables.get_string("mapname") == "source_second");
    REQUIRE(context.network.server().is_running());
    REQUIRE(context.network.client().state() == openstrike::NetworkConnectionState::Connecting);

    context.network.stop_server();
    context.network.disconnect_client(0);
    std::filesystem::remove_all(root);
}

void test_disconnect_command_unloads_world_and_network()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_disconnect_command_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_minimal_bsp(root / "game/maps/disconnect_world.bsp", entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    REQUIRE(context.world.load_map("disconnect_world", context.filesystem));
    context.variables.set("host_map", "disconnect_world");
    context.variables.set("mapname", "disconnect_world");
    REQUIRE(context.network.start_server(0));
    REQUIRE(context.network.server().is_running());

    context.command_buffer.add_text("disconnect");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    REQUIRE(context.world.current_world() == nullptr);
    REQUIRE(context.variables.get_string("mapname") == "");
    REQUIRE(context.variables.get_string("host_map") == "");
    REQUIRE(!context.network.server().is_running());
    REQUIRE(context.network.client().state() == openstrike::NetworkConnectionState::Disconnected);

    std::filesystem::remove_all(root);
}

void test_quit_and_exit_commands_disconnect_before_shutdown()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_quit_command_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_minimal_bsp(root / "game/maps/quit_world.bsp", entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    REQUIRE(context.world.load_map("quit_world", context.filesystem));
    context.variables.set("host_map", "quit_world");
    context.variables.set("mapname", "quit_world");
    REQUIRE(context.network.start_server(0));

    context.command_buffer.add_text("quit");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    REQUIRE(context.quit_requested);
    REQUIRE(context.world.current_world() == nullptr);
    REQUIRE(context.variables.get_string("mapname") == "");
    REQUIRE(!context.network.server().is_running());

    openstrike::EngineContext exit_context;
    openstrike::configure_engine_context(exit_context, config);
    exit_context.command_buffer.add_text("exit");
    openstrike::ConsoleCommandContext exit_console_context = exit_context.console_context();
    exit_context.command_buffer.execute(exit_context.commands, exit_console_context);
    REQUIRE(exit_context.quit_requested);

    std::filesystem::remove_all(root);
}

void test_game_simulation_updates_hud_for_loaded_world()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_game_hud_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n"
        "{\n"
        "\"classname\" \"info_player_counterterrorist\"\n"
        "\"origin\" \"8 16 64\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/hud_world.bsp", entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);

    openstrike::GameSimulation simulation;
    simulation.on_start(config, context);
    REQUIRE(!context.hud.visible);
    REQUIRE(context.hud.round_phase == "MENU");

    REQUIRE(context.world.load_map("hud_world", context.filesystem));
    simulation.on_fixed_update(openstrike::SimulationStep{0, 1.0 / 64.0}, context);

    REQUIRE(context.hud.visible);
    REQUIRE(context.hud.health == 100);
    REQUIRE(context.hud.max_health == 100);
    REQUIRE(context.hud.money == 800);
    REQUIRE(context.hud.ammo_in_clip == 12);
    REQUIRE(context.hud.reserve_ammo == 24);
    REQUIRE(context.hud.round_phase == "hud_world");
    REQUIRE(context.hud.grounded);
    REQUIRE(context.hud.crosshair_gap >= 12.0F);
    REQUIRE(context.camera.active);

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

void test_walk_and_duck_limit_movement_speed()
{
    openstrike::MovementTuning tuning;

    openstrike::PlayerState walking_player;
    openstrike::InputCommand walk_forward;
    walk_forward.move_x = 1.0F;
    walk_forward.walk = true;

    for (int tick = 0; tick < 64; ++tick)
    {
        openstrike::simulate_player_move(walking_player, walk_forward, tuning);
    }

    REQUIRE(walking_player.velocity.length_2d() <= (tuning.max_ground_speed * tuning.walk_speed_modifier) + 0.01F);

    openstrike::PlayerState ducking_player;
    openstrike::InputCommand duck_forward;
    duck_forward.move_x = 1.0F;
    duck_forward.duck = true;

    for (int tick = 0; tick < 64; ++tick)
    {
        openstrike::simulate_player_move(ducking_player, duck_forward, tuning);
    }

    REQUIRE(ducking_player.ducked);
    REQUIRE(ducking_player.velocity.length_2d() <= (tuning.max_ground_speed * tuning.duck_speed_modifier) + 0.01F);
}

void test_air_movement_uses_source_air_speed_cap()
{
    openstrike::PlayerState player;
    player.origin.z = 128.0F;
    player.on_ground = false;

    openstrike::MovementTuning tuning;
    openstrike::InputCommand strafe;
    strafe.move_x = 1.0F;

    for (int tick = 0; tick < 64; ++tick)
    {
        openstrike::simulate_player_move(player, strafe, tuning, std::nullopt);
    }

    REQUIRE(player.velocity.x > 0.0F);
    REQUIRE(player.velocity.length_2d() <= tuning.air_speed_cap + 0.01F);
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

void test_jump_requires_release_unless_autobunny_is_enabled()
{
    openstrike::PlayerState player;
    openstrike::MovementTuning tuning;
    openstrike::InputCommand jump;
    jump.jump = true;

    openstrike::simulate_player_move(player, jump, tuning);
    REQUIRE(!player.on_ground);

    for (int tick = 0; tick < 256 && !player.on_ground; ++tick)
    {
        openstrike::simulate_player_move(player, jump, tuning);
    }

    REQUIRE(player.on_ground);
    openstrike::simulate_player_move(player, jump, tuning);
    REQUIRE(player.on_ground);
    REQUIRE(player.velocity.z == 0.0F);

    jump.jump = false;
    openstrike::simulate_player_move(player, jump, tuning);
    jump.jump = true;
    openstrike::simulate_player_move(player, jump, tuning);
    REQUIRE(!player.on_ground);
}

void test_stamina_penalty_recovers_over_time()
{
    openstrike::PlayerState player;
    openstrike::MovementTuning tuning;
    openstrike::InputCommand jump;
    jump.jump = true;

    openstrike::simulate_player_move(player, jump, tuning);
    REQUIRE(player.stamina > 0.0F);

    for (int tick = 0; tick < 256 && !player.on_ground; ++tick)
    {
        openstrike::simulate_player_move(player, {}, tuning);
    }

    const float stamina_after_landing = player.stamina;
    REQUIRE(stamina_after_landing > 0.0F);

    for (int tick = 0; tick < 64; ++tick)
    {
        openstrike::simulate_player_move(player, {}, tuning);
    }

    REQUIRE(player.stamina < stamina_after_landing);
}

void test_fps_controller_builds_view_relative_move_command()
{
    openstrike::FpsViewState view;
    openstrike::FpsInputSample input;
    input.move_forward = true;

    openstrike::InputCommand command = openstrike::build_fps_move_command(view, input);
    REQUIRE(command.move_x > 0.999F);
    REQUIRE(std::fabs(command.move_y) < 0.001F);

    view.yaw_degrees = 90.0F;
    command = openstrike::build_fps_move_command(view, input);
    REQUIRE(std::fabs(command.move_x) < 0.001F);
    REQUIRE(command.move_y > 0.999F);

    input.move_right = true;
    command = openstrike::build_fps_move_command({}, input);
    const openstrike::Vec3 diagonal_move{command.move_x, command.move_y, 0.0F};
    REQUIRE(command.move_x > 0.0F);
    REQUIRE(command.move_y < 0.0F);
    REQUIRE(std::fabs(command.move_x + command.move_y) < 0.001F);
    REQUIRE(std::fabs(diagonal_move.length_2d() - 1.0F) < 0.001F);
}

void test_fps_controller_maps_walk_duck_and_duck_eye_height()
{
    openstrike::InputState raw_input;
    raw_input.sprint = true;
    raw_input.duck = true;

    const openstrike::FpsInputSample sample = openstrike::sample_fps_input(raw_input);
    REQUIRE(sample.walk);
    REQUIRE(sample.duck);

    const openstrike::InputCommand command = openstrike::build_fps_move_command({}, sample);
    REQUIRE(command.walk);
    REQUIRE(command.duck);

    openstrike::PlayerState player;
    player.duck_amount = 0.5F;
    const openstrike::FpsControllerSettings settings;
    const openstrike::Vec3 eye = openstrike::fps_eye_origin(player, settings);
    REQUIRE(std::fabs(eye.z - 46.0F) < 0.001F);
}

void test_sdl_gameplay_input_adapter_maps_source_style_buttons()
{
    openstrike::InputState input;

    REQUIRE(openstrike::apply_sdl_gameplay_key(input, SDLK_W, true));
    REQUIRE(input.move_forward);
    REQUIRE(openstrike::apply_sdl_gameplay_key(input, SDLK_W, false));
    REQUIRE(!input.move_forward);

    REQUIRE(openstrike::apply_sdl_gameplay_key(input, SDLK_LSHIFT, true));
    REQUIRE(input.sprint);
    REQUIRE(openstrike::apply_sdl_gameplay_key(input, SDLK_LSHIFT, false));
    REQUIRE(!input.sprint);

    REQUIRE(openstrike::apply_sdl_gameplay_key(input, SDLK_C, true));
    REQUIRE(input.duck);
    REQUIRE(openstrike::apply_sdl_gameplay_key(input, SDLK_C, false));
    REQUIRE(!input.duck);

    SDL_Event event{};
    input.mouse_captured = true;
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.xrel = 3.0F;
    event.motion.yrel = -2.0F;

    REQUIRE(openstrike::handle_sdl_gameplay_input_event(input, event));
    REQUIRE(input.mouse_delta.x == 3.0F);
    REQUIRE(input.mouse_delta.y == -2.0F);
}

void test_fps_controller_consumes_mouse_and_clamps_pitch()
{
    openstrike::InputState raw_input;
    raw_input.move_forward = true;
    raw_input.mouse_delta = {100.0F, 10000.0F};

    const openstrike::FpsInputSample sample = openstrike::sample_fps_input(raw_input);
    REQUIRE(sample.move_forward);
    REQUIRE(raw_input.mouse_delta.x == 0.0F);
    REQUIRE(raw_input.mouse_delta.y == 0.0F);

    openstrike::FpsViewState view;
    openstrike::FpsControllerSettings settings;
    openstrike::update_fps_view(view, sample, settings);

    REQUIRE(std::fabs(view.yaw_degrees + 5.5F) < 0.001F);
    REQUIRE(view.pitch_degrees == settings.max_pitch_degrees);
}

void add_collision_triangle(openstrike::LoadedWorld& world, openstrike::Vec3 a, openstrike::Vec3 b, openstrike::Vec3 c, openstrike::Vec3 normal)
{
    openstrike::WorldTriangle triangle;
    triangle.points[0] = a;
    triangle.points[1] = b;
    triangle.points[2] = c;
    triangle.normal = normal;
    world.mesh.collision_triangles.push_back(triangle);
}

void test_physics_world_blocks_character_against_static_mesh()
{
    openstrike::LoadedWorld world;
    world.name = "physics_static_mesh";

    add_collision_triangle(world, {-128.0F, -128.0F, 0.0F}, {128.0F, -128.0F, 0.0F}, {128.0F, 128.0F, 0.0F}, {0.0F, 0.0F, 1.0F});
    add_collision_triangle(world, {-128.0F, -128.0F, 0.0F}, {128.0F, 128.0F, 0.0F}, {-128.0F, 128.0F, 0.0F}, {0.0F, 0.0F, 1.0F});
    add_collision_triangle(world, {48.0F, -128.0F, 0.0F}, {48.0F, 128.0F, 0.0F}, {48.0F, 128.0F, 128.0F}, {-1.0F, 0.0F, 0.0F});
    add_collision_triangle(world, {48.0F, -128.0F, 0.0F}, {48.0F, 128.0F, 128.0F}, {48.0F, -128.0F, 128.0F}, {-1.0F, 0.0F, 0.0F});

    openstrike::PhysicsWorld physics;
    REQUIRE(physics.load_static_world(world));

    openstrike::PhysicsCharacterConfig config;
    config.radius = 16.0F;
    config.height = 72.0F;
    physics.reset_character({0.0F, 0.0F, 0.0F}, {}, config);

    openstrike::PhysicsCharacterState state = physics.character_state();
    constexpr float dt = 1.0F / 64.0F;
    for (int tick = 0; tick < 64; ++tick)
    {
        state = physics.move_character_to(state.origin + openstrike::Vec3{4.0F, 0.0F, 0.0F}, {256.0F, 0.0F, 0.0F}, dt);
    }

    REQUIRE(state.on_ground);
    REQUIRE(state.origin.x < 33.0F);
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
        test_material_system_resolves_source_vmt_without_shader_combos();
        test_source_texture_decodes_compressed_vtf_to_rgba();
        test_renderer_shader_files_are_dedicated();
        test_network_stream_and_protocol_roundtrip();
        test_network_udp_loopback_connects_and_exchanges_text();
        test_command_buffer_cvars_and_quit();
        test_audio_sound_chars_match_source_prefix_rules();
        test_audio_source_soundlevel_gain_falls_with_distance();
        test_world_manager_loads_source_bsp();
        test_world_manager_builds_bsp_mesh_and_floor();
        test_world_manager_tracks_source_sky_surfaces();
        test_world_manager_loads_static_props_and_renders_bounds();
        test_source_fgd_loads_classes_and_metadata();
        test_source_fgd_accepts_concatenated_source_strings();
        test_map_command_loads_current_world();
        test_map_and_changelevel_match_source_server_rules();
        test_disconnect_command_unloads_world_and_network();
        test_quit_and_exit_commands_disconnect_before_shutdown();
        test_game_simulation_updates_hud_for_loaded_world();
        test_engine_startup_quit_command();
        test_ground_movement_accelerates_and_friction_slows();
        test_walk_and_duck_limit_movement_speed();
        test_air_movement_uses_source_air_speed_cap();
        test_jump_returns_to_ground();
        test_jump_requires_release_unless_autobunny_is_enabled();
        test_stamina_penalty_recovers_over_time();
        test_fps_controller_builds_view_relative_move_command();
        test_fps_controller_maps_walk_duck_and_duck_eye_height();
        test_sdl_gameplay_input_adapter_maps_source_style_buttons();
        test_fps_controller_consumes_mouse_and_clamps_pitch();
        test_physics_world_blocks_character_against_static_mesh();
    }
    catch (const std::exception& error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }

    std::cout << "openstrike tests passed\n";
    return 0;
}
