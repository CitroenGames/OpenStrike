#include "openstrike/game/game_simulation.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/game/team_system.hpp"
#include "openstrike/world/world.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace openstrike
{
const char* GameSimulation::name() const
{
    return "game";
}

void GameSimulation::on_start(const RuntimeConfig& config, EngineContext& engine)
{
    movement_.tick_interval_seconds = static_cast<float>(config.tick_interval_seconds());
    local_player_.on_ground = true;
    update_hud(engine);
    log_info("game simulation started");
}

void GameSimulation::on_fixed_update(const SimulationStep&, EngineContext& engine)
{
    sync_world(engine);
    sync_team_state(engine);
    if (!local_player_active(engine))
    {
        engine.input.clear_gameplay_buttons();
        engine.camera.active = false;
        update_hud(engine);
        return;
    }

    const FpsInputSample input_sample = sample_fps_input(engine.input);
    update_fps_view(fps_view_, input_sample, fps_settings_);
    input_ = build_fps_move_command(fps_view_, input_sample);

    if (const LoadedWorld* world = engine.world.current_world())
    {
        const std::optional<float> floor_z = find_floor_z(*world, local_player_.origin, 512.0F);
        PlayerState desired_player = local_player_;
        simulate_player_move(desired_player, input_, movement_, floor_z);

        if (physics_.has_character())
        {
            const PhysicsCharacterState resolved =
                physics_.move_character_to(desired_player.origin, desired_player.velocity, movement_.tick_interval_seconds);
            local_player_ = desired_player;
            local_player_.origin = resolved.origin;
            local_player_.velocity = resolved.velocity;
            local_player_.on_ground = resolved.on_ground;
            update_camera(engine);
            update_hud(engine);
            return;
        }

        local_player_ = desired_player;
        if (const std::optional<float> post_move_floor_z = find_floor_z(*world, local_player_.origin, 512.0F);
            post_move_floor_z && local_player_.velocity.z <= 0.0F && local_player_.origin.z <= *post_move_floor_z + 2.0F)
        {
            local_player_.origin.z = *post_move_floor_z;
            local_player_.velocity.z = 0.0F;
            local_player_.on_ground = true;
        }
        update_camera(engine);
        update_hud(engine);
        return;
    }

    simulate_player_move(local_player_, input_, movement_);
    engine.camera.active = false;
    update_hud(engine);
}

void GameSimulation::on_frame(const FrameContext& context, EngineContext& engine)
{
    update_camera(engine);
    update_hud(engine);
    if (context.frame_index == 0)
    {
        log_info("player origin=({}, {}, {})", local_player_.origin.x, local_player_.origin.y, local_player_.origin.z);
    }
}

const PlayerState& GameSimulation::local_player() const
{
    return local_player_;
}

void GameSimulation::sync_world(EngineContext& engine)
{
    const std::uint64_t world_generation = engine.world.generation();
    if (world_generation == observed_world_generation_)
    {
        return;
    }

    observed_world_generation_ = world_generation;
    const LoadedWorld* world = engine.world.current_world();
    if (world == nullptr)
    {
        physics_.clear_character();
        physics_.clear_static_world();
        engine.teams.reset_for_new_world();
        engine.camera.active = false;
        return;
    }

    physics_.clear_character();
    physics_.clear_static_world();
    local_player_ = {};
    local_player_.on_ground = false;
    engine.teams.reset_for_new_world();
    engine.teams.request_team_menu(local_team_connection_id(engine.network));

    if (physics_.load_static_world(*world))
    {
    }
    else
    {
        log_warning("falling back to legacy floor probing for world '{}'", world->name);
    }

    log_info("game simulation entered world '{}'", world->name);
    update_camera(engine);
}

void GameSimulation::sync_team_state(EngineContext& engine)
{
    const std::uint64_t team_revision = engine.teams.revision();
    if (team_revision == observed_team_revision_)
    {
        return;
    }

    observed_team_revision_ = team_revision;
    const LoadedWorld* world = engine.world.current_world();
    const TeamPlayerState* team_player = engine.teams.find_player(local_team_connection_id(engine.network));
    if (world == nullptr || team_player == nullptr || !team_player->alive || !is_playing_team(team_player->current_team))
    {
        physics_.clear_character();
        engine.camera.active = false;
        return;
    }

    if (const std::optional<WorldSpawnPoint> spawn =
            engine.teams.select_spawn_point(*world, team_player->current_team, team_player->connection_id))
    {
        local_player_ = {};
        local_player_.origin = spawn->origin;
        local_player_.velocity = {};
        local_player_.on_ground = true;
        fps_view_.pitch_degrees = spawn->angles.x;
        fps_view_.yaw_degrees = spawn->angles.y;
        if (const std::optional<float> floor_z = find_floor_z(*world, local_player_.origin, 512.0F))
        {
            local_player_.origin.z = *floor_z;
        }
        if (physics_.has_static_world())
        {
            physics_.reset_character(local_player_.origin, local_player_.velocity);
        }
        log_info("spawned local player on {} at ({}, {}, {})",
            team_name(team_player->current_team),
            local_player_.origin.x,
            local_player_.origin.y,
            local_player_.origin.z);
    }

    update_camera(engine);
}

void GameSimulation::update_camera(EngineContext& engine) const
{
    if (engine.world.current_world() == nullptr || !local_player_active(engine))
    {
        engine.camera.active = false;
        return;
    }

    engine.camera.active = true;
    engine.camera.origin = fps_eye_origin(local_player_, fps_settings_);
    engine.camera.pitch_degrees = fps_view_.pitch_degrees;
    engine.camera.yaw_degrees = fps_view_.yaw_degrees;
}

void GameSimulation::update_hud(EngineContext& engine) const
{
    const LoadedWorld* world = engine.world.current_world();
    const TeamPlayerState* team_player = engine.teams.find_player(local_team_connection_id(engine.network));
    const int current_team = team_player != nullptr ? team_player->current_team : TEAM_UNASSIGNED;
    const TeamInfo* terrorist_team = engine.teams.find_team(TEAM_TERRORIST);
    const TeamInfo* ct_team = engine.teams.find_team(TEAM_CT);

    HudState next;
    next.visible = world != nullptr;
    next.alive = team_player != nullptr && team_player->alive && is_playing_team(current_team);
    next.grounded = local_player_.on_ground;
    next.health = next.alive ? 100 : 0;
    next.max_health = 100;
    next.armor = 0;
    next.money = 800;
    next.ammo_in_clip = 12;
    next.reserve_ammo = 24;
    next.kills = team_player != nullptr ? team_player->kills : 0;
    next.deaths = team_player != nullptr ? team_player->deaths : 0;
    next.counter_terrorist_score = ct_team != nullptr ? ct_team->score_total : 0;
    next.terrorist_score = terrorist_team != nullptr ? terrorist_team->score_total : 0;
    next.ping_ms = team_player != nullptr ? team_player->ping_ms : 0;
    next.team_name = std::string(team_name(current_team));
    next.weapon_name = "USP-S";
    next.round_phase = world != nullptr ? world->name : "MENU";
    if (world != nullptr && current_team == TEAM_UNASSIGNED)
    {
        next.round_phase = "CHOOSE TEAM";
    }
    else if (world != nullptr && current_team == TEAM_SPECTATOR)
    {
        next.round_phase = "SPECTATING";
    }
    next.round_time_seconds = 115.0;
    next.speed = local_player_.velocity.length_2d();
    next.crosshair_gap = std::clamp(10.0F + (next.speed * 0.04F), 12.0F, 28.0F);

    const NetworkClient& client = engine.network.client();
    const NetworkServer& server = engine.network.server();
    if (client.is_connected())
    {
        next.connected = true;
        next.network_label = client.remote_address().to_string();
    }
    else if (server.is_running())
    {
        next.connected = !server.clients().empty();
        next.network_label = "Listen " + std::to_string(server.local_port());
    }
    else
    {
        next.network_label = "Local";
    }

    next.revision = engine.hud.revision + 1;
    engine.hud = next;
}

bool GameSimulation::local_player_active(EngineContext& engine) const
{
    const TeamPlayerState* player = engine.teams.find_player(local_team_connection_id(engine.network));
    return player != nullptr && player->alive && is_playing_team(player->current_team);
}
}
