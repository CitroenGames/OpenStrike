#include "openstrike/core/command_line.hpp"
#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/animation/animation_scene.hpp"
#include "openstrike/animation/csgo_player_anim_state.hpp"
#include "openstrike/animation/source_studio.hpp"
#include "openstrike/app/application.hpp"
#include "openstrike/app/openstrike_application.hpp"
#include "openstrike/audio/audio_system.hpp"
#include "openstrike/engine/engine.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/fixed_timestep.hpp"
#include "openstrike/engine/loading_screen_state.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/engine/sdl_input.hpp"
#include "openstrike/game/fps_controller.hpp"
#include "openstrike/game/game_simulation.hpp"
#include "openstrike/game/movement.hpp"
#include "openstrike/game/team_system.hpp"
#include "openstrike/material/material_system.hpp"
#include "openstrike/nav/navigation.hpp"
#include "openstrike/network/network_channel.hpp"
#include "openstrike/network/network_messages.hpp"
#include "openstrike/network/network_prediction.hpp"
#include "openstrike/network/network_protocol.hpp"
#include "openstrike/network/network_replication.hpp"
#include "openstrike/network/network_session.hpp"
#include "openstrike/network/network_socket.hpp"
#include "openstrike/network/network_stream.hpp"
#include "openstrike/network/user_command.hpp"
#include "openstrike/physics/physics_world.hpp"
#include "openstrike/renderer/null_renderer.hpp"
#include "openstrike/renderer/world_material_shader.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_fgd.hpp"
#include "openstrike/source/source_keyvalues.hpp"
#include "openstrike/world/world.hpp"
#include "../tools/editor/src/brush_mesh_builder.hpp"

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
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
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

