#pragma once

#include "openstrike/core/math.hpp"
#include "openstrike/game/movement.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openstrike
{
enum UserCommandButton : std::uint32_t
{
    UserCommandButtonAttack = 1U << 0U,
    UserCommandButtonJump = 1U << 1U,
    UserCommandButtonDuck = 1U << 2U,
    UserCommandButtonForward = 1U << 3U,
    UserCommandButtonBack = 1U << 4U,
    UserCommandButtonUse = 1U << 5U,
    UserCommandButtonLeft = 1U << 6U,
    UserCommandButtonRight = 1U << 7U,
    UserCommandButtonMoveLeft = 1U << 9U,
    UserCommandButtonMoveRight = 1U << 10U,
    UserCommandButtonRun = 1U << 12U,
    UserCommandButtonReload = 1U << 13U,
    UserCommandButtonAlt1 = 1U << 14U,
    UserCommandButtonScore = 1U << 16U,
    UserCommandButtonSpeed = 1U << 17U,
};

struct UserCommand
{
    std::int32_t command_number = 0;
    std::int32_t tick_count = 0;
    Vec3 viewangles;
    Vec3 aimdirection;
    float forwardmove = 0.0F;
    float sidemove = 0.0F;
    float upmove = 0.0F;
    std::uint32_t buttons = 0;
    std::uint8_t impulse = 0;
    std::int32_t weaponselect = 0;
    std::int32_t weaponsubtype = 0;
    std::int32_t random_seed = 0;
    std::int16_t mousedx = 0;
    std::int16_t mousedy = 0;
    bool has_been_predicted = false;
};

struct UserCommandBatch
{
    std::uint8_t num_backup_commands = 0;
    std::uint8_t num_new_commands = 0;
    std::vector<UserCommand> commands;
};

[[nodiscard]] std::uint32_t checksum_user_command(const UserCommand& command);
[[nodiscard]] InputCommand movement_input_from_user_command(const UserCommand& command);
[[nodiscard]] std::vector<unsigned char> encode_user_command_delta(const UserCommand& command, const UserCommand* from = nullptr);
[[nodiscard]] std::optional<UserCommand> decode_user_command_delta(std::span<const unsigned char> bytes, const UserCommand* from = nullptr);
[[nodiscard]] std::vector<unsigned char> encode_user_command_batch(const UserCommandBatch& batch);
[[nodiscard]] std::optional<UserCommandBatch> decode_user_command_batch(std::span<const unsigned char> bytes);
}
