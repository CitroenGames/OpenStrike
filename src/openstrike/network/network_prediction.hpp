#pragma once

#include "openstrike/game/movement.hpp"
#include "openstrike/network/user_command.hpp"

#include <cstddef>
#include <vector>

namespace openstrike
{
struct PredictedCommandState
{
    UserCommand command;
    PlayerState before;
    PlayerState after;
};

struct PredictionReconcileResult
{
    bool corrected = false;
    std::size_t replayed_commands = 0;
    PlayerState state;
};

class ClientPredictionBuffer
{
public:
    explicit ClientPredictionBuffer(std::size_t max_commands = 128);

    void reset(PlayerState state = {});
    [[nodiscard]] PlayerState predict(const UserCommand& command, const MovementTuning& tuning);
    [[nodiscard]] PredictionReconcileResult reconcile(
        std::int32_t authoritative_command_number,
        const PlayerState& authoritative_state,
        const MovementTuning& tuning,
        float epsilon = 0.03125F);
    [[nodiscard]] const PlayerState& current_state() const;
    [[nodiscard]] const std::vector<PredictedCommandState>& history() const;

private:
    std::size_t max_commands_ = 128;
    PlayerState current_state_{};
    std::vector<PredictedCommandState> history_;
};
}