void write_lump_version(std::vector<unsigned char>& bytes, std::size_t lump, std::uint32_t version)
{
    write_u32_le(bytes, 16 + (lump * 16), version);
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

std::vector<unsigned char> make_resource_vtf(std::uint16_t width, std::uint16_t height, std::int32_t image_format, std::vector<unsigned char> image_bytes)
{
    constexpr std::size_t header_size = 88;
    std::vector<unsigned char> bytes(header_size, 0);
    bytes[0] = 'V';
    bytes[1] = 'T';
    bytes[2] = 'F';
    bytes[3] = '\0';
    write_u32_le(bytes, 4, 7);
    write_u32_le(bytes, 8, 5);
    write_u32_le(bytes, 12, static_cast<std::uint32_t>(header_size));
    write_u16_le(bytes, 16, width);
    write_u16_le(bytes, 18, height);
    write_u16_le(bytes, 24, 1);
    write_f32_le(bytes, 32, 0.25F);
    write_f32_le(bytes, 36, 0.5F);
    write_f32_le(bytes, 40, 0.75F);
    write_f32_le(bytes, 48, 1.0F);
    write_s32_le(bytes, 52, image_format);
    bytes[56] = 1;
    write_s32_le(bytes, 57, -1);
    write_u16_le(bytes, 63, 1);
    write_u32_le(bytes, 68, 1);
    write_u32_le(bytes, 80, 0x30);
    write_u32_le(bytes, 84, static_cast<std::uint32_t>(header_size));
    bytes.insert(bytes.end(), image_bytes.begin(), image_bytes.end());
    return bytes;
}

std::vector<unsigned char> make_minimal_vpk_dir(std::string_view directory,
    std::string_view filename,
    std::string_view extension,
    std::string_view contents)
{
    std::vector<unsigned char> tree;
    const auto append_zero_string = [&](std::string_view value) {
        tree.insert(tree.end(), value.begin(), value.end());
        tree.push_back(0);
    };

    append_zero_string(extension);
    append_zero_string(directory);
    append_zero_string(filename);
    append_u32_le(tree, 0);
    append_u16_le(tree, 0);
    append_u16_le(tree, 0x7FFFU);
    append_u32_le(tree, 0);
    append_u32_le(tree, static_cast<std::uint32_t>(contents.size()));
    append_u16_le(tree, 0xFFFFU);
    append_zero_string({});
    append_zero_string({});
    append_zero_string({});

    std::vector<unsigned char> bytes(12, 0);
    write_u32_le(bytes, 0, 0x55AA1234U);
    write_u32_le(bytes, 4, 1);
    write_u32_le(bytes, 8, static_cast<std::uint32_t>(tree.size()));
    bytes.insert(bytes.end(), tree.begin(), tree.end());
    bytes.insert(bytes.end(), contents.begin(), contents.end());
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

void write_minimal_source_model(
    const std::filesystem::path& path,
    openstrike::Vec3 mins,
    openstrike::Vec3 maxs,
    std::uint32_t vertex_count = 3)
{
    std::filesystem::create_directories(path.parent_path());
    constexpr std::uint32_t checksum = 0x12345678U;
    vertex_count = std::clamp(vertex_count, 3U, 65535U);
    vertex_count -= vertex_count % 3U;
    if (vertex_count < 3U)
    {
        vertex_count = 3U;
    }

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
    write_u32_le(bytes, model + 80, vertex_count);
    write_u32_le(bytes, model + 84, 0);
    const std::size_t mesh = model + 148;
    write_u32_le(bytes, mesh, 0);
    write_u32_le(bytes, mesh + 8, vertex_count);
    write_u32_le(bytes, mesh + 12, 0);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    std::vector<unsigned char> vvd(64 + (static_cast<std::size_t>(vertex_count) * 48U), 0);
    vvd[0] = 'I';
    vvd[1] = 'D';
    vvd[2] = 'S';
    vvd[3] = 'V';
    write_u32_le(vvd, 4, 4);
    write_u32_le(vvd, 8, checksum);
    write_u32_le(vvd, 12, 1);
    write_u32_le(vvd, 16, vertex_count);
    write_u32_le(vvd, 56, 64);
    auto write_vertex = [&](std::size_t index, openstrike::Vec3 position, openstrike::Vec3 normal, openstrike::Vec2 texcoord) {
        const std::size_t vertex = 64 + (index * 48);
        vvd[vertex + 15] = 1;
        write_vec3_le(vvd, vertex + 16, position);
        write_vec3_le(vvd, vertex + 28, normal);
        write_vec2_le(vvd, vertex + 40, texcoord);
    };
    for (std::uint32_t vertex = 0; vertex < vertex_count; ++vertex)
    {
        const std::uint32_t corner = vertex % 3U;
        if (corner == 0)
        {
            write_vertex(vertex, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F});
        }
        else if (corner == 1)
        {
            write_vertex(vertex, {8.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F});
        }
        else
        {
            write_vertex(vertex, {0.0F, 0.0F, 16.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F});
        }
    }
    std::ofstream vvd_file(path.parent_path() / (path.stem().string() + ".vvd"), std::ios::binary);
    vvd_file.write(reinterpret_cast<const char*>(vvd.data()), static_cast<std::streamsize>(vvd.size()));

    const std::uint32_t index_count = vertex_count;
    const std::size_t strip_group = 73;
    const std::size_t vertex_table = 33;
    const std::size_t index_table = vertex_table + (static_cast<std::size_t>(vertex_count) * 9U);
    const std::size_t strip_table = index_table + (static_cast<std::size_t>(index_count) * 2U);
    const std::size_t strip = strip_group + strip_table;
    std::vector<unsigned char> vtx(strip + 35U, 0);
    write_u32_le(vtx, 0, 7);
    write_u16_le(vtx, 8, 32);
    write_u16_le(vtx, 10, 3);
    write_u32_le(vtx, 12, vertex_count);
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
    write_u32_le(vtx, strip_group, vertex_count);
    write_u32_le(vtx, strip_group + 4, static_cast<std::uint32_t>(vertex_table));
    write_u32_le(vtx, strip_group + 8, index_count);
    write_u32_le(vtx, strip_group + 12, static_cast<std::uint32_t>(index_table));
    write_u32_le(vtx, strip_group + 16, 1);
    write_u32_le(vtx, strip_group + 20, static_cast<std::uint32_t>(strip_table));
    for (std::uint32_t vertex = 0; vertex < vertex_count; ++vertex)
    {
        write_u16_le(vtx, strip_group + vertex_table + (static_cast<std::size_t>(vertex) * 9U) + 4U, static_cast<std::uint16_t>(vertex));
    }
    for (std::uint32_t index = 0; index < index_count; ++index)
    {
        write_u16_le(vtx, strip_group + index_table + (static_cast<std::size_t>(index) * 2U), static_cast<std::uint16_t>(index));
    }
    write_u32_le(vtx, strip, index_count);
    write_u32_le(vtx, strip + 4, 0);
    write_u32_le(vtx, strip + 8, vertex_count);
    vtx[strip + 18] = 0x01;
    std::ofstream vtx_file(path.parent_path() / (path.stem().string() + ".dx90.vtx"), std::ios::binary);
    vtx_file.write(reinterpret_cast<const char*>(vtx.data()), static_cast<std::streamsize>(vtx.size()));
}

std::vector<unsigned char> make_minimal_animated_source_model()
{
    std::vector<unsigned char> bytes(8192, 0);
    bytes[0] = 'I';
    bytes[1] = 'D';
    bytes[2] = 'S';
    bytes[3] = 'T';
    write_u32_le(bytes, 4, 49);
    write_u32_le(bytes, 8, 0xA11CE001U);
    write_zero_string(bytes, 12, "animated_test");
    write_u32_le(bytes, 76, static_cast<std::uint32_t>(bytes.size()));
    write_vec3_le(bytes, 80, {0.0F, 0.0F, 64.0F});
    write_vec3_le(bytes, 104, {-16.0F, -16.0F, 0.0F});
    write_vec3_le(bytes, 116, {16.0F, 16.0F, 72.0F});
    write_vec3_le(bytes, 128, {-16.0F, -16.0F, 0.0F});
    write_vec3_le(bytes, 140, {16.0F, 16.0F, 72.0F});

    constexpr std::size_t bone_table = 512;
    constexpr std::size_t bone_size = 216;
    constexpr std::size_t bone_controller_table = 980;
    constexpr std::size_t hitbox_set_table = 1040;
    constexpr std::size_t anim_table = 1200;
    constexpr std::size_t sequence_table = 1600;
    constexpr std::size_t sequence_size = 216;
    constexpr std::int32_t sequence_count = 9;
    constexpr std::size_t event_table = sequence_table + (sequence_size * sequence_count);
    constexpr std::size_t pose_parameter_table = 3700;
    constexpr std::size_t attachment_table = 3900;
    std::size_t string_cursor = 4300;

    auto put_string = [&](std::string_view text) {
        const std::size_t offset = string_cursor;
        write_zero_string(bytes, offset, text);
        string_cursor += text.size() + 1;
        return offset;
    };

    auto write_identity = [&](std::size_t offset) {
        write_f32_le(bytes, offset + 0, 1.0F);
        write_f32_le(bytes, offset + 20, 1.0F);
        write_f32_le(bytes, offset + 40, 1.0F);
    };

    auto write_bone = [&](std::size_t offset, std::string_view name, std::int32_t parent, openstrike::Vec3 position) {
        const std::size_t name_offset = put_string(name);
        write_s32_le(bytes, offset + 0, static_cast<std::int32_t>(name_offset - offset));
        write_s32_le(bytes, offset + 4, parent);
        for (std::size_t controller = 0; controller < 6; ++controller)
        {
            write_s32_le(bytes, offset + 8 + (controller * 4), -1);
        }
        write_vec3_le(bytes, offset + 32, position);
        write_f32_le(bytes, offset + 56, 1.0F);
        write_vec3_le(bytes, offset + 72, {1.0F, 1.0F, 1.0F});
        write_vec3_le(bytes, offset + 84, {1.0F / 512.0F, 1.0F / 512.0F, 1.0F / 512.0F});
        write_identity(offset + 96);
        write_f32_le(bytes, offset + 156, 1.0F);
        write_s32_le(bytes, offset + 172, -1);
    };

    write_u32_le(bytes, 156, 2);
    write_u32_le(bytes, 160, static_cast<std::uint32_t>(bone_table));
    write_bone(bone_table, "root", -1, {});
    write_bone(bone_table + bone_size, "spine", 0, {0.0F, 0.0F, 10.0F});

    write_u32_le(bytes, 164, 1);
    write_u32_le(bytes, 168, static_cast<std::uint32_t>(bone_controller_table));
    write_s32_le(bytes, bone_controller_table, 1);
    write_s32_le(bytes, bone_controller_table + 4, 0);
    write_f32_le(bytes, bone_controller_table + 8, -45.0F);
    write_f32_le(bytes, bone_controller_table + 12, 45.0F);
    write_s32_le(bytes, bone_controller_table + 16, 0);
    write_s32_le(bytes, bone_controller_table + 20, 0);

    write_u32_le(bytes, 172, 1);
    write_u32_le(bytes, 176, static_cast<std::uint32_t>(hitbox_set_table));
    const std::size_t hitbox_set_name = put_string("default");
    write_s32_le(bytes, hitbox_set_table, static_cast<std::int32_t>(hitbox_set_name - hitbox_set_table));
    write_s32_le(bytes, hitbox_set_table + 4, 1);
    write_s32_le(bytes, hitbox_set_table + 8, 12);
    const std::size_t hitbox = hitbox_set_table + 12;
    write_s32_le(bytes, hitbox, 1);
    write_s32_le(bytes, hitbox + 4, 2);
    write_vec3_le(bytes, hitbox + 8, {-4.0F, -4.0F, -4.0F});
    write_vec3_le(bytes, hitbox + 20, {4.0F, 4.0F, 4.0F});
    const std::size_t hitbox_name = put_string("spine_hitbox");
    write_s32_le(bytes, hitbox + 32, static_cast<std::int32_t>(hitbox_name - hitbox));

    write_u32_le(bytes, 180, 1);
    write_u32_le(bytes, 184, static_cast<std::uint32_t>(anim_table));
    const std::size_t anim_name = put_string("spine_move");
    write_s32_le(bytes, anim_table + 4, static_cast<std::int32_t>(anim_name - anim_table));
    write_f32_le(bytes, anim_table + 8, 4.0F);
    write_s32_le(bytes, anim_table + 12, 1);
    write_s32_le(bytes, anim_table + 16, 4);
    write_s32_le(bytes, anim_table + 56, 100);

    const std::size_t anim_data = anim_table + 100;
    bytes[anim_data] = 1;
    bytes[anim_data + 1] = 0x0CU;
    write_s16_le(bytes, anim_data + 2, 0);
    const std::size_t rotation_values = anim_data + 4;
    const std::size_t position_values = rotation_values + 6;
    std::size_t channel_cursor = position_values + 6;
    auto write_channel = [&](std::size_t value_pointer, std::size_t axis, std::initializer_list<std::int16_t> samples) {
        const std::size_t channel_offset = channel_cursor;
        write_s16_le(bytes, value_pointer + (axis * 2), static_cast<std::int16_t>(channel_offset - value_pointer));
        bytes[channel_offset] = static_cast<unsigned char>(samples.size());
        bytes[channel_offset + 1] = static_cast<unsigned char>(samples.size());
        std::size_t sample_offset = channel_offset + 2;
        for (const std::int16_t sample : samples)
        {
            write_s16_le(bytes, sample_offset, sample);
            sample_offset += 2;
        }
        channel_cursor = sample_offset;
    };
    write_channel(rotation_values, 0, {0, 256, 512, 768});
    write_channel(position_values, 0, {0, 10, 20, 30});

    write_u32_le(bytes, 188, sequence_count);
    write_u32_le(bytes, 192, static_cast<std::uint32_t>(sequence_table));
    const std::array<std::pair<std::string_view, std::string_view>, sequence_count> sequences{{
        {"idle", "ACT_CSGO_IDLE"},
        {"run", "ACT_CSGO_RUN"},
        {"walk", "ACT_CSGO_WALK"},
        {"jump", "ACT_CSGO_JUMP"},
        {"fall", "ACT_CSGO_FALL"},
        {"land_light", "ACT_CSGO_LAND_LIGHT"},
        {"fire", "ACT_CSGO_FIRE_PRIMARY"},
        {"alive_loop", "ACT_CSGO_ALIVE_LOOP"},
        {"flinch", "ACT_CSGO_FLINCH"},
    }};
    for (std::size_t index = 0; index < sequences.size(); ++index)
    {
        const std::size_t sequence = sequence_table + (index * sequence_size);
        const std::size_t label = put_string(sequences[index].first);
        const std::size_t activity = put_string(sequences[index].second);
        write_s32_le(bytes, sequence + 4, static_cast<std::int32_t>(label - sequence));
        write_s32_le(bytes, sequence + 8, static_cast<std::int32_t>(activity - sequence));
        write_s32_le(bytes, sequence + 12, index == 6 ? 0 : 1);
        write_s32_le(bytes, sequence + 16, static_cast<std::int32_t>(index));
        write_s32_le(bytes, sequence + 20, 1);
        write_vec3_le(bytes, sequence + 32, {-16.0F, -16.0F, 0.0F});
        write_vec3_le(bytes, sequence + 44, {16.0F, 16.0F, 72.0F});
        write_s32_le(bytes, sequence + 56, 1);
        write_s32_le(bytes, sequence + 60, 204);
        write_s32_le(bytes, sequence + 68, 1);
        write_s32_le(bytes, sequence + 72, 1);
        write_s32_le(bytes, sequence + 76, -1);
        write_s32_le(bytes, sequence + 80, -1);
        write_f32_le(bytes, sequence + 104, 0.2F);
        write_f32_le(bytes, sequence + 108, 0.2F);
        write_f32_le(bytes, sequence + 132, 3.0F);
        write_s16_le(bytes, sequence + 204, 0);
    }
    write_s32_le(bytes, sequence_table + 24, 1);
    write_s32_le(bytes, sequence_table + 28, static_cast<std::int32_t>(event_table - sequence_table));
    write_f32_le(bytes, event_table, 0.25F);
    write_s32_le(bytes, event_table + 4, 5004);
    write_zero_string(bytes, event_table + 12, "footstep");
    const std::size_t event_name = put_string("AE_CL_PLAYSOUND");
    write_s32_le(bytes, event_table + 76, static_cast<std::int32_t>(event_name - event_table));

    write_u32_le(bytes, 240, 1);
    write_u32_le(bytes, 244, static_cast<std::uint32_t>(attachment_table));
    const std::size_t attachment_name = put_string("eyes");
    write_s32_le(bytes, attachment_table, static_cast<std::int32_t>(attachment_name - attachment_table));
    write_s32_le(bytes, attachment_table + 8, 1);
    write_identity(attachment_table + 12);
    write_f32_le(bytes, attachment_table + 12 + 44, 64.0F);

    write_u32_le(bytes, 300, 6);
    write_u32_le(bytes, 304, static_cast<std::uint32_t>(pose_parameter_table));
    const std::array<std::tuple<std::string_view, float, float, float>, 6> pose_parameters{{
        {"move_yaw", -180.0F, 180.0F, 360.0F},
        {"body_yaw", -58.0F, 58.0F, 0.0F},
        {"body_pitch", -90.0F, 90.0F, 0.0F},
        {"aim_pitch", -90.0F, 90.0F, 0.0F},
        {"aim_yaw", -58.0F, 58.0F, 0.0F},
        {"lean_yaw", -180.0F, 180.0F, 360.0F},
    }};
    for (std::size_t index = 0; index < pose_parameters.size(); ++index)
    {
        const std::size_t pose = pose_parameter_table + (index * 20);
        const std::size_t name = put_string(std::get<0>(pose_parameters[index]));
        write_s32_le(bytes, pose, static_cast<std::int32_t>(name - pose));
        write_f32_le(bytes, pose + 8, std::get<1>(pose_parameters[index]));
        write_f32_le(bytes, pose + 12, std::get<2>(pose_parameters[index]));
        write_f32_le(bytes, pose + 16, std::get<3>(pose_parameters[index]));
    }

    const std::size_t surface_prop = put_string("flesh");
    write_u32_le(bytes, 308, static_cast<std::uint32_t>(surface_prop));
    const std::string key_values = "animated 1";
    const std::size_t key_value_offset = put_string(key_values);
    write_u32_le(bytes, 312, static_cast<std::uint32_t>(key_value_offset));
    write_u32_le(bytes, 316, static_cast<std::uint32_t>(key_values.size()));
    write_f32_le(bytes, 328, 80.0F);
    write_s32_le(bytes, 332, 0x2000000);

    return bytes;
}

void write_minimal_animated_source_model(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    const std::vector<unsigned char> bytes = make_minimal_animated_source_model();
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_static_prop_bsp(const std::filesystem::path& path, const std::string& entities, std::uint8_t solid = 6)
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
    write_u16_le(static_prop_data, record + 26, 0);
    write_u16_le(static_prop_data, record + 28, 1);
    static_prop_data[record + 30] = solid;
    static_prop_data[record + 31] = 0;
    write_u32_le(static_prop_data, record + 32, 2);
    write_f32_le(static_prop_data, record + 36, 128.0F);
    write_f32_le(static_prop_data, record + 40, 1024.0F);
    write_vec3_le(static_prop_data, record + 44, {100.0F, 200.0F, 80.0F});
    write_f32_le(static_prop_data, record + 56, 1.25F);
    static_prop_data[record + 60] = 1;
    static_prop_data[record + 61] = 3;
    static_prop_data[record + 62] = 2;
    static_prop_data[record + 63] = 4;
    static_prop_data[record + 64] = 64;
    static_prop_data[record + 65] = 128;
    static_prop_data[record + 66] = 255;
    static_prop_data[record + 67] = 200;
    static_prop_data[record + 68] = 1;
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

enum class TestLightmapMode
{
    None,
    Ldr,
    Hdr,
};

std::vector<unsigned char> make_test_lightmap(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::int8_t exponent)
{
    std::vector<unsigned char> bytes;
    bytes.reserve(4 * 4);
    for (std::size_t index = 0; index < 4; ++index)
    {
        bytes.push_back(red);
        bytes.push_back(green);
        bytes.push_back(blue);
        bytes.push_back(static_cast<unsigned char>(exponent));
    }
    return bytes;
}

void write_square_bsp(
    const std::filesystem::path& path,
    const std::string& entities,
    std::uint32_t surface_flags = 0,
    TestLightmapMode lightmap_mode = TestLightmapMode::None,
    bool with_displacement = false)
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
    write_f32_le(texinfo, 32, 1.0F / 128.0F);
    write_f32_le(texinfo, 44, 0.5F);
    write_f32_le(texinfo, 52, 1.0F / 128.0F);
    write_f32_le(texinfo, 60, 0.5F);
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
    write_s16_le(faces, 12, with_displacement ? 0 : -1);
    std::fill(faces.begin() + 16, faces.begin() + 20, static_cast<unsigned char>(0xFF));
    if (lightmap_mode != TestLightmapMode::None)
    {
        faces[16] = 0;
        write_s32_le(faces, 20, 0);
        write_s32_le(faces, 28, 0);
        write_s32_le(faces, 32, 0);
        write_s32_le(faces, 36, 1);
        write_s32_le(faces, 40, 1);
    }
    write_f32_le(faces, 24, 128.0F * 128.0F);
    append_lump(7, faces);

    if (with_displacement)
    {
        constexpr std::uint32_t displacement_power = 2;
        constexpr std::uint32_t displacement_grid_size = (1U << displacement_power) + 1U;
        constexpr std::uint32_t displacement_vertex_count = displacement_grid_size * displacement_grid_size;
        constexpr std::uint32_t displacement_triangle_count = (1U << displacement_power) * (1U << displacement_power) * 2U;

        std::vector<unsigned char> dispinfo(176, 0);
        write_vec3_le(dispinfo, 0, points[0]);
        write_s32_le(dispinfo, 12, 0);
        write_s32_le(dispinfo, 16, 0);
        write_s32_le(dispinfo, 20, static_cast<std::int32_t>(displacement_power));
        write_s32_le(dispinfo, 24, 0);
        write_f32_le(dispinfo, 28, 45.0F);
        write_u32_le(dispinfo, 32, 0x00000001U);
        write_u16_le(dispinfo, 36, 0);
        for (std::size_t word = 0; word < 10; ++word)
        {
            write_u32_le(dispinfo, 136 + (word * 4), 0xFFFFFFFFU);
        }
        append_lump(26, dispinfo);

        std::vector<unsigned char> dispverts(displacement_vertex_count * 20U, 0);
        for (std::uint32_t row = 0; row < displacement_grid_size; ++row)
        {
            for (std::uint32_t column = 0; column < displacement_grid_size; ++column)
            {
                const std::uint32_t index = (row * displacement_grid_size) + column;
                const std::size_t offset = static_cast<std::size_t>(index) * 20U;
                write_vec3_le(dispverts, offset, {0.0F, 0.0F, 1.0F});
                write_f32_le(dispverts, offset + 12, row == 2 && column == 2 ? 16.0F : 0.0F);
                write_f32_le(dispverts, offset + 16, static_cast<float>(index) / static_cast<float>(displacement_vertex_count - 1U));
            }
        }
        append_lump(33, dispverts);

        std::vector<unsigned char> disptris(displacement_triangle_count * 2U, 0);
        for (std::uint32_t index = 0; index < displacement_triangle_count; ++index)
        {
            write_u16_le(disptris, static_cast<std::size_t>(index) * 2U, 1U);
        }
        append_lump(48, disptris);
    }

    if (lightmap_mode != TestLightmapMode::None)
    {
        append_lump(8, make_test_lightmap(64, 128, 192, 0));
        if (lightmap_mode == TestLightmapMode::Hdr)
        {
            append_lump(53, make_test_lightmap(200, 150, 100, 0));
            append_lump(58, faces);
        }
    }

    std::vector<unsigned char> pakfile;
    append_stored_zip_file(pakfile, "materials/test/floor.vmt", "\"LightmappedGeneric\" { \"$basetexture\" \"test/floor\" }");
    append_lump(40, pakfile);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

constexpr std::uint32_t kTestContentsSolid = 0x00000001;
constexpr std::uint32_t kTestContentsWindow = 0x00000002;
constexpr std::uint32_t kTestContentsGrate = 0x00000008;
constexpr std::uint32_t kTestContentsWater = 0x00000020;
constexpr std::uint32_t kTestContentsBlockLos = 0x00000040;
constexpr std::uint32_t kTestContentsOpaque = 0x00000080;
constexpr std::uint32_t kTestContentsMoveable = 0x00004000;
constexpr std::uint32_t kTestContentsPlayerClip = 0x00010000;
constexpr std::uint32_t kTestContentsMonsterClip = 0x00020000;
constexpr std::uint32_t kTestContentsMonster = 0x02000000;
constexpr std::uint32_t kTestSourceSolidMask =
    kTestContentsSolid | kTestContentsMoveable | kTestContentsWindow | kTestContentsMonster | kTestContentsGrate;
constexpr std::uint32_t kTestSourceWorldCollisionMask = kTestSourceSolidMask | kTestContentsPlayerClip | kTestContentsMonsterClip;

struct TestBrushBox
{
    openstrike::Vec3 mins;
    openstrike::Vec3 maxs;
    std::uint32_t contents = kTestContentsSolid;
};

void append_test_plane(std::vector<unsigned char>& planes, openstrike::Vec3 normal, float dist)
{
    const std::size_t offset = planes.size();
    planes.resize(offset + 20, 0);
    write_vec3_le(planes, offset, normal);
    write_f32_le(planes, offset + 12, dist);
}

void append_test_brush_box(
    std::vector<unsigned char>& planes,
    std::vector<unsigned char>& brush_sides,
    std::vector<unsigned char>& brushes,
    const TestBrushBox& box)
{
    const std::uint32_t first_plane = static_cast<std::uint32_t>(planes.size() / 20U);
    append_test_plane(planes, {1.0F, 0.0F, 0.0F}, box.maxs.x);
    append_test_plane(planes, {-1.0F, 0.0F, 0.0F}, -box.mins.x);
    append_test_plane(planes, {0.0F, 1.0F, 0.0F}, box.maxs.y);
    append_test_plane(planes, {0.0F, -1.0F, 0.0F}, -box.mins.y);
    append_test_plane(planes, {0.0F, 0.0F, 1.0F}, box.maxs.z);
    append_test_plane(planes, {0.0F, 0.0F, -1.0F}, -box.mins.z);

    const std::uint32_t first_side = static_cast<std::uint32_t>(brush_sides.size() / 8U);
    for (std::uint32_t side = 0; side < 6; ++side)
    {
        const std::size_t offset = brush_sides.size();
        brush_sides.resize(offset + 8, 0);
        write_u16_le(brush_sides, offset, static_cast<std::uint16_t>(first_plane + side));
        write_s16_le(brush_sides, offset + 2, -1);
        write_s16_le(brush_sides, offset + 4, -1);
    }

    const std::size_t brush = brushes.size();
    brushes.resize(brush + 12, 0);
    write_s32_le(brushes, brush, static_cast<std::int32_t>(first_side));
    write_s32_le(brushes, brush + 4, 6);
    write_u32_le(brushes, brush + 8, box.contents);
}

void write_brush_collision_bsp(const std::filesystem::path& path, const std::string& entities, const std::vector<TestBrushBox>& boxes)
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

    std::vector<unsigned char> planes;
    std::vector<unsigned char> brush_sides;
    std::vector<unsigned char> brushes;
    for (const TestBrushBox& box : boxes)
    {
        append_test_brush_box(planes, brush_sides, brushes, box);
    }
    append_lump(1, planes);
    append_lump(18, brushes);
    append_lump(19, brush_sides);

    std::uint32_t union_contents = 0;
    std::vector<unsigned char> leaf_brushes;
    for (std::uint32_t index = 0; index < boxes.size(); ++index)
    {
        append_u16_le(leaf_brushes, static_cast<std::uint16_t>(index));
        union_contents |= boxes[index].contents;
    }
    append_lump(17, leaf_brushes);

    std::vector<unsigned char> leaves(32, 0);
    write_u32_le(leaves, 0, union_contents);
    write_s16_le(leaves, 4, -1);
    write_u16_le(leaves, 24, 0);
    write_u16_le(leaves, 26, static_cast<std::uint16_t>(boxes.size()));
    write_s16_le(leaves, 28, -1);
    append_lump(10, leaves);
    write_lump_version(bytes, 10, 1);

    std::vector<unsigned char> models(48, 0);
    write_s32_le(models, 36, -1);
    append_lump(14, models);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool atlas_contains_color(const openstrike::WorldLightmapAtlas& atlas, float red, float green, float blue)
{
    for (std::size_t offset = 0; offset + 3 < atlas.rgba.size(); offset += 4)
    {
        if (std::fabs(atlas.rgba[offset + 0] - red) < 0.001F &&
            std::fabs(atlas.rgba[offset + 1] - green) < 0.001F &&
            std::fabs(atlas.rgba[offset + 2] - blue) < 0.001F)
        {
            return true;
        }
    }
    return false;
}

VmfSolid make_editor_test_box_solid(const Vec3& mins, const Vec3& maxs, const std::string& material)
{
    VmfSolid solid;
    solid.id = 1;

    const float x0 = mins.x, y0 = mins.y, z0 = mins.z;
    const float x1 = maxs.x, y1 = maxs.y, z1 = maxs.z;

    auto make_side = [&](const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& u_axis, const Vec3& v_axis) {
        VmfSide side;
        side.planePoints[0] = p0;
        side.planePoints[1] = p1;
        side.planePoints[2] = p2;
        side.material = material;
        side.uaxis.axis = u_axis;
        side.uaxis.scale = 0.25f;
        side.vaxis.axis = v_axis;
        side.vaxis.scale = 0.25f;
        side.lightmapScale = 16;
        return side;
    };

    solid.sides.push_back(make_side({x0, y0, z1}, {x0, y1, z1}, {x1, y1, z1}, {1, 0, 0}, {0, -1, 0}));
    solid.sides.push_back(make_side({x0, y1, z0}, {x0, y0, z0}, {x1, y0, z0}, {1, 0, 0}, {0, 1, 0}));
    solid.sides.push_back(make_side({x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {1, 0, 0}, {0, 0, -1}));
    solid.sides.push_back(make_side({x1, y0, z0}, {x0, y0, z0}, {x0, y0, z1}, {-1, 0, 0}, {0, 0, -1}));
    solid.sides.push_back(make_side({x1, y1, z0}, {x1, y0, z0}, {x1, y0, z1}, {0, 1, 0}, {0, 0, -1}));
    solid.sides.push_back(make_side({x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1}, {0, -1, 0}, {0, 0, -1}));
    return solid;
}

std::array<float, 2> editor_face_uv(const Vec3& pos_gl, const VmfSide& side)
{
    const Vec3 src = GLToSource(pos_gl);
    const float scale_u = side.uaxis.scale != 0.0f ? side.uaxis.scale : 0.25f;
    const float scale_v = side.vaxis.scale != 0.0f ? side.vaxis.scale : 0.25f;
    return {
        Dot(src, side.uaxis.axis) / scale_u + side.uaxis.shift,
        Dot(src, side.vaxis.axis) / scale_v + side.vaxis.shift,
    };
}

std::array<float, 2> lerp_editor_uv(std::array<float, 2> a, std::array<float, 2> b, float t)
{
    return {
        a[0] + (b[0] - a[0]) * t,
        a[1] + (b[1] - a[1]) * t,
    };
}

void test_editor_displacement_uvs_stay_parent_quad_anchored()
{
    VmfSolid solid = make_editor_test_box_solid({0, 0, 0}, {128, 128, 16}, "test/floor");
    VmfSide& top_side = solid.sides[0];

    std::vector<Vec3> parent_quad = ComputeSidePolygon(solid, 0);
    REQUIRE(parent_quad.size() == 4);

    VmfDispInfo disp;
    disp.power = 2;
    disp.startPosition = GLToSource(parent_quad[0]);
    const int grid_size = disp.GridSize();
    const int moved_index = (grid_size / 2) * grid_size + (grid_size / 2);
    disp.normals.resize(grid_size * grid_size, {0, 0, 1});
    disp.distances.resize(grid_size * grid_size, 0.0f);
    disp.offsets.resize(grid_size * grid_size, {0, 0, 0});
    disp.offsetNormals.resize(grid_size * grid_size, {0, 0, 1});
    disp.alphas.resize(grid_size * grid_size, 0.0f);
    disp.offsets[moved_index] = {13.0f, 0.0f, 0.0f};
    top_side.dispinfo = disp;

    std::vector<Vec3> grid_positions = ComputeDispGridPositions(solid, 0);
    REQUIRE(grid_positions.size() == static_cast<std::size_t>(grid_size * grid_size));
    const Vec3 moved_position = grid_positions[moved_index];

    const std::array<float, 2> uv0 = editor_face_uv(parent_quad[0], top_side);
    const std::array<float, 2> uv1 = editor_face_uv(parent_quad[1], top_side);
    const std::array<float, 2> uv2 = editor_face_uv(parent_quad[2], top_side);
    const std::array<float, 2> uv3 = editor_face_uv(parent_quad[3], top_side);
    const float t = 0.5f;
    const float s = 0.5f;
    const std::array<float, 2> expected_uv =
        lerp_editor_uv(lerp_editor_uv(uv0, uv1, t), lerp_editor_uv(uv3, uv2, t), s);

    BrushMesh mesh = BuildBrushMesh(solid);
    bool found_moved_vertex = false;
    for (const BrushFace& face : mesh.faces)
    {
        if (!face.isDisplacement)
            continue;

        for (const BrushVertex& vertex : face.vertices)
        {
            if (Length(vertex.pos - moved_position) >= 0.001f)
                continue;

            found_moved_vertex = true;
            REQUIRE(std::abs(vertex.u - expected_uv[0]) < 0.001f);
            REQUIRE(std::abs(vertex.v - expected_uv[1]) < 0.001f);

            const std::array<float, 2> displaced_position_uv = editor_face_uv(vertex.pos, top_side);
            REQUIRE(std::abs(vertex.u - displaced_position_uv[0]) > 1.0f);
        }
    }
    REQUIRE(found_moved_vertex);
}

void test_editor_displacement_position_edits_round_trip_source_field_vectors()
{
    VmfSolid solid = make_editor_test_box_solid({0, 0, 0}, {128, 128, 16}, "test/floor");
    VmfSide& top_side = solid.sides[0];

    std::vector<Vec3> parent_quad = ComputeSidePolygon(solid, 0);
    REQUIRE(parent_quad.size() == 4);

    VmfDispInfo disp;
    disp.power = 2;
    disp.startPosition = GLToSource(parent_quad[0]);
    const int grid_size = disp.GridSize();
    const int center = (grid_size / 2) * grid_size + (grid_size / 2);
    const int vert_count = grid_size * grid_size;
    disp.normals.resize(vert_count, {0, 0, 0});
    disp.distances.resize(vert_count, 0.0f);
    disp.offsets.resize(vert_count, {0, 0, 0});
    disp.offsetNormals.resize(vert_count, {0, 0, 1});
    disp.alphas.resize(vert_count, 0.0f);
    disp.triangleTags.resize((grid_size - 1) * (grid_size - 1) * 2, 0);
    top_side.dispinfo = disp;

    std::vector<Vec3> before = ComputeDispGridPositions(solid, 0);
    REQUIRE(before.size() == static_cast<std::size_t>(vert_count));

    const Vec3 target = before[center] + Vec3{0.0f, 16.0f, 0.0f};
    REQUIRE(SetDispVertexPositionGL(solid, 0, center, target));

    const VmfDispInfo& edited = solid.sides[0].dispinfo.value();
    REQUIRE(std::abs(edited.distances[center] - 16.0f) < 0.001f);
    REQUIRE(Dot(Normalize(SourceToGL(edited.normals[center])), Vec3{0, 1, 0}) > 0.999f);

    std::vector<Vec3> after = ComputeDispGridPositions(solid, 0);
    REQUIRE(Length(after[center] - target) < 0.001f);

    UpdateDispTriangleTags(solid, 0);
    for (int tag : solid.sides[0].dispinfo->triangleTags)
    {
        REQUIRE((tag & DISP_TRI_TAG_WALKABLE) != 0);
        REQUIRE((tag & DISP_TRI_TAG_BUILDABLE) != 0);
    }
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
        "--dx12-profile",
        "--dx12-no-async-recording",
        "--dx12-profile-path=dx12-profile.csv",
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
    REQUIRE(config.dx12_profile);
    REQUIRE(!config.dx12_async_recording);
    REQUIRE(config.dx12_profile_path == std::filesystem::path("dx12-profile.csv"));
    REQUIRE(config.deterministic_frames);
    REQUIRE(std::abs(config.tick_rate - 128.0) < 0.001);
    REQUIRE(config.content_root == std::filesystem::path("game"));
    REQUIRE(config.startup_commands.size() == 4);
    REQUIRE(config.startup_commands[0] == "exec autoexec.cfg");
    REQUIRE(config.startup_commands[1] == "connect 127.0.0.1:27016");
    REQUIRE(config.startup_commands[2] == "map de_dust2");
    REQUIRE(config.startup_commands[3] == "sv_cheats 1");
}

void test_runtime_defaults_support_other_games()
{
    const std::filesystem::path original_path = std::filesystem::current_path();
    struct CurrentPathGuard
    {
        std::filesystem::path path;
        ~CurrentPathGuard()
        {
            std::error_code error;
            std::filesystem::current_path(path, error);
        }
    } guard{original_path};

    const std::filesystem::path root = original_path / "build/test_runtime_defaults_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "data");
    std::ofstream(root / "data/game.marker").put('\n');
    std::filesystem::current_path(root);

    openstrike::RuntimeDefaults defaults;
    defaults.application_name = "ExampleGame";
    defaults.content_root_environment_variable.clear();
    defaults.fallback_content_root = "data";
    defaults.default_rml_document = "ui/example.rml";
    defaults.content_markers = {"game.marker"};

    const openstrike::RuntimeConfig config = openstrike::RuntimeConfig::from_command_line(openstrike::CommandLine({}), defaults);
    REQUIRE(config.application_name == "ExampleGame");
    REQUIRE(config.content_root == root / "data");
    REQUIRE(config.rml_document == std::filesystem::path("ui/example.rml"));

    std::filesystem::current_path(original_path);
    std::filesystem::remove_all(root);
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

void test_source_asset_store_reuses_vpk_directory_mounts()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_vpk_mount_cache";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game");

    const std::vector<unsigned char> vpk = make_minimal_vpk_dir("scripts", "sample", "txt", "cached vpk text");
    std::ofstream vpk_file(root / "game/pak01_dir.vpk", std::ios::binary);
    vpk_file.write(reinterpret_cast<const char*>(vpk.data()), static_cast<std::streamsize>(vpk.size()));
    vpk_file.close();

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::Logger::instance().clear_history();
    {
        openstrike::SourceAssetStore first(filesystem);
        REQUIRE(first.read_text("scripts/sample.txt") == "cached vpk text");

        openstrike::SourceAssetStore second(filesystem);
        REQUIRE(second.read_text("scripts/sample.txt") == "cached vpk text");
    }

    int mount_logs = 0;
    for (const openstrike::LogEntry& entry : openstrike::Logger::instance().recent_entries())
    {
        if (entry.message.find("mounted Source VPK") != std::string::npos)
        {
            ++mount_logs;
        }
    }
    REQUIRE(mount_logs == 1);

    std::filesystem::remove_all(root);
}

void test_source_keyvalues_parser_handles_source_roots_and_vectors()
{
    const std::string text =
        "versioninfo { \"formatversion\" \"100\" }\n"
        "world\n"
        "{\n"
        "    \"id\" \"1\"\n"
        "    \"classname\" \"worldspawn\"\n"
        "}\n"
        "\"LightmappedGeneric\"\n"
        "{\n"
        "    \"$color\" [128 64 32]\n"
        "    \"replace\" { \"$basetexture\" \"Brick\\Wall01.vtf\" }\n"
        "}\n";

    openstrike::SourceKeyValueParseResult parsed = openstrike::parse_source_keyvalues(text);
    REQUIRE(parsed.ok);
    REQUIRE(parsed.roots.size() == 3);
    REQUIRE(parsed.roots[1]->key == "world");
    REQUIRE(std::string(parsed.roots[1]->GetString("classname")) == "worldspawn");
    REQUIRE(openstrike::source_kv_find_value_ci(*parsed.roots[2], "$COLOR").has_value());
    REQUIRE(*openstrike::source_kv_find_value_ci(*parsed.roots[2], "$COLOR") == "[128 64 32]");
    REQUIRE(openstrike::source_kv_find_child_ci(*parsed.roots[2], "REPLACE") != nullptr);
}

void test_material_system_resolves_source_vmt_without_shader_combos()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_material_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "materials/brick");
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

        std::ofstream(root / "materials/maps/textured.vmt") <<
            "\"LightmappedGeneric\"\n"
            "{\n"
            "    \"$basetexture\" \"brick/textured\"\n"
            "}\n";

        std::ofstream(root / "materials/maps/unquoted_color.vmt") <<
            "\"LightmappedGeneric\"\n"
            "{\n"
            "    \"$color\" [128 64 32]\n"
            "}\n";

        const std::vector<unsigned char> rgba_pixels = {
            10, 20, 30, 255, 40, 50, 60, 255,
            70, 80, 90, 255, 100, 110, 120, 255,
        };
        const std::vector<unsigned char> vtf = make_minimal_vtf(2, 2, 0, rgba_pixels);
        std::ofstream vtf_file(root / "materials/brick/textured.vtf", std::ios::binary);
        vtf_file.write(reinterpret_cast<const char*>(vtf.data()), static_cast<std::streamsize>(vtf.size()));
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

    const openstrike::LoadedMaterial textured = materials.load_world_material("maps/textured");
    REQUIRE(textured.definition.found_source_material);
    REQUIRE(textured.definition.base_texture == "brick/textured");
    REQUIRE(textured.base_texture_loaded);
    REQUIRE(!textured.using_fallback_texture);
    REQUIRE(textured.base_texture.width == 2);
    REQUIRE(textured.base_texture.height == 2);
    REQUIRE(textured.base_texture.format == openstrike::SourceTextureFormat::Rgba8);
    REQUIRE(textured.base_texture.mips.size() == 1);
    REQUIRE(textured.base_texture.mips[0].bytes[0] == 10);
    REQUIRE(textured.base_texture.mips[0].bytes[1] == 20);
    REQUIRE(textured.base_texture.mips[0].bytes[2] == 30);
    REQUIRE(textured.base_texture.mips[0].bytes[3] == 255);

    const openstrike::LoadedMaterial unquoted_color = materials.load_world_material("maps/unquoted_color");
    REQUIRE(unquoted_color.definition.found_source_material);
    REQUIRE(std::abs(unquoted_color.definition.constants.base_color[0] - (128.0F / 255.0F)) < 0.001F);
    REQUIRE(std::abs(unquoted_color.definition.constants.base_color[1] - (64.0F / 255.0F)) < 0.001F);
    REQUIRE(std::abs(unquoted_color.definition.constants.base_color[2] - (32.0F / 255.0F)) < 0.001F);

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
    constexpr std::int32_t image_format_ati2n = 34;
    constexpr std::int32_t image_format_ati1n = 35;

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

    std::vector<unsigned char> ati1n_block(8, 0);
    ati1n_block[0] = 255;
    ati1n_block[1] = 0;
    const std::optional<openstrike::SourceTexture> bc4_texture = openstrike::load_vtf_texture(make_minimal_vtf(4, 4, image_format_ati1n, ati1n_block));
    REQUIRE(bc4_texture.has_value());
    REQUIRE(bc4_texture->format == openstrike::SourceTextureFormat::Bc4);

    const std::optional<openstrike::SourceTexture> bc4_rgba = openstrike::source_texture_to_rgba8(*bc4_texture);
    REQUIRE(bc4_rgba.has_value());
    REQUIRE(bc4_rgba->mips[0].bytes[0] == 255);
    REQUIRE(bc4_rgba->mips[0].bytes[1] == 255);
    REQUIRE(bc4_rgba->mips[0].bytes[2] == 255);
    REQUIRE(bc4_rgba->mips[0].bytes[3] == 255);

    std::vector<unsigned char> ati2n_block(16, 0);
    ati2n_block[0] = 255;
    ati2n_block[1] = 0;
    ati2n_block[8] = 128;
    ati2n_block[9] = 0;
    const std::optional<openstrike::SourceTexture> bc5_texture = openstrike::load_vtf_texture(make_minimal_vtf(4, 4, image_format_ati2n, ati2n_block));
    REQUIRE(bc5_texture.has_value());
    REQUIRE(bc5_texture->format == openstrike::SourceTextureFormat::Bc5);

    const std::optional<openstrike::SourceTexture> bc5_rgba = openstrike::source_texture_to_rgba8(*bc5_texture);
    REQUIRE(bc5_rgba.has_value());
    REQUIRE(bc5_rgba->mips[0].bytes[0] == 255);
    REQUIRE(bc5_rgba->mips[0].bytes[1] == 128);
    REQUIRE(bc5_rgba->mips[0].bytes[3] == 255);
}

