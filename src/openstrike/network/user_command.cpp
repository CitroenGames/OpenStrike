#include "openstrike/network/user_command.hpp"

#include "openstrike/network/network_stream.hpp"

#include <bit>
#include <cstring>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kUserCommandBatchMagic = 0x444D4355U; // UCMD
constexpr std::uint16_t kUserCommandBatchVersion = 1;
constexpr std::uint32_t kMaxUserCommandsPerBatch = 32;

enum UserCommandDeltaBits : std::uint32_t
{
    CommandNumber = 1U << 0U,
    TickCount = 1U << 1U,
    ViewAngles = 1U << 2U,
    AimDirection = 1U << 3U,
    ForwardMove = 1U << 4U,
    SideMove = 1U << 5U,
    UpMove = 1U << 6U,
    Buttons = 1U << 7U,
    Impulse = 1U << 8U,
    WeaponSelect = 1U << 9U,
    WeaponSubtype = 1U << 10U,
    RandomSeed = 1U << 11U,
    MouseDelta = 1U << 12U,
};

std::uint32_t fnv1a_append(std::uint32_t hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 16777619U;
    }
    return hash;
}

void write_i32(NetworkByteWriter& writer, std::int32_t value)
{
    writer.write_u32(static_cast<std::uint32_t>(value));
}

bool read_i32(NetworkByteReader& reader, std::int32_t& value)
{
    std::uint32_t raw = 0;
    if (!reader.read_u32(raw))
    {
        return false;
    }
    value = static_cast<std::int32_t>(raw);
    return true;
}

void write_i16(NetworkByteWriter& writer, std::int16_t value)
{
    writer.write_u16(static_cast<std::uint16_t>(value));
}

bool read_i16(NetworkByteReader& reader, std::int16_t& value)
{
    std::uint16_t raw = 0;
    if (!reader.read_u16(raw))
    {
        return false;
    }
    value = static_cast<std::int16_t>(raw);
    return true;
}

void write_f32(NetworkByteWriter& writer, float value)
{
    writer.write_u32(std::bit_cast<std::uint32_t>(value));
}

bool read_f32(NetworkByteReader& reader, float& value)
{
    std::uint32_t raw = 0;
    if (!reader.read_u32(raw))
    {
        return false;
    }
    value = std::bit_cast<float>(raw);
    return true;
}

void write_vec3(NetworkByteWriter& writer, Vec3 value)
{
    write_f32(writer, value.x);
    write_f32(writer, value.y);
    write_f32(writer, value.z);
}

bool read_vec3(NetworkByteReader& reader, Vec3& value)
{
    return read_f32(reader, value.x) && read_f32(reader, value.y) && read_f32(reader, value.z);
}

bool same_vec3(Vec3 lhs, Vec3 rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}
}

std::uint32_t checksum_user_command(const UserCommand& command)
{
    std::uint32_t hash = 2166136261U;
    hash = fnv1a_append(hash, &command.command_number, sizeof(command.command_number));
    hash = fnv1a_append(hash, &command.tick_count, sizeof(command.tick_count));
    hash = fnv1a_append(hash, &command.viewangles, sizeof(command.viewangles));
    hash = fnv1a_append(hash, &command.aimdirection, sizeof(command.aimdirection));
    hash = fnv1a_append(hash, &command.forwardmove, sizeof(command.forwardmove));
    hash = fnv1a_append(hash, &command.sidemove, sizeof(command.sidemove));
    hash = fnv1a_append(hash, &command.upmove, sizeof(command.upmove));
    hash = fnv1a_append(hash, &command.buttons, sizeof(command.buttons));
    hash = fnv1a_append(hash, &command.impulse, sizeof(command.impulse));
    hash = fnv1a_append(hash, &command.weaponselect, sizeof(command.weaponselect));
    hash = fnv1a_append(hash, &command.weaponsubtype, sizeof(command.weaponsubtype));
    hash = fnv1a_append(hash, &command.random_seed, sizeof(command.random_seed));
    hash = fnv1a_append(hash, &command.mousedx, sizeof(command.mousedx));
    hash = fnv1a_append(hash, &command.mousedy, sizeof(command.mousedy));
    return hash;
}

