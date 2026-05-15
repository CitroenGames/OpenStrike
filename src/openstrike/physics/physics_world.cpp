#include "openstrike/physics/physics_world.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/world/world.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <utility>

JPH_SUPPRESS_WARNINGS

namespace openstrike
{
namespace
{
namespace PhysicsLayers
{
constexpr JPH::ObjectLayer Static = 0;
constexpr JPH::ObjectLayer Moving = 1;
constexpr JPH::ObjectLayer Count = 2;
}

namespace BroadPhaseLayers
{
constexpr JPH::BroadPhaseLayer Static(0);
constexpr JPH::BroadPhaseLayer Moving(1);
constexpr JPH::uint Count = 2;
}

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
    {
        if (first == PhysicsLayers::Static)
        {
            return second == PhysicsLayers::Moving;
        }

        if (first == PhysicsLayers::Moving)
        {
            return true;
        }

        return false;
    }
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    BroadPhaseLayerInterface()
    {
        object_to_broad_phase_[PhysicsLayers::Static] = BroadPhaseLayers::Static;
        object_to_broad_phase_[PhysicsLayers::Moving] = BroadPhaseLayers::Moving;
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
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::Static):
            return "Static";
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::Moving):
            return "Moving";
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
        if (layer == PhysicsLayers::Static)
        {
            return broad_phase_layer == BroadPhaseLayers::Moving;
        }

        if (layer == PhysicsLayers::Moving)
        {
            return true;
        }

        return false;
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

JPH::Vec3 to_jolt(Vec3 value)
{
    return JPH::Vec3(value.x, value.y, value.z);
}

JPH::RVec3 to_jolt_position(Vec3 value)
{
    return JPH::RVec3(value.x, value.y, value.z);
}

Vec3 from_jolt(JPH::Vec3Arg value)
{
    return {value.GetX(), value.GetY(), value.GetZ()};
}

Vec3 from_jolt_position(JPH::RVec3Arg value)
{
    return {static_cast<float>(value.GetX()), static_cast<float>(value.GetY()), static_cast<float>(value.GetZ())};
}

float length_squared(Vec3 value)
{
    return (value.x * value.x) + (value.y * value.y) + (value.z * value.z);
}

JPH::Triangle make_triangle(Vec3 a, Vec3 b, Vec3 c)
{
    return JPH::Triangle(JPH::Float3(a.x, a.y, a.z), JPH::Float3(b.x, b.y, b.z), JPH::Float3(c.x, c.y, c.z));
}
}

class PhysicsWorld::Impl
{
public:
    Impl()
        : job_system_(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 0)
    {
        system_.Init(4096, 0, 8192, 4096, broad_phase_layers_, object_vs_broad_phase_filter_, object_layer_pair_filter_);
        system_.SetGravity(JPH::Vec3(0.0F, 0.0F, -800.0F));
    }

    ~Impl()
    {
        clear_character();
        clear_static_world();
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
        for (const WorldTriangle& triangle : world.mesh.collision_triangles)
        {
            triangles.push_back(make_triangle(triangle.points[0], triangle.points[1], triangle.points[2]));
            triangles.push_back(make_triangle(triangle.points[2], triangle.points[1], triangle.points[0]));
        }

        JPH::MeshShapeSettings shape_settings(triangles);
        shape_settings.mActiveEdgeCosThresholdAngle = -1.0F;
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (shape_result.HasError())
        {
            log_error("failed to create physics mesh for world '{}': {}", world.name, shape_result.GetError().c_str());
            return false;
        }

        static_shape_ = shape_result.Get();

        JPH::BodyCreationSettings body_settings(static_shape_, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, PhysicsLayers::Static);
        body_settings.mEnhancedInternalEdgeRemoval = true;

        JPH::BodyInterface& bodies = system_.GetBodyInterface();
        static_body_id_ = bodies.CreateAndAddBody(body_settings, JPH::EActivation::DontActivate);
        has_static_body_ = !static_body_id_.IsInvalid();
        if (!has_static_body_)
        {
            static_shape_ = nullptr;
            log_error("failed to add physics mesh body for world '{}'", world.name);
            return false;
        }

        return true;
    }

    void clear_static_world()
    {
        if (has_static_body_)
        {
            JPH::BodyInterface& bodies = system_.GetBodyInterface();
            bodies.RemoveBody(static_body_id_);
            bodies.DestroyBody(static_body_id_);
            static_body_id_ = JPH::BodyID();
            has_static_body_ = false;
        }

        static_shape_ = nullptr;
    }

    bool has_static_world() const
    {
        return has_static_body_;
    }

    void reset_character(Vec3 origin, Vec3 velocity, const PhysicsCharacterConfig& config)
    {
        clear_character();
        character_config_ = config;

        const float radius = std::max(1.0F, config.radius);
        const float height = std::max(radius * 2.0F, config.height);
        const float half_cylinder_height = std::max(0.0F, (height - (radius * 2.0F)) * 0.5F);

        JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(half_cylinder_height, radius);
        JPH::ShapeSettings::ShapeResult shape_result =
            JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0F, 0.0F, half_cylinder_height + radius),
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
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisZ(), -(height * 0.25F));
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(config.max_slope_degrees);
        settings.mEnhancedInternalEdgeRemoval = true;
        settings.mCharacterPadding = std::max(0.0F, config.character_padding);
        settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;

        character_ = new JPH::CharacterVirtual(&settings, to_jolt_position(origin), JPH::Quat::sIdentity(), 0, &system_);
        character_->SetLinearVelocity(to_jolt(velocity));

        character_update_.mStickToFloorStepDown = JPH::Vec3(0.0F, 0.0F, -std::max(0.0F, config.max_step_height));
        character_update_.mWalkStairsStepUp = JPH::Vec3(0.0F, 0.0F, std::max(0.0F, config.max_step_height));
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
        state.velocity = from_jolt(character_->GetLinearVelocity());
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
            character_->SetLinearVelocity(to_jolt(target_velocity));
            refresh_character_contacts();
            return character_state();
        }

        const Vec3 controller_velocity = delta * (1.0F / dt);
        character_->SetLinearVelocity(to_jolt(controller_velocity));

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
            character_->SetLinearVelocity(to_jolt(state.velocity));
        }

        return state;
    }

private:
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

    JPH::TempAllocatorImpl temp_allocator_{4 * 1024 * 1024};
    JPH::JobSystemThreadPool job_system_;
    BroadPhaseLayerInterface broad_phase_layers_;
    ObjectVsBroadPhaseLayerFilter object_vs_broad_phase_filter_;
    ObjectLayerPairFilter object_layer_pair_filter_;
    JPH::PhysicsSystem system_;
    JPH::BodyID static_body_id_;
    JPH::RefConst<JPH::Shape> static_shape_;
    bool has_static_body_ = false;
    JPH::Ref<JPH::CharacterVirtual> character_;
    JPH::CharacterVirtual::ExtendedUpdateSettings character_update_;
    PhysicsCharacterConfig character_config_;
};

PhysicsWorld::PhysicsWorld()
{
    initialize_jolt();
    impl_ = std::make_unique<Impl>();
}

PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

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