void test_source_texture_loads_resource_vtf_metadata_and_legacy_formats()
{
    constexpr std::int32_t image_format_i8 = 5;
    const std::vector<unsigned char> intensity_pixels = {0, 64, 128, 255};
    const std::optional<openstrike::SourceTexture> texture =
        openstrike::load_vtf_texture(make_resource_vtf(2, 2, image_format_i8, intensity_pixels));

    REQUIRE(texture.has_value());
    REQUIRE(texture->format == openstrike::SourceTextureFormat::Rgba8);
    REQUIRE(texture->width == 2);
    REQUIRE(texture->height == 2);
    REQUIRE(texture->info.major_version == 7);
    REQUIRE(texture->info.minor_version == 5);
    REQUIRE(texture->info.header_size == 88);
    REQUIRE(texture->info.image_format == image_format_i8);
    REQUIRE(texture->info.resources.size() == 1);
    REQUIRE(texture->info.resources[0].type == 0x30);
    REQUIRE(texture->info.resources[0].data == 88);
    REQUIRE(std::abs(texture->info.reflectivity[1] - 0.5F) < 0.001F);
    REQUIRE(texture->mips.size() == 1);
    REQUIRE(texture->mips[0].bytes.size() == 16);
    REQUIRE(texture->mips[0].bytes[0] == 0);
    REQUIRE(texture->mips[0].bytes[1] == 0);
    REQUIRE(texture->mips[0].bytes[2] == 0);
    REQUIRE(texture->mips[0].bytes[3] == 255);
    REQUIRE(texture->mips[0].bytes[4] == 64);
    REQUIRE(texture->mips[0].bytes[5] == 64);
    REQUIRE(texture->mips[0].bytes[6] == 64);
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

    const std::vector<unsigned char> binary_payload{0x00U, 0x7FU, 0x80U, 0xFFU, 0x42U};
    const std::vector<unsigned char> command_packet =
        openstrike::encode_network_packet(openstrike::NetworkMessageType::UserCommand, 4, 3, 65, binary_payload);
    const std::optional<openstrike::NetworkPacket> decoded_command = openstrike::decode_network_packet(command_packet);
    REQUIRE(decoded_command.has_value());
    REQUIRE(decoded_command->header.type == openstrike::NetworkMessageType::UserCommand);
    REQUIRE(decoded_command->header.sequence == 4);
    REQUIRE(decoded_command->header.ack == 3);
    REQUIRE(decoded_command->header.tick == 65);
    REQUIRE(decoded_command->payload == binary_payload);

    const std::vector<unsigned char> snapshot_packet =
        openstrike::encode_network_packet(openstrike::NetworkMessageType::Snapshot, 5, 4, 66, binary_payload);
    const std::optional<openstrike::NetworkPacket> decoded_snapshot = openstrike::decode_network_packet(snapshot_packet);
    REQUIRE(decoded_snapshot.has_value());
    REQUIRE(decoded_snapshot->header.type == openstrike::NetworkMessageType::Snapshot);
    REQUIRE(decoded_snapshot->header.tick == 66);
    REQUIRE(decoded_snapshot->payload == binary_payload);

    std::vector<unsigned char> corrupt = packet;
    corrupt[0] = 0;
    REQUIRE(!openstrike::decode_network_packet(corrupt).has_value());
}

void test_network_user_command_delta_batch_and_input_roundtrip()
{
    openstrike::UserCommand base;
    base.command_number = 10;
    base.tick_count = 100;
    base.viewangles = {0.0F, 90.0F, 0.0F};
    base.forwardmove = 0.25F;
    base.random_seed = 12345;

    openstrike::UserCommand command = base;
    command.command_number = 11;
    command.tick_count = 101;
    command.viewangles = {-4.0F, 100.0F, 0.0F};
    command.aimdirection = {1.0F, 0.0F, 0.0F};
    command.forwardmove = 1.0F;
    command.sidemove = -0.5F;
    command.buttons = openstrike::UserCommandButtonForward | openstrike::UserCommandButtonJump | openstrike::UserCommandButtonSpeed;
    command.weaponselect = 2;
    command.random_seed = 12346;
    command.mousedx = 12;
    command.mousedy = -5;

    const std::vector<unsigned char> delta = openstrike::encode_user_command_delta(command, &base);
    const std::optional<openstrike::UserCommand> decoded = openstrike::decode_user_command_delta(delta, &base);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->command_number == command.command_number);
    REQUIRE(decoded->tick_count == command.tick_count);
    REQUIRE(decoded->viewangles.y == command.viewangles.y);
    REQUIRE(decoded->aimdirection.x == command.aimdirection.x);
    REQUIRE(decoded->forwardmove == command.forwardmove);
    REQUIRE(decoded->sidemove == command.sidemove);
    REQUIRE(decoded->buttons == command.buttons);
    REQUIRE(decoded->weaponselect == command.weaponselect);
    REQUIRE(decoded->random_seed == command.random_seed);
    REQUIRE(decoded->mousedx == command.mousedx);
    REQUIRE(decoded->mousedy == command.mousedy);

    std::vector<unsigned char> corrupt = delta;
    corrupt.back() ^= 0x5AU;
    REQUIRE(!openstrike::decode_user_command_delta(corrupt, &base).has_value());

    openstrike::UserCommandBatch batch;
    batch.num_backup_commands = 1;
    batch.num_new_commands = 1;
    batch.commands = {base, command};
    const std::vector<unsigned char> batch_bytes = openstrike::encode_user_command_batch(batch);
    const std::optional<openstrike::UserCommandBatch> decoded_batch = openstrike::decode_user_command_batch(batch_bytes);
    REQUIRE(decoded_batch.has_value());
    REQUIRE(decoded_batch->num_backup_commands == 1);
    REQUIRE(decoded_batch->num_new_commands == 1);
    REQUIRE(decoded_batch->commands.size() == 2);
    REQUIRE(decoded_batch->commands[1].command_number == command.command_number);
    REQUIRE(decoded_batch->commands[1].buttons == command.buttons);

    const openstrike::InputCommand movement = openstrike::movement_input_from_user_command(command);
    REQUIRE(movement.jump);
    REQUIRE(movement.walk);
    REQUIRE(movement.move_y == command.forwardmove);
    REQUIRE(movement.move_x == command.sidemove);
}

void test_network_messages_round_trip_source_families()
{
    openstrike::UserCommand command;
    command.command_number = 7;
    command.tick_count = 64;
    command.forwardmove = 1.0F;
    command.buttons = openstrike::UserCommandButtonForward;

    openstrike::UserCommandBatch batch;
    batch.num_new_commands = 1;
    batch.commands.push_back(command);

    openstrike::NetStringTableMessage table;
    table.table_id = 1;
    table.name = "userinfo";
    table.max_entries = 64;
    table.revision = 2;
    table.entries.push_back(openstrike::NetStringTableEntry{.index = 3, .value = "player", .user_data = {1, 2, 3, 4}});

    const std::vector<unsigned char> entity_payload{0x10U, 0x20U, 0x30U, 0x40U};
    std::vector<openstrike::NetMessage> messages;
    messages.push_back(openstrike::make_signon_state_message(openstrike::NetSignonStateMessage{
        .state = openstrike::NetSignonState::Full,
        .spawn_count = 2,
        .num_server_players = 1,
        .map_name = "de_dust2",
    }));
    messages.push_back(openstrike::make_server_info_message(openstrike::NetServerInfoMessage{
        .server_count = 2,
        .dedicated = true,
        .max_clients = 32,
        .max_classes = 128,
        .player_slot = 4,
        .map_name = "de_dust2",
    }));
    messages.push_back(openstrike::make_create_string_table_message(table));
    table.revision = 3;
    messages.push_back(openstrike::make_update_string_table_message(table));
    messages.push_back(openstrike::make_move_message(batch));
    messages.push_back(openstrike::make_packet_entities_message(openstrike::NetPacketEntitiesMessage{
        .max_entries = 2048,
        .updated_entries = 1,
        .is_delta = true,
        .delta_from = 63,
        .entity_data = entity_payload,
    }));

    const std::vector<unsigned char> encoded = openstrike::encode_net_messages(messages);
    const std::optional<std::vector<openstrike::NetMessage>> decoded = openstrike::decode_net_messages(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == messages.size());

    const std::optional<openstrike::NetSignonStateMessage> signon = openstrike::read_signon_state_message((*decoded)[0]);
    REQUIRE(signon.has_value());
    REQUIRE(signon->state == openstrike::NetSignonState::Full);
    REQUIRE(signon->map_name == "de_dust2");

    const std::optional<openstrike::NetServerInfoMessage> server_info = openstrike::read_server_info_message((*decoded)[1]);
    REQUIRE(server_info.has_value());
    REQUIRE(server_info->max_clients == 32);
    REQUIRE(server_info->map_name == "de_dust2");

    const std::optional<openstrike::NetStringTableMessage> created_table = openstrike::read_string_table_message((*decoded)[2]);
    REQUIRE(created_table.has_value());
    REQUIRE(created_table->name == "userinfo");
    REQUIRE(created_table->entries.size() == 1);
    REQUIRE(created_table->entries[0].user_data == std::vector<unsigned char>({1, 2, 3, 4}));

    const std::optional<openstrike::UserCommandBatch> decoded_move = openstrike::read_move_message((*decoded)[4]);
    REQUIRE(decoded_move.has_value());
    REQUIRE(decoded_move->commands.size() == 1);
    REQUIRE(decoded_move->commands[0].command_number == 7);

    const std::optional<openstrike::NetPacketEntitiesMessage> entities = openstrike::read_packet_entities_message((*decoded)[5]);
    REQUIRE(entities.has_value());
    REQUIRE(entities->is_delta);
    REQUIRE(entities->delta_from == 63);
    REQUIRE(entities->entity_data == entity_payload);
}

void test_network_channel_reliable_fragmentation_and_ack()
{
    openstrike::NetChannel sender("sender");
    openstrike::NetChannel receiver("receiver");
    openstrike::NetChannelConfig config;
    config.max_routable_payload_bytes = 96;
    config.fragment_payload_bytes = 64;
    sender.set_config(config);
    receiver.set_config(config);

    const std::string command(512, 'x');
    sender.queue_message(openstrike::make_string_command_message(command));
    const std::vector<std::vector<unsigned char>> datagrams = sender.transmit(0.0, true);
    REQUIRE(datagrams.size() > 1);
    REQUIRE(sender.has_pending_reliable_data());

    std::vector<openstrike::NetMessage> received_messages;
    for (const std::vector<unsigned char>& datagram : datagrams)
    {
        openstrike::NetChannelProcessResult result = receiver.process_datagram(datagram);
        REQUIRE(result.accepted);
        received_messages.insert(received_messages.end(), result.messages.begin(), result.messages.end());
    }

    REQUIRE(received_messages.size() == 1);
    const std::optional<std::string> decoded_command = openstrike::read_string_command_message(received_messages.front());
    REQUIRE(decoded_command.has_value());
    REQUIRE(*decoded_command == command);

    const std::vector<std::vector<unsigned char>> ack_datagrams = receiver.transmit(1.0, true);
    REQUIRE(!ack_datagrams.empty());
    for (const std::vector<unsigned char>& datagram : ack_datagrams)
    {
        const openstrike::NetChannelProcessResult result = sender.process_datagram(datagram);
        REQUIRE(result.accepted);
    }
    REQUIRE(!sender.has_pending_reliable_data());
}