InputCommand movement_input_from_user_command(const UserCommand& command)
{
    return InputCommand{
        .move_x = command.sidemove,
        .move_y = command.forwardmove,
        .jump = (command.buttons & UserCommandButtonJump) != 0,
        .duck = (command.buttons & UserCommandButtonDuck) != 0,
        .walk = (command.buttons & UserCommandButtonSpeed) != 0,
    };
}

std::vector<unsigned char> encode_user_command_delta(const UserCommand& command, const UserCommand* from)
{
    const UserCommand baseline = from != nullptr ? *from : UserCommand{};
    std::uint32_t changed = 0;
    changed |= command.command_number != baseline.command_number ? CommandNumber : 0U;
    changed |= command.tick_count != baseline.tick_count ? TickCount : 0U;
    changed |= !same_vec3(command.viewangles, baseline.viewangles) ? ViewAngles : 0U;
    changed |= !same_vec3(command.aimdirection, baseline.aimdirection) ? AimDirection : 0U;
    changed |= command.forwardmove != baseline.forwardmove ? ForwardMove : 0U;
    changed |= command.sidemove != baseline.sidemove ? SideMove : 0U;
    changed |= command.upmove != baseline.upmove ? UpMove : 0U;
    changed |= command.buttons != baseline.buttons ? Buttons : 0U;
    changed |= command.impulse != baseline.impulse ? Impulse : 0U;
    changed |= command.weaponselect != baseline.weaponselect ? WeaponSelect : 0U;
    changed |= command.weaponsubtype != baseline.weaponsubtype ? WeaponSubtype : 0U;
    changed |= command.random_seed != baseline.random_seed ? RandomSeed : 0U;
    changed |= command.mousedx != baseline.mousedx || command.mousedy != baseline.mousedy ? MouseDelta : 0U;

    NetworkByteWriter writer;
    writer.write_u32(changed);
    if ((changed & CommandNumber) != 0)
    {
        write_i32(writer, command.command_number);
    }
    if ((changed & TickCount) != 0)
    {
        write_i32(writer, command.tick_count);
    }
    if ((changed & ViewAngles) != 0)
    {
        write_vec3(writer, command.viewangles);
    }
    if ((changed & AimDirection) != 0)
    {
        write_vec3(writer, command.aimdirection);
    }
    if ((changed & ForwardMove) != 0)
    {
        write_f32(writer, command.forwardmove);
    }
    if ((changed & SideMove) != 0)
    {
        write_f32(writer, command.sidemove);
    }
    if ((changed & UpMove) != 0)
    {
        write_f32(writer, command.upmove);
    }
    if ((changed & Buttons) != 0)
    {
        writer.write_u32(command.buttons);
    }
    if ((changed & Impulse) != 0)
    {
        writer.write_u8(command.impulse);
    }
    if ((changed & WeaponSelect) != 0)
    {
        write_i32(writer, command.weaponselect);
    }
    if ((changed & WeaponSubtype) != 0)
    {
        write_i32(writer, command.weaponsubtype);
    }
    if ((changed & RandomSeed) != 0)
    {
        write_i32(writer, command.random_seed);
    }
    if ((changed & MouseDelta) != 0)
    {
        write_i16(writer, command.mousedx);
        write_i16(writer, command.mousedy);
    }
    writer.write_u32(checksum_user_command(command));
    return writer.take_bytes();
}

