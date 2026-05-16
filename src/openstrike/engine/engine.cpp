#include "openstrike/engine/engine.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/fixed_timestep.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace openstrike
{
namespace
{
constexpr double kFallbackFrameSeconds = 1.0 / 60.0;

double frame_seconds(std::chrono::steady_clock::time_point& previous)
{
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - previous;
    previous = now;

    if (elapsed.count() <= 0.0)
    {
        return kFallbackFrameSeconds;
    }

    return elapsed.count();
}
}

Engine::Engine(std::unique_ptr<IRenderer> renderer)
    : renderer_(std::move(renderer))
{
}

void Engine::add_module(std::unique_ptr<EngineModule> module)
{
    modules_.push_back(std::move(module));
}

EngineStats Engine::run(const RuntimeConfig& config)
{
    log_info("starting {} mode={} tickrate={}", config.application_name, to_string(config.mode), config.tick_rate);
    configure_engine_context(context_, config);
    renderer_->set_engine_context(&context_);

    if (!renderer_->initialize(config))
    {
        log_error("renderer initialization failed");
        return {};
    }

    for (const auto& module : modules_)
    {
        log_info("starting module '{}'", module->name());
        module->on_start(config, context_);
    }

    EngineStats stats;
    FixedStepAccumulator fixed_step(config.tick_interval_seconds());
    auto previous_frame = std::chrono::steady_clock::now();

    while (!renderer_->should_close() && !context_.quit_requested && (config.max_frames == 0 || stats.frame_count < config.max_frames))
    {
        ConsoleCommandContext console_context = context_.console_context();
        context_.command_buffer.execute(context_.commands, console_context);
        if (context_.quit_requested)
        {
            break;
        }

        const double elapsed = config.deterministic_frames ? kFallbackFrameSeconds : frame_seconds(previous_frame);
        const int ticks_to_run = fixed_step.consume(elapsed, config.max_ticks_per_frame);

        for (int tick = 0; tick < ticks_to_run; ++tick)
        {
            SimulationStep step{
                .tick_index = stats.tick_count,
                .delta_seconds = config.tick_interval_seconds(),
            };

            for (const auto& module : modules_)
            {
                module->on_fixed_update(step, context_);
            }

            ++stats.tick_count;
            stats.simulated_seconds += config.tick_interval_seconds();
        }

        FrameContext context{
            .frame_index = stats.frame_count,
            .tick_index = stats.tick_count,
            .interpolation_alpha = fixed_step.interpolation_alpha(),
        };

        for (const auto& module : modules_)
        {
            module->on_frame(context, context_);
        }

        RenderFrame render_frame{
            .timing = context,
            .scene =
                {
                    .world = context_.world.current_world(),
                    .world_generation = context_.world.generation(),
                    .filesystem = &context_.filesystem,
                    .animation = &context_.animation,
                },
            .camera = context_.camera,
        };
        renderer_->render(render_frame);
        ++stats.frame_count;

        if (!config.deterministic_frames)
        {
            std::this_thread::yield();
        }
    }

    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it)
    {
        (*it)->on_stop(context_);
    }

    renderer_->shutdown();
    return stats;
}
}