void test_network_channel_reassembles_reordered_fragments()
{
    openstrike::NetChannel sender("sender");
    openstrike::NetChannel receiver("receiver");
    openstrike::NetChannelConfig config;
    config.max_routable_payload_bytes = 96;
    config.fragment_payload_bytes = 64;
    sender.set_config(config);
    receiver.set_config(config);

    const std::string command(512, 'r');
    sender.queue_message(openstrike::make_string_command_message(command));
    const std::vector<std::vector<unsigned char>> datagrams = sender.transmit(0.0, true);
    REQUIRE(datagrams.size() > 1);

    std::vector<openstrike::NetMessage> received_messages;
    for (auto it = datagrams.rbegin(); it != datagrams.rend(); ++it)
    {
        openstrike::NetChannelProcessResult result = receiver.process_datagram(*it);
        REQUIRE(result.accepted);
        received_messages.insert(received_messages.end(), result.messages.begin(), result.messages.end());
    }

    REQUIRE(received_messages.size() == 1);
    const std::optional<std::string> decoded_command = openstrike::read_string_command_message(received_messages.front());
    REQUIRE(decoded_command.has_value());
    REQUIRE(*decoded_command == command);
}

void test_network_channel_reliable_retransmit_repairs_lost_ack()
{
    openstrike::NetChannel sender("sender");
    openstrike::NetChannel receiver("receiver");

    sender.queue_message(openstrike::make_string_command_message("first"));
    const std::vector<std::vector<unsigned char>> first_datagrams = sender.transmit(0.0, true);
    REQUIRE(first_datagrams.size() == 1);

    openstrike::NetChannelProcessResult first_result = receiver.process_datagram(first_datagrams.front());
    REQUIRE(first_result.accepted);
    REQUIRE(first_result.messages.size() == 1);
    const std::optional<std::string> first_command = openstrike::read_string_command_message(first_result.messages.front());
    REQUIRE(first_command.has_value());
    REQUIRE(*first_command == "first");

    sender.queue_message(openstrike::make_string_command_message("second"));
    const std::vector<std::vector<unsigned char>> retransmit_datagrams = sender.transmit(1.0, true);
    REQUIRE(retransmit_datagrams.size() == 1);
    openstrike::NetChannelProcessResult duplicate_result = receiver.process_datagram(retransmit_datagrams.front());
    REQUIRE(duplicate_result.accepted);
    REQUIRE(duplicate_result.needs_ack);
    REQUIRE(duplicate_result.messages.empty());

    const std::vector<std::vector<unsigned char>> ack_datagrams = receiver.transmit(2.0, true);
    REQUIRE(!ack_datagrams.empty());
    for (const std::vector<unsigned char>& datagram : ack_datagrams)
    {
        REQUIRE(sender.process_datagram(datagram).accepted);
    }
    REQUIRE(sender.has_pending_reliable_data());

    const std::vector<std::vector<unsigned char>> second_datagrams = sender.transmit(3.0, true);
    REQUIRE(second_datagrams.size() == 1);
    openstrike::NetChannelProcessResult second_result = receiver.process_datagram(second_datagrams.front());
    REQUIRE(second_result.accepted);
    REQUIRE(second_result.messages.size() == 1);
    const std::optional<std::string> second_command = openstrike::read_string_command_message(second_result.messages.front());
    REQUIRE(second_command.has_value());
    REQUIRE(*second_command == "second");
}

void test_network_channel_rejects_excessive_fragment_count()
{
    openstrike::NetworkByteWriter writer;
    writer.write_u32(0x4E414843U);
    writer.write_u16(1);
    writer.write_u32(1);
    writer.write_u32(0);
    writer.write_u32(0);
    writer.write_u32(0);
    writer.write_u8(openstrike::NetChannelPacketSplit);
    writer.write_u8(0);
    writer.write_u16(1);
    writer.write_u16(65535);
    writer.write_u16(0);
    writer.write_u32(1);
    writer.write_u32(0);

    openstrike::NetChannel receiver("receiver");
    const openstrike::NetChannelProcessResult result = receiver.process_datagram(writer.take_bytes());
    REQUIRE(!result.accepted);
    REQUIRE(receiver.stats().packets_dropped == 1);
}

void test_network_snapshot_delta_round_trip_and_apply()
{
    openstrike::NetworkClassRegistry registry;
    REQUIRE(registry.register_class(openstrike::NetworkClassDefinition{
        .class_id = 1,
        .class_name = "player",
        .fields = {
            {"health", openstrike::NetworkFieldType::Int32},
            {"origin", openstrike::NetworkFieldType::Vec3},
            {"weapon", openstrike::NetworkFieldType::String},
        },
    }));
    REQUIRE(!registry.register_class(openstrike::NetworkClassDefinition{.class_id = 1, .class_name = "duplicate"}));
    REQUIRE(registry.find(1) != nullptr);
    REQUIRE(registry.crc() != 0);

    openstrike::NetworkSnapshot from;
    from.tick = 10;
    from.entities.push_back(openstrike::NetworkEntityState{
        .entity_id = 1,
        .class_id = 1,
        .serial = 100,
        .fields = {std::int32_t{100}, openstrike::Vec3{1.0F, 2.0F, 3.0F}, std::string("ak47")},
    });
    from.entities.push_back(openstrike::NetworkEntityState{
        .entity_id = 2,
        .class_id = 1,
        .serial = 200,
        .fields = {std::int32_t{50}, openstrike::Vec3{4.0F, 5.0F, 6.0F}, std::string("m4a1")},
    });

    openstrike::NetworkSnapshot to;
    to.tick = 11;
    to.entities.push_back(openstrike::NetworkEntityState{
        .entity_id = 1,
        .class_id = 1,
        .serial = 100,
        .fields = {std::int32_t{87}, openstrike::Vec3{8.0F, 2.0F, 3.0F}, std::string("ak47")},
    });
    to.entities.push_back(openstrike::NetworkEntityState{
        .entity_id = 3,
        .class_id = 1,
        .serial = 300,
        .fields = {std::int32_t{100}, openstrike::Vec3{9.0F, 9.0F, 9.0F}, std::string("awp")},
    });

    const std::vector<unsigned char> snapshot_bytes = openstrike::encode_network_snapshot(from);
    const std::optional<openstrike::NetworkSnapshot> decoded_snapshot = openstrike::decode_network_snapshot(snapshot_bytes);
    REQUIRE(decoded_snapshot.has_value());
    REQUIRE(decoded_snapshot->tick == from.tick);
    REQUIRE(decoded_snapshot->entities.size() == 2);
    REQUIRE(std::get<std::int32_t>(decoded_snapshot->entities[0].fields[0]) == 100);

    const openstrike::NetworkSnapshotDelta delta = openstrike::make_network_snapshot_delta(from, to);
    REQUIRE(delta.from_tick == 10);
    REQUIRE(delta.to_tick == 11);
    REQUIRE(delta.changed_entities.size() == 2);
    REQUIRE(delta.removed_entities.size() == 1);
    REQUIRE(delta.removed_entities[0] == 2);

    const std::vector<unsigned char> delta_bytes = openstrike::encode_network_snapshot_delta(delta);
    const std::optional<openstrike::NetworkSnapshotDelta> decoded_delta = openstrike::decode_network_snapshot_delta(delta_bytes);
    REQUIRE(decoded_delta.has_value());
    const std::optional<openstrike::NetworkSnapshot> applied = openstrike::apply_network_snapshot_delta(from, *decoded_delta);
    REQUIRE(applied.has_value());
    REQUIRE(applied->tick == to.tick);
    REQUIRE(applied->entities.size() == 2);
    REQUIRE(applied->entities[0].entity_id == 1);
    REQUIRE(std::get<std::int32_t>(applied->entities[0].fields[0]) == 87);
    const openstrike::Vec3 origin = std::get<openstrike::Vec3>(applied->entities[0].fields[1]);
    REQUIRE(origin.x == 8.0F);
    REQUIRE(applied->entities[1].entity_id == 3);
}

void test_network_prediction_reconciles_and_replays_user_commands()
{
    openstrike::PlayerState start;
    start.on_ground = true;

    openstrike::ClientPredictionBuffer prediction;
    prediction.reset(start);

    openstrike::MovementTuning tuning;
    openstrike::UserCommand first;
    first.command_number = 1;
    first.tick_count = 1;
    first.forwardmove = 1.0F;
    openstrike::UserCommand second = first;
    second.command_number = 2;
    second.tick_count = 2;
    second.sidemove = 1.0F;

    const openstrike::PlayerState predicted_after_first = prediction.predict(first, tuning);
    REQUIRE(predicted_after_first.on_ground);
    const openstrike::PlayerState predicted_after_second = prediction.predict(second, tuning);
    REQUIRE(prediction.history().size() == 2);

    openstrike::PlayerState authoritative = prediction.history().front().after;
    authoritative.origin.x += 8.0F;
    const openstrike::PredictionReconcileResult reconciled = prediction.reconcile(1, authoritative, tuning, 0.001F);
    REQUIRE(reconciled.corrected);
    REQUIRE(reconciled.replayed_commands == 1);
    REQUIRE(prediction.history().size() == 1);
    REQUIRE(prediction.history().front().command.command_number == 2);
    REQUIRE(prediction.current_state().origin.x != predicted_after_second.origin.x);
}

