#include "openstrike/network/network_prediction.hpp"

#include <algorithm>
#include <cmath>

namespace openstrike
{
namespace
{
bool needs_correction(const PlayerState& predicted, const PlayerState& authoritative, float epsilon)
{
    return std::fabs(predicted.origin.x - authoritative.origin.x) > epsilon ||
           std::fabs(predicted.origin.y - authoritative.origin.y) > epsilon ||
           std::fabs(predicted.origin.z - authoritative.origin.z) > epsilon ||
           std::fabs(predicted.velocity.x - authoritative.velocity.x) > epsilon ||
           std::fabs(predicted.velocity.y - authoritative.velocity.y) > epsilon ||
           std::fabs(predicted.velocity.z - authoritative.velocity.z) > epsilon ||
           predicted.on_ground != authoritative.on_ground || predicted.ducked != authoritative.ducked ||
           predicted.ducking != authoritative.ducking;
}
}

ClientPredictionBuffer::ClientPredictionBuffer(std::size_t max_commands)
    : max_commands_(std::max<std::size_t>(1, max_commands))
{
}

void ClientPredictionBuffer::reset(PlayerState state)
{
    current_state_ = state;
    history_.clear();
}

PlayerState ClientPredictionBuffer::predict(const UserCommand& command, const MovementTuning& tuning)
{
    PredictedCommandState entry;
    entry.command = command;
    entry.before = current_state_;
    current_state_ = entry.before;
    simulate_player_move(current_state_, movement_input_from_user_command(command), tuning);
    entry.after = current_state_;
    history_.push_back(entry);
    if (history_.size() > max_commands_)
    {
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - max_commands_));
    }
    return current_state_;
}

PredictionReconcileResult ClientPredictionBuffer::reconcile(
    std::int32_t authoritative_command_number,
    const PlayerState& authoritative_state,
    const MovementTuning& tuning,
    float epsilon)
{
    PredictionReconcileResult result;
    result.state = current_state_;
    auto acked = std::find_if(history_.begin(), history_.end(), [&](const PredictedCommandState& entry) {
        return entry.command.command_number == authoritative_command_number;
    });
    if (acked == history_.end())
    {
        current_state_ = authoritative_state;
        history_.clear();
        result.corrected = true;
        result.state = current_state_;
        return result;
    }

    result.corrected = needs_correction(acked->after, authoritative_state, epsilon);
    std::vector<UserCommand> replay;
    for (auto it = acked + 1; it != history_.end(); ++it)
    {
        replay.push_back(it->command);
    }

    history_.erase(history_.begin(), acked + 1);
    if (result.corrected)
    {
        current_state_ = authoritative_state;
        history_.clear();
        for (const UserCommand& command : replay)
        {
            result.state = predict(command, tuning);
            ++result.replayed_commands;
        }
    }
    result.state = current_state_;
    return result;
}

const PlayerState& ClientPredictionBuffer::current_state() const
{
    return current_state_;
}

const std::vector<PredictedCommandState>& ClientPredictionBuffer::history() const
{
    return history_;
}
}