std::optional<UserCommand> decode_user_command_delta(std::span<const unsigned char> bytes, const UserCommand* from)
{
    NetworkByteReader reader(bytes);
    const UserCommand baseline = from != nullptr ? *from : UserCommand{};
    UserCommand command = baseline;
    std::uint32_t changed = 0;
    if (!reader.read_u32(changed))
    {
        return std::nullopt;
    }
    if ((changed & CommandNumber) != 0 && !read_i32(reader, command.command_number))
    {
        return std::nullopt;
    }
    if ((changed & TickCount) != 0 && !read_i32(reader, command.tick_count))
    {
        return std::nullopt;
    }
    if ((changed & ViewAngles) != 0 && !read_vec3(reader, command.viewangles))
    {
        return std::nullopt;
    }
    if ((changed & AimDirection) != 0 && !read_vec3(reader, command.aimdirection))
    {
        return std::nullopt;
    }
    if ((changed & ForwardMove) != 0 && !read_f32(reader, command.forwardmove))
    {
        return std::nullopt;
    }
    if ((changed & SideMove) != 0 && !read_f32(reader, command.sidemove))
    {
        return std::nullopt;
    }
    if ((changed & UpMove) != 0 && !read_f32(reader, command.upmove))
    {
        return std::nullopt;
    }
    if ((changed & Buttons) != 0 && !reader.read_u32(command.buttons))
    {
        return std::nullopt;
    }
    if ((changed & Impulse) != 0 && !reader.read_u8(command.impulse))
    {
        return std::nullopt;
    }
    if ((changed & WeaponSelect) != 0 && !read_i32(reader, command.weaponselect))
    {
        return std::nullopt;
    }
    if ((changed & WeaponSubtype) != 0 && !read_i32(reader, command.weaponsubtype))
    {
        return std::nullopt;
    }
    if ((changed & RandomSeed) != 0 && !read_i32(reader, command.random_seed))
    {
        return std::nullopt;
    }
    if ((changed & MouseDelta) != 0 && (!read_i16(reader, command.mousedx) || !read_i16(reader, command.mousedy)))
    {
        return std::nullopt;
    }

    std::uint32_t checksum = 0;
    if (!reader.read_u32(checksum) || !reader.empty() || checksum != checksum_user_command(command))
    {
        return std::nullopt;
    }
    return command;
}

std::vector<unsigned char> encode_user_command_batch(const UserCommandBatch& batch)
{
    if (batch.commands.size() > kMaxUserCommandsPerBatch ||
        batch.commands.size() != static_cast<std::size_t>(batch.num_backup_commands + batch.num_new_commands))
    {
        return {};
    }

    NetworkByteWriter writer;
    writer.write_u32(kUserCommandBatchMagic);
    writer.write_u16(kUserCommandBatchVersion);
    writer.write_u8(batch.num_backup_commands);
    writer.write_u8(batch.num_new_commands);
    UserCommand baseline;
    const UserCommand* previous = nullptr;
    for (const UserCommand& command : batch.commands)
    {
        const std::vector<unsigned char> delta = encode_user_command_delta(command, previous);
        if (delta.size() > UINT16_MAX)
        {
            return {};
        }
        writer.write_u16(static_cast<std::uint16_t>(delta.size()));
        writer.write_bytes(delta);
        baseline = command;
        previous = &baseline;
    }
    return writer.take_bytes();
}

std::optional<UserCommandBatch> decode_user_command_batch(std::span<const unsigned char> bytes)
{
    NetworkByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    UserCommandBatch batch;
    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u8(batch.num_backup_commands) ||
        !reader.read_u8(batch.num_new_commands))
    {
        return std::nullopt;
    }
    const std::uint32_t command_count = static_cast<std::uint32_t>(batch.num_backup_commands) + batch.num_new_commands;
    if (magic != kUserCommandBatchMagic || version != kUserCommandBatchVersion || command_count > kMaxUserCommandsPerBatch)
    {
        return std::nullopt;
    }

    UserCommand baseline;
    const UserCommand* previous = nullptr;
    batch.commands.reserve(command_count);
    for (std::uint32_t index = 0; index < command_count; ++index)
    {
        std::uint16_t delta_size = 0;
        if (!reader.read_u16(delta_size) || reader.remaining_bytes().size() < delta_size)
        {
            return std::nullopt;
        }
        const std::span<const unsigned char> delta = reader.remaining_bytes().subspan(0, delta_size);
        std::optional<UserCommand> command = decode_user_command_delta(delta, previous);
        if (!command)
        {
            return std::nullopt;
        }
        batch.commands.push_back(*command);
        baseline = *command;
        previous = &baseline;

        std::vector<unsigned char> discard(delta_size);
        if (!reader.read_bytes(discard))
        {
            return std::nullopt;
        }
    }
    if (!reader.empty())
    {
        return std::nullopt;
    }
    return batch;
}
}