void connect_loopback_client(openstrike::NetworkServer& server, openstrike::NetworkClient& client)
{
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

    bool client_reached_full = client.signon_state() == openstrike::NetSignonState::Full;
    for (int attempt = 0; attempt < 64 && !client_reached_full; ++attempt)
    {
        server.poll(static_cast<std::uint64_t>(80 + attempt));
        (void)server.drain_events();
        client.poll(static_cast<std::uint64_t>(80 + attempt));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::SignonStateChanged &&
                event.signon_state == openstrike::NetSignonState::Full)
            {
                client_reached_full = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client_reached_full);

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        server.poll(static_cast<std::uint64_t>(160 + attempt));
        (void)server.drain_events();
        client.poll(static_cast<std::uint64_t>(160 + attempt));
        (void)client.drain_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void test_network_udp_loopback_connects_and_exchanges_text()
{
    openstrike::NetworkServer server;
    REQUIRE(server.start(0));
    const std::vector<openstrike::NetworkEvent> startup_events = server.drain_events();
    REQUIRE(!startup_events.empty());

    openstrike::NetworkClient client;
    connect_loopback_client(server, client);

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

void test_network_udp_loopback_client_sends_user_command()
{
    openstrike::NetworkServer server;
    REQUIRE(server.start(0));
    (void)server.drain_events();

    openstrike::NetworkClient client;
    connect_loopback_client(server, client);

    const std::vector<unsigned char> command{0x01U, 0x00U, 0xFEU, 0x7FU, 0x33U};
    REQUIRE(client.send_user_command(command, 410));

    bool server_received_command = false;
    for (int attempt = 0; attempt < 64 && !server_received_command; ++attempt)
    {
        server.poll(static_cast<std::uint64_t>(410 + attempt));
        for (const openstrike::NetworkEvent& event : server.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::UserCommandReceived && event.payload == command && event.tick == 410)
            {
                REQUIRE(event.connection_id == server.clients().front().connection_id);
                server_received_command = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(server_received_command);

    client.disconnect(500);
    server.stop();
}

void test_network_udp_loopback_server_sends_snapshot()
{
    openstrike::NetworkServer server;
    REQUIRE(server.start(0));
    (void)server.drain_events();

    openstrike::NetworkClient client;
    connect_loopback_client(server, client);

    const std::vector<unsigned char> snapshot{0x10U, 0x20U, 0x00U, 0xFFU, 0x5AU, 0xC3U};
    server.broadcast_snapshot(snapshot, 620);

    int snapshot_events = 0;
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        client.poll(static_cast<std::uint64_t>(620 + attempt));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::SnapshotReceived && event.payload == snapshot && event.tick == 620)
            {
                REQUIRE(event.connection_id == client.connection_id());
                ++snapshot_events;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(snapshot_events == 1);

    client.disconnect(700);
    server.stop();
}

void test_network_client_defers_channel_signon_until_connect_accept()
{
    openstrike::UdpSocket fake_server;
    REQUIRE(fake_server.open(0));

    openstrike::NetworkClient client;
    REQUIRE(client.connect(openstrike::NetworkAddress::localhost(fake_server.local_port()), 1));
    const openstrike::NetworkAddress client_address = openstrike::NetworkAddress::localhost(client.local_port());

    for (int attempt = 0; attempt < 16; ++attempt)
    {
        if (fake_server.receive())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    openstrike::NetChannel server_channel("fake-server");
    server_channel.queue_message(openstrike::make_signon_state_message(openstrike::NetSignonStateMessage{
        .state = openstrike::NetSignonState::Full,
        .spawn_count = 1,
        .num_server_players = 1,
    }));
    const std::vector<std::vector<unsigned char>> channel_datagrams = server_channel.transmit(2.0, true);
    REQUIRE(channel_datagrams.size() == 1);

    const std::vector<unsigned char> channel_packet = openstrike::encode_network_packet(
        openstrike::NetworkMessageType::Channel,
        2,
        0,
        2,
        channel_datagrams.front());
    REQUIRE(fake_server.send_to(client_address, channel_packet));

    bool saw_early_channel_packet = false;
    bool saw_zero_connection_signon = false;
    for (int attempt = 0; attempt < 16 && !saw_early_channel_packet; ++attempt)
    {
        client.poll(2);
        saw_early_channel_packet = client.stats().packets_received > 0;
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::SignonStateChanged && event.connection_id == 0)
            {
                saw_zero_connection_signon = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(saw_early_channel_packet);
    REQUIRE(!saw_zero_connection_signon);
    REQUIRE(client.connection_id() == 0);
    REQUIRE(client.signon_state() != openstrike::NetSignonState::Full);

    const std::vector<unsigned char> accept_payload = openstrike::make_connect_accept_payload(42);
    const std::vector<unsigned char> accept_packet = openstrike::encode_network_packet(
        openstrike::NetworkMessageType::ConnectAccept,
        1,
        0,
        3,
        accept_payload);
    REQUIRE(fake_server.send_to(client_address, accept_packet));

    bool connected = false;
    bool reached_full = false;
    for (int attempt = 0; attempt < 16 && !reached_full; ++attempt)
    {
        client.poll(3);
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.connection_id == 0 &&
                (event.type == openstrike::NetworkEventType::ConnectedToServer ||
                    event.type == openstrike::NetworkEventType::SignonStateChanged))
            {
                saw_zero_connection_signon = true;
            }
            if (event.type == openstrike::NetworkEventType::ConnectedToServer && event.connection_id == 42)
            {
                connected = true;
            }
            if (event.type == openstrike::NetworkEventType::SignonStateChanged &&
                event.connection_id == 42 &&
                event.signon_state == openstrike::NetSignonState::Full)
            {
                reached_full = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(connected);
    REQUIRE(reached_full);
    REQUIRE(!saw_zero_connection_signon);
    REQUIRE(client.connection_id() == 42);
    REQUIRE(client.signon_state() == openstrike::NetSignonState::Full);

    client.disconnect(10);
    fake_server.close();
}

void test_network_udp_loopback_signon_and_user_command_batch()
{
    openstrike::NetworkServer server;
    REQUIRE(server.start(0));
    (void)server.drain_events();

    openstrike::NetworkClient client;
    connect_loopback_client(server, client);

    bool client_reached_full = client.signon_state() == openstrike::NetSignonState::Full;
    for (int attempt = 0; attempt < 64 && !client_reached_full; ++attempt)
    {
        server.poll(static_cast<std::uint64_t>(720 + attempt));
        (void)server.drain_events();
        client.poll(static_cast<std::uint64_t>(720 + attempt));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::SignonStateChanged &&
                event.signon_state == openstrike::NetSignonState::Full)
            {
                client_reached_full = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client_reached_full);
    REQUIRE(client.signon_state() == openstrike::NetSignonState::Full);

    openstrike::UserCommand backup;
    backup.command_number = 20;
    backup.tick_count = 120;
    backup.forwardmove = 1.0F;
    backup.buttons = openstrike::UserCommandButtonForward;

    openstrike::UserCommand current = backup;
    current.command_number = 21;
    current.tick_count = 121;
    current.forwardmove = 0.75F;
    current.sidemove = -0.25F;
    current.buttons = openstrike::UserCommandButtonForward | openstrike::UserCommandButtonJump;

    openstrike::UserCommandBatch batch;
    batch.num_backup_commands = 1;
    batch.num_new_commands = 1;
    batch.commands = {backup, current};
    REQUIRE(client.send_user_commands(batch, 800));

    bool server_received_batch = false;
    for (int attempt = 0; attempt < 64 && !server_received_batch; ++attempt)
    {
        server.poll(static_cast<std::uint64_t>(800 + attempt));
        for (const openstrike::NetworkEvent& event : server.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::UserCommandBatchReceived)
            {
                REQUIRE(event.connection_id == server.clients().front().connection_id);
                REQUIRE(event.num_backup_commands == 1);
                REQUIRE(event.num_new_commands == 1);
                REQUIRE(event.user_commands.size() == 2);
                REQUIRE(event.user_commands[1].command_number == current.command_number);
                REQUIRE(event.user_commands[1].buttons == current.buttons);
                server_received_batch = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(server_received_batch);

    const std::vector<unsigned char> entity_payload{0xABU, 0xCDU, 0x01U, 0x02U};
    server.broadcast_message(openstrike::make_packet_entities_message(openstrike::NetPacketEntitiesMessage{
        .max_entries = 2048,
        .updated_entries = 1,
        .is_delta = true,
        .delta_from = 799,
        .entity_data = entity_payload,
    }),
        840);

    bool client_received_channel_snapshot = false;
    for (int attempt = 0; attempt < 64 && !client_received_channel_snapshot; ++attempt)
    {
        client.poll(static_cast<std::uint64_t>(840 + attempt));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::SnapshotReceived && event.payload == entity_payload)
            {
                client_received_channel_snapshot = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client_received_channel_snapshot);

    client.disconnect(900);
    server.stop();
}

void test_team_id_helpers_match_counter_strike()
{
    REQUIRE(openstrike::TEAM_UNASSIGNED == 0);
    REQUIRE(openstrike::TEAM_SPECTATOR == 1);
    REQUIRE(openstrike::TEAM_TERRORIST == 2);
    REQUIRE(openstrike::TEAM_CT == 3);
    REQUIRE(openstrike::is_joinable_team_id(0));
    REQUIRE(openstrike::is_joinable_team_id(1));
    REQUIRE(openstrike::is_joinable_team_id(2));
    REQUIRE(openstrike::is_joinable_team_id(3));
    REQUIRE(!openstrike::is_joinable_team_id(4));
    REQUIRE(openstrike::team_name(openstrike::TEAM_TERRORIST) == "Terrorists");
    REQUIRE(openstrike::team_short_name(openstrike::TEAM_CT) == "CT");
}

void test_team_manager_membership_and_scores()
{
    openstrike::TeamManager teams;
    openstrike::TeamJoinRules rules;
    rules.require_join_game = false;

    REQUIRE(teams.try_join_team(10, openstrike::TEAM_TERRORIST, true, rules).accepted);
    REQUIRE(teams.try_join_team(11, openstrike::TEAM_CT, true, rules).accepted);
    REQUIRE(teams.find_team(openstrike::TEAM_TERRORIST)->members.size() == 1);
    REQUIRE(teams.find_team(openstrike::TEAM_CT)->members.size() == 1);
    REQUIRE(teams.find_team(openstrike::TEAM_TERRORIST)->alive_count == 1);

    teams.add_team_score(openstrike::TEAM_TERRORIST, 2);
    teams.set_team_score(openstrike::TEAM_CT, 3);
    teams.mark_surrendered(openstrike::TEAM_TERRORIST);
    REQUIRE(teams.find_team(openstrike::TEAM_TERRORIST)->score_total == 2);
    REQUIRE(teams.find_team(openstrike::TEAM_CT)->score_total == 3);
    REQUIRE(teams.find_team(openstrike::TEAM_TERRORIST)->surrendered);

    REQUIRE(teams.try_join_team(10, openstrike::TEAM_SPECTATOR, true, rules).accepted);
    REQUIRE(teams.find_team(openstrike::TEAM_TERRORIST)->members.empty());
    REQUIRE(teams.find_team(openstrike::TEAM_SPECTATOR)->members.size() == 1);
    REQUIRE(teams.find_player(10)->alive == false);
}

void test_team_auto_assign_uses_smaller_team_then_lower_score()
{
    openstrike::TeamManager teams;
    openstrike::TeamJoinRules rules;
    rules.require_join_game = false;
    rules.limit_teams = 0;

    REQUIRE(teams.try_join_team(1, openstrike::TEAM_TERRORIST, true, rules).accepted);
    openstrike::TeamJoinResult result = teams.try_join_team(2, openstrike::TEAM_UNASSIGNED, true, rules);
    REQUIRE(result.accepted);
    REQUIRE(result.resolved_team == openstrike::TEAM_CT);

    openstrike::TeamManager score_tie;
    score_tie.set_team_score(openstrike::TEAM_TERRORIST, 1);
    score_tie.set_team_score(openstrike::TEAM_CT, 3);
    result = score_tie.try_join_team(3, openstrike::TEAM_UNASSIGNED, true, rules);
    REQUIRE(result.accepted);
    REQUIRE(result.resolved_team == openstrike::TEAM_TERRORIST);
}

void test_team_join_rejections_for_capacity_stack_spectator_and_humanteam()
{
    openstrike::TeamJoinRules rules;
    rules.require_join_game = false;
    rules.max_players = 2;
    rules.terrorist_spawn_count = 1;
    rules.ct_spawn_count = 1;
    rules.limit_teams = 0;

    openstrike::TeamManager full;
    REQUIRE(full.try_join_team(1, openstrike::TEAM_TERRORIST, true, rules).accepted);
    openstrike::TeamJoinResult result = full.try_join_team(2, openstrike::TEAM_TERRORIST, true, rules);
    REQUIRE(!result.accepted);
    REQUIRE(result.reason == openstrike::TeamJoinFailedReason::TerroristsFull);

    openstrike::TeamManager stacked;
    rules.max_players = 10;
    rules.terrorist_spawn_count = 0;
    rules.ct_spawn_count = 0;
    rules.limit_teams = 1;
    REQUIRE(stacked.try_join_team(1, openstrike::TEAM_TERRORIST, true, rules).accepted);
    result = stacked.try_join_team(2, openstrike::TEAM_TERRORIST, true, rules);
    REQUIRE(!result.accepted);
    REQUIRE(result.reason == openstrike::TeamJoinFailedReason::TooManyTs);

    openstrike::TeamManager spectator;
    rules.limit_teams = 0;
    rules.allow_spectators = false;
    result = spectator.try_join_team(3, openstrike::TEAM_SPECTATOR, true, rules);
    REQUIRE(!result.accepted);
    REQUIRE(result.reason == openstrike::TeamJoinFailedReason::CannotJoinSpectator);

    openstrike::TeamManager humans;
    rules.allow_spectators = true;
    rules.human_team = "CT";
    result = humans.try_join_team(4, openstrike::TEAM_TERRORIST, true, rules);
    REQUIRE(!result.accepted);
    REQUIRE(result.reason == openstrike::TeamJoinFailedReason::HumansCanOnlyJoinCts);
    result = humans.try_join_team(4, openstrike::TEAM_CT, true, rules);
    REQUIRE(result.accepted);
}

void test_team_command_jointeam_applies_local_offline()
{
    openstrike::RuntimeConfig config;
    config.content_root = std::filesystem::current_path();
    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);

    context.command_buffer.add_text("joingame; jointeam 2 1");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    const openstrike::TeamPlayerState* player = context.teams.find_player(openstrike::kLocalTeamPlayerId);
    REQUIRE(player != nullptr);
    REQUIRE(player->current_team == openstrike::TEAM_TERRORIST);
    REQUIRE(player->alive);
}

void test_team_command_jointeam_requires_joingame()
{
    openstrike::RuntimeConfig config;
    config.content_root = std::filesystem::current_path();
    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);

    context.command_buffer.add_text("jointeam 2 1");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    const openstrike::TeamPlayerState* player = context.teams.find_player(openstrike::kLocalTeamPlayerId);
    REQUIRE(player != nullptr);
    REQUIRE(player->current_team == openstrike::TEAM_UNASSIGNED);
    REQUIRE(!player->alive);
    REQUIRE(player->last_join_failure == openstrike::TeamJoinFailedReason::ChangedTooOften);

    context.command_buffer.add_text("joingame; jointeam 2 1");
    context.command_buffer.execute(context.commands, console_context);

    player = context.teams.find_player(openstrike::kLocalTeamPlayerId);
    REQUIRE(player != nullptr);
    REQUIRE(player->current_team == openstrike::TEAM_TERRORIST);
    REQUIRE(player->alive);
    REQUIRE(player->last_join_failure == openstrike::TeamJoinFailedReason::None);
}

void test_team_payloads_and_snapshot_round_trip()
{
    const openstrike::TeamUserCommand command{
        .kind = openstrike::TeamUserCommandKind::JoinTeam,
        .requested_team = openstrike::TEAM_CT,
        .force = true,
    };
    const std::vector<unsigned char> command_payload = openstrike::make_team_user_command_payload(command);
    const std::optional<openstrike::TeamUserCommand> decoded_command = openstrike::read_team_user_command_payload(command_payload);
    REQUIRE(decoded_command.has_value());
    REQUIRE(decoded_command->kind == openstrike::TeamUserCommandKind::JoinTeam);
    REQUIRE(decoded_command->requested_team == openstrike::TEAM_CT);
    REQUIRE(decoded_command->force);

    openstrike::TeamManager teams;
    openstrike::TeamJoinRules rules;
    rules.require_join_game = false;
    REQUIRE(teams.try_join_team(77, openstrike::TEAM_CT, true, rules).accepted);
    teams.set_team_score(openstrike::TEAM_CT, 4);

    const std::vector<unsigned char> snapshot_payload = openstrike::make_team_snapshot_payload(teams.make_snapshot());
    const std::optional<openstrike::TeamSnapshot> decoded_snapshot = openstrike::read_team_snapshot_payload(snapshot_payload);
    REQUIRE(decoded_snapshot.has_value());
    REQUIRE(decoded_snapshot->players.size() == 1);
    REQUIRE(decoded_snapshot->players[0].connection_id == 77);
    REQUIRE(decoded_snapshot->players[0].current_team == openstrike::TEAM_CT);
    REQUIRE(decoded_snapshot->teams[openstrike::TEAM_CT].score_total == 4);

    std::vector<unsigned char> malformed_members = snapshot_payload;
    REQUIRE(malformed_members.size() > 33);
    write_u32_le(malformed_members, 29, 0x0000FFFFU);
    REQUIRE(!openstrike::read_team_snapshot_payload(malformed_members).has_value());

    std::vector<unsigned char> malformed_player_team = snapshot_payload;
    REQUIRE(malformed_player_team.size() > 133);
    write_u32_le(malformed_player_team, 130, 99U);
    REQUIRE(!openstrike::read_team_snapshot_payload(malformed_player_team).has_value());

    std::vector<unsigned char> malformed_alive_spectator = snapshot_payload;
    REQUIRE(malformed_alive_spectator.size() > 142);
    write_u32_le(malformed_alive_spectator, 130, openstrike::TEAM_SPECTATOR);
    REQUIRE(!openstrike::read_team_snapshot_payload(malformed_alive_spectator).has_value());

    std::vector<unsigned char> malformed_failure = snapshot_payload;
    REQUIRE(malformed_failure.size() > 156);
    malformed_failure[156] = 99U;
    REQUIRE(!openstrike::read_team_snapshot_payload(malformed_failure).has_value());
}

void test_team_network_join_command_validates_and_replicates_snapshot()
{
    openstrike::RuntimeConfig config;
    config.content_root = std::filesystem::current_path();
    openstrike::EngineContext server_context;
    openstrike::configure_engine_context(server_context, config);
    REQUIRE(server_context.network.start_server(0));
    (void)server_context.network.drain_events();

    openstrike::NetworkClient client;
    REQUIRE(client.connect(openstrike::NetworkAddress::localhost(server_context.network.server().local_port()), 1));

    bool client_connected = false;
    for (int attempt = 0; attempt < 64 && !client_connected; ++attempt)
    {
        server_context.network.poll(static_cast<std::uint64_t>(2 + attempt));
        for (const openstrike::NetworkEvent& event : server_context.network.drain_events())
        {
            openstrike::handle_team_network_event(server_context, event);
        }
        client.poll(static_cast<std::uint64_t>(2 + attempt));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            client_connected = client_connected || event.type == openstrike::NetworkEventType::ConnectedToServer;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client_connected);

    const std::vector<unsigned char> join_game = openstrike::make_team_user_command_payload(openstrike::TeamUserCommand{
        .kind = openstrike::TeamUserCommandKind::JoinGame,
    });
    REQUIRE(client.send_user_command(join_game, 100));
    const std::vector<unsigned char> join_team = openstrike::make_team_user_command_payload(openstrike::TeamUserCommand{
        .kind = openstrike::TeamUserCommandKind::JoinTeam,
        .requested_team = openstrike::TEAM_TERRORIST,
        .force = true,
    });
    REQUIRE(client.send_user_command(join_team, 101));

    openstrike::TeamManager client_teams;
    bool client_applied_snapshot = false;
    for (int attempt = 0; attempt < 64 && !client_applied_snapshot; ++attempt)
    {
        server_context.network.poll(static_cast<std::uint64_t>(110 + attempt));
        for (const openstrike::NetworkEvent& event : server_context.network.drain_events())
        {
            openstrike::handle_team_network_event(server_context, event);
        }

        client.poll(static_cast<std::uint64_t>(110 + attempt));
        for (const openstrike::NetworkEvent& event : client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::SnapshotReceived)
            {
                const std::optional<openstrike::TeamSnapshot> snapshot = openstrike::read_team_snapshot_payload(event.payload);
                if (snapshot)
                {
                    client_teams.apply_snapshot(*snapshot, client.connection_id());
                    const openstrike::TeamPlayerState* player = client_teams.find_player(client.connection_id());
                    client_applied_snapshot = player != nullptr && player->current_team == openstrike::TEAM_TERRORIST;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(client_applied_snapshot);
    REQUIRE(server_context.teams.find_player(client.connection_id())->current_team == openstrike::TEAM_TERRORIST);

    client.disconnect(200);
    server_context.network.stop_server();
}

void test_connected_team_menu_targets_local_connection()
{
    openstrike::RuntimeConfig config;
    config.content_root = std::filesystem::current_path();
    openstrike::EngineContext server_context;
    openstrike::configure_engine_context(server_context, config);
    REQUIRE(server_context.network.start_server(0));
    (void)server_context.network.drain_events();

    openstrike::EngineContext client_context;
    openstrike::configure_engine_context(client_context, config);
    REQUIRE(client_context.network.connect_client(openstrike::NetworkAddress::localhost(server_context.network.server().local_port()), 1));

    bool client_connected = false;
    for (int attempt = 0; attempt < 64 && !client_connected; ++attempt)
    {
        server_context.network.poll(static_cast<std::uint64_t>(2 + attempt));
        for (const openstrike::NetworkEvent& event : server_context.network.drain_events())
        {
            openstrike::handle_team_network_event(server_context, event);
        }
        client_context.network.poll(static_cast<std::uint64_t>(2 + attempt));
        for (const openstrike::NetworkEvent& event : client_context.network.drain_events())
        {
            client_connected = client_connected || event.type == openstrike::NetworkEventType::ConnectedToServer;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client_connected);

    const std::uint32_t local_id = openstrike::local_team_connection_id(client_context.network);
    REQUIRE(local_id != openstrike::kLocalTeamPlayerId);
    client_context.teams.mark_join_game(local_id);
    openstrike::TeamJoinRules rules;
    REQUIRE(client_context.teams.try_join_team(local_id, openstrike::TEAM_CT, true, rules).accepted);
    REQUIRE(!client_context.teams.should_show_team_menu(local_id, true));

    client_context.command_buffer.add_text("teammenu");
    openstrike::ConsoleCommandContext console_context = client_context.console_context();
    client_context.command_buffer.execute(client_context.commands, console_context);

    const openstrike::TeamPlayerState* player = client_context.teams.find_player(local_id);
    REQUIRE(player != nullptr);
    REQUIRE(player->team_menu_requested);
    REQUIRE(client_context.teams.should_show_team_menu(local_id, true));
    const openstrike::TeamPlayerState* player_zero = client_context.teams.find_player(openstrike::kLocalTeamPlayerId);
    REQUIRE(player_zero == nullptr || !player_zero->team_menu_requested);

    client_context.network.disconnect_client(200);
    server_context.network.stop_server();
}

void test_listen_server_local_team_join_broadcasts_snapshot()
{
    openstrike::RuntimeConfig config;
    config.content_root = std::filesystem::current_path();
    openstrike::EngineContext server_context;
    openstrike::configure_engine_context(server_context, config);
    REQUIRE(server_context.network.start_server(0));
    (void)server_context.network.drain_events();

    openstrike::NetworkClient remote_client;
    REQUIRE(remote_client.connect(openstrike::NetworkAddress::localhost(server_context.network.server().local_port()), 1));

    bool client_connected = false;
    for (int attempt = 0; attempt < 64 && !client_connected; ++attempt)
    {
        server_context.network.poll(static_cast<std::uint64_t>(2 + attempt));
        for (const openstrike::NetworkEvent& event : server_context.network.drain_events())
        {
            openstrike::handle_team_network_event(server_context, event);
        }
        remote_client.poll(static_cast<std::uint64_t>(2 + attempt));
        for (const openstrike::NetworkEvent& event : remote_client.drain_events())
        {
            client_connected = client_connected || event.type == openstrike::NetworkEventType::ConnectedToServer;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(client_connected);

    server_context.command_buffer.add_text("joingame; jointeam 2 1");
    openstrike::ConsoleCommandContext console_context = server_context.console_context();
    server_context.command_buffer.execute(server_context.commands, console_context);

    bool received_local_team_snapshot = false;
    for (int attempt = 0; attempt < 64 && !received_local_team_snapshot; ++attempt)
    {
        remote_client.poll(static_cast<std::uint64_t>(110 + attempt));
        for (const openstrike::NetworkEvent& event : remote_client.drain_events())
        {
            if (event.type == openstrike::NetworkEventType::SnapshotReceived)
            {
                const std::optional<openstrike::TeamSnapshot> snapshot = openstrike::read_team_snapshot_payload(event.payload);
                if (snapshot)
                {
                    const auto it = std::find_if(snapshot->players.begin(), snapshot->players.end(), [](const openstrike::TeamPlayerState& player) {
                        return player.connection_id == openstrike::kLocalTeamPlayerId &&
                               player.current_team == openstrike::TEAM_TERRORIST;
                    });
                    received_local_team_snapshot = it != snapshot->players.end();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(received_local_team_snapshot);
    REQUIRE(server_context.teams.find_player(openstrike::kLocalTeamPlayerId)->current_team == openstrike::TEAM_TERRORIST);

    remote_client.disconnect(200);
    server_context.network.stop_server();
}

void test_failed_team_join_snapshot_preserves_current_team()
{
    openstrike::TeamManager teams;
    openstrike::TeamJoinRules rules;
    rules.require_join_game = false;
    rules.max_players = 2;
    rules.terrorist_spawn_count = 1;
    rules.limit_teams = 0;
    REQUIRE(teams.try_join_team(1, openstrike::TEAM_TERRORIST, true, rules).accepted);

    const openstrike::TeamJoinResult result = teams.try_join_team(2, openstrike::TEAM_TERRORIST, true, rules);
    REQUIRE(!result.accepted);
    REQUIRE(result.reason == openstrike::TeamJoinFailedReason::TerroristsFull);
    const openstrike::TeamPlayerState* player = teams.find_player(2);
    REQUIRE(player != nullptr);
    REQUIRE(player->current_team == openstrike::TEAM_UNASSIGNED);
    REQUIRE(player->last_join_failure == openstrike::TeamJoinFailedReason::TerroristsFull);
}

void test_team_specific_spawn_selection_from_bsp_entities()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_team_spawn_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n\"classname\" \"worldspawn\"\n}\n"
        "{\n\"classname\" \"info_player_terrorist\"\n\"origin\" \"10 20 30\"\n}\n"
        "{\n\"classname\" \"info_player_counterterrorist\"\n\"origin\" \"40 50 60\"\n}\n"
        "{\n\"classname\" \"info_player_teamspawn\"\n\"TeamNum\" \"3\"\n\"origin\" \"70 80 90\"\n}\n"
        "{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"1 2 3\"\n}\n";
    write_minimal_bsp(root / "game/maps/team_spawns.bsp", entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";
    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    REQUIRE(context.world.load_map("team_spawns", context.filesystem));
    const openstrike::LoadedWorld* world = context.world.current_world();
    REQUIRE(world != nullptr);
    REQUIRE(world->spawn_points.size() == 4);

    openstrike::TeamManager teams;
    const std::optional<openstrike::WorldSpawnPoint> t_spawn =
        teams.select_spawn_point(*world, openstrike::TEAM_TERRORIST, 0);
    const std::optional<openstrike::WorldSpawnPoint> ct_spawn =
        teams.select_spawn_point(*world, openstrike::TEAM_CT, 0);
    REQUIRE(t_spawn.has_value());
    REQUIRE(ct_spawn.has_value());
    REQUIRE(t_spawn->team_id == openstrike::TEAM_TERRORIST);
    REQUIRE(ct_spawn->team_id == openstrike::TEAM_CT);

    std::filesystem::remove_all(root);
}

void test_hud_state_reflects_authoritative_team()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_team_hud_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n\"classname\" \"worldspawn\"\n}\n"
        "{\n\"classname\" \"info_player_terrorist\"\n\"origin\" \"8 16 32\"\n}\n";
    write_minimal_bsp(root / "game/maps/team_hud.bsp", entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";
    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    REQUIRE(context.world.load_map("team_hud", context.filesystem));

    openstrike::GameSimulation simulation;
    simulation.on_start(config, context);
    simulation.on_fixed_update({}, context);
    context.teams.mark_join_game(openstrike::kLocalTeamPlayerId);
    openstrike::TeamJoinRules rules = openstrike::team_join_rules_from_context(context.variables, context.world.current_world());
    REQUIRE(context.teams.try_join_team(openstrike::kLocalTeamPlayerId, openstrike::TEAM_TERRORIST, true, rules).accepted);
    simulation.on_fixed_update({}, context);

    REQUIRE(context.hud.team_name == "Terrorists");
    REQUIRE(context.hud.alive);
    REQUIRE(context.hud.health == 100);

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

void test_loading_screen_state_matches_source_map_lifecycle()
{
    openstrike::LoadingScreenState state;
    state.open_for_map("maps/de_dust2.bsp", "Competitive", "Settings:\n- Friendly fire is OFF", "Hold angles with teammates.");

    const std::uint64_t open_revision = state.snapshot().revision;
    REQUIRE(state.visible());
    REQUIRE(state.snapshot().map_name == "de_dust2");
    REQUIRE(state.snapshot().game_mode == "Competitive");
    REQUIRE(state.snapshot().progress == 0.0F);
    REQUIRE(!state.snapshot().auto_close);

    state.set_progress(0.55F, "Building game world...");
    REQUIRE(state.snapshot().revision > open_revision);
    REQUIRE(std::fabs(state.snapshot().progress - 0.55F) < 0.001F);
    REQUIRE(state.snapshot().status == "Building game world...");

    state.complete("Sending client info...");
    REQUIRE(state.snapshot().progress == 1.0F);
    REQUIRE(state.snapshot().auto_close);
    REQUIRE(state.visible());

    state.close();
    REQUIRE(!state.visible());
    REQUIRE(!state.snapshot().auto_close);
    REQUIRE(openstrike::loading_screen_map_name("assets/levels/workshop_test.level.json") == "workshop_test");
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
        "}\n"
        "{\n"
        "\"classname\" \"light\"\n"
        "\"origin\" \"128 64 32\"\n"
        "\"_light\" \"255 128 64 400\"\n"
        "\"_distance\" \"768\"\n"
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
    REQUIRE(loaded->entity_count == 3);
    REQUIRE(loaded->entities.size() == 3);
    REQUIRE(loaded->entities[0].class_name == "worldspawn");
    REQUIRE(loaded->entities[1].properties.at("classname") == "info_player_terrorist");
    REQUIRE(loaded->worldspawn.at("mapversion") == "7");
    REQUIRE(loaded->worldspawn.at("skyname") == "sky_openstrike_test");
    REQUIRE(loaded->spawn_points.size() == 1);
    REQUIRE(loaded->spawn_points[0].origin.x == 10.0F);
    REQUIRE(loaded->spawn_points[0].origin.y == 20.0F);
    REQUIRE(loaded->spawn_points[0].origin.z == 30.0F);
    REQUIRE(loaded->lights.size() == 1);
    REQUIRE(loaded->lights[0].position.x == 128.0F);
    REQUIRE(loaded->lights[0].position.y == 64.0F);
    REQUIRE(loaded->lights[0].position.z == 32.0F);
    REQUIRE(std::abs(loaded->lights[0].color[1] - (128.0F / 255.0F)) < 0.001F);
    REQUIRE(std::abs(loaded->lights[0].color[2] - (64.0F / 255.0F)) < 0.001F);
    REQUIRE(std::abs(loaded->lights[0].intensity - (400.0F / 255.0F)) < 0.001F);
    REQUIRE(loaded->lights[0].radius == 768.0F);

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
    REQUIRE(!loaded->mesh.lightmap_atlas.has_baked_samples);

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
        REQUIRE(vertex.lightmap_weight == 0.0F);
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

void test_world_manager_builds_source_displacement_mesh()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_source_displacement_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/displacement_world.bsp", entities, 0, TestLightmapMode::None, true);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::WorldManager world;
    REQUIRE(world.load_map("displacement_world", filesystem));

    const openstrike::LoadedWorld* loaded = world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->mesh.vertices.size() == 25);
    REQUIRE(loaded->mesh.indices.size() == 96);
    REQUIRE(loaded->mesh.collision_triangles.size() == 32);
    REQUIRE(loaded->mesh.materials.size() == 1);
    REQUIRE(loaded->mesh.batches.size() == 1);
    REQUIRE(loaded->mesh.batches[0].index_count == 96);
    REQUIRE(loaded->mesh.has_bounds);
    REQUIRE(std::abs(loaded->mesh.bounds_min.z - 32.0F) < 0.001F);
    REQUIRE(std::abs(loaded->mesh.bounds_max.z - 48.0F) < 0.001F);

    REQUIRE(loaded->mesh.indices[0] == 0);
    REQUIRE(loaded->mesh.indices[1] == 5);
    REQUIRE(loaded->mesh.indices[2] == 6);
    REQUIRE(loaded->mesh.indices[3] == 0);
    REQUIRE(loaded->mesh.indices[4] == 6);
    REQUIRE(loaded->mesh.indices[5] == 1);

    const std::optional<float> displaced_floor_z = openstrike::find_floor_z(*loaded, {0.0F, 0.0F, 96.0F}, 128.0F);
    REQUIRE(displaced_floor_z.has_value());
    REQUIRE(std::abs(*displaced_floor_z - 48.0F) < 0.001F);

    std::filesystem::remove_all(root);
}

void test_world_manager_builds_bsp_static_lightmap()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_world_lightmap_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/lightmap_world.bsp", entities, 0, TestLightmapMode::Ldr);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::WorldManager world;
    REQUIRE(world.load_map("lightmap_world", filesystem));

    const openstrike::LoadedWorld* loaded = world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->mesh.lightmap_atlas.has_baked_samples);
    REQUIRE(loaded->mesh.lightmap_atlas.width >= 4);
    REQUIRE(loaded->mesh.lightmap_atlas.height >= 4);
    REQUIRE(loaded->mesh.lightmap_atlas.rgba.size() == static_cast<std::size_t>(loaded->mesh.lightmap_atlas.width) *
                                                          loaded->mesh.lightmap_atlas.height * 4U);
    REQUIRE(atlas_contains_color(loaded->mesh.lightmap_atlas, 64.0F / 255.0F, 128.0F / 255.0F, 192.0F / 255.0F));

    bool found_lightmapped_vertex = false;
    for (const openstrike::WorldMeshVertex& vertex : loaded->mesh.vertices)
    {
        if (vertex.lightmap_weight > 0.999F)
        {
            found_lightmapped_vertex = true;
            REQUIRE(vertex.lightmap_texcoord.x > 0.0F);
            REQUIRE(vertex.lightmap_texcoord.x < 1.0F);
            REQUIRE(vertex.lightmap_texcoord.y > 0.0F);
            REQUIRE(vertex.lightmap_texcoord.y < 1.0F);
        }
    }
    REQUIRE(found_lightmapped_vertex);

    std::filesystem::remove_all(root);
}

void test_world_manager_prefers_hdr_bsp_lightmap()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_world_hdr_lightmap_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/hdr_lightmap_world.bsp", entities, 0, TestLightmapMode::Hdr);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::WorldManager world;
    REQUIRE(world.load_map("hdr_lightmap_world", filesystem));

    const openstrike::LoadedWorld* loaded = world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->mesh.lightmap_atlas.has_baked_samples);
    REQUIRE(atlas_contains_color(loaded->mesh.lightmap_atlas, 200.0F / 255.0F, 150.0F / 255.0F, 100.0F / 255.0F));
    REQUIRE(!atlas_contains_color(loaded->mesh.lightmap_atlas, 64.0F / 255.0F, 128.0F / 255.0F, 192.0F / 255.0F));

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

void test_world_manager_uses_source_brush_contents_for_collision()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_world_brush_contents_collision";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    const std::vector<TestBrushBox> boxes{
        {{-16.0F, -16.0F, 0.0F}, {16.0F, 16.0F, 32.0F}, kTestContentsSolid},
        {{48.0F, -16.0F, 0.0F}, {80.0F, 16.0F, 32.0F}, kTestContentsWindow},
        {{112.0F, -16.0F, 0.0F}, {144.0F, 16.0F, 32.0F}, kTestContentsGrate},
        {{176.0F, -16.0F, 0.0F}, {208.0F, 16.0F, 32.0F}, kTestContentsPlayerClip},
        {{240.0F, -16.0F, 0.0F}, {272.0F, 16.0F, 32.0F}, kTestContentsMonsterClip},
        {{304.0F, -16.0F, 0.0F}, {336.0F, 16.0F, 32.0F}, kTestContentsOpaque},
        {{368.0F, -16.0F, 0.0F}, {400.0F, 16.0F, 32.0F}, kTestContentsBlockLos},
        {{432.0F, -16.0F, 0.0F}, {464.0F, 16.0F, 32.0F}, kTestContentsWater},
    };
    write_brush_collision_bsp(root / "game/maps/brush_contents.bsp", entities, boxes);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");

    openstrike::WorldManager world;
    REQUIRE(world.load_map("brush_contents", filesystem));

    const openstrike::LoadedWorld* loaded = world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->mesh.collision_triangles.size() == 60);
    for (const openstrike::WorldTriangle& triangle : loaded->mesh.collision_triangles)
    {
        REQUIRE((triangle.contents & kTestSourceWorldCollisionMask) != 0);
        REQUIRE((triangle.contents & (kTestContentsOpaque | kTestContentsBlockLos | kTestContentsWater)) == 0);
    }

    const std::optional<float> solid_top = openstrike::find_floor_z(*loaded, {0.0F, 0.0F, 64.0F}, 96.0F);
    REQUIRE(solid_top.has_value());
    REQUIRE(std::abs(*solid_top - 32.0F) < 0.001F);

    const std::optional<float> opaque_top = openstrike::find_floor_z(*loaded, {320.0F, 0.0F, 64.0F}, 96.0F);
    REQUIRE(!opaque_top.has_value());

    const std::optional<float> monster_clip_top = openstrike::find_floor_z(*loaded, {256.0F, 0.0F, 64.0F}, 96.0F);
    REQUIRE(!monster_clip_top.has_value());

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
    REQUIRE(loaded->props[0].first_leaf == 0);
    REQUIRE(loaded->props[0].leaf_count == 1);
    REQUIRE(loaded->props[0].flags_ex == 4);
    REQUIRE(std::abs(loaded->props[0].fade_min_dist - 128.0F) < 0.001F);
    REQUIRE(std::abs(loaded->props[0].fade_max_dist - 1024.0F) < 0.001F);
    REQUIRE(std::abs(loaded->props[0].forced_fade_scale - 1.25F) < 0.001F);
    REQUIRE(loaded->props[0].min_cpu_level == 1);
    REQUIRE(loaded->props[0].max_cpu_level == 3);
    REQUIRE(loaded->props[0].min_gpu_level == 2);
    REQUIRE(loaded->props[0].max_gpu_level == 4);
    REQUIRE(loaded->props[0].disable_x360);
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
    REQUIRE(loaded->mesh.collision_triangles.size() == 1);
    REQUIRE(loaded->mesh.batches.size() == 1);
    REQUIRE(loaded->mesh.has_bounds);
    REQUIRE(loaded->mesh.bounds_max.z == 80.0F);

    std::filesystem::remove_all(root);
}

void test_world_manager_loads_large_static_prop_studio_mesh()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_large_static_prop_studio_mesh";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_static_prop_bsp(root / "game/maps/large_static_prop.bsp", entities);
    write_minimal_source_model(root / "game/models/test/crate.mdl", {-4.0F, -8.0F, 0.0F}, {4.0F, 8.0F, 16.0F}, 4098);

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");
    openstrike::WorldManager manager;
    REQUIRE(manager.load_map("large_static_prop", filesystem));

    const openstrike::LoadedWorld* loaded = manager.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->props.size() == 1);
    REQUIRE(loaded->props[0].model_mesh_loaded);
    REQUIRE(loaded->mesh.vertices.size() == 4098);
    REQUIRE(loaded->mesh.indices.size() == 4098);
    REQUIRE(loaded->mesh.collision_triangles.size() == 1366);
    REQUIRE(loaded->mesh.batches.size() == 1);
    REQUIRE(loaded->mesh.batches[0].index_count == 4098);

    std::filesystem::remove_all(root);
}

void test_world_manager_loads_bbox_static_prop_collision()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_static_prop_bbox_collision";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_static_prop_bsp(root / "game/maps/static_prop_bbox.bsp", entities, 2);
    write_minimal_source_model(root / "game/models/test/crate.mdl", {-4.0F, -8.0F, 0.0F}, {4.0F, 8.0F, 16.0F});

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");
    openstrike::WorldManager manager;
    REQUIRE(manager.load_map("static_prop_bbox", filesystem));

    const openstrike::LoadedWorld* loaded = manager.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->props.size() == 1);
    REQUIRE(loaded->props[0].solid == 2);
    REQUIRE(loaded->mesh.collision_triangles.size() == 12);

    const std::optional<float> prop_top = openstrike::find_floor_z(*loaded, {100.0F, 200.0F, 96.0F}, 64.0F);
    REQUIRE(prop_top.has_value());
    REQUIRE(std::abs(*prop_top - 80.0F) < 0.001F);

    openstrike::PhysicsWorld physics;
    REQUIRE(physics.load_static_world(*loaded));
    REQUIRE(physics.has_static_world());

    std::filesystem::remove_all(root);
}

void test_source_studio_parses_animation_metadata()
{
    const std::vector<unsigned char> bytes = make_minimal_animated_source_model();
    const std::optional<openstrike::StudioModel> model = openstrike::parse_source_studio_model(bytes, "models/test/player.mdl");
    REQUIRE(model.has_value());
    REQUIRE(model->name == "animated_test");
    REQUIRE(model->bones.size() == 2);
    REQUIRE(model->bones[0].name == "root");
    REQUIRE(model->bones[1].parent == 0);
    REQUIRE(model->bone_controllers.size() == 1);
    REQUIRE(model->hitbox_sets.size() == 1);
    REQUIRE(model->hitbox_sets[0].hitboxes.size() == 1);
    REQUIRE(model->animations.size() == 1);
    REQUIRE(model->animations[0].clip.tracks.size() == 1);
    REQUIRE(model->animations[0].clip.tracks[0].bone == 1);
    REQUIRE(model->animations[0].clip.tracks[0].position_values[0].size() == 4);
    REQUIRE(std::abs(model->animations[0].clip.tracks[0].position_values[0][2] - 20.0F) < 0.001F);
    REQUIRE(model->sequences.size() == 9);
    REQUIRE(openstrike::lookup_sequence(*model, "run") == 1);
    REQUIRE(openstrike::lookup_sequence(*model, "ACT_CSGO_FIRE_PRIMARY") == 6);
    REQUIRE(model->sequences[0].events.size() == 1);
    REQUIRE(model->sequences[0].events[0].event == 5004);
    REQUIRE(model->sequences[0].events[0].options == "footstep");
    REQUIRE(model->sequences[0].events[0].name == "AE_CL_PLAYSOUND");
    REQUIRE(openstrike::lookup_pose_parameter(*model, "move_yaw") == 0);
    REQUIRE(model->attachments.size() == 1);
    REQUIRE(model->attachments[0].name == "eyes");
    REQUIRE(model->surface_prop == "flesh");
    REQUIRE(model->key_values == "animated 1");
}

void test_source_animation_state_advances_cycles_events_and_pose()
{
    const std::vector<unsigned char> bytes = make_minimal_animated_source_model();
    const std::optional<openstrike::StudioModel> model = openstrike::parse_source_studio_model(bytes);
    REQUIRE(model.has_value());

    openstrike::AnimationPlaybackState playback;
    openstrike::reset_animation_state(playback, *model, 0);
    REQUIRE(playback.sequence == 0);
    REQUIRE(playback.sequence_loops);
    REQUIRE(std::abs(openstrike::sequence_duration(*model, 0) - 0.75F) < 0.001F);
    openstrike::advance_animation_state(playback, *model, 0.2F);
    REQUIRE(playback.cycle > 0.25F);
    REQUIRE(!playback.fired_events.empty());
    REQUIRE(playback.fired_events[0].event == 5004);

    playback.cycle = 0.5F;
    const openstrike::StudioPose pose = openstrike::evaluate_studio_pose(*model, playback);
    REQUIRE(pose.positions.size() == 2);
    REQUIRE(std::abs(pose.positions[1].x - 15.0F) < 0.001F);
    REQUIRE(std::abs(pose.positions[1].z - 10.0F) < 0.001F);
    REQUIRE(std::abs(pose.bone_to_model[1].origin.x - 15.0F) < 0.001F);

    openstrike::set_sequence(playback, *model, 6);
    REQUIRE(!playback.sequence_loops);
    openstrike::advance_animation_state(playback, *model, 2.0F);
    REQUIRE(playback.sequence_finished);
    REQUIRE(std::abs(playback.cycle - 1.0F) < 0.001F);
}

void test_csgo_player_anim_state_sets_layers_and_pose_parameters()
{
    const std::vector<unsigned char> bytes = make_minimal_animated_source_model();
    const std::optional<openstrike::StudioModel> model = openstrike::parse_source_studio_model(bytes);
    REQUIRE(model.has_value());

    openstrike::AnimationPlaybackState playback;
    openstrike::reset_animation_state(playback, *model, 0);
    openstrike::CsgoPlayerAnimState anim_state;
    openstrike::reset_csgo_player_anim_state(anim_state);

    openstrike::CsgoAnimInput input;
    input.delta_seconds = 1.0F / 64.0F;
    input.frame_index = 1;
    input.velocity = {240.0F, 40.0F, 0.0F};
    input.eye_yaw = 90.0F;
    input.eye_pitch = -12.0F;
    input.lower_body_yaw_target = 85.0F;
    input.duck_amount = 0.5F;
    input.firing = true;

    openstrike::update_csgo_player_anim_state(anim_state, playback, *model, input);
    REQUIRE(playback.sequence == 0);
    REQUIRE(playback.playback_rate == 0.0F);
    REQUIRE(playback.overlays.size() >= static_cast<std::size_t>(openstrike::CsgoAnimLayer::Count));
    REQUIRE(playback.overlays[static_cast<std::size_t>(openstrike::CsgoAnimLayer::MovementMove)].active);
    REQUIRE(playback.overlays[static_cast<std::size_t>(openstrike::CsgoAnimLayer::WeaponAction)].active);
    REQUIRE(playback.overlays[static_cast<std::size_t>(openstrike::CsgoAnimLayer::AliveLoop)].active);
    REQUIRE(playback.pose_parameters.size() == model->pose_parameters.size());
    REQUIRE(playback.pose_parameters[static_cast<std::size_t>(openstrike::lookup_pose_parameter(*model, "move_yaw"))] >= 0.0F);
    REQUIRE(playback.pose_parameters[static_cast<std::size_t>(openstrike::lookup_pose_parameter(*model, "move_yaw"))] <= 1.0F);
    REQUIRE(anim_state.velocity_length_xy > 200.0F);
    REQUIRE(anim_state.anim_duck_amount > 0.0F);
    REQUIRE(std::isfinite(anim_state.foot_yaw));
}

void test_animation_scene_loads_and_advances_source_model()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_animation_scene_content";
    std::filesystem::remove_all(root);
    write_minimal_animated_source_model(root / "game/models/test/player.mdl");

    openstrike::ContentFileSystem filesystem;
    filesystem.add_search_path(root / "game", "GAME");
    openstrike::SourceAssetStore assets(filesystem);

    openstrike::AnimationScene scene;
    openstrike::AnimationEntity& entity = scene.upsert_entity(17, "models/test/player", assets);
    REQUIRE(entity.entity_id == 17);
    REQUIRE(entity.model_loaded);
    REQUIRE(scene.find_model("models/test/player.mdl") != nullptr);
    scene.advance(assets, 0.2F);
    const openstrike::AnimationEntity* advanced = scene.find_entity(17);
    REQUIRE(advanced != nullptr);
    REQUIRE(!advanced->playback.fired_events.empty());
    REQUIRE(advanced->pose.positions.size() == 2);
    REQUIRE(advanced->pose.positions[1].x > 0.0F);

    std::filesystem::remove_all(root);
}

void test_game_simulation_builds_animation_scene_for_dynamic_props()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_dynamic_prop_animation_scene";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n"
        "{\n"
        "\"classname\" \"prop_dynamic\"\n"
        "\"model\" \"models/test/player.mdl\"\n"
        "\"origin\" \"32 64 16\"\n"
        "\"angles\" \"0 90 0\"\n"
        "\"skin\" \"2\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/anim_props.bsp", entities);
    write_minimal_animated_source_model(root / "game/models/test/player.mdl");

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";
    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    REQUIRE(context.world.load_map("anim_props", context.filesystem));

    openstrike::GameSimulation simulation;
    simulation.on_start(config, context);
    simulation.on_fixed_update({.tick_index = 0, .delta_seconds = config.tick_interval_seconds()}, context);

    const openstrike::AnimationEntity* entity = context.animation.find_entity(1);
    REQUIRE(entity != nullptr);
    REQUIRE(entity->model_loaded);
    REQUIRE(entity->origin.x == 32.0F);
    REQUIRE(entity->angles.y == 90.0F);
    REQUIRE(entity->skin == 2);
    REQUIRE(entity->pose.positions.size() == 2);
    REQUIRE(entity->playback.cycle > 0.0F);

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
    REQUIRE(context.loading_screen.snapshot().visible);
    REQUIRE(context.loading_screen.snapshot().auto_close);
    REQUIRE(context.loading_screen.snapshot().map_name == "command_world");
    REQUIRE(context.loading_screen.snapshot().progress == 1.0F);
    REQUIRE(context.loading_screen.snapshot().status == "Sending client info...");

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

void test_game_mode_cvars_aliases_cfg_and_loading()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_game_mode_cvars";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");
    std::filesystem::create_directories(root / "game/cfg");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "\"mapversion\" \"1\"\n"
        "}\n";
    write_minimal_bsp(root / "game/maps/de_dust2.bsp", entities);
    {
        std::ofstream(root / "game/cfg/gamemode_competitive.cfg") << "set mp_roundtime 1.92; set mp_friendlyfire 1";
        std::ofstream(root / "game/cfg/gamemode_competitive_short.cfg") << "set mp_maxrounds 12";
        std::ofstream(root / "game/cfg/gamemode_competitive_server.cfg") << "set mp_freezetime 7";
    }

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    REQUIRE(context.variables.get_string("game_type") == "0");
    REQUIRE(context.variables.get_string("game_mode") == "1");

    context.variables.set("sv_skirmish_id", "12");
    context.command_buffer.add_text("game_alias wingman");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);
    REQUIRE(context.variables.get_string("game_type") == "0");
    REQUIRE(context.variables.get_string("game_mode") == "2");
    REQUIRE(context.variables.get_string("sv_skirmish_id") == "12");

    context.command_buffer.add_text("set sv_skirmish_id 0; set sv_game_mode_flags 32; set mp_roundtime 9; map de_dust2 comp");
    context.command_buffer.execute(context.commands, console_context);

    const openstrike::LoadedWorld* loaded = context.world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->name == "de_dust2");
    REQUIRE(context.variables.get_string("game_type") == "0");
    REQUIRE(context.variables.get_string("game_mode") == "1");
    REQUIRE(context.variables.get_string("sv_game_mode_flags") == "32");
    REQUIRE(context.variables.get_string("maxplayers") == "10");
    REQUIRE(context.variables.get_string("mp_roundtime") == "1.92");
    REQUIRE(context.variables.get_string("mp_friendlyfire") == "1");
    REQUIRE(context.variables.get_string("mp_maxrounds") == "12");
    REQUIRE(context.variables.get_string("mp_freezetime") == "7");
    REQUIRE(context.loading_screen.snapshot().game_mode == "Short Competitive");

    std::filesystem::remove_all(root);
}

void test_game_mode_auto_alias_reads_map_defaults()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_game_mode_auto_alias";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");
    std::filesystem::create_directories(root / "game/cfg");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_minimal_bsp(root / "game/maps/auto_mode.bsp", entities);
    {
        std::ofstream(root / "game/maps/auto_mode.kv") << "\"auto_mode\" { \"default_game_type\" \"1\" \"default_game_mode\" \"0\" }";
        std::ofstream(root / "game/cfg/gamemode_armsrace.cfg") << "set bot_quota 5";
    }

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);
    context.command_buffer.add_text("map auto_mode auto");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);

    const openstrike::LoadedWorld* loaded = context.world.current_world();
    REQUIRE(loaded != nullptr);
    REQUIRE(context.variables.get_string("game_type") == "1");
    REQUIRE(context.variables.get_string("game_mode") == "0");
    REQUIRE(context.variables.get_string("maxplayers") == "10");
    REQUIRE(context.variables.get_string("bot_quota") == "5");
    REQUIRE(context.loading_screen.snapshot().game_mode == "Arms Race");

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
    REQUIRE(!context.hud.alive);
    REQUIRE(context.hud.health == 0);
    REQUIRE(context.hud.round_phase == "CHOOSE TEAM");
    REQUIRE(!context.camera.active);

    const std::uint32_t local_id = openstrike::local_team_connection_id(context.network);
    context.teams.mark_join_game(local_id);
    const openstrike::TeamJoinResult join_result = context.teams.try_join_team(
        local_id,
        openstrike::TEAM_CT,
        true,
        openstrike::team_join_rules_from_context(context.variables, context.world.current_world()));
    REQUIRE(join_result.accepted);
    simulation.on_fixed_update(openstrike::SimulationStep{0, 1.0 / 64.0}, context);

    REQUIRE(context.hud.visible);
    REQUIRE(context.hud.alive);
    REQUIRE(context.hud.health == 100);
    REQUIRE(context.hud.max_health == 100);
    REQUIRE(context.hud.money == 800);
    REQUIRE(context.hud.ammo_in_clip == 12);
    REQUIRE(context.hud.reserve_ammo == 24);
    REQUIRE(context.hud.round_phase == "hud_world");
    REQUIRE(context.hud.team_name == "Counter-Terrorists");
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

