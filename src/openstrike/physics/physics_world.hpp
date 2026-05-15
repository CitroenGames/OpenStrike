#pragma once

#include "openstrike/core/math.hpp"

#include <memory>

namespace openstrike
{
struct LoadedWorld;

struct PhysicsCharacterConfig
{
    // Source-facing distances are in Source units. The Jolt backend converts them to meters internally.
    float radius = 16.0F;
    float height = 72.0F;
    float max_step_height = 18.0F;
    float max_slope_degrees = 45.573F;
    float max_teleport_distance = 24.0F;
    // Jolt contact tolerance, in meters, matching the VPhysics-Jolt convention.
    float character_padding = 0.02F;
};

struct PhysicsCharacterState
{
    Vec3 origin;
    Vec3 velocity;
    bool on_ground = false;
};

class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    [[nodiscard]] bool load_static_world(const LoadedWorld& world);
    void clear_static_world();
    [[nodiscard]] bool has_static_world() const;

    void reset_character(Vec3 origin, Vec3 velocity = {}, const PhysicsCharacterConfig& config = {});
    void clear_character();
    [[nodiscard]] bool has_character() const;
    [[nodiscard]] PhysicsCharacterState character_state() const;

    [[nodiscard]] PhysicsCharacterState move_character_to(Vec3 target_origin, Vec3 target_velocity, float delta_seconds);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
