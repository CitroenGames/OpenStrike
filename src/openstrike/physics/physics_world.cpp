#include "openstrike/physics/physics_world.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/world/world.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace openstrike
{
namespace
{
namespace PhysicsLayers
{
constexpr JPH::ObjectLayer NonMovingWorld = 0;
constexpr JPH::ObjectLayer NonMovingObject = 1;
constexpr JPH::ObjectLayer Moving = 2;
constexpr JPH::ObjectLayer NoCollide = 3;
constexpr JPH::ObjectLayer Debris = 4;
constexpr JPH::ObjectLayer Count = 5;
}

namespace BroadPhaseLayers
{
constexpr JPH::BroadPhaseLayer NonMovingWorld(0);
constexpr JPH::BroadPhaseLayer NonMovingObject(1);
constexpr JPH::BroadPhaseLayer Moving(2);
constexpr JPH::BroadPhaseLayer NoCollide(3);
constexpr JPH::BroadPhaseLayer Debris(4);
constexpr JPH::uint Count = 5;
}

constexpr float kSourceUnitsToMeters = 0.0254F;
constexpr float kMetersToSourceUnits = 1.0F / kSourceUnitsToMeters;
constexpr float kMaxConvexRadius = 0.25F * kSourceUnitsToMeters;

bool triangle_blocks_player_movement(const WorldTriangle& triangle)
{
    return triangle.contents == 0 || (triangle.contents & PhysicsContents::MaskPlayerSolid) != 0;
}

JPH::ObjectLayer to_jolt_layer(PhysicsObjectLayer layer)
{
    switch (layer)
    {
    case PhysicsObjectLayer::NonMovingWorld:
        return PhysicsLayers::NonMovingWorld;
    case PhysicsObjectLayer::NonMovingObject:
        return PhysicsLayers::NonMovingObject;
    case PhysicsObjectLayer::Moving:
        return PhysicsLayers::Moving;
    case PhysicsObjectLayer::NoCollide:
        return PhysicsLayers::NoCollide;
    case PhysicsObjectLayer::Debris:
        return PhysicsLayers::Debris;
    }

    return PhysicsLayers::NoCollide;
}

PhysicsObjectLayer from_jolt_layer(JPH::ObjectLayer layer)
{
    switch (layer)
    {
    case PhysicsLayers::NonMovingWorld:
        return PhysicsObjectLayer::NonMovingWorld;
    case PhysicsLayers::NonMovingObject:
        return PhysicsObjectLayer::NonMovingObject;
    case PhysicsLayers::Moving:
        return PhysicsObjectLayer::Moving;
    case PhysicsLayers::NoCollide:
        return PhysicsObjectLayer::NoCollide;
    case PhysicsLayers::Debris:
        return PhysicsObjectLayer::Debris;
    default:
        return PhysicsObjectLayer::NoCollide;
    }
}

JPH::EMotionType to_jolt_motion_type(PhysicsBodyMotionType motion_type)
{
    switch (motion_type)
    {
    case PhysicsBodyMotionType::Static:
        return JPH::EMotionType::Static;
    case PhysicsBodyMotionType::Kinematic:
        return JPH::EMotionType::Kinematic;
    case PhysicsBodyMotionType::Dynamic:
        return JPH::EMotionType::Dynamic;
    }

    return JPH::EMotionType::Static;
}

PhysicsBodyMotionType from_jolt_motion_type(JPH::EMotionType motion_type)
{
    switch (motion_type)
    {
    case JPH::EMotionType::Static:
        return PhysicsBodyMotionType::Static;
    case JPH::EMotionType::Kinematic:
        return PhysicsBodyMotionType::Kinematic;
    case JPH::EMotionType::Dynamic:
        return PhysicsBodyMotionType::Dynamic;
    }

    return PhysicsBodyMotionType::Static;
}

PhysicsBodyMotionType resolve_motion_type(const PhysicsBodyDesc& desc)
{
    if (!desc.dynamic || desc.layer == PhysicsObjectLayer::NonMovingWorld || desc.layer == PhysicsObjectLayer::NonMovingObject)
    {
        return PhysicsBodyMotionType::Static;
    }

    return desc.motion_type;
}

bool layers_should_collide(JPH::ObjectLayer first, JPH::ObjectLayer second)
{
    switch (first)
    {
    case PhysicsLayers::NoCollide:
        return false;
    case PhysicsLayers::NonMovingWorld:
    case PhysicsLayers::NonMovingObject:
        return second == PhysicsLayers::Moving || second == PhysicsLayers::Debris;
    case PhysicsLayers::Moving:
        return second == PhysicsLayers::Moving || second == PhysicsLayers::NonMovingWorld || second == PhysicsLayers::NonMovingObject;
    case PhysicsLayers::Debris:
        return second == PhysicsLayers::NonMovingWorld || second == PhysicsLayers::NonMovingObject;
    default:
        return false;
    }
}

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
    {
        return layers_should_collide(first, second);
    }
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    BroadPhaseLayerInterface()
    {
        object_to_broad_phase_[PhysicsLayers::NonMovingWorld] = BroadPhaseLayers::NonMovingWorld;
        object_to_broad_phase_[PhysicsLayers::NonMovingObject] = BroadPhaseLayers::NonMovingObject;
        object_to_broad_phase_[PhysicsLayers::Moving] = BroadPhaseLayers::Moving;
        object_to_broad_phase_[PhysicsLayers::NoCollide] = BroadPhaseLayers::NoCollide;
        object_to_broad_phase_[PhysicsLayers::Debris] = BroadPhaseLayers::Debris;
    }

    JPH::uint GetNumBroadPhaseLayers() const override
    {
        return BroadPhaseLayers::Count;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return object_to_broad_phase_[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(layer))
        {
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::NonMovingWorld):
            return "NonMovingWorld";
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::NonMovingObject):
            return "NonMovingObject";
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::Moving):
            return "Moving";
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::NoCollide):
            return "NoCollide";
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::Debris):
            return "Debris";
        default:
            return "Invalid";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer object_to_broad_phase_[PhysicsLayers::Count];
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase_layer) const override
    {
        switch (layer)
        {
        case PhysicsLayers::NoCollide:
            return false;
        case PhysicsLayers::NonMovingWorld:
        case PhysicsLayers::NonMovingObject:
            return broad_phase_layer == BroadPhaseLayers::Moving || broad_phase_layer == BroadPhaseLayers::Debris;
        case PhysicsLayers::Moving:
            return broad_phase_layer == BroadPhaseLayers::Moving || broad_phase_layer == BroadPhaseLayers::NonMovingWorld ||
                   broad_phase_layer == BroadPhaseLayers::NonMovingObject;
        case PhysicsLayers::Debris:
            return broad_phase_layer == BroadPhaseLayers::NonMovingWorld || broad_phase_layer == BroadPhaseLayers::NonMovingObject;
        default:
            return false;
        }
    }
};