void test_application_runs_null_renderer_smoke()
{
    openstrike::RuntimeConfig config;
    config.mode = openstrike::AppMode::DedicatedServer;
    config.renderer_backend = openstrike::RendererBackend::Null;
    config.content_root = std::filesystem::current_path();
    config.network_port = 0;
    config.startup_commands.push_back("quit");

    openstrike::Application application(std::move(config));
    REQUIRE(application.run() == 0);
}

void test_openstrike_application_definition_filters_default_modules()
{
    const openstrike::ApplicationDefinition definition = openstrike::make_openstrike_application_definition();
    REQUIRE(definition.name == "OpenStrike");

    const auto module_names_for_mode = [&](openstrike::AppMode mode) {
        std::vector<std::string> names;
        for (const openstrike::ApplicationModuleRegistration& registration : definition.modules)
        {
            if (!openstrike::application_module_runs_in_mode(registration.modes, mode))
            {
                continue;
            }

            std::unique_ptr<openstrike::EngineModule> module = registration.create();
            REQUIRE(module != nullptr);
            names.emplace_back(module->name());
        }
        return names;
    };

    const std::vector<std::string> client_modules{"game", "audio", "client"};
    const std::vector<std::string> server_modules{"game", "server"};
    const std::vector<std::string> editor_modules{"game", "audio", "client", "editor"};
    REQUIRE(module_names_for_mode(openstrike::AppMode::Client) == client_modules);
    REQUIRE(module_names_for_mode(openstrike::AppMode::DedicatedServer) == server_modules);
    REQUIRE(module_names_for_mode(openstrike::AppMode::Editor) == editor_modules);
}

