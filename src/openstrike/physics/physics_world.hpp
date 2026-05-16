#pragma once

#include "openstrike/core/math.hpp"

#include <cstdint>
#include <memory>

namespace openstrike
{
struct LoadedWorld;

namespace PhysicsContents
{
inline constexpr std::uint32_t Empty = 0;
inline constexpr std::uint32_t Solid = 0x00000001;
inline constexpr std::uint32_t Window = 0x00000002;
inline constexpr std::uint32_t Grate = 0x00000008;
inline constexpr std::uint32_t Water = 0x00000020;
inline constexpr std::uint32_t Moveable = 0x00004000;
inline constexpr std::uint32_t PlayerClip = 0x00010000;
inline constexpr std::uint32_t MonsterClip = 0x00020000;
inline constexpr std::uint32_t Monster = 0x02000000;

inline constexpr std::uint32_t MaskSolid = Solid | Moveable | Window | Monster | Grate;
inline constexpr std::uint32_t MaskPlayerSolid = MaskSolid | PlayerClip;
inline constexpr std::uint32_t MaskWorldStatic = MaskPlayerSolid | MonsterClip;
inline constexpr std::uint32_t MaskAll = 0xFFFFFFFFU;
}

enum class PhysicsObjectLayer : std::uint8_t
{
    NonMovingWorld,
    NonMovingObject,
    Moving,
    NoCollide,
    Debris
};

[[nodiscard]] bool physics_layers_should_collide(PhysicsObjectLayer first, PhysicsObjectLayer second);

struct PhysicsMaterial
{
    float density = 2000.0F;
    float friction = 0.8F;
    float elasticity = 0.25F;
    char game_material = '\0';
};

enum class PhysicsBodyShape
{
    Box,
    Sphere
};

struct PhysicsBodyHandle
{
    std::uint32_t value = 0xFFFFFFFFU;

    [[nodiscard]] bool valid() const
    {
        return value != 0xFFFFFFFFU;
    }
};

struct PhysicsBodyDesc
{
    PhysicsBodyShape shape = PhysicsBodyShape::Box;
    PhysicsObjectLayer layer = PhysicsObjectLayer::Moving;
    Vec3 origin;
    Vec3 velocity;
    Vec3 half_extents{16.0F, 16.0F, 16.0F};
    float radius = 16.0F;
    float mass = 10.0F;
    std::uint32_t contents = PhysicsContents::Solid;
    PhysicsMaterial material;
    bool dynamic = true;
    bool sensor = false;
    bool enhanced_internal_edge_removal = true;
};

struct PhysicsBodyState
{
    Vec3 origin;
    Vec3 velocity;
    PhysicsObjectLayer layer = PhysicsObjectLayer::NoCollide;
    std::uint32_t contents = PhysicsContents::Empty;
    bool active = false;
};

struct PhysicsTraceDesc
{
    Vec3 start;
    Vec3 end;
    Vec3 mins;
    Vec3 maxs;
    PhysicsObjectLayer query_layer = PhysicsObjectLayer::Moving;
    std::uint32_t contents_mask = PhysicsContents::MaskPlayerSolid;
    float collision_tolerance = 0.025F;
    bool collide_with_backfaces = true;
};

struct PhysicsTraceResult
{
    Vec3 startpos;
    Vec3 endpos;
    Vec3 normal;
    PhysicsBodyHandle body;
    PhysicsObjectLayer layer = PhysicsObjectLayer::NoCollide;
    std::uint32_t contents = PhysicsContents::Empty;
    std::uint32_t surface_flags = 0;
    float fraction = 1.0F;
    bool hit = false;
    bool start_solid = false;
    bool all_solid = false;
};

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

    [[nodiscard]] PhysicsBodyHandle create_body(const PhysicsBodyDesc& desc);
    void destroy_body(PhysicsBodyHandle handle);
    [[nodiscard]] bool set_body_layer(PhysicsBodyHandle handle, PhysicsObjectLayer layer);
    [[nodiscard]] bool set_body_velocity(PhysicsBodyHandle handle, Vec3 velocity);
    [[nodiscard]] PhysicsBodyState body_state(PhysicsBodyHandle handle) const;
    void step_simulation(float delta_seconds, int collision_sub_steps = 1);

    [[nodiscard]] PhysicsTraceResult trace_ray(Vec3 start, Vec3 end, std::uint32_t contents_mask = PhysicsContents::MaskPlayerSolid) const;
    [[nodiscard]] PhysicsTraceResult trace_box(const PhysicsTraceDesc& desc) const;

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