void jolt_trace(const char* format, ...)
{
    char buffer[1024]{};

    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    log_info("jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool jolt_assert_failed(const char* expression, const char* message, const char* file, JPH::uint line)
{
    log_error("jolt assert {}:{}: ({}) {}", file, line, expression != nullptr ? expression : "", message != nullptr ? message : "");
    return true;
}
#endif

void initialize_jolt()
{
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = jolt_trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert_failed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

float to_jolt_distance(float value)
{
    return value * kSourceUnitsToMeters;
}

float from_jolt_distance(float value)
{
    return value * kMetersToSourceUnits;
}

JPH::Vec3 to_jolt_distance(Vec3 value)
{
    return JPH::Vec3(to_jolt_distance(value.x), to_jolt_distance(value.y), to_jolt_distance(value.z));
}

JPH::RVec3 to_jolt_position(Vec3 value)
{
    return JPH::RVec3(to_jolt_distance(value.x), to_jolt_distance(value.y), to_jolt_distance(value.z));
}

JPH::Float3 to_jolt_distance_float3(Vec3 value)
{
    return JPH::Float3(to_jolt_distance(value.x), to_jolt_distance(value.y), to_jolt_distance(value.z));
}

Vec3 from_jolt_distance(JPH::Vec3Arg value)
{
    return {from_jolt_distance(value.GetX()), from_jolt_distance(value.GetY()), from_jolt_distance(value.GetZ())};
}

Vec3 from_jolt_position(JPH::RVec3Arg value)
{
    return {from_jolt_distance(static_cast<float>(value.GetX())),
        from_jolt_distance(static_cast<float>(value.GetY())),
        from_jolt_distance(static_cast<float>(value.GetZ()))};
}

float length_squared(Vec3 value)
{
    return (value.x * value.x) + (value.y * value.y) + (value.z * value.z);
}

struct PhysicsBodyMetadata
{
    std::uint32_t contents = PhysicsContents::Solid;
    std::uint32_t surface_flags = 0;
    PhysicsBodyMotionType motion_type = PhysicsBodyMotionType::Static;
    PhysicsMaterial material;
};

struct StaticTriangleMetadata
{
    std::uint32_t contents = PhysicsContents::Solid;
    std::uint32_t surface_flags = 0;
};

JPH::Triangle make_triangle(Vec3 a, Vec3 b, Vec3 c, std::uint32_t user_data = 0)
{
    return JPH::Triangle(to_jolt_distance_float3(a), to_jolt_distance_float3(b), to_jolt_distance_float3(c), 0, user_data);
}

std::uint32_t contents_for_shape(
    const JPH::Shape* shape,
    const JPH::SubShapeID& sub_shape_id,
    const std::vector<StaticTriangleMetadata>* triangle_metadata)
{
    if (shape != nullptr && shape->GetSubType() == JPH::EShapeSubType::Mesh && triangle_metadata != nullptr)
    {
        const auto* mesh_shape = static_cast<const JPH::MeshShape*>(shape);
        const std::uint32_t user_data = mesh_shape->GetTriangleUserData(sub_shape_id);
        if (user_data > 0 && user_data <= triangle_metadata->size())
        {
            return (*triangle_metadata)[static_cast<std::size_t>(user_data - 1U)].contents;
        }
    }

    if (shape != nullptr && shape->GetUserData() != 0)
    {
        return static_cast<std::uint32_t>(shape->GetUserData());
    }

    return PhysicsContents::Solid;
}

std::uint32_t surface_flags_for_shape(
    const JPH::Shape* shape,
    const JPH::SubShapeID& sub_shape_id,
    const std::vector<StaticTriangleMetadata>* triangle_metadata,
    const PhysicsBodyMetadata* body_metadata)
{
    if (shape != nullptr && shape->GetSubType() == JPH::EShapeSubType::Mesh && triangle_metadata != nullptr)
    {
        const auto* mesh_shape = static_cast<const JPH::MeshShape*>(shape);
        const std::uint32_t user_data = mesh_shape->GetTriangleUserData(sub_shape_id);
        if (user_data > 0 && user_data <= triangle_metadata->size())
        {
            return (*triangle_metadata)[static_cast<std::size_t>(user_data - 1U)].surface_flags;
        }
    }

    return body_metadata != nullptr ? body_metadata->surface_flags : 0;
}

class ContentsShapeFilter final : public JPH::ShapeFilter
{
public:
    ContentsShapeFilter(std::uint32_t contents_mask, const std::vector<StaticTriangleMetadata>* triangle_metadata)
        : contents_mask_(contents_mask)
        , triangle_metadata_(triangle_metadata)
    {
    }

    bool ShouldCollide(const JPH::Shape* shape, const JPH::SubShapeID& sub_shape_id) const override
    {
        if (shape != nullptr && shape->GetSubType() == JPH::EShapeSubType::Mesh)
        {
            return true;
        }

        return (contents_for_shape(shape, sub_shape_id, triangle_metadata_) & contents_mask_) != 0;
    }

    bool ShouldCollide(
        const JPH::Shape*,
        const JPH::SubShapeID&,
        const JPH::Shape* shape2,
        const JPH::SubShapeID& sub_shape_id2) const override
    {
        return ShouldCollide(shape2, sub_shape_id2);
    }

private:
    std::uint32_t contents_mask_ = PhysicsContents::MaskAll;
    const std::vector<StaticTriangleMetadata>* triangle_metadata_ = nullptr;
};

float trace_fraction(float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

Vec3 unitless_from_jolt(JPH::Vec3Arg value)
{
    return {value.GetX(), value.GetY(), value.GetZ()};
}

Vec3 trace_end(Vec3 start, Vec3 end, float fraction)
{
    return start + ((end - start) * trace_fraction(fraction));
}

Vec3 normal_from_penetration_axis(JPH::Vec3Arg axis)
{
    if (axis.LengthSq() <= 0.000001F)
    {
        return {};
    }

    return unitless_from_jolt(-axis.Normalized());
}

bool has_box_extents(Vec3 mins, Vec3 maxs)
{
    const Vec3 extents = maxs - mins;
    return std::fabs(extents.x) > 0.001F || std::fabs(extents.y) > 0.001F || std::fabs(extents.z) > 0.001F;
}

PhysicsWorldSettings sanitize_settings(PhysicsWorldSettings settings)
{
    settings.max_bodies = std::max<std::uint32_t>(settings.max_bodies, 1);
    settings.max_body_pairs = std::max<std::uint32_t>(settings.max_body_pairs, 1);
    settings.max_contact_constraints = std::max<std::uint32_t>(settings.max_contact_constraints, 1);
    settings.temp_allocator_size_bytes = std::max<std::size_t>(settings.temp_allocator_size_bytes, 1024 * 1024);
    settings.default_collision_sub_steps = std::clamp(settings.default_collision_sub_steps, 1, 8);
    return settings;
}
}

bool physics_layers_should_collide(PhysicsObjectLayer first, PhysicsObjectLayer second)
{
    return layers_should_collide(to_jolt_layer(first), to_jolt_layer(second));
}

class PhysicsWorld::Impl
{
public:
    explicit Impl(PhysicsWorldSettings settings)
        : settings_(sanitize_settings(settings))
        , temp_allocator_(static_cast<int>(settings_.temp_allocator_size_bytes))
        , job_system_(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(settings_.worker_threads))
    {
        system_.Init(settings_.max_bodies,
            settings_.max_body_mutexes,
            settings_.max_body_pairs,
            settings_.max_contact_constraints,
            broad_phase_layers_,
            object_vs_broad_phase_filter_,
            object_layer_pair_filter_);
        system_.SetGravity(to_jolt_distance(settings_.gravity));
        character_config_ = settings_.default_character;
        system_.SetCombineFriction([](const JPH::Body& first, const JPH::SubShapeID&, const JPH::Body& second, const JPH::SubShapeID&) {
            return std::sqrt(std::max(first.GetFriction(), 0.0F) * std::max(second.GetFriction(), 0.0F));
        });
    }

    ~Impl()
    {
        clear_character();
        clear_static_world();
        for (const auto& entry : body_metadata_)
        {
            const JPH::BodyID body_id(entry.first);
            JPH::BodyInterface& bodies = system_.GetBodyInterface();
            if (!body_id.IsInvalid())
            {
                bodies.RemoveBody(body_id);
                bodies.DestroyBody(body_id);
            }
        }
        body_metadata_.clear();
    }

    const PhysicsWorldSettings& settings() const
    {
        return settings_;
    }

    void set_gravity(Vec3 gravity)
    {
        settings_.gravity = gravity;
        system_.SetGravity(to_jolt_distance(settings_.gravity));
    }

    Vec3 gravity() const
    {
        return settings_.gravity;
    }

    bool load_static_world(const LoadedWorld& world)
    {
        clear_static_world();

        if (world.mesh.collision_triangles.empty())
        {
            return true;
        }

        JPH::TriangleList triangles;
        triangles.reserve(world.mesh.collision_triangles.size() * 2);
        static_triangle_metadata_.clear();
        static_triangle_metadata_.reserve(world.mesh.collision_triangles.size() * 2);
        for (const WorldTriangle& triangle : world.mesh.collision_triangles)
        {
            if (!triangle_blocks_player_movement(triangle))
            {
                continue;
            }

            static_triangle_metadata_.push_back(StaticTriangleMetadata{
                .contents = triangle.contents != 0 ? triangle.contents : PhysicsContents::Solid,
                .surface_flags = triangle.surface_flags,
            });
            triangles.push_back(make_triangle(triangle.points[0],
                triangle.points[1],
                triangle.points[2],
                static_cast<std::uint32_t>(static_triangle_metadata_.size())));
            static_triangle_metadata_.push_back(StaticTriangleMetadata{
                .contents = triangle.contents != 0 ? triangle.contents : PhysicsContents::Solid,
                .surface_flags = triangle.surface_flags,
            });
            triangles.push_back(make_triangle(triangle.points[2],
                triangle.points[1],
                triangle.points[0],
                static_cast<std::uint32_t>(static_triangle_metadata_.size())));
        }
        if (triangles.empty())
        {
            static_triangle_metadata_.clear();
            return true;
        }

        JPH::MeshShapeSettings shape_settings(triangles);
        shape_settings.mPerTriangleUserData = true;
        shape_settings.mActiveEdgeCosThresholdAngle = -1.0F;
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (shape_result.HasError())
        {
            log_error("failed to create physics mesh for world '{}': {}", world.name, shape_result.GetError().c_str());
            return false;
        }

        JPH::Ref<JPH::Shape> static_shape = shape_result.Get();
        static_shape->SetUserData(PhysicsContents::Solid);
        static_shape_ = static_shape;

        JPH::BodyCreationSettings body_settings(static_shape_,
            JPH::RVec3::sZero(),
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            PhysicsLayers::NonMovingWorld);
        body_settings.mEnhancedInternalEdgeRemoval = true;
        body_settings.mFriction = std::max(settings_.static_world_material.friction, 0.0F);
        body_settings.mRestitution = std::max(settings_.static_world_material.elasticity, 0.0F);

        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        JPH::Body* static_body = bodies.CreateBody(body_settings);
        if (static_body == nullptr)
        {
            static_shape_ = nullptr;
            static_triangle_metadata_.clear();
            log_error("failed to create physics mesh body for world '{}'", world.name);
            return false;
        }

        static_body_id_ = static_body->GetID();
        static_body->SetUserData(static_body_id_.GetIndexAndSequenceNumber());
        bodies.AddBody(static_body_id_, JPH::EActivation::DontActivate);
        has_static_body_ = !static_body_id_.IsInvalid();
        if (!has_static_body_)
        {
            static_shape_ = nullptr;
            static_triangle_metadata_.clear();
            log_error("failed to add physics mesh body for world '{}'", world.name);
            return false;
        }

        body_metadata_[static_body_id_.GetIndexAndSequenceNumber()] = PhysicsBodyMetadata{
            .contents = PhysicsContents::Solid,
            .surface_flags = 0,
            .motion_type = PhysicsBodyMotionType::Static,
            .material = settings_.static_world_material,
        };
        broad_phase_optimized_ = false;
        return true;
    }

    void clear_static_world()
    {
        if (has_static_body_)
        {
            JPH::BodyInterface& bodies = system_.GetBodyInterface();
            bodies.RemoveBody(static_body_id_);
            bodies.DestroyBody(static_body_id_);
            body_metadata_.erase(static_body_id_.GetIndexAndSequenceNumber());
            static_body_id_ = JPH::BodyID();
            has_static_body_ = false;
        }

        static_shape_ = nullptr;
        static_triangle_metadata_.clear();
        broad_phase_optimized_ = false;
    }

    bool has_static_world() const
    {
        return has_static_body_;
    }

    PhysicsBodyHandle create_body(const PhysicsBodyDesc& desc)
    {
        JPH::Ref<JPH::Shape> shape = create_body_shape(desc);
        if (shape == nullptr)
        {
            return {};
        }

        const PhysicsBodyMotionType motion_type = resolve_motion_type(desc);
        JPH::BodyCreationSettings body_settings(shape,
            to_jolt_position(desc.origin),
            JPH::Quat::sIdentity(),
            to_jolt_motion_type(motion_type),
            to_jolt_layer(desc.layer));
        body_settings.mLinearVelocity = to_jolt_distance(desc.velocity);
        body_settings.mMotionQuality = motion_type == PhysicsBodyMotionType::Dynamic ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
        body_settings.mEnhancedInternalEdgeRemoval = desc.enhanced_internal_edge_removal;
        body_settings.mFriction = std::max(desc.material.friction, 0.0F);
        body_settings.mRestitution = std::max(desc.material.elasticity, 0.0F);
        body_settings.mIsSensor = desc.sensor;
        body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        body_settings.mMassPropertiesOverride.mMass = std::max(desc.mass, 0.001F);

        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        JPH::Body* body = bodies.CreateBody(body_settings);
        if (body == nullptr)
        {
            return {};
        }

        const JPH::BodyID body_id = body->GetID();
        PhysicsBodyHandle handle{body_id.GetIndexAndSequenceNumber()};
        body->SetUserData(handle.value);
        body_metadata_[handle.value] = PhysicsBodyMetadata{
            .contents = desc.contents != 0 ? desc.contents : PhysicsContents::Solid,
            .surface_flags = 0,
            .motion_type = motion_type,
            .material = desc.material,
        };
        bodies.AddBody(body_id, motion_type == PhysicsBodyMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        broad_phase_optimized_ = false;
        return handle;
    }

    void destroy_body(PhysicsBodyHandle handle)
    {
        if (!handle.valid())
        {
            return;
        }

        const JPH::BodyID body_id(handle.value);
        if (has_static_body_ && body_id == static_body_id_)
        {
            clear_static_world();
            return;
        }

        auto metadata = body_metadata_.find(handle.value);
        if (metadata == body_metadata_.end())
        {
            return;
        }

        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        if (bodies.IsAdded(body_id))
        {
            bodies.RemoveBody(body_id);
        }
        bodies.DestroyBody(body_id);
        body_metadata_.erase(metadata);
        broad_phase_optimized_ = false;
    }

    bool set_body_layer(PhysicsBodyHandle handle, PhysicsObjectLayer layer)
    {
        if (!handle.valid() || body_metadata_.find(handle.value) == body_metadata_.end())
        {
            return false;
        }

        const JPH::BodyID body_id(handle.value);
        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        bodies.SetObjectLayer(body_id, to_jolt_layer(layer));
        if (layer != PhysicsObjectLayer::NoCollide)
        {
            bodies.ActivateBody(body_id);
        }
        broad_phase_optimized_ = false;
        return true;
    }

    bool set_body_origin(PhysicsBodyHandle handle, Vec3 origin, bool activate)
    {
        if (!handle.valid() || body_metadata_.find(handle.value) == body_metadata_.end())
        {
            return false;
        }

        const JPH::BodyID body_id(handle.value);
        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        bodies.SetPosition(body_id, to_jolt_position(origin), activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        broad_phase_optimized_ = false;
        return true;
    }

    bool set_body_velocity(PhysicsBodyHandle handle, Vec3 velocity)
    {
        if (!handle.valid() || body_metadata_.find(handle.value) == body_metadata_.end())
        {
            return false;
        }

        const JPH::BodyID body_id(handle.value);
        {
            JPH::BodyLockRead lock(system_.GetBodyLockInterface(), body_id);
            if (!lock.Succeeded() || lock.GetBody().GetMotionType() == JPH::EMotionType::Static)
            {
                return false;
            }
        }

        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        bodies.SetLinearVelocity(body_id, to_jolt_distance(velocity));
        bodies.ActivateBody(body_id);
        return true;
    }

    bool move_kinematic_body(PhysicsBodyHandle handle, Vec3 target_origin, float delta_seconds)
    {
        if (!handle.valid() || body_metadata_.find(handle.value) == body_metadata_.end())
        {
            return false;
        }

        const JPH::BodyID body_id(handle.value);
        {
            JPH::BodyLockRead lock(system_.GetBodyLockInterface(), body_id);
            if (!lock.Succeeded() || lock.GetBody().GetMotionType() != JPH::EMotionType::Kinematic)
            {
                return false;
            }
        }

        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        bodies.MoveKinematic(body_id, to_jolt_position(target_origin), JPH::Quat::sIdentity(), std::max(delta_seconds, 0.0001F));
        return true;
    }

    bool apply_body_impulse(PhysicsBodyHandle handle, Vec3 impulse)
    {
        if (!handle.valid() || body_metadata_.find(handle.value) == body_metadata_.end())
        {
            return false;
        }

        const JPH::BodyID body_id(handle.value);
        {
            JPH::BodyLockRead lock(system_.GetBodyLockInterface(), body_id);
            if (!lock.Succeeded() || lock.GetBody().GetMotionType() != JPH::EMotionType::Dynamic)
            {
                return false;
            }
        }

        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        bodies.AddImpulse(body_id, to_jolt_distance(impulse));
        return true;
    }

    PhysicsBodyState body_state(PhysicsBodyHandle handle) const
    {
        auto metadata = body_metadata_.find(handle.value);
        if (!handle.valid() || metadata == body_metadata_.end())
        {
            return {};
        }

        const JPH::BodyID body_id(handle.value);
        JPH::BodyLockRead lock(system_.GetBodyLockInterface(), body_id);
        if (!lock.Succeeded())
        {
            return {};
        }

        const JPH::Body& body = lock.GetBody();
        PhysicsBodyState state;
        state.origin = from_jolt_position(body.GetPosition());
        state.velocity = from_jolt_distance(body.GetLinearVelocity());
        state.layer = from_jolt_layer(body.GetObjectLayer());
        state.motion_type = from_jolt_motion_type(body.GetMotionType());
        state.contents = metadata->second.contents;
        state.active = body.IsActive();
        return state;
    }

    void step_simulation(float delta_seconds, int collision_sub_steps)
    {
        if (delta_seconds <= 0.0F)
        {
            return;
        }

        if (!broad_phase_optimized_)
        {
            system_.OptimizeBroadPhase();
            broad_phase_optimized_ = true;
        }

        const int sub_steps = collision_sub_steps > 0 ? collision_sub_steps : settings_.default_collision_sub_steps;
        system_.Update(delta_seconds, std::clamp(sub_steps, 1, 8), &temp_allocator_, &job_system_);
    }

    PhysicsTraceResult trace_ray(Vec3 start, Vec3 end, std::uint32_t contents_mask) const
    {
        PhysicsTraceDesc desc;
        desc.start = start;
        desc.end = end;
        desc.contents_mask = contents_mask;
        return trace_box(desc);
    }

    PhysicsTraceResult trace_box(const PhysicsTraceDesc& desc) const
    {
        PhysicsTraceResult result;
        result.startpos = desc.start;
        result.endpos = desc.end;

        if (desc.contents_mask == PhysicsContents::Empty)
        {
            return result;
        }

        const JPH::ObjectLayer query_layer = to_jolt_layer(desc.query_layer);
        const ContentsShapeFilter shape_filter(desc.contents_mask, &static_triangle_metadata_);
        const Vec3 delta = desc.end - desc.start;
        const JPH::EBackFaceMode backface_mode =
            desc.collide_with_backfaces ? JPH::EBackFaceMode::CollideWithBackFaces : JPH::EBackFaceMode::IgnoreBackFaces;

        if (!has_box_extents(desc.mins, desc.maxs))
        {
            if (length_squared(delta) <= 0.000001F)
            {
                return result;
            }

            JPH::RayCastSettings settings;
            settings.SetBackFaceMode(backface_mode);
            settings.mTreatConvexAsSolid = true;

            JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
            const JPH::RRayCast ray(to_jolt_position(desc.start), to_jolt_distance(delta));
            system_.GetNarrowPhaseQuery().CastRay(ray,
                settings,
                collector,
                system_.GetDefaultBroadPhaseLayerFilter(query_layer),
                system_.GetDefaultLayerFilter(query_layer),
                {},
                shape_filter);
            collector.Sort();

            for (const JPH::RayCastResult& hit : collector.mHits)
            {
                std::optional<PhysicsTraceResult> trace = trace_result_from_ray_hit(desc, ray, hit);
                if (trace.has_value() && (trace->contents & desc.contents_mask) != 0)
                {
                    return *trace;
                }
            }

            return result;
        }

        const Vec3 center_offset = (desc.mins + desc.maxs) * 0.5F;
        const Vec3 half_extents{
            std::max(std::fabs(desc.maxs.x - desc.mins.x) * 0.5F, 0.125F),
            std::max(std::fabs(desc.maxs.y - desc.mins.y) * 0.5F, 0.125F),
            std::max(std::fabs(desc.maxs.z - desc.mins.z) * 0.5F, 0.125F),
        };
        JPH::Ref<JPH::Shape> box_shape = new JPH::BoxShape(to_jolt_distance(half_extents), kMaxConvexRadius);
        box_shape->SetUserData(PhysicsContents::Solid);

        const JPH::RMat44 start_transform = JPH::RMat44::sTranslation(to_jolt_position(desc.start + center_offset));
        if (length_squared(delta) <= 0.000001F)
        {
            JPH::CollideShapeSettings settings;
            settings.mBackFaceMode = backface_mode;
            settings.mMaxSeparationDistance = to_jolt_distance(std::max(desc.collision_tolerance, 0.0F));

            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            system_.GetNarrowPhaseQuery().CollideShape(box_shape,
                JPH::Vec3::sReplicate(1.0F),
                start_transform,
                settings,
                JPH::RVec3::sZero(),
                collector,
                system_.GetDefaultBroadPhaseLayerFilter(query_layer),
                system_.GetDefaultLayerFilter(query_layer),
                {},
                shape_filter);
            collector.Sort();

            for (const JPH::CollideShapeResult& hit : collector.mHits)
            {
                std::optional<PhysicsTraceResult> trace = trace_result_from_overlap_hit(desc, hit);
                if (trace.has_value() && (trace->contents & desc.contents_mask) != 0)
                {
                    return *trace;
                }
            }

            return result;
        }

        JPH::ShapeCastSettings settings;
        settings.SetBackFaceMode(backface_mode);
        settings.mCollisionTolerance = to_jolt_distance(std::max(desc.collision_tolerance, 0.0F));
        settings.mUseShrunkenShapeAndConvexRadius = true;
        settings.mReturnDeepestPoint = true;

        JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
        const JPH::RShapeCast shape_cast = JPH::RShapeCast::sFromWorldTransform(
            box_shape,
            JPH::Vec3::sReplicate(1.0F),
            start_transform,
            to_jolt_distance(delta));
        system_.GetNarrowPhaseQuery().CastShape(shape_cast,
            settings,
            JPH::RVec3::sZero(),
            collector,
            system_.GetDefaultBroadPhaseLayerFilter(query_layer),
            system_.GetDefaultLayerFilter(query_layer),
            {},
            shape_filter);
        collector.Sort();

        for (const JPH::ShapeCastResult& hit : collector.mHits)
        {
            std::optional<PhysicsTraceResult> trace = trace_result_from_shape_hit(desc, hit);
            if (trace.has_value() && (trace->contents & desc.contents_mask) != 0)
            {
                return *trace;
            }
        }

        return result;
    }

    void reset_character(Vec3 origin, Vec3 velocity, const PhysicsCharacterConfig& config)
    {
        clear_character();
        character_config_ = config;

        const float radius = std::max(1.0F, config.radius);
        const float height = std::max(radius * 2.0F, config.height);
        const float half_cylinder_height = std::max(0.0F, (height - (radius * 2.0F)) * 0.5F);
        const float jolt_radius = to_jolt_distance(radius);
        const float jolt_half_cylinder_height = to_jolt_distance(half_cylinder_height);

        JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(jolt_half_cylinder_height, jolt_radius);
        JPH::ShapeSettings::ShapeResult shape_result =
            JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0F, 0.0F, jolt_half_cylinder_height + jolt_radius),
                JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0F)),
                capsule)
                .Create();
        if (shape_result.HasError())
        {
            log_error("failed to create character physics shape: {}", shape_result.GetError().c_str());
            return;
        }

        JPH::CharacterVirtualSettings settings;
        settings.mShape = shape_result.Get();
        settings.mUp = JPH::Vec3::sAxisZ();
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisZ(), -to_jolt_distance(height * 0.25F));
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(config.max_slope_degrees);
        settings.mEnhancedInternalEdgeRemoval = true;
        settings.mCharacterPadding = std::max(0.0F, config.character_padding);
        settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;

        character_ = new JPH::CharacterVirtual(&settings, to_jolt_position(origin), JPH::Quat::sIdentity(), 0, &system_);
        character_->SetLinearVelocity(to_jolt_distance(velocity));

        character_update_.mStickToFloorStepDown = to_jolt_distance(Vec3{0.0F, 0.0F, -std::max(0.0F, config.max_step_height)});
        character_update_.mWalkStairsStepUp = to_jolt_distance(Vec3{0.0F, 0.0F, std::max(0.0F, config.max_step_height)});
        character_update_.mWalkStairsStepDownExtra = character_update_.mStickToFloorStepDown;
        character_update_.mWalkStairsMinStepForward = 0.02F;
        character_update_.mWalkStairsStepForwardTest = 1.0F;

        refresh_character_contacts();
    }

    void clear_character()
    {
        character_ = nullptr;
    }

    bool has_character() const
    {
        return character_ != nullptr;
    }

    PhysicsCharacterState character_state() const
    {
        if (character_ == nullptr)
        {
            return {};
        }

        PhysicsCharacterState state;
        state.origin = from_jolt_position(character_->GetPosition());
        state.velocity = from_jolt_distance(character_->GetLinearVelocity());
        state.on_ground = character_->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        return state;
    }

    PhysicsCharacterState move_character_to(Vec3 target_origin, Vec3 target_velocity, float delta_seconds)
    {
        if (character_ == nullptr)
        {
            reset_character(target_origin, target_velocity, character_config_);
            return character_state();
        }

        const float dt = std::max(delta_seconds, 0.0001F);
        const Vec3 current_origin = from_jolt_position(character_->GetPosition());
        const Vec3 delta = target_origin - current_origin;

        if (length_squared(delta) > character_config_.max_teleport_distance * character_config_.max_teleport_distance)
        {
            character_->SetPosition(to_jolt_position(target_origin));
            character_->SetLinearVelocity(to_jolt_distance(target_velocity));
            refresh_character_contacts();
            return character_state();
        }

        const Vec3 controller_velocity = delta * (1.0F / dt);
        character_->SetLinearVelocity(to_jolt_distance(controller_velocity));

        character_->ExtendedUpdate(dt,
            system_.GetGravity(),
            character_update_,
            system_.GetDefaultBroadPhaseLayerFilter(PhysicsLayers::Moving),
            system_.GetDefaultLayerFilter(PhysicsLayers::Moving),
            {},
            {},
            temp_allocator_);

        PhysicsCharacterState state = character_state();
        if (state.on_ground && state.velocity.z < 0.0F)
        {
            state.velocity.z = 0.0F;
            character_->SetLinearVelocity(to_jolt_distance(state.velocity));
        }

        return state;
    }

private:
    JPH::Ref<JPH::Shape> create_body_shape(const PhysicsBodyDesc& desc) const
    {
        const std::uint32_t contents = desc.contents != 0 ? desc.contents : PhysicsContents::Solid;
        JPH::Ref<JPH::Shape> shape;
        if (desc.shape == PhysicsBodyShape::Sphere)
        {
            shape = new JPH::SphereShape(to_jolt_distance(std::max(desc.radius, 0.125F)));
        }
        else if (desc.shape == PhysicsBodyShape::Capsule)
        {
            const float radius = std::max(desc.radius, 0.125F);
            const float height = std::max(desc.height, (radius * 2.0F) + 0.125F);
            const float half_cylinder_height = std::max((height - (radius * 2.0F)) * 0.5F, 0.125F);
            JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(to_jolt_distance(half_cylinder_height), to_jolt_distance(radius));
            JPH::ShapeSettings::ShapeResult shape_result =
                JPH::RotatedTranslatedShapeSettings(JPH::Vec3::sZero(),
                    JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(90.0F)),
                    capsule)
                    .Create();
            if (shape_result.HasError())
            {
                log_error("failed to create capsule physics shape: {}", shape_result.GetError().c_str());
                return nullptr;
            }
            shape = shape_result.Get();
        }
        else
        {
            const Vec3 half_extents{
                std::max(std::fabs(desc.half_extents.x), 0.125F),
                std::max(std::fabs(desc.half_extents.y), 0.125F),
                std::max(std::fabs(desc.half_extents.z), 0.125F),
            };
            shape = new JPH::BoxShape(to_jolt_distance(half_extents), kMaxConvexRadius);
        }

        shape->SetUserData(contents);
        return shape;
    }

    const PhysicsBodyMetadata* metadata_for_body(JPH::BodyID body_id) const
    {
        const auto metadata = body_metadata_.find(body_id.GetIndexAndSequenceNumber());
        return metadata != body_metadata_.end() ? &metadata->second : nullptr;
    }

    std::uint32_t contents_for_hit(const JPH::Body& body, const JPH::SubShapeID& sub_shape_id) const
    {
        const std::uint32_t shape_contents = contents_for_shape(body.GetShape(), sub_shape_id, &static_triangle_metadata_);
        if (shape_contents != PhysicsContents::Solid || body.GetShape()->GetSubType() == JPH::EShapeSubType::Mesh)
        {
            return shape_contents;
        }

        const PhysicsBodyMetadata* metadata = metadata_for_body(body.GetID());
        return metadata != nullptr ? metadata->contents : shape_contents;
    }

    std::uint32_t surface_flags_for_hit(const JPH::Body& body, const JPH::SubShapeID& sub_shape_id) const
    {
        return surface_flags_for_shape(body.GetShape(), sub_shape_id, &static_triangle_metadata_, metadata_for_body(body.GetID()));
    }

    std::optional<PhysicsTraceResult> trace_result_from_ray_hit(
        const PhysicsTraceDesc& desc,
        const JPH::RRayCast& ray,
        const JPH::RayCastResult& hit) const
    {
        JPH::BodyLockRead lock(system_.GetBodyLockInterface(), hit.mBodyID);
        if (!lock.Succeeded())
        {
            return std::nullopt;
        }

        const JPH::Body& body = lock.GetBody();
        PhysicsTraceResult result;
        result.hit = true;
        result.startpos = desc.start;
        result.fraction = trace_fraction(hit.mFraction);
        result.endpos = trace_end(desc.start, desc.end, result.fraction);
        result.normal = unitless_from_jolt(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(result.fraction)));
        result.body = PhysicsBodyHandle{hit.mBodyID.GetIndexAndSequenceNumber()};
        result.layer = from_jolt_layer(body.GetObjectLayer());
        result.contents = contents_for_hit(body, hit.mSubShapeID2);
        result.surface_flags = surface_flags_for_hit(body, hit.mSubShapeID2);
        result.start_solid = result.fraction <= 0.0F;
        result.all_solid = false;
        return result;
    }

    std::optional<PhysicsTraceResult> trace_result_from_shape_hit(const PhysicsTraceDesc& desc, const JPH::ShapeCastResult& hit) const
    {
        JPH::BodyLockRead lock(system_.GetBodyLockInterface(), hit.mBodyID2);
        if (!lock.Succeeded())
        {
            return std::nullopt;
        }

        const JPH::Body& body = lock.GetBody();
        PhysicsTraceResult result;
        result.hit = true;
        result.startpos = desc.start;
        result.fraction = trace_fraction(hit.mFraction);
        result.endpos = trace_end(desc.start, desc.end, result.fraction);
        result.normal = normal_from_penetration_axis(hit.mPenetrationAxis);
        result.body = PhysicsBodyHandle{hit.mBodyID2.GetIndexAndSequenceNumber()};
        result.layer = from_jolt_layer(body.GetObjectLayer());
        result.contents = contents_for_hit(body, hit.mSubShapeID2);
        result.surface_flags = surface_flags_for_hit(body, hit.mSubShapeID2);
        result.start_solid = hit.mFraction <= 0.0F;
        result.all_solid = hit.GetEarlyOutFraction() < 0.0F;
        return result;
    }

    std::optional<PhysicsTraceResult> trace_result_from_overlap_hit(const PhysicsTraceDesc& desc, const JPH::CollideShapeResult& hit) const
    {
        JPH::BodyLockRead lock(system_.GetBodyLockInterface(), hit.mBodyID2);
        if (!lock.Succeeded())
        {
            return std::nullopt;
        }

        const JPH::Body& body = lock.GetBody();
        PhysicsTraceResult result;
        result.hit = true;
        result.startpos = desc.start;
        result.endpos = desc.start;
        result.fraction = 0.0F;
        result.normal = normal_from_penetration_axis(hit.mPenetrationAxis);
        result.body = PhysicsBodyHandle{hit.mBodyID2.GetIndexAndSequenceNumber()};
        result.layer = from_jolt_layer(body.GetObjectLayer());
        result.contents = contents_for_hit(body, hit.mSubShapeID2);
        result.surface_flags = surface_flags_for_hit(body, hit.mSubShapeID2);
        result.start_solid = true;
        result.all_solid = hit.mPenetrationDepth > 0.0F;
        return result;
    }

    void refresh_character_contacts()
    {
        if (character_ == nullptr)
        {
            return;
        }

        character_->RefreshContacts(system_.GetDefaultBroadPhaseLayerFilter(PhysicsLayers::Moving),
            system_.GetDefaultLayerFilter(PhysicsLayers::Moving),
            {},
            {},
            temp_allocator_);
    }

    PhysicsWorldSettings settings_;
    JPH::TempAllocatorImpl temp_allocator_;
    JPH::JobSystemThreadPool job_system_;
    BroadPhaseLayerInterface broad_phase_layers_;
    ObjectVsBroadPhaseLayerFilter object_vs_broad_phase_filter_;
    ObjectLayerPairFilter object_layer_pair_filter_;
    JPH::PhysicsSystem system_;
    JPH::BodyID static_body_id_;
    JPH::RefConst<JPH::Shape> static_shape_;
    bool has_static_body_ = false;
    std::unordered_map<std::uint32_t, PhysicsBodyMetadata> body_metadata_;
    std::vector<StaticTriangleMetadata> static_triangle_metadata_;
    bool broad_phase_optimized_ = false;
    JPH::Ref<JPH::CharacterVirtual> character_;
    JPH::CharacterVirtual::ExtendedUpdateSettings character_update_;
    PhysicsCharacterConfig character_config_;
};