void test_application_accepts_custom_game_module_stack()
{
    struct ModuleCounts
    {
        int starts = 0;
        int frames = 0;
        int stops = 0;
        std::string application_name;
    };

    class CountingModule final : public openstrike::EngineModule
    {
    public:
        explicit CountingModule(std::shared_ptr<ModuleCounts> counts)
            : counts_(std::move(counts))
        {
        }

        const char* name() const override
        {
            return "counting";
        }

        void on_start(const openstrike::RuntimeConfig& config, openstrike::EngineContext&) override
        {
            ++counts_->starts;
            counts_->application_name = config.application_name;
        }

        void on_frame(const openstrike::FrameContext&, openstrike::EngineContext&) override
        {
            ++counts_->frames;
        }

        void on_stop(openstrike::EngineContext&) override
        {
            ++counts_->stops;
        }

    private:
        std::shared_ptr<ModuleCounts> counts_;
    };

    auto counts = std::make_shared<ModuleCounts>();
    openstrike::ApplicationDefinition definition;
    definition.name = "ExampleGame";
    definition.modules.push_back(openstrike::ApplicationModuleRegistration{
        .modes = openstrike::ApplicationModuleMode::Client,
        .create = [counts]() -> std::unique_ptr<openstrike::EngineModule> { return std::make_unique<CountingModule>(counts); },
    });

    openstrike::RuntimeConfig config;
    config.mode = openstrike::AppMode::Client;
    config.renderer_backend = openstrike::RendererBackend::Null;
    config.content_root = std::filesystem::current_path();
    config.max_frames = 1;
    config.deterministic_frames = true;

    openstrike::Application application(config, definition);
    REQUIRE(application.config().application_name == "ExampleGame");
    REQUIRE(application.run() == 0);
    REQUIRE(counts->starts == 1);
    REQUIRE(counts->frames == 1);
    REQUIRE(counts->stops == 1);
    REQUIRE(counts->application_name == "ExampleGame");

    counts->starts = 0;
    counts->frames = 0;
    counts->stops = 0;
    counts->application_name.clear();
    config.mode = openstrike::AppMode::DedicatedServer;

    openstrike::Application dedicated_application(config, std::move(definition));
    REQUIRE(dedicated_application.run() == 0);
    REQUIRE(counts->starts == 0);
    REQUIRE(counts->frames == 0);
    REQUIRE(counts->stops == 0);
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

void add_collision_triangle(
    openstrike::LoadedWorld& world,
    openstrike::Vec3 a,
    openstrike::Vec3 b,
    openstrike::Vec3 c,
    openstrike::Vec3 normal,
    std::uint32_t contents = 0)
{
    openstrike::WorldTriangle triangle;
    triangle.points[0] = a;
    triangle.points[1] = b;
    triangle.points[2] = c;
    triangle.normal = normal;
    triangle.contents = contents;
    world.mesh.collision_triangles.push_back(triangle);
}

void test_nav_area_geometry_matches_source_semantics()
{
    openstrike::NavArea area;
    area.id = 1;
    area.build({0.0F, 0.0F, 10.0F}, {100.0F, 0.0F, 20.0F}, {100.0F, 100.0F, 40.0F}, {0.0F, 100.0F, 30.0F});

    REQUIRE(std::abs(area.get_z({50.0F, 50.0F, 0.0F}) - 25.0F) < 0.001F);
    REQUIRE(area.contains({50.0F, 50.0F, 80.0F}));
    REQUIRE(!area.contains({50.0F, 50.0F, 100.0F}));
    REQUIRE(area.closest_point({120.0F, 50.0F, 0.0F}).x == 100.0F);
    REQUIRE(area.compute_direction({125.0F, 50.0F, 0.0F}) == openstrike::NavDirection::East);

    openstrike::NavArea east;
    east.id = 2;
    east.build({100.0F, 20.0F, 40.0F}, {180.0F, 20.0F, 40.0F}, {180.0F, 80.0F, 40.0F}, {100.0F, 80.0F, 40.0F});

    openstrike::Vec3 portal;
    float half_width = 0.0F;
    REQUIRE(area.compute_portal(east, openstrike::NavDirection::East, portal, half_width));
    REQUIRE(portal.x == 100.0F);
    REQUIRE(portal.y == 50.0F);
    REQUIRE(std::abs(portal.z - 30.0F) < 0.001F);
    REQUIRE(std::abs(half_width - 30.0F) < 0.001F);

    area.connect_to(east.id, openstrike::NavDirection::East);
    REQUIRE(area.is_connected(east.id, openstrike::NavDirection::East));
    area.disconnect(east.id);
    REQUIRE(!area.is_connected(east.id));
}

void test_nav_mesh_pathing_uses_source_costs_blocking_and_ladders()
{
    openstrike::NavMesh mesh;
    openstrike::NavArea* area = mesh.create_area();
    const std::uint32_t a_id = area->id;
    area->build({0.0F, 0.0F, 0.0F}, {64.0F, 0.0F, 0.0F}, {64.0F, 64.0F, 0.0F}, {0.0F, 64.0F, 0.0F});
    area = mesh.create_area();
    const std::uint32_t b_id = area->id;
    area->build({64.0F, 0.0F, 0.0F}, {128.0F, 0.0F, 0.0F}, {128.0F, 64.0F, 0.0F}, {64.0F, 64.0F, 0.0F});
    area->set_attributes(openstrike::NavMeshCrouch);
    area = mesh.create_area();
    const std::uint32_t c_id = area->id;
    area->build({128.0F, 0.0F, 0.0F}, {192.0F, 0.0F, 0.0F}, {192.0F, 64.0F, 0.0F}, {128.0F, 64.0F, 0.0F});

    mesh.area_by_id(a_id)->connect_to(b_id, openstrike::NavDirection::East);
    mesh.area_by_id(b_id)->connect_to(c_id, openstrike::NavDirection::East);

    openstrike::NavPath path = mesh.build_path({{16.0F, 32.0F, 0.0F}, {176.0F, 32.0F, 0.0F}});
    REQUIRE(path.reached_goal);
    REQUIRE(path.segments.size() >= 4);
    REQUIRE(path.length() > 150.0F);
    REQUIRE(mesh.travel_distance({16.0F, 32.0F, 0.0F}, {176.0F, 32.0F, 0.0F}) > 150.0F);

    mesh.area_by_id(b_id)->mark_blocked(-1, true);
    openstrike::NavPath blocked_path = mesh.build_path({{16.0F, 32.0F, 0.0F}, {176.0F, 32.0F, 0.0F}});
    REQUIRE(!blocked_path.reached_goal);

    openstrike::NavMesh ladder_mesh;
    openstrike::NavArea* bottom = ladder_mesh.create_area();
    const std::uint32_t bottom_id = bottom->id;
    bottom->build({0.0F, 0.0F, 0.0F}, {64.0F, 0.0F, 0.0F}, {64.0F, 64.0F, 0.0F}, {0.0F, 64.0F, 0.0F});
    openstrike::NavArea* top = ladder_mesh.create_area();
    const std::uint32_t top_id = top->id;
    top->build({0.0F, 0.0F, 128.0F}, {64.0F, 0.0F, 128.0F}, {64.0F, 64.0F, 128.0F}, {0.0F, 64.0F, 128.0F});

    openstrike::NavLadder* ladder = ladder_mesh.create_ladder();
    const std::uint32_t ladder_id = ladder->id;
    ladder->width = 16.0F;
    ladder->bottom = {32.0F, 32.0F, 0.0F};
    ladder->top = {32.0F, 32.0F, 128.0F};
    ladder->length = 128.0F;
    ladder->set_direction(openstrike::NavDirection::North);
    ladder->bottom_area_id = bottom_id;
    ladder->top_forward_area_id = top_id;
    ladder_mesh.area_by_id(bottom_id)->add_ladder(ladder_id, true);
    ladder_mesh.area_by_id(top_id)->add_ladder(ladder_id, false);

    openstrike::NavPath ladder_path = ladder_mesh.build_path({{32.0F, 32.0F, 0.0F}, {32.0F, 32.0F, 128.0F}});
    REQUIRE(ladder_path.reached_goal);
    REQUIRE(std::any_of(ladder_path.segments.begin(), ladder_path.segments.end(), [](const openstrike::NavPathSegment& segment) {
        return segment.how == openstrike::NavTraverseType::GoLadderUp;
    }));
}

void test_nav_mesh_round_trips_source_v16_cs_subversion()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_nav_roundtrip";
    std::filesystem::remove_all(root);

    openstrike::NavMesh mesh;
    openstrike::NavArea* area = mesh.create_area();
    const std::uint32_t a_id = area->id;
    area->build({0.0F, 0.0F, 0.0F}, {64.0F, 0.0F, 0.0F}, {64.0F, 64.0F, 0.0F}, {0.0F, 64.0F, 0.0F});
    area->hiding_spots.push_back({7,
        {16.0F, 16.0F, 0.0F},
        static_cast<std::uint8_t>(openstrike::NavHidingSpot::InCover | openstrike::NavHidingSpot::GoodSniperSpot),
        true});

    area = mesh.create_area();
    const std::uint32_t b_id = area->id;
    area->build({64.0F, 0.0F, 0.0F}, {128.0F, 0.0F, 0.0F}, {128.0F, 64.0F, 0.0F}, {64.0F, 64.0F, 0.0F});

    mesh.area_by_id(a_id)->connect_to(b_id, openstrike::NavDirection::East);
    mesh.area_by_id(a_id)->approach_areas.push_back({a_id, 0, openstrike::NavTraverseType::Count, b_id, openstrike::NavTraverseType::GoEast});

    openstrike::NavLadder* ladder = mesh.create_ladder();
    const std::uint32_t ladder_id = ladder->id;
    ladder->width = 16.0F;
    ladder->bottom = {32.0F, 32.0F, 0.0F};
    ladder->top = {96.0F, 32.0F, 96.0F};
    ladder->length = 96.0F;
    ladder->set_direction(openstrike::NavDirection::East);
    ladder->bottom_area_id = a_id;
    ladder->top_forward_area_id = b_id;
    mesh.area_by_id(a_id)->add_ladder(ladder_id, true);
    mesh.area_by_id(b_id)->add_ladder(ladder_id, false);
    mesh.set_analyzed(true);

    const std::filesystem::path nav_path = root / "roundtrip.nav";
    REQUIRE(mesh.save_file(nav_path, 1234));

    openstrike::NavMesh loaded;
    REQUIRE(loaded.load_file(nav_path, 1234) == openstrike::NavError::Ok);
    REQUIRE(loaded.loaded());
    REQUIRE(!loaded.out_of_date());
    REQUIRE(loaded.analyzed());
    REQUIRE(loaded.file_version() == openstrike::kNavCurrentVersion);
    REQUIRE(loaded.sub_version() == openstrike::kCounterStrikeNavSubVersion);
    REQUIRE(loaded.areas().size() == 2);
    REQUIRE(loaded.ladders().size() == 1);
    REQUIRE(loaded.area_by_id(a_id)->hiding_spots.size() == 1);
    REQUIRE(loaded.area_by_id(a_id)->approach_areas.size() == 1);
    REQUIRE(loaded.area_by_id(a_id)->is_connected(b_id, openstrike::NavDirection::East));
    REQUIRE(loaded.load_file(nav_path, 5678) == openstrike::NavError::Ok);
    REQUIRE(loaded.out_of_date());

    std::filesystem::remove_all(root);
}

