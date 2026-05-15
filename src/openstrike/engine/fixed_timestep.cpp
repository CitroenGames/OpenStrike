#include "openstrike/engine/fixed_timestep.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openstrike
{
FixedStepAccumulator::FixedStepAccumulator(double tick_interval_seconds)
    : tick_interval_seconds_(tick_interval_seconds)
{
    if (tick_interval_seconds_ <= 0.0)
    {
        throw std::invalid_argument("tick interval must be positive");
    }
}

double FixedStepAccumulator::tick_interval_seconds() const
{
    return tick_interval_seconds_;
}

double FixedStepAccumulator::interpolation_alpha() const
{
    return std::clamp(accumulator_seconds_ / tick_interval_seconds_, 0.0, 1.0);
}

int FixedStepAccumulator::consume(double frame_seconds, int max_ticks)
{
    if (frame_seconds < 0.0)
    {
        frame_seconds = 0.0;
    }

    accumulator_seconds_ += frame_seconds;

    int tick_count = 0;
    while (accumulator_seconds_ >= tick_interval_seconds_ && tick_count < max_ticks)
    {
        accumulator_seconds_ -= tick_interval_seconds_;
        ++tick_count;
    }

    if (tick_count == max_ticks && accumulator_seconds_ >= tick_interval_seconds_)
    {
        accumulator_seconds_ = std::fmod(accumulator_seconds_, tick_interval_seconds_);
    }

    return tick_count;
}
}