PhysicsWorld::PhysicsWorld()
    : PhysicsWorld(PhysicsWorldSettings{})
{
}

PhysicsWorld::PhysicsWorld(const PhysicsWorldSettings& settings)
{
    initialize_jolt();
    impl_ = std::make_unique<Impl>(settings);
}

PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

const PhysicsWorldSettings& PhysicsWorld::settings() const
{
    return impl_->settings();
}

void PhysicsWorld::set_gravity(Vec3 gravity)
{
    impl_->set_gravity(gravity);
}

Vec3 PhysicsWorld::gravity() const
{
    return impl_->gravity();
}

bool PhysicsWorld::load_static_world(const LoadedWorld& world)
{
    return impl_->load_static_world(world);
}

void PhysicsWorld::clear_static_world()
{
    impl_->clear_static_world();
}

bool PhysicsWorld::has_static_world() const
{
    return impl_->has_static_world();
}

PhysicsBodyHandle PhysicsWorld::create_body(const PhysicsBodyDesc& desc)
{
    return impl_->create_body(desc);
}

void PhysicsWorld::destroy_body(PhysicsBodyHandle handle)
{
    impl_->destroy_body(handle);
}

bool PhysicsWorld::set_body_layer(PhysicsBodyHandle handle, PhysicsObjectLayer layer)
{
    return impl_->set_body_layer(handle, layer);
}