void test_navigation_system_generates_from_world_collision()
{
    openstrike::LoadedWorld world;
    world.name = "nav_generated";
    world.byte_size = 512;
    add_collision_triangle(world, {-64.0F, -64.0F, 0.0F}, {64.0F, -64.0F, 0.0F}, {64.0F, 64.0F, 0.0F}, {0.0F, 0.0F, 1.0F});
    add_collision_triangle(world, {-64.0F, -64.0F, 0.0F}, {64.0F, 64.0F, 0.0F}, {-64.0F, 64.0F, 0.0F}, {0.0F, 0.0F, 1.0F});
    add_collision_triangle(world, {-64.0F, -64.0F, 0.0F}, {64.0F, -64.0F, 64.0F}, {64.0F, 64.0F, 64.0F}, {0.0F, 1.0F, 0.0F});

    openstrike::NavigationSystem navigation;
    REQUIRE(navigation.generate_from_world(world));
    REQUIRE(navigation.mesh().loaded());
    REQUIRE(navigation.mesh().analyzed());
    REQUIRE(navigation.mesh().areas().size() == 1);
    REQUIRE(navigation.mesh().areas()[0].hiding_spots.size() == 4);

    const std::optional<float> ground = navigation.mesh().ground_height({0.0F, 0.0F, 32.0F});
    REQUIRE(ground.has_value());
    REQUIRE(std::abs(*ground) < 0.001F);

    openstrike::NavPath path = navigation.mesh().build_path({{-32.0F, 0.0F, 0.0F}, {32.0F, 0.0F, 0.0F}});
    REQUIRE(path.reached_goal);
}

void test_navigation_commands_sync_with_loaded_world()
{
    const std::filesystem::path root = std::filesystem::current_path() / "build/test_nav_command_content";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "game/maps");

    const std::string entities =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n";
    write_square_bsp(root / "game/maps/nav_command_world.bsp", entities);

    openstrike::RuntimeConfig config;
    config.content_root = root / "game";

    openstrike::EngineContext context;
    openstrike::configure_engine_context(context, config);

    context.command_buffer.add_text("map nav_command_world");
    openstrike::ConsoleCommandContext console_context = context.console_context();
    context.command_buffer.execute(context.commands, console_context);
    REQUIRE(context.world.current_world() != nullptr);
    REQUIRE(context.navigation.mesh().loaded());
    REQUIRE(!context.navigation.mesh().areas().empty());

    context.command_buffer.add_text("nav_save");
    context.command_buffer.execute(context.commands, console_context);
    REQUIRE(std::filesystem::exists(root / "game/maps/nav_command_world.nav"));

    context.navigation.clear();
    REQUIRE(!context.navigation.mesh().loaded());
    context.command_buffer.add_text("nav_load");
    context.command_buffer.execute(context.commands, console_context);
    REQUIRE(context.navigation.mesh().loaded());

    context.command_buffer.add_text("disconnect");
    context.command_buffer.execute(context.commands, console_context);
    REQUIRE(!context.navigation.mesh().loaded());

    std::filesystem::remove_all(root);
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

void test_physics_world_uses_source_player_solid_mask()
{
    openstrike::LoadedWorld world;
    world.name = "physics_player_solid_mask";

    add_collision_triangle(world,
        {-128.0F, -128.0F, 0.0F},
        {128.0F, -128.0F, 0.0F},
        {128.0F, 128.0F, 0.0F},
        {0.0F, 0.0F, 1.0F},
        kTestContentsSolid);
    add_collision_triangle(world,
        {-128.0F, -128.0F, 0.0F},
        {128.0F, 128.0F, 0.0F},
        {-128.0F, 128.0F, 0.0F},
        {0.0F, 0.0F, 1.0F},
        kTestContentsSolid);
    add_collision_triangle(world,
        {48.0F, -128.0F, 0.0F},
        {48.0F, 128.0F, 0.0F},
        {48.0F, 128.0F, 128.0F},
        {-1.0F, 0.0F, 0.0F},
        kTestContentsMonsterClip);
    add_collision_triangle(world,
        {48.0F, -128.0F, 0.0F},
        {48.0F, 128.0F, 128.0F},
        {48.0F, -128.0F, 128.0F},
        {-1.0F, 0.0F, 0.0F},
        kTestContentsMonsterClip);

    openstrike::PhysicsWorld physics;
    REQUIRE(physics.load_static_world(world));

    openstrike::PhysicsCharacterConfig config;
    config.radius = 16.0F;
    config.height = 72.0F;
    physics.reset_character({0.0F, 0.0F, 0.0F}, {}, config);

    openstrike::PhysicsCharacterState state = physics.character_state();
    constexpr float dt = 1.0F / 64.0F;
    for (int tick = 0; tick < 32; ++tick)
    {
        state = physics.move_character_to(state.origin + openstrike::Vec3{4.0F, 0.0F, 0.0F}, {256.0F, 0.0F, 0.0F}, dt);
    }

    REQUIRE(state.on_ground);
    REQUIRE(state.origin.x > 96.0F);
}

void test_physics_layers_match_vphysics_jolt_stack()
{
    using Layer = openstrike::PhysicsObjectLayer;

    REQUIRE(!openstrike::physics_layers_should_collide(Layer::NoCollide, Layer::Moving));
    REQUIRE(!openstrike::physics_layers_should_collide(Layer::Moving, Layer::NoCollide));
    REQUIRE(!openstrike::physics_layers_should_collide(Layer::NonMovingWorld, Layer::NonMovingWorld));
    REQUIRE(!openstrike::physics_layers_should_collide(Layer::NonMovingObject, Layer::NonMovingObject));
    REQUIRE(openstrike::physics_layers_should_collide(Layer::NonMovingWorld, Layer::Moving));
    REQUIRE(openstrike::physics_layers_should_collide(Layer::NonMovingObject, Layer::Moving));
    REQUIRE(openstrike::physics_layers_should_collide(Layer::Moving, Layer::Moving));
    REQUIRE(openstrike::physics_layers_should_collide(Layer::Debris, Layer::NonMovingWorld));
    REQUIRE(openstrike::physics_layers_should_collide(Layer::NonMovingObject, Layer::Debris));
    REQUIRE(!openstrike::physics_layers_should_collide(Layer::Debris, Layer::Moving));
}

void test_physics_world_steps_dynamic_body_on_static_world()
{
    openstrike::LoadedWorld world;
    world.name = "physics_dynamic_body";
    add_collision_triangle(world, {-128.0F, -128.0F, 0.0F}, {128.0F, -128.0F, 0.0F}, {128.0F, 128.0F, 0.0F}, {0.0F, 0.0F, 1.0F});
    add_collision_triangle(world, {-128.0F, -128.0F, 0.0F}, {128.0F, 128.0F, 0.0F}, {-128.0F, 128.0F, 0.0F}, {0.0F, 0.0F, 1.0F});

    openstrike::PhysicsWorld physics;
    REQUIRE(physics.load_static_world(world));

    openstrike::PhysicsBodyDesc desc;
    desc.shape = openstrike::PhysicsBodyShape::Box;
    desc.origin = {0.0F, 0.0F, 96.0F};
    desc.half_extents = {8.0F, 8.0F, 8.0F};
    desc.mass = 25.0F;

    const openstrike::PhysicsBodyHandle body = physics.create_body(desc);
    REQUIRE(body.valid());

    for (int step = 0; step < 180; ++step)
    {
        physics.step_simulation(1.0F / 120.0F);
    }

    const openstrike::PhysicsBodyState state = physics.body_state(body);
    REQUIRE(state.layer == openstrike::PhysicsObjectLayer::Moving);
    REQUIRE(state.contents == openstrike::PhysicsContents::Solid);
    REQUIRE(state.origin.z < 32.0F);
    REQUIRE(state.origin.z > 6.0F);
}

void test_physics_world_uses_engine_settings()
{
    openstrike::PhysicsWorldSettings settings;
    settings.gravity = {0.0F, 0.0F, 0.0F};
    settings.max_bodies = 64;
    settings.default_collision_sub_steps = 4;

    openstrike::PhysicsWorld physics(settings);
    REQUIRE(physics.settings().max_bodies == 64);
    REQUIRE(physics.settings().default_collision_sub_steps == 4);
    REQUIRE(std::abs(physics.gravity().z) < 0.001F);

    openstrike::PhysicsBodyDesc desc;
    desc.origin = {0.0F, 0.0F, 96.0F};
    desc.half_extents = {8.0F, 8.0F, 8.0F};

    const openstrike::PhysicsBodyHandle body = physics.create_body(desc);
    REQUIRE(body.valid());

    for (int step = 0; step < 60; ++step)
    {
        physics.step_simulation(1.0F / 60.0F);
    }

    openstrike::PhysicsBodyState state = physics.body_state(body);
    REQUIRE(state.motion_type == openstrike::PhysicsBodyMotionType::Dynamic);
    REQUIRE(std::abs(state.origin.z - 96.0F) < 0.01F);

    physics.set_gravity({0.0F, 0.0F, -400.0F});
    REQUIRE(std::abs(physics.gravity().z + 400.0F) < 0.001F);
    REQUIRE(physics.set_body_velocity(body, {}));
    for (int step = 0; step < 60; ++step)
    {
        physics.step_simulation(1.0F / 60.0F);
    }

    state = physics.body_state(body);
    REQUIRE(state.origin.z < 80.0F);
}

void test_physics_world_exposes_engine_body_controls()
{
    openstrike::PhysicsWorldSettings settings;
    settings.gravity = {0.0F, 0.0F, 0.0F};
    openstrike::PhysicsWorld physics(settings);

    openstrike::PhysicsBodyDesc kinematic;
    kinematic.motion_type = openstrike::PhysicsBodyMotionType::Kinematic;
    kinematic.origin = {0.0F, 0.0F, 16.0F};
    kinematic.half_extents = {8.0F, 8.0F, 8.0F};
    const openstrike::PhysicsBodyHandle kinematic_body = physics.create_body(kinematic);
    REQUIRE(kinematic_body.valid());
    REQUIRE(physics.body_state(kinematic_body).motion_type == openstrike::PhysicsBodyMotionType::Kinematic);

    REQUIRE(physics.set_body_origin(kinematic_body, {32.0F, 0.0F, 16.0F}));
    REQUIRE(std::abs(physics.body_state(kinematic_body).origin.x - 32.0F) < 0.01F);
    REQUIRE(physics.move_kinematic_body(kinematic_body, {64.0F, 0.0F, 16.0F}, 1.0F / 60.0F));
    physics.step_simulation(1.0F / 60.0F);
    REQUIRE(physics.body_state(kinematic_body).origin.x > 48.0F);

    openstrike::PhysicsBodyDesc capsule;
    capsule.shape = openstrike::PhysicsBodyShape::Capsule;
    capsule.motion_type = openstrike::PhysicsBodyMotionType::Static;
    capsule.origin = {0.0F, 64.0F, 32.0F};
    capsule.radius = 8.0F;
    capsule.height = 48.0F;
    const openstrike::PhysicsBodyHandle capsule_body = physics.create_body(capsule);
    REQUIRE(capsule_body.valid());
    REQUIRE(physics.body_state(capsule_body).motion_type == openstrike::PhysicsBodyMotionType::Static);
    REQUIRE(!physics.apply_body_impulse(capsule_body, {0.0F, 0.0F, 100.0F}));

    openstrike::PhysicsBodyDesc dynamic;
    dynamic.origin = {0.0F, 128.0F, 16.0F};
    dynamic.half_extents = {8.0F, 8.0F, 8.0F};
    const openstrike::PhysicsBodyHandle dynamic_body = physics.create_body(dynamic);
    REQUIRE(dynamic_body.valid());
    REQUIRE(!physics.move_kinematic_body(dynamic_body, {64.0F, 128.0F, 16.0F}, 1.0F / 60.0F));
    REQUIRE(physics.apply_body_impulse(dynamic_body, {0.0F, 0.0F, 1000.0F}));
    physics.step_simulation(1.0F / 60.0F);
    REQUIRE(physics.body_state(dynamic_body).velocity.z > 0.0F);
}

void test_physics_world_traces_filter_contents_and_layers()
{
    openstrike::PhysicsWorld physics;

    openstrike::PhysicsBodyDesc monster_clip;
    monster_clip.dynamic = false;
    monster_clip.layer = openstrike::PhysicsObjectLayer::NonMovingObject;
    monster_clip.origin = {64.0F, 0.0F, 16.0F};
    monster_clip.half_extents = {8.0F, 32.0F, 32.0F};
    monster_clip.contents = openstrike::PhysicsContents::MonsterClip;
    REQUIRE(physics.create_body(monster_clip).valid());

    const openstrike::PhysicsTraceResult player_mask_trace =
        physics.trace_ray({0.0F, 0.0F, 16.0F}, {128.0F, 0.0F, 16.0F}, openstrike::PhysicsContents::MaskPlayerSolid);
    REQUIRE(!player_mask_trace.hit);

    openstrike::PhysicsTraceDesc world_mask_sweep;
    world_mask_sweep.start = {0.0F, 0.0F, 16.0F};
    world_mask_sweep.end = {128.0F, 0.0F, 16.0F};
    world_mask_sweep.mins = {-4.0F, -4.0F, -4.0F};
    world_mask_sweep.maxs = {4.0F, 4.0F, 4.0F};
    world_mask_sweep.contents_mask = openstrike::PhysicsContents::MaskWorldStatic;
    const openstrike::PhysicsTraceResult world_mask_trace = physics.trace_box(world_mask_sweep);
    REQUIRE(world_mask_trace.hit);
    REQUIRE(world_mask_trace.layer == openstrike::PhysicsObjectLayer::NonMovingObject);
    REQUIRE(world_mask_trace.contents == openstrike::PhysicsContents::MonsterClip);
    REQUIRE(world_mask_trace.fraction > 0.0F);
    REQUIRE(world_mask_trace.fraction < 1.0F);

    openstrike::PhysicsBodyDesc moving_body;
    moving_body.origin = {64.0F, 80.0F, 16.0F};
    moving_body.half_extents = {8.0F, 32.0F, 32.0F};
    moving_body.contents = openstrike::PhysicsContents::Solid;
    REQUIRE(physics.create_body(moving_body).valid());

    const openstrike::PhysicsTraceResult moving_trace =
        physics.trace_ray({0.0F, 80.0F, 16.0F}, {128.0F, 80.0F, 16.0F}, openstrike::PhysicsContents::MaskSolid);
    REQUIRE(moving_trace.hit);
    REQUIRE(moving_trace.layer == openstrike::PhysicsObjectLayer::Moving);

    openstrike::PhysicsTraceDesc debris_trace_desc;
    debris_trace_desc.start = {0.0F, 80.0F, 16.0F};
    debris_trace_desc.end = {128.0F, 80.0F, 16.0F};
    debris_trace_desc.query_layer = openstrike::PhysicsObjectLayer::Debris;
    debris_trace_desc.contents_mask = openstrike::PhysicsContents::MaskSolid;
    REQUIRE(!physics.trace_box(debris_trace_desc).hit);
}
}

int main()
{
    try
    {
        test_command_line_config();
        test_runtime_defaults_support_other_games();
        test_renderer_aliases();
        test_fixed_step_accumulates_ticks();
        test_fixed_step_clamps_runaway_frames();
        test_content_filesystem_path_ids();
        test_source_asset_store_reuses_vpk_directory_mounts();
        test_editor_displacement_uvs_stay_parent_quad_anchored();
        test_editor_displacement_position_edits_round_trip_source_field_vectors();
        test_source_keyvalues_parser_handles_source_roots_and_vectors();
        test_material_system_resolves_source_vmt_without_shader_combos();
        test_source_texture_decodes_compressed_vtf_to_rgba();
        test_source_texture_loads_resource_vtf_metadata_and_legacy_formats();
        test_renderer_shader_files_are_dedicated();
        test_network_stream_and_protocol_roundtrip();
        test_network_user_command_delta_batch_and_input_roundtrip();
        test_network_messages_round_trip_source_families();
        test_network_channel_reliable_fragmentation_and_ack();
        test_network_channel_reassembles_reordered_fragments();
        test_network_channel_reliable_retransmit_repairs_lost_ack();
        test_network_channel_rejects_excessive_fragment_count();
        test_network_snapshot_delta_round_trip_and_apply();
        test_network_prediction_reconciles_and_replays_user_commands();
        test_network_udp_loopback_connects_and_exchanges_text();
        test_network_udp_loopback_client_sends_user_command();
        test_network_udp_loopback_server_sends_snapshot();
        test_network_client_defers_channel_signon_until_connect_accept();
        test_network_udp_loopback_signon_and_user_command_batch();
        test_team_id_helpers_match_counter_strike();
        test_team_manager_membership_and_scores();
        test_team_auto_assign_uses_smaller_team_then_lower_score();
        test_team_join_rejections_for_capacity_stack_spectator_and_humanteam();
        test_team_command_jointeam_applies_local_offline();
        test_team_command_jointeam_requires_joingame();
        test_team_payloads_and_snapshot_round_trip();
        test_team_network_join_command_validates_and_replicates_snapshot();
        test_connected_team_menu_targets_local_connection();
        test_listen_server_local_team_join_broadcasts_snapshot();
        test_failed_team_join_snapshot_preserves_current_team();
        test_team_specific_spawn_selection_from_bsp_entities();
        test_hud_state_reflects_authoritative_team();
        test_command_buffer_cvars_and_quit();
        test_loading_screen_state_matches_source_map_lifecycle();
        test_audio_sound_chars_match_source_prefix_rules();
        test_audio_source_soundlevel_gain_falls_with_distance();
        test_world_manager_loads_source_bsp();
        test_world_manager_builds_bsp_mesh_and_floor();
        test_world_manager_builds_source_displacement_mesh();
        test_world_manager_builds_bsp_static_lightmap();
        test_world_manager_prefers_hdr_bsp_lightmap();
        test_world_manager_tracks_source_sky_surfaces();
        test_world_manager_uses_source_brush_contents_for_collision();
        test_world_manager_loads_static_props_and_renders_bounds();
        test_world_manager_loads_large_static_prop_studio_mesh();
        test_world_manager_loads_bbox_static_prop_collision();
        test_source_studio_parses_animation_metadata();
        test_source_animation_state_advances_cycles_events_and_pose();
        test_csgo_player_anim_state_sets_layers_and_pose_parameters();
        test_animation_scene_loads_and_advances_source_model();
        test_game_simulation_builds_animation_scene_for_dynamic_props();
        test_source_fgd_loads_classes_and_metadata();
        test_source_fgd_accepts_concatenated_source_strings();
        test_map_command_loads_current_world();
        test_map_and_changelevel_match_source_server_rules();
        test_game_mode_cvars_aliases_cfg_and_loading();
        test_game_mode_auto_alias_reads_map_defaults();
        test_disconnect_command_unloads_world_and_network();
        test_quit_and_exit_commands_disconnect_before_shutdown();
        test_game_simulation_updates_hud_for_loaded_world();
        test_engine_startup_quit_command();
        test_application_runs_null_renderer_smoke();
        test_openstrike_application_definition_filters_default_modules();
        test_application_accepts_custom_game_module_stack();
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
        test_nav_area_geometry_matches_source_semantics();
        test_nav_mesh_pathing_uses_source_costs_blocking_and_ladders();
        test_nav_mesh_round_trips_source_v16_cs_subversion();
        test_navigation_system_generates_from_world_collision();
        test_navigation_commands_sync_with_loaded_world();
        test_physics_world_blocks_character_against_static_mesh();
        test_physics_world_uses_source_player_solid_mask();
        test_physics_layers_match_vphysics_jolt_stack();
        test_physics_world_steps_dynamic_body_on_static_world();
        test_physics_world_uses_engine_settings();
        test_physics_world_exposes_engine_body_controls();
        test_physics_world_traces_filter_contents_and_layers();
    }
    catch (const std::exception& error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }

    std::cout << "openstrike tests passed\n";
    return 0;
}
