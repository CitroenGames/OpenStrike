#pragma once

namespace openstrike
{
class FixedStepAccumulator
{
public:
    explicit FixedStepAccumulator(double tick_interval_seconds);

    [[nodiscard]] double tick_interval_seconds() const;
    [[nodiscard]] double interpolation_alpha() const;
    [[nodiscard]] int consume(double frame_seconds, int max_ticks);

private:
    double tick_interval_seconds_ = 1.0 / 64.0;
    double accumulator_seconds_ = 0.0;
};
}