bool PhysicsWorld::set_body_origin(PhysicsBodyHandle handle, Vec3 origin, bool activate)
{
    return impl_->set_body_origin(handle, origin, activate);
}

bool PhysicsWorld::set_body_velocity(PhysicsBodyHandle handle, Vec3 velocity)
{
    return impl_->set_body_velocity(handle, velocity);
}

bool PhysicsWorld::move_kinematic_body(PhysicsBodyHandle handle, Vec3 target_origin, float delta_seconds)
{
    return impl_->move_kinematic_body(handle, target_origin, delta_seconds);
}

bool PhysicsWorld::apply_body_impulse(PhysicsBodyHandle handle, Vec3 impulse)
{
    return impl_->apply_body_impulse(handle, impulse);
}

PhysicsBodyState PhysicsWorld::body_state(PhysicsBodyHandle handle) const
{
    return impl_->body_state(handle);
}

void PhysicsWorld::step_simulation(float delta_seconds, int collision_sub_steps)
{
    impl_->step_simulation(delta_seconds, collision_sub_steps);
}

PhysicsTraceResult PhysicsWorld::trace_ray(Vec3 start, Vec3 end, std::uint32_t contents_mask) const
{
    return impl_->trace_ray(start, end, contents_mask);
}

PhysicsTraceResult PhysicsWorld::trace_box(const PhysicsTraceDesc& desc) const
{
    return impl_->trace_box(desc);
}

void PhysicsWorld::reset_character(Vec3 origin, Vec3 velocity)
{
    impl_->reset_character(origin, velocity, impl_->settings().default_character);
}

void PhysicsWorld::reset_character(Vec3 origin, Vec3 velocity, const PhysicsCharacterConfig& config)
{
    impl_->reset_character(origin, velocity, config);
}

void PhysicsWorld::clear_character()
{
    impl_->clear_character();
}

bool PhysicsWorld::has_character() const
{
    return impl_->has_character();
}

PhysicsCharacterState PhysicsWorld::character_state() const
{
    return impl_->character_state();
}

PhysicsCharacterState PhysicsWorld::move_character_to(Vec3 target_origin, Vec3 target_velocity, float delta_seconds)
{
    return impl_->move_character_to(target_origin, target_velocity, delta_seconds);
}
}
