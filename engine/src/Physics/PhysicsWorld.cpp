#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsMaterial.h"
#include "Engine/Scene/Scene.h"

// Jolt stays inside this file: no Jolt type appears in engine headers.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine {

namespace {

// --- Global Jolt state (allocator hooks, factory, type registry), refcounted per PhysicsWorld ---

std::mutex g_JoltMutex;
int        g_JoltUsers = 0;

void JoltTrace(const char* format, ...)
{
    char    buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    ENGINE_INFO("Jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
{
    ENGINE_ERROR("Jolt assert {}:{}: ({}) {}", file, line, expression, message ? message : "");
    return false; // no breakpoint
}
#endif

void AcquireJolt()
{
    std::scoped_lock lock(g_JoltMutex);
    if (g_JoltUsers++ > 0)
        return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void ReleaseJolt()
{
    std::scoped_lock lock(g_JoltMutex);
    if (--g_JoltUsers > 0)
        return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// --- Job system on the engine ThreadPool ---
// Every job of a PhysicsSystem::Update is also added to its barrier: the main thread executes
// whatever the pool has not picked up yet while it waits (Job::Execute runs a job only once),
// so a pool busy with asset loading slows physics down but cannot stall it.
class EngineJobSystem final : public JPH::JobSystemWithBarrier {
public:
    EngineJobSystem(ThreadPool& pool, JPH::uint maxJobs, JPH::uint maxBarriers)
        : JobSystemWithBarrier(maxBarriers), m_Pool(pool)
    {
        m_Jobs.Init(maxJobs, maxJobs);
    }
    ~EngineJobSystem() override
    {
        while (m_InFlight.load() != 0) // pool closures still reference m_Jobs
            std::this_thread::yield();
    }

    int GetMaxConcurrency() const override { return static_cast<int>(m_Pool.ThreadCount()) + 1; }

    JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                             JPH::uint32 dependencies) override
    {
        JPH::uint32 index;
        while ((index = m_Jobs.ConstructObject(name, color, this, function, dependencies)) == Jobs::cInvalidObjectIndex)
            std::this_thread::yield(); // all jobs in use: wait for one to finish
        Job*           job = &m_Jobs.Get(index);
        JPH::JobHandle handle(job); // keeps a reference: the job may complete right after queueing
        if (dependencies == 0)
            QueueJob(job);
        return handle;
    }

protected:
    void QueueJob(Job* job) override
    {
        job->AddRef();
        m_InFlight.fetch_add(1);
        m_Pool.Enqueue([this, job] {
            job->Execute();
            job->Release();
            m_InFlight.fetch_sub(1); // last access to this
        });
    }
    void QueueJobs(Job** jobs, JPH::uint count) override
    {
        for (JPH::uint i = 0; i < count; ++i)
            QueueJob(jobs[i]);
    }
    void FreeJob(Job* job) override { m_Jobs.DestructObject(job); }

private:
    using Jobs = JPH::FixedSizeFreeList<Job>;
    ThreadPool&      m_Pool;
    Jobs             m_Jobs;
    std::atomic<int> m_InFlight{0};
};

// --- Layers: object layer = user collision layer << 1 | moving. Static bodies never collide
// with each other; user layers follow PhysicsSettings::layerCollision. ---

namespace Layers {
constexpr JPH::ObjectLayer Make(std::uint32_t userLayer, bool moving)
{
    return static_cast<JPH::ObjectLayer>(((userLayer & (kPhysicsLayers - 1)) << 1) | (moving ? 1u : 0u));
}
constexpr bool          IsMoving(JPH::ObjectLayer layer) { return (layer & 1u) != 0; }
constexpr std::uint32_t User(JPH::ObjectLayer layer) { return static_cast<std::uint32_t>(layer) >> 1; }
constexpr JPH::ObjectLayer kMoving = Make(0, true); // characters
} // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kNonMoving{0};
constexpr JPH::BroadPhaseLayer kMoving{1};
constexpr JPH::uint            kCount = 2;
} // namespace BroadPhaseLayers

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return Layers::IsMoving(layer) ? BroadPhaseLayers::kMoving : BroadPhaseLayers::kNonMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BroadPhaseLayers::kNonMoving ? "NonMoving" : "Moving";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override
    {
        return Layers::IsMoving(layer) || broadPhase == BroadPhaseLayers::kMoving;
    }
};

// Reads the matrix copied from PhysicsSettings before every update (never during one).
class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectPairFilter(const std::array<std::uint16_t, kPhysicsLayers>& matrix) : m_Matrix(matrix) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        return (Layers::IsMoving(a) || Layers::IsMoving(b)) && ((m_Matrix[Layers::User(a)] >> Layers::User(b)) & 1u) != 0;
    }

private:
    const std::array<std::uint16_t, kPhysicsLayers>& m_Matrix;
};

// Queries: bodies on the layers of the mask.
class LayerMaskFilter final : public JPH::ObjectLayerFilter {
public:
    explicit LayerMaskFilter(std::uint16_t mask) : m_Mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return ((m_Mask >> Layers::User(layer)) & 1u) != 0; }

private:
    std::uint16_t m_Mask;
};

// --- Physics materials as Jolt sees them. The contact listener reads friction / restitution, queries
// the surface. Hot reload updates them in place (main thread, never during an update). ---

class SurfaceMaterial final : public JPH::PhysicsMaterial {
public:
    float       friction    = 0.5f;
    float       restitution = 0.0f;
    std::string surface;

    const char* GetDebugName() const override { return surface.c_str(); }
};

const SurfaceMaterial* AsSurface(const JPH::PhysicsMaterial* material)
{
    return dynamic_cast<const SurfaceMaterial*>(material);
}

std::string SurfaceName(const JPH::PhysicsMaterial* material)
{
    const SurfaceMaterial* m = AsSurface(material);
    return m ? m->surface : std::string();
}

// --- Contacts: recorded on the physics threads, turned into events on the main thread ---

// One contact manifold (bodies sorted: a < b; the normal points from a to b).
struct ContactSample {
    JPH::BodyID            a, b;
    int                    delta   = 0;     // +1 added, -1 removed, 0 persisted
    bool                   trigger = false;
    float                  depth   = 0.0f;
    glm::vec3              point{0.0f}, normal{0.0f, 1.0f, 0.0f}, relativeVelocity{0.0f};
    float                  approachSpeed = 0.0f, impulse = 0.0f;
    const SurfaceMaterial* surfaceA = nullptr;
    const SurfaceMaterial* surfaceB = nullptr;
};

ContactInfo ToInfo(const ContactSample& s)
{
    return {.point            = s.point,
            .normal           = s.normal,
            .relativeVelocity = s.relativeVelocity,
            .approachSpeed    = s.approachSpeed,
            .impulse          = s.impulse,
            .surfaceA         = s.surfaceA ? s.surfaceA->surface : std::string(),
            .surfaceB         = s.surfaceB ? s.surfaceB->surface : std::string()};
}

class ContactRecorder final : public JPH::ContactListener {
public:
    std::atomic<bool> recordPersisted{true}; // PhysicsSettings::persistEvents

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override
    {
        ApplyMaterials(a, b, manifold, settings);
        Record(Sample(a, b, manifold, settings, +1));
    }
    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override
    {
        ApplyMaterials(a, b, manifold, settings); // the settings are reset to the bodies' every step
        if (recordPersisted.load(std::memory_order_relaxed))
            Record(Sample(a, b, manifold, settings, 0));
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        ContactSample s;
        s.a = pair.GetBody1ID();
        s.b = pair.GetBody2ID();
        s.delta = -1;
        if (s.b < s.a)
            std::swap(s.a, s.b);
        std::scoped_lock lock(m_Mutex);
        m_Samples.push_back(s);
    }
    std::vector<ContactSample> Take()
    {
        std::scoped_lock lock(m_Mutex);
        return std::exchange(m_Samples, {});
    }

private:
    // Friction sqrt(f1 * f2), restitution max(r1, r2) of the touching sub shapes' materials (the
    // body's values where a shape has none).
    static void ApplyMaterials(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                               JPH::ContactSettings& settings)
    {
        const SurfaceMaterial* ma = AsSurface(a.GetShape()->GetMaterial(manifold.mSubShapeID1));
        const SurfaceMaterial* mb = AsSurface(b.GetShape()->GetMaterial(manifold.mSubShapeID2));
        if (!ma && !mb)
            return;
        const float fa = ma ? ma->friction : a.GetFriction(), fb = mb ? mb->friction : b.GetFriction();
        const float ra = ma ? ma->restitution : a.GetRestitution(), rb = mb ? mb->restitution : b.GetRestitution();
        settings.mCombinedFriction    = std::sqrt(std::max(fa, 0.0f) * std::max(fb, 0.0f));
        settings.mCombinedRestitution = std::max(ra, rb);
    }

    static float InverseMass(const JPH::Body& body)
    {
        return body.IsDynamic() ? body.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked() : 0.0f;
    }

    static ContactSample Sample(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                                const JPH::ContactSettings& settings, int delta)
    {
        ContactSample s;
        s.a       = a.GetID();
        s.b       = b.GetID();
        s.delta   = delta;
        s.trigger = a.IsSensor() || b.IsSensor();
        s.depth   = manifold.mPenetrationDepth;
        JPH::Vec3 sum = JPH::Vec3::sZero();
        const JPH::uint count = manifold.mRelativeContactPointsOn1.size();
        for (JPH::uint i = 0; i < count; ++i)
            sum += manifold.mRelativeContactPointsOn1[i];
        const JPH::RVec3 point = manifold.mBaseOffset + (count > 0 ? sum / static_cast<float>(count) : JPH::Vec3::sZero());
        const JPH::Vec3  normal   = manifold.mWorldSpaceNormal; // moves b out of a: from a towards b
        const JPH::Vec3  relative = b.GetPointVelocity(point) - a.GetPointVelocity(point);
        const float      approach = std::max(-relative.Dot(normal), 0.0f);
        const float      inverse  = InverseMass(a) + InverseMass(b);
        s.point            = {static_cast<float>(point.GetX()), static_cast<float>(point.GetY()), static_cast<float>(point.GetZ())};
        s.normal           = {normal.GetX(), normal.GetY(), normal.GetZ()};
        s.relativeVelocity = {relative.GetX(), relative.GetY(), relative.GetZ()};
        s.approachSpeed    = approach;
        s.impulse          = inverse > 0.0f ? approach * (1.0f + settings.mCombinedRestitution) / inverse : 0.0f;
        s.surfaceA         = AsSurface(a.GetShape()->GetMaterial(manifold.mSubShapeID1));
        s.surfaceB         = AsSurface(b.GetShape()->GetMaterial(manifold.mSubShapeID2));
        if (s.b < s.a) {
            std::swap(s.a, s.b);
            std::swap(s.surfaceA, s.surfaceB);
            s.normal           = -s.normal;
            s.relativeVelocity = -s.relativeVelocity;
        }
        return s;
    }

    void Record(const ContactSample& sample)
    {
        std::scoped_lock lock(m_Mutex);
        m_Samples.push_back(sample);
    }
    std::mutex                 m_Mutex;
    std::vector<ContactSample> m_Samples;
};

// Bodies joined by a joint that must not collide (Joint::collideConnected = false). Group id = the
// body's BodyID (index + sequence); the set changes on the main thread, never during an update.
class JointGroupFilter final : public JPH::GroupFilter {
public:
    static std::uint64_t PairOf(JPH::uint32 a, JPH::uint32 b)
    {
        return a < b ? (std::uint64_t{a} << 32) | b : (std::uint64_t{b} << 32) | a;
    }
    bool CanCollide(const JPH::CollisionGroup& a, const JPH::CollisionGroup& b) const override
    {
        return !disabled.contains(PairOf(a.GetGroupID(), b.GetGroupID()));
    }
    std::unordered_map<std::uint64_t, int> disabled; // pair -> joints asking for it
};

// Queries: skip triggers and one entity (a character's inner body carries its entity too).
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(Entity ignore) : m_Ignore(static_cast<JPH::uint64>(ignore)) {}
    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        return !body.IsSensor() && body.GetUserData() != m_Ignore;
    }

private:
    JPH::uint64 m_Ignore;
};

// --- Conversions ---

JPH::Vec3 ToJolt(const glm::vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat ToJolt(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }
glm::vec3 ToGlm(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat ToGlm(JPH::QuatArg q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }

std::uint64_t Key(Entity e) { return static_cast<std::uint64_t>(e); }
std::uint64_t PairKey(JPH::BodyID a, JPH::BodyID b)
{
    return (std::uint64_t{a.GetIndexAndSequenceNumber()} << 32) | b.GetIndexAndSequenceNumber();
}

struct Pose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// Mirroring (negative scale) is not represented: the scale is taken as positive.
Pose Decompose(const glm::mat4& m)
{
    Pose p;
    p.position = m[3];
    p.scale    = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
    p.scale    = glm::max(p.scale, glm::vec3(1e-6f));
    p.rotation = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(m[0]) / p.scale.x, glm::vec3(m[1]) / p.scale.y,
                                                         glm::vec3(m[2]) / p.scale.z)));
    return p;
}

bool Finite(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool Finite(const glm::vec4& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
bool Finite(const glm::quat& q)
{
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}
bool ValidPhysicsScale(const glm::vec3& scale)
{
    constexpr float kMinPhysicsScale = 1e-5f;
    return Finite(scale) && glm::all(glm::greaterThan(glm::abs(scale), glm::vec3(kMinPhysicsScale)));
}

bool Moved(const glm::vec3& a, const glm::vec3& b) { return glm::any(glm::greaterThan(glm::abs(a - b), glm::vec3(1e-5f))); }
bool Rotated(const glm::quat& a, const glm::quat& b) { return std::abs(glm::dot(a, b)) < 1.0f - 1e-6f; }
bool Rescaled(const glm::vec3& a, const glm::vec3& b)
{
    return glm::any(glm::greaterThan(glm::abs(a - b), glm::max(glm::abs(a), glm::abs(b)) * 1e-4f));
}

constexpr float kMinExtent = 1e-3f;

// Collider extents after scaling (shared by shape creation and debug drawing). A rotated shape
// takes the entity's scale along its own axes (exact for axis permutations, approximate otherwise).
struct ScaledCollider {
    glm::vec3 halfExtents;
    float     radius;
    float     halfHeight;
    glm::vec3 center;
};

ScaledCollider Scaled(const Collider& c, const glm::vec3& scale)
{
    const glm::vec3 entityScale = glm::abs(scale);
    const glm::vec3 safeScale   = glm::abs(glm::conjugate(glm::normalize(c.rotation)) * entityScale);
    const float uniform = std::max({safeScale.x, safeScale.y, safeScale.z});
    return {.halfExtents = glm::max(c.halfExtents * safeScale, glm::vec3(kMinExtent)),
            .radius      = std::max(c.radius * (c.shape == ColliderShape::Capsule ? std::max(safeScale.x, safeScale.z) : uniform), kMinExtent),
            .halfHeight  = std::max(c.halfHeight * safeScale.y, 0.0f),
            .center      = c.center * entityScale};
}

constexpr float kTwoPi = 6.28318530718f;

// Yaw (radians about +Y) of a rotation: the heading of its local -Z axis.
float YawOf(const glm::quat& q)
{
    const glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, -1.0f);
    return std::atan2(-forward.x, -forward.z);
}
glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }
float WrapAngle(float a) { return a - kTwoPi * std::floor((a + 0.5f * kTwoPi) / kTwoPi); }

// Entity, its world pose and the anchor: the joint frame in world space (position includes scale).
Pose JointFrame(const glm::mat4& world, const Joint& j)
{
    const Pose entity = Decompose(world);
    Pose       frame;
    frame.position = glm::vec3(world * glm::vec4(j.anchor, 1.0f));
    frame.rotation = glm::normalize(entity.rotation * glm::normalize(j.anchorRotation));
    return frame;
}

glm::mat4 PoseMatrix(const Pose& p) { return glm::translate(glm::mat4(1.0f), p.position) * glm::mat4_cast(p.rotation); }

// The ragdoll a bone belongs to (nearest ancestor with Ragdoll).
const Ragdoll* RagdollOf(const Registry& registry, Entity e)
{
    for (Entity x = e; x != NullEntity && registry.Valid(x); x = registry.Get<Hierarchy>(x).parent)
        if (const auto* ragdoll = registry.TryGet<Ragdoll>(x))
            return ragdoll;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------------------------

struct PhysicsWorld::Impl {
    struct BodyRecord {
        Entity        entity = NullEntity;
        JPH::BodyID   id;
        RigidBody     body;
        Collider      collider;
        BodyType      type = BodyType::Static; // effective (mesh colliders are never dynamic)
        glm::vec3     scale{1.0f};
        ModelHandle   model; // mesh colliders
        std::uint32_t meshIndex = 0;
        std::uint32_t revision  = 0; // AssetManager::Revision(model): a reload rebuilds the shape
        Pose          last;  // entity world pose the body was last synced to
        Pose          simPrevious, simCurrent; // dynamic: poses after the last two steps (Interpolate)
        bool          between = false;         // the scene shows a pose between them
        bool          kinematicMove = false; // kinematic: target changed since the last Step
        std::uint32_t visit = 0;
        std::uint32_t teleported = 0; // visit of the last teleport (joints are remade)
        std::uint32_t materialGeneration = 0;
    };
    struct CharacterRecord {
        Entity                          entity = NullEntity;
        JPH::Ref<JPH::CharacterVirtual> character;
        CharacterController             settings;
        JPH::RefConst<JPH::Shape>       standing, crouched; // capsules with the feet at the origin
        glm::vec3                       lastPosition{0.0f};
        glm::vec3                       input{0.0f};
        glm::vec3                       simPrevious{0.0f}, simCurrent{0.0f}; // Interpolate
        float                           yaw = 0.0f, targetYaw = 0.0f;       // CharacterRotation
        float                           lastYaw = 0.0f; // entity yaw last synced / written (external turns)
        float                           simPreviousYaw = 0.0f, simCurrentYaw = 0.0f;
        bool                            between = false;
        bool                            jump  = false;
        bool                            crouching = false, wantCrouch = false;
        std::uint32_t                   visit = 0;
        struct Touch {
            Entity      other   = NullEntity;
            bool        trigger = false;
            ContactInfo contact;
        };
        // Bodies it touches (BodyID index + sequence): Begin / Persist / End events.
        std::unordered_map<std::uint32_t, Touch> touching;
    };
    struct Pair {
        Entity      a, b;
        int         count     = 0;
        bool        trigger   = false;
        bool        suspended = false; // a body fell asleep: Jolt removed the contacts, they still touch
        ContactInfo last;              // for the End event
    };
    struct JointRecord {
        Entity                            entity = NullEntity;
        Joint                             joint;
        JPH::Ref<JPH::TwoBodyConstraint>  constraint;
        JPH::BodyID                       a, b;   // b invalid: the world
        Entity                            entityA = NullEntity, entityB = NullEntity;
        glm::mat4                         relative{1.0f}; // joint frame relative to body A (edits in edit mode)
        bool                              noCollision = false; // registered in the group filter
        glm::vec3                         force{0.0f}, torque{0.0f}; // last step
        std::uint32_t                     visit = 0;
    };
    struct MaterialEntry {
        JPH::Ref<SurfaceMaterial>  material;
        std::filesystem::file_time_type time{};
        bool                       failed = false;
    };

    Impl(ThreadPool& threads, EventBus& bus, const AssetManager* assetManager) : events(bus), assets(assetManager)
    {
        AcquireJolt();
        groupFilter   = new JointGroupFilter();
        tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(32u << 20);
        jobSystem     = std::make_unique<EngineJobSystem>(threads, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
        system        = std::make_unique<JPH::PhysicsSystem>();
        system->Init(65536, 0, 65536, 20480, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        system->SetContactListener(&contacts);
    }

    ~Impl()
    {
        Clear();
        materials.clear();   // Jolt objects go before Jolt itself
        groupFilter = nullptr;
        system.reset();
        jobSystem.reset();
        tempAllocator.reset();
        ReleaseJolt();
    }

    JPH::BodyInterface&       Bodies() { return system->GetBodyInterfaceNoLock(); } // main thread, outside Update
    const JPH::BodyInterface& Bodies() const { return system->GetBodyInterfaceNoLock(); }

    void Clear()
    {
        for (auto& [key, joint] : joints) // constraints reference the bodies: first
            if (joint.constraint)
                system->RemoveConstraint(joint.constraint);
        joints.clear();
        groupFilter->disabled.clear();
        for (auto& [key, record] : bodies)
            DestroyBody(record.id);
        bodies.clear();
        characters.clear(); // removes the inner bodies
        innerBodies.clear();
        pairs.clear();
        meshShapes.clear();
        removedBodies.clear();
        invalidScale.clear();
        contacts.Take();
        pendingEvents.clear();
        pendingPersist.clear();
        pendingBroken.clear();
    }

    void DestroyBody(JPH::BodyID id)
    {
        Bodies().RemoveBody(id);
        Bodies().DestroyBody(id);
    }

    // --- Physics materials (.uphysmat, cached by path, hot-reloaded in place) ---

    const SurfaceMaterial* Material(const std::string& path)
    {
        if (path.empty())
            return nullptr;
        auto [it, inserted] = materials.try_emplace(path);
        if (inserted)
            LoadMaterial(path, it->second);
        return it->second.failed ? nullptr : it->second.material.GetPtr();
    }

    void LoadMaterial(const std::string& path, MaterialEntry& entry)
    {
        const std::filesystem::path file = PathFromUtf8(path);
        entry.time                       = Vfs::ModifiedTime(file);
        std::string                        error;
        const std::optional<PhysicsMaterialData> data = LoadPhysicsMaterial(file, &error);
        if (!data) {
            if (!entry.failed)
                ENGINE_WARN("Physics: material {}", error);
            if (!entry.material) // never loaded: colliders use their own friction / restitution
                entry.failed = true;
            return;
        }
        if (!entry.material)
            entry.material = new SurfaceMaterial();
        if (entry.failed) // shapes built without it are remade
            ++materialGeneration;
        entry.failed                = false;
        entry.material->friction    = data->friction;
        entry.material->restitution = data->restitution;
        entry.material->surface     = data->surface;
    }

    // Hot reload: changed files once a second (values change in place; a material that failed
    // before rebuilds the shapes that use it).
    void CheckMaterials()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextMaterialCheck)
            return;
        nextMaterialCheck = now + std::chrono::seconds(1);
        for (auto& [path, entry] : materials)
            if (Vfs::ModifiedTime(PathFromUtf8(path)) != entry.time)
                LoadMaterial(path, entry);
    }

    // Materials a collider uses, as one key (mesh shapes are cached per assignment).
    static std::string MaterialKey(const Collider& c)
    {
        std::string key = c.material;
        for (const auto& [name, path] : c.meshMaterials)
            key += '\n' + name + '=' + path;
        return key;
    }

    // --- Shapes ---

    JPH::RefConst<JPH::Shape> MeshShapeFor(const Model& model, ModelHandle handle, std::uint32_t meshIndex,
                                           std::uint32_t revision, const Collider& collider)
    {
        const MeshKey key{handle.index, handle.generation, meshIndex, revision, MaterialKey(collider) + '#' +
                                                                               std::to_string(materialGeneration)};
        if (const auto it = meshShapes.find(key); it != meshShapes.end())
            return it->second;

        // Material per submesh: its glTF material's assignment, else the collider's material.
        JPH::PhysicsMaterialList list;
        const auto materialIndex = [&](const Submesh& sm) -> JPH::uint32 {
            const SurfaceMaterial* material = nullptr;
            if (sm.material < model.previewMaterials.size())
                if (const auto it = collider.meshMaterials.find(model.previewMaterials[sm.material].name);
                    it != collider.meshMaterials.end())
                    material = Material(it->second);
            if (!material)
                material = Material(collider.material);
            if (!material)
                return 0; // list entry 0: default material (added below when needed)
            for (JPH::uint32 i = 0; i < list.size(); ++i)
                if (list[i] == material)
                    return i;
            list.push_back(material);
            return static_cast<JPH::uint32>(list.size() - 1);
        };

        JPH::VertexList vertices;
        vertices.reserve(model.collisionPositions.size());
        for (const glm::vec3& p : model.collisionPositions)
            vertices.push_back(JPH::Float3(p.x, p.y, p.z));
        JPH::IndexedTriangleList triangles;
        list.push_back(JPH::PhysicsMaterial::sDefault); // index 0
        for (const Submesh& sm : model.meshes[meshIndex].submeshes) {
            const JPH::uint32 material = materialIndex(sm);
            for (std::uint32_t i = 0; i + 2 < sm.indexCount; i += 3) {
                const auto index = [&](std::uint32_t k) {
                    return static_cast<JPH::uint32>(static_cast<std::int64_t>(model.collisionIndices[sm.firstIndex + i + k]) +
                                                    sm.vertexOffset);
                };
                triangles.push_back(JPH::IndexedTriangle(index(0), index(1), index(2), material));
            }
        }
        JPH::MeshShapeSettings meshSettings(std::move(vertices), std::move(triangles), std::move(list));
        const JPH::ShapeSettings::ShapeResult result = meshSettings.Create();
        if (result.HasError()) {
            ENGINE_WARN("Physics: mesh collider for '{}' mesh {} failed: {}", model.name, meshIndex, result.GetError().c_str());
            return nullptr;
        }
        return meshShapes[key] = result.Get();
    }

    JPH::RefConst<JPH::Shape> BuildShape(const Collider& c, const glm::vec3& scale, const Model* model,
                                         ModelHandle handle, std::uint32_t meshIndex, std::uint32_t revision)
    {
        const ScaledCollider    s        = Scaled(c, scale);
        const SurfaceMaterial*  material = Material(c.material);
        JPH::RefConst<JPH::Shape> shape;
        switch (c.shape) {
        case ColliderShape::Box: {
            const float convex = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({s.halfExtents.x, s.halfExtents.y, s.halfExtents.z}));
            shape = new JPH::BoxShape(ToJolt(s.halfExtents), convex, material);
            break;
        }
        case ColliderShape::Sphere: shape = new JPH::SphereShape(s.radius, material); break;
        case ColliderShape::Capsule:
            shape = s.halfHeight > kMinExtent ? JPH::RefConst<JPH::Shape>(new JPH::CapsuleShape(s.halfHeight, s.radius, material))
                                              : JPH::RefConst<JPH::Shape>(new JPH::SphereShape(s.radius, material));
            break;
        case ColliderShape::Mesh: {
            if (!model)
                return nullptr;
            shape = MeshShapeFor(*model, handle, meshIndex, revision, c);
            if (!shape)
                return nullptr;
            if (Rescaled(scale, glm::vec3(1.0f)))
                shape = new JPH::ScaledShape(shape, ToJolt(scale));
            break;
        }
        }
        const bool rotated = c.shape != ColliderShape::Mesh && Rotated(glm::normalize(c.rotation), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        if (rotated || glm::any(glm::notEqual(s.center, glm::vec3(0.0f))))
            shape = new JPH::RotatedTranslatedShape(ToJolt(s.center), rotated ? ToJolt(glm::normalize(c.rotation)) : JPH::Quat::sIdentity(),
                                                    shape);
        return shape;
    }

    // --- Bodies ---

    void SyncAll(Scene& scene, bool stepping); // stepping: kinematic moves in Step

    // False: not possible yet (mesh collider without a ready model).
    bool CreateBody(BodyRecord& r, const Pose& pose, const Model* model)
    {
        const JPH::RefConst<JPH::Shape> shape = BuildShape(r.collider, pose.scale, model, r.model, r.meshIndex, r.revision);
        if (!shape)
            return false;

        const JPH::EMotionType motion = r.type == BodyType::Static      ? JPH::EMotionType::Static
                                        : r.type == BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                                        : JPH::EMotionType::Dynamic;
        JPH::BodyCreationSettings bodySettings(shape, JPH::RVec3(ToJolt(pose.position)), ToJolt(pose.rotation), motion,
                                           Layers::Make(r.collider.layer, r.type != BodyType::Static));
        bodySettings.mMotionQuality = r.type == BodyType::Dynamic && r.body.continuous ? JPH::EMotionQuality::LinearCast
                                                                                   : JPH::EMotionQuality::Discrete;
        bodySettings.mUserData       = Key(r.entity);
        const SurfaceMaterial* material = Material(r.collider.material);
        bodySettings.mFriction       = material ? material->friction : r.collider.friction;
        bodySettings.mRestitution    = material ? material->restitution : r.collider.restitution;
        bodySettings.mIsSensor       = r.collider.trigger;
        bodySettings.mLinearDamping  = r.body.linearDamping;
        bodySettings.mAngularDamping = r.body.angularDamping;
        bodySettings.mGravityFactor  = r.body.gravityFactor;
        bodySettings.mAllowSleeping  = r.body.allowSleeping;
        if (r.type == BodyType::Dynamic) {
            bodySettings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = std::max(r.body.mass, 1e-3f);
        }
        r.id = Bodies().CreateAndAddBody(bodySettings, r.type == BodyType::Static ? JPH::EActivation::DontActivate
                                                                               : JPH::EActivation::Activate);
        if (r.id.IsInvalid()) {
            ENGINE_WARN("Physics: body limit reached");
            return false;
        }
        r.scale       = pose.scale;
        r.last        = pose;
        r.simPrevious = r.simCurrent = pose;
        r.between     = false;
        r.teleported  = visit;
        r.materialGeneration = materialGeneration;
        return true;
    }

    // Removes the body (and the joints on it first); its contacts end in EndRemovedPairs (events are
    // sent even if the entity is gone).
    void RemoveBody(const BodyRecord& r)
    {
        RemoveJointsOf(r.id);
        EndPairsOf(r.id);
        DestroyBody(r.id);
    }

    void EndPairsOf(JPH::BodyID id) { removedBodies.insert(id.GetIndexAndSequenceNumber()); }

    // One sweep over the pairs for all bodies removed by this Sync.
    void EndRemovedPairs()
    {
        if (removedBodies.empty())
            return;
        for (auto it = pairs.begin(); it != pairs.end();) {
            const auto a = static_cast<JPH::uint32>(it->first >> 32), b = static_cast<JPH::uint32>(it->first);
            if (removedBodies.contains(a) || removedBodies.contains(b)) {
                pendingEvents.push_back({it->second.a, it->second.b, false, it->second.trigger, it->second.last});
                it = pairs.erase(it);
            } else {
                ++it;
            }
        }
        removedBodies.clear();
    }

    void SyncBody(Scene& scene, Entity e, const RigidBody& body, const Collider& collider,
                  bool stepping)
    {
        const Registry& registry = scene.GetRegistry();
        const Pose      pose     = Decompose(registry.Get<WorldTransform>(e).matrix);

        // Degenerate scales (near-zero, NaN/Inf) would poison Jolt with non-finite shape data:
        // drop the body until the scale is valid again (retried every Sync, like pending meshes).
        if (!ValidPhysicsScale(pose.scale)) {
            if (auto it = bodies.find(Key(e)); it != bodies.end()) {
                RemoveBody(it->second);
                bodies.erase(it);
                ++stats.removed;
            }
            if (invalidScale.insert(Key(e)).second)
                ENGINE_WARN("Physics: refusing body for entity {} with invalid scale", static_cast<std::uint64_t>(e));
            return;
        }
        invalidScale.erase(Key(e));

        const MeshRenderer* renderer = collider.shape == ColliderShape::Mesh ? registry.TryGet<MeshRenderer>(e) : nullptr;
        const Model*        model    = renderer && assets ? assets->Get(renderer->model) : nullptr;
        if (model && renderer->meshIndex >= model->meshes.size())
            model = nullptr;
        const ModelHandle   handle    = renderer ? renderer->model : ModelHandle{};
        const std::uint32_t meshIndex = renderer ? renderer->meshIndex : 0;
        const std::uint32_t revision  = renderer && assets ? assets->Revision(handle) : 0;
        BodyType type = collider.shape == ColliderShape::Mesh && body.type == BodyType::Dynamic ? BodyType::Static : body.type;
        if (registry.Has<RagdollBone>(e) && collider.shape != ColliderShape::Mesh) // follows its ragdoll
            if (const Ragdoll* ragdoll = RagdollOf(registry, e))
                type = ragdoll->simulate ? BodyType::Dynamic : BodyType::Kinematic;
        const bool usesMaterials = !collider.material.empty() || !collider.meshMaterials.empty();

        glm::vec3 carriedLinear{0.0f}, carriedAngular{0.0f}; // a remade moving body keeps its motion
        auto it = bodies.find(Key(e));
        if (it != bodies.end()) {
            BodyRecord& r      = it->second;
            const bool  remake = r.body != body || r.collider != collider || r.type != type || r.model != handle ||
                                r.meshIndex != meshIndex || r.revision != revision || Rescaled(r.scale, pose.scale) ||
                                (collider.shape == ColliderShape::Mesh && !model) ||
                                (usesMaterials && r.materialGeneration != materialGeneration);
            if (!remake) {
                r.visit = visit;
                if (Moved(pose.position, r.last.position) || Rotated(pose.rotation, r.last.rotation))
                    Teleport(r, pose, stepping);
                return;
            }
            if (r.type != BodyType::Static) {
                carriedLinear  = ToGlm(Bodies().GetLinearVelocity(r.id));
                carriedAngular = ToGlm(Bodies().GetAngularVelocity(r.id));
            }
            RemoveBody(r);
            bodies.erase(it);
            ++stats.removed;
        }

        BodyRecord r;
        r.entity    = e;
        r.body      = body;
        r.collider  = collider;
        r.type      = type;
        r.model     = handle;
        r.meshIndex = meshIndex;
        r.revision  = revision;
        r.visit     = visit;
        if (collider.shape == ColliderShape::Mesh && !model) {
            ++stats.pendingMeshes; // model loading (or missing): retried every Sync
            return;
        }
        if (CreateBody(r, pose, model)) {
            if (type == BodyType::Dynamic && (carriedLinear != glm::vec3(0.0f) || carriedAngular != glm::vec3(0.0f)))
                Bodies().SetLinearAndAngularVelocity(r.id, ToJolt(carriedLinear), ToJolt(carriedAngular));
            bodies.emplace(Key(e), r);
            ++stats.created;
        }
    }

    // The entity was moved from outside (editor, game code).
    void Teleport(BodyRecord& r, const Pose& pose, bool stepping)
    {
        if (!(stepping && r.type == BodyType::Kinematic)) // kinematic motion during play keeps joints
            r.teleported = visit;
        r.last        = pose;
        r.simPrevious = r.simCurrent = pose; // no interpolation across a teleport
        r.between     = false;
        const JPH::RVec3 position(ToJolt(pose.position));
        const JPH::Quat  rotation = ToJolt(pose.rotation);
        switch (r.type) {
        case BodyType::Kinematic:
            if (stepping) {
                r.kinematicMove = true; // moved with velocity in Step: pushes dynamic bodies
                return;
            }
            Bodies().SetPositionAndRotation(r.id, position, rotation, JPH::EActivation::DontActivate);
            break;
        case BodyType::Dynamic:
            Bodies().SetPositionAndRotation(r.id, position, rotation, JPH::EActivation::Activate);
            Bodies().SetLinearAndAngularVelocity(r.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            break;
        case BodyType::Static: {
            // Wake what rests on it, before and after the move.
            JPH::AABox bounds = Bodies().GetTransformedShape(r.id).GetWorldSpaceBounds();
            Bodies().SetPositionAndRotation(r.id, position, rotation, JPH::EActivation::DontActivate);
            bounds.Encapsulate(Bodies().GetTransformedShape(r.id).GetWorldSpaceBounds());
            bounds.ExpandBy(JPH::Vec3::sReplicate(0.05f));
            Bodies().ActivateBodiesInAABox(bounds, {}, {}); // any layer
            break;
        }
        }
    }

    // --- Characters ---

    // Capsule of a character's height with the feet at the origin.
    static JPH::RefConst<JPH::Shape> CharacterShape(float radius, float height)
    {
        radius                 = std::max(radius, 0.01f);
        const float halfHeight = std::max(height * 0.5f - radius, 0.01f); // cylinder part
        return new JPH::RotatedTranslatedShape(JPH::Vec3(0.0f, halfHeight + radius, 0.0f), JPH::Quat::sIdentity(),
                                               new JPH::CapsuleShape(halfHeight, radius));
    }

    void SyncCharacter(Scene& scene, Entity e, const CharacterController& cc)
    {
        const glm::mat4& world    = scene.GetRegistry().Get<WorldTransform>(e).matrix;
        const glm::vec3  position = world[3];
        auto             it       = characters.find(Key(e));
        if (it != characters.end() && it->second.settings == cc) {
            CharacterRecord& r = it->second;
            r.visit            = visit;
            if (Moved(position, r.lastPosition)) {
                r.character->SetPosition(JPH::RVec3(ToJolt(position)));
                r.character->SetLinearVelocity(JPH::Vec3::sZero());
                r.lastPosition = r.simPrevious = r.simCurrent = position;
                r.between = false;
            }
            const float entityYaw = YawOf(Decompose(world).rotation);
            if (std::abs(WrapAngle(entityYaw - r.lastYaw)) > 1e-4f) { // turned from outside: start from there
                r.yaw = r.simPreviousYaw = r.simCurrentYaw = r.lastYaw = entityYaw;
                r.character->SetRotation(ToJolt(YawRotation(r.yaw)));
            }
            return;
        }
        glm::vec3 input{0.0f};
        bool      wantCrouch = false;
        float     targetYaw  = YawOf(Decompose(world).rotation);
        if (it != characters.end()) {
            input      = it->second.input;
            wantCrouch = it->second.wantCrouch;
            targetYaw  = it->second.targetYaw;
            EndCharacterContacts(it->second);
            EndPairsOf(it->second.character->GetInnerBodyID());
            innerBodies.erase(it->second.character->GetInnerBodyID().GetIndexAndSequenceNumber());
            characters.erase(it);
            ++stats.removed;
        }

        CharacterRecord r;
        r.standing = CharacterShape(cc.radius, cc.height);
        r.crouched = CharacterShape(cc.radius, std::clamp(cc.crouchHeight, 2.0f * std::max(cc.radius, 0.01f), cc.height));
        const float radius = std::max(cc.radius, 0.01f);

        JPH::CharacterVirtualSettings characterSettings;
        characterSettings.mShape            = r.standing;
        characterSettings.mMaxSlopeAngle    = cc.maxSlope;
        characterSettings.mMass             = std::max(cc.mass, 1e-3f);
        characterSettings.mMaxStrength      = std::max(cc.pushStrength, 0.0f);
        characterSettings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius); // contacts below the lower hemisphere center
        characterSettings.mInnerBodyShape   = r.standing; // lets rigid bodies and queries see the character
        characterSettings.mInnerBodyLayer   = Layers::kMoving; // user layer 0

        r.entity       = e;
        r.settings     = cc;
        r.lastPosition = r.simPrevious = r.simCurrent = position;
        r.yaw = r.simPreviousYaw = r.simCurrentYaw = r.lastYaw = YawOf(Decompose(world).rotation);
        r.targetYaw    = targetYaw;
        r.between      = false;
        r.input        = input;
        r.wantCrouch   = wantCrouch;
        r.visit        = visit;
        r.character = new JPH::CharacterVirtual(&characterSettings, JPH::RVec3(ToJolt(position)), ToJolt(YawRotation(r.yaw)),
                                                Key(e), system.get());
        innerBodies.insert(r.character->GetInnerBodyID().GetIndexAndSequenceNumber());
        characters.emplace(Key(e), std::move(r));
        ++stats.created;
    }

    // Contact as seen from the character (a) towards the other body (b).
    ContactInfo CharacterContactInfo(const CharacterRecord& r, const JPH::CharacterContact& c) const
    {
        ContactInfo info;
        info.point            = ToGlm(JPH::Vec3(c.mPosition));
        info.normal           = -ToGlm(c.mContactNormal); // the contact normal points towards the character
        info.relativeVelocity = ToGlm(c.mLinearVelocity - r.character->GetLinearVelocity());
        info.approachSpeed    = std::max(-glm::dot(info.relativeVelocity, info.normal), 0.0f);
        float inverse         = 1.0f / std::max(r.settings.mass, 1e-3f);
        if (c.mMotionTypeB == JPH::EMotionType::Dynamic) {
            JPH::BodyLockRead lock(system->GetBodyLockInterfaceNoLock(), c.mBodyB);
            if (lock.Succeeded() && lock.GetBody().IsDynamic())
                inverse += lock.GetBody().GetMotionProperties()->GetInverseMass();
        }
        info.impulse  = info.approachSpeed / inverse;
        info.surfaceB = SurfaceName(c.mMaterial);
        return info;
    }

    // Character contacts (CharacterVirtual does not go through the contact listener): diff of the
    // touched bodies after each update.
    void UpdateCharacterContacts(bool persist)
    {
        for (auto& [key, r] : characters) {
            std::unordered_map<std::uint32_t, CharacterRecord::Touch> now;
            for (const JPH::CharacterContact& c : r.character->GetActiveContacts()) {
                if (c.mBodyB.IsInvalid() || !(c.mHadCollision || c.mIsSensorB))
                    continue;
                now.try_emplace(c.mBodyB.GetIndexAndSequenceNumber(),
                                CharacterRecord::Touch{Entity{c.mUserData}, c.mIsSensorB, CharacterContactInfo(r, c)});
            }
            for (const auto& [id, touch] : now) {
                if (!r.touching.contains(id))
                    pendingEvents.push_back({r.entity, touch.other, true, touch.trigger, touch.contact});
                else if (persist)
                    pendingPersist.push_back({r.entity, touch.other, touch.trigger, touch.contact});
            }
            for (const auto& [id, touch] : r.touching)
                if (!now.contains(id))
                    pendingEvents.push_back({r.entity, touch.other, false, touch.trigger, touch.contact});
            r.touching = std::move(now);
        }
    }

    void EndCharacterContacts(CharacterRecord& r)
    {
        for (const auto& [id, touch] : r.touching)
            pendingEvents.push_back({r.entity, touch.other, false, touch.trigger, touch.contact});
        r.touching.clear();
    }

    // Yaw the Camera rotation mode turns towards (nullopt: no primary camera).
    static std::optional<std::pair<Entity, float>> CameraYaw(Scene& scene)
    {
        const Entity camera = scene.FindPrimaryCamera();
        if (camera == NullEntity)
            return std::nullopt;
        return std::pair{camera, YawOf(Decompose(scene.GetRegistry().Get<WorldTransform>(camera).matrix).rotation)};
    }

    void StepCharacters(Scene& scene, float dt, const glm::vec3& gravity, float airControl)
    {
        const JPH::Vec3 g = ToJolt(gravity);
        const auto camera = CameraYaw(scene);
        const auto broadPhase = system->GetDefaultBroadPhaseLayerFilter(Layers::kMoving);
        const auto layers     = system->GetDefaultLayerFilter(Layers::kMoving);
        for (auto& [key, r] : characters) {
            JPH::CharacterVirtual& c = *r.character;

            // Crouch / stand up: the new capsule must fit (standing up waits for room above).
            if (r.wantCrouch != r.crouching) {
                const JPH::RefConst<JPH::Shape>& shape = r.wantCrouch ? r.crouched : r.standing;
                if (c.SetShape(shape, 1.5f * system->GetPhysicsSettings().mPenetrationSlop, broadPhase, layers, {}, {},
                               *tempAllocator)) {
                    c.SetInnerBodyShape(shape);
                    r.crouching = r.wantCrouch;
                }
            }

            c.UpdateGroundVelocity();
            const JPH::Vec3 ground   = r.settings.movingPlatforms ? c.GetGroundVelocity() : JPH::Vec3::sZero();
            const JPH::Vec3 current  = c.GetLinearVelocity();
            const JPH::Vec3 desired(r.input.x, 0.0f, r.input.z);
            const auto      state    = c.GetGroundState();
            const bool      grounded = state == JPH::CharacterBase::EGroundState::OnGround &&
                                  (current - c.GetGroundVelocity()).GetY() < 0.1f; // not moving away (jump start)
            const bool      holdOnSteep = state == JPH::CharacterBase::EGroundState::OnSteepGround && !r.settings.slideOnSteepSlopes;
            JPH::Vec3 velocity;
            if (grounded || holdOnSteep) {
                velocity = ground + desired;
                if (r.jump && grounded)
                    velocity += JPH::Vec3(0.0f, r.settings.jumpSpeed, 0.0f);
            } else {
                const JPH::Vec3 horizontal(current.GetX(), 0.0f, current.GetZ());
                velocity = horizontal + (desired - horizontal) * std::min(airControl * dt, 1.0f) +
                           JPH::Vec3(0.0f, current.GetY(), 0.0f);
            }
            r.jump = false;
            if (!holdOnSteep)
                velocity += g * dt;
            c.SetLinearVelocity(velocity);

            // Rotation about +Y (yaw) by its mode; standing on a turning platform turns it along.
            if (r.settings.rotation != CharacterRotation::None) {
                if (grounded && r.settings.movingPlatforms && !c.GetGroundBodyID().IsInvalid())
                    r.yaw += Bodies().GetAngularVelocity(c.GetGroundBodyID()).GetY() * dt;
                std::optional<float> target;
                switch (r.settings.rotation) {
                case CharacterRotation::Movement:
                    if (r.input.x * r.input.x + r.input.z * r.input.z > 1e-4f)
                        target = std::atan2(-r.input.x, -r.input.z);
                    break;
                case CharacterRotation::Camera:
                    if (camera && !scene.IsAncestor(r.entity, camera->first)) // its own camera would turn with it
                        target = camera->second;
                    break;
                case CharacterRotation::Script: target = r.targetYaw; break;
                case CharacterRotation::None: break;
                }
                if (target) {
                    const float diff    = WrapAngle(*target - r.yaw);
                    const float maxTurn = glm::radians(r.settings.turnSpeed) * dt;
                    r.yaw += r.settings.turnSpeed > 0.0f ? std::clamp(diff, -maxTurn, maxTurn) : diff;
                }
                r.yaw = WrapAngle(r.yaw);
                c.SetRotation(ToJolt(YawRotation(r.yaw)));
            }

            JPH::CharacterVirtual::ExtendedUpdateSettings update;
            update.mWalkStairsStepUp     = JPH::Vec3(0.0f, r.settings.stepHeight, 0.0f);
            update.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(r.settings.stepHeight, 0.1f), 0.0f);
            c.ExtendedUpdate(dt, g, update, broadPhase, layers, {}, {}, *tempAllocator);
        }
    }

    // --- Events ---

    void ProcessContacts(bool persist)
    {
        struct Accum {
            JPH::BodyID          a, b;
            int                  delta   = 0;
            bool                 trigger = false;
            const ContactSample* added   = nullptr; // strongest new contact (impulse)
            const ContactSample* touched = nullptr; // deepest contact this step (added or persisted)
        };
        const std::vector<ContactSample> samples = contacts.Take();
        std::unordered_map<std::uint64_t, Accum> accum;
        for (const ContactSample& d : samples) {
            Accum& acc = accum.try_emplace(PairKey(d.a, d.b), Accum{d.a, d.b}).first->second;
            acc.delta += d.delta;
            acc.trigger |= d.trigger;
            if (d.delta > 0 && (!acc.added || d.impulse > acc.added->impulse))
                acc.added = &d;
            if (d.delta >= 0 && (!acc.touched || d.depth > acc.touched->depth))
                acc.touched = &d;
        }
        const auto entityOf = [&](JPH::BodyID id) {
            return Bodies().IsAdded(id) ? Entity{Bodies().GetUserData(id)} : NullEntity;
        };
        const auto sleeping = [&](JPH::BodyID id) {
            return Bodies().IsAdded(id) && Bodies().GetMotionType(id) != JPH::EMotionType::Static && !Bodies().IsActive(id);
        };

        for (const auto& [key, acc] : accum) {
            // Characters report their own contacts (UpdateCharacterContacts).
            if (innerBodies.contains(acc.a.GetIndexAndSequenceNumber()) || innerBodies.contains(acc.b.GetIndexAndSequenceNumber()))
                continue;
            auto it = pairs.find(key);
            if (it == pairs.end()) {
                if (acc.delta <= 0)
                    continue; // removal of a pair that already ended (body removed)
                const Entity a = entityOf(acc.a), b = entityOf(acc.b);
                const ContactSample* sample = acc.added ? acc.added : acc.touched;
                const ContactInfo    info   = sample ? ToInfo(*sample) : ContactInfo{};
                pairs.emplace(key, Pair{a, b, acc.delta, acc.trigger, false, info});
                pendingEvents.push_back({a, b, true, acc.trigger, info});
                continue;
            }
            Pair& pair = it->second;
            pair.count += acc.delta;
            pair.trigger |= acc.trigger;
            if (pair.count > 0) {
                pair.suspended = false; // woke up and still touching: no new Begin
                if (acc.touched) {
                    pair.last = ToInfo(*acc.touched);
                    if (persist)
                        pendingPersist.push_back({pair.a, pair.b, pair.trigger, pair.last});
                }
            } else if (sleeping(acc.a) || sleeping(acc.b)) {
                pair.count     = 0;
                pair.suspended = true; // contacts dropped because the bodies sleep
            } else {
                pendingEvents.push_back({pair.a, pair.b, false, pair.trigger, pair.last});
                pairs.erase(it);
            }
        }
        // Suspended pairs whose bodies woke up without touching again.
        for (auto it = pairs.begin(); it != pairs.end();) {
            Pair& pair = it->second;
            if (pair.suspended && !accum.contains(it->first)) {
                const JPH::BodyID a(static_cast<JPH::uint32>(it->first >> 32)), b(static_cast<JPH::uint32>(it->first));
                if (!sleeping(a) && !sleeping(b)) {
                    pendingEvents.push_back({pair.a, pair.b, false, pair.trigger, pair.last});
                    it = pairs.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    void PublishEvents()
    {
        const std::vector<CollisionEvent> events_ = std::exchange(pendingEvents, {});
        for (const CollisionEvent& e : events_)
            events.Publish(e);
        const std::vector<CollisionPersistEvent> persisted = std::exchange(pendingPersist, {});
        for (const CollisionPersistEvent& e : persisted)
            events.Publish(e);
        const std::vector<JointBrokenEvent> broken = std::exchange(pendingBroken, {});
        for (const JointBrokenEvent& e : broken)
            events.Publish(e);
    }

    // --- Joints ---

    void SyncJoints(Scene& scene, bool stepping);
    void SyncJoint(Scene& scene, Entity e, const Joint& joint, bool stepping);
    bool CreateJoint(JointRecord& record, const Pose& frame);
    void ApplyMotor(JointRecord& record);
    void CheckBrokenJoints(Scene& scene, float dt, int collisionSteps);

    void RemoveJoint(JointRecord& record)
    {
        if (record.constraint) {
            system->RemoveConstraint(record.constraint);
            for (const JPH::BodyID id : {record.a, record.b}) // what it held may fall now
                if (!id.IsInvalid() && Bodies().IsAdded(id) && Bodies().GetMotionType(id) == JPH::EMotionType::Dynamic)
                    Bodies().ActivateBody(id);
            record.constraint = nullptr;
        }
        if (record.noCollision) {
            const auto it = groupFilter->disabled.find(JointGroupFilter::PairOf(record.a.GetIndexAndSequenceNumber(),
                                                                                record.b.GetIndexAndSequenceNumber()));
            if (it != groupFilter->disabled.end() && --it->second <= 0)
                groupFilter->disabled.erase(it);
            record.noCollision = false;
        }
    }

    void RemoveJointsOf(JPH::BodyID id)
    {
        for (auto& [key, record] : joints)
            if (record.constraint && (record.a == id || record.b == id))
                RemoveJoint(record);
    }

    // --- Write-back ---

    struct PoseWrite {
        Entity                   entity = NullEntity;
        glm::vec3                position{0.0f};
        std::optional<glm::quat> rotation; // characters keep theirs
    };

    // World poses -> local transforms, parents before children and each against its parent's new
    // world (a dynamic child of a moving body follows it). Then remembers what the scene shows,
    // so Sync does not mistake the write-back for a teleport.
    void WritePoses(Scene& scene, const std::vector<PoseWrite>& writes)
    {
        if (writes.empty())
            return;
        Registry&  registry = scene.GetRegistry();
        const auto parentOf = [&](Entity e) {
            const Entity p = registry.Get<Hierarchy>(e).parent;
            return p != NullEntity && registry.Valid(p) ? p : NullEntity;
        };
        std::vector<std::pair<int, std::size_t>> order; // depth, index
        for (std::size_t i = 0; i < writes.size(); ++i) {
            int depth = 0;
            for (Entity p = parentOf(writes[i].entity); p != NullEntity; p = parentOf(p))
                ++depth;
            order.emplace_back(depth, i);
        }
        std::ranges::sort(order);

        std::unordered_map<std::uint64_t, glm::mat4> fresh; // new world matrices written so far
        const auto worldOf = [&](Entity e) {
            std::vector<Entity> chain; // e and its ancestors up to one with a new world
            glm::mat4           world{1.0f};
            for (Entity x = e; x != NullEntity; x = parentOf(x)) {
                if (const auto it = fresh.find(Key(x)); it != fresh.end()) {
                    world = it->second;
                    break;
                }
                chain.push_back(x);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                world = world * registry.Get<Transform>(*it).LocalMatrix();
            return world;
        };
        for (const auto& [depth, i] : order) {
            const PoseWrite& w      = writes[i];
            const Entity     parent = parentOf(w.entity);
            const glm::mat4 parentWorld = parent != NullEntity ? worldOf(parent) : glm::mat4(1.0f);
            if (!Finite(w.position) || (w.rotation && !Finite(*w.rotation)) ||
                !Finite(parentWorld[0]) || !Finite(parentWorld[1]) || !Finite(parentWorld[2]) ||
                !Finite(parentWorld[3])) {
                ENGINE_WARN("Physics: refusing invalid pose write for entity {}", static_cast<std::uint64_t>(w.entity));
                continue;
            }
            const float determinant = glm::determinant(parentWorld);
            if (parent != NullEntity && std::abs(determinant) < 1e-8f) {
                ENGINE_WARN("Physics: refusing pose write through singular parent for entity {}",
                            static_cast<std::uint64_t>(w.entity));
                continue;
            }
            Transform& t = scene.EditTransform(w.entity);
            t.position = glm::vec3(glm::inverse(parentWorld) * glm::vec4(w.position, 1.0f));
            if (w.rotation)
                t.rotation = parent != NullEntity ? glm::normalize(glm::conjugate(Decompose(parentWorld).rotation) * *w.rotation)
                                                  : *w.rotation;
            if (!Finite(t.position) || !Finite(t.rotation)) {
                ENGINE_WARN("Physics: refusing non-finite local pose for entity {}", static_cast<std::uint64_t>(w.entity));
                continue;
            }
            fresh[Key(w.entity)] = parentWorld * t.LocalMatrix();
        }

        scene.UpdateTransforms();
        for (const PoseWrite& w : writes) {
            const Pose pose = Decompose(registry.Get<WorldTransform>(w.entity).matrix);
            if (auto it = bodies.find(Key(w.entity)); it != bodies.end()) {
                it->second.last.position = pose.position;
                it->second.last.rotation = pose.rotation;
            } else if (auto c = characters.find(Key(w.entity)); c != characters.end()) {
                c->second.lastPosition = pose.position;
                c->second.lastYaw      = YawOf(pose.rotation);
            }
        }
    }

    // --- State ---

    EventBus&                                    events;
    const AssetManager*                          assets;
    BroadPhaseLayerMap                           broadPhaseLayers;
    std::array<std::uint16_t, kPhysicsLayers>    layerMatrix{}; // settings.layerCollision, copied per Step / Sync
    ObjectVsBroadPhaseFilter                     objectVsBroadPhase;
    ObjectPairFilter                             objectPairs{layerMatrix};
    std::unordered_set<std::uint32_t>            innerBodies; // BodyID index + sequence of character inner bodies
    ContactRecorder                              contacts;
    std::unique_ptr<JPH::TempAllocatorImpl>      tempAllocator;
    std::unique_ptr<EngineJobSystem>             jobSystem;
    std::unique_ptr<JPH::PhysicsSystem>          system;
    std::unordered_map<std::uint64_t, BodyRecord>      bodies;     // by entity
    std::unordered_map<std::uint64_t, CharacterRecord> characters; // by entity
    std::unordered_map<std::uint64_t, Pair>            pairs;      // by body pair
    // model index, generation, mesh, revision, materials (MaterialKey + generation)
    using MeshKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::string>;
    std::map<MeshKey, JPH::RefConst<JPH::Shape>> meshShapes; // unscaled
    std::unordered_map<std::uint64_t, JointRecord>   joints;      // by joint entity
    JPH::Ref<JointGroupFilter>                       groupFilter; // Jolt objects: created after AcquireJolt
    std::unordered_set<std::uint64_t>                jointWarned; // joint entities warned about (invalid setup)
    std::unordered_map<std::string, MaterialEntry>   materials;   // by path
    std::chrono::steady_clock::time_point            nextMaterialCheck{};
    std::uint32_t                                    materialGeneration = 1;
    std::unordered_set<JPH::uint32>              removedBodies; // since the last EndRemovedPairs
    std::unordered_set<std::uint64_t>            invalidScale; // entities refused for a degenerate scale (warned once)
    std::vector<CollisionEvent>                  pendingEvents;
    std::vector<CollisionPersistEvent>           pendingPersist;
    std::vector<JointBrokenEvent>                pendingBroken;
    PhysicsStats                                 stats;
    std::uint32_t                                visit = 0;
};

// ---------------------------------------------------------------------------------------------

Collider FitCollider(ColliderShape shape, const glm::vec3& boundsMin, const glm::vec3& boundsMax)
{
    Collider        c;
    const glm::vec3 half = glm::max((boundsMax - boundsMin) * 0.5f, glm::vec3(kMinExtent));
    c.shape              = shape;
    c.center             = (boundsMin + boundsMax) * 0.5f;
    switch (shape) {
    case ColliderShape::Box: c.halfExtents = half; break;
    case ColliderShape::Sphere: c.radius = std::max({half.x, half.y, half.z}); break;
    case ColliderShape::Capsule:
        c.radius     = std::max(half.x, half.z);
        c.halfHeight = std::max(half.y - c.radius, 0.0f);
        break;
    case ColliderShape::Mesh: c.center = glm::vec3(0.0f); break;
    }
    return c;
}

PhysicsWorld::PhysicsWorld(ThreadPool& threads, EventBus& events, const AssetManager* assets)
    : m_Impl(std::make_unique<Impl>(threads, events, assets))
{
}

PhysicsWorld::~PhysicsWorld() = default;


// --- Joints -------------------------------------------------------------------------------------

namespace {

glm::mat4 ToGlm(const JPH::RMat44& m)
{
    glm::mat4 out(1.0f);
    for (int c = 0; c < 3; ++c) {
        const JPH::Vec3 column = m.GetColumn3(static_cast<JPH::uint>(c));
        out[c] = glm::vec4(column.GetX(), column.GetY(), column.GetZ(), 0.0f);
    }
    const JPH::RVec3 t = m.GetTranslation();
    out[3] = glm::vec4(static_cast<float>(t.GetX()), static_cast<float>(t.GetY()), static_cast<float>(t.GetZ()), 1.0f);
    return out;
}

bool Differs(const glm::mat4& a, const glm::mat4& b)
{
    for (int c = 0; c < 4; ++c)
        if (glm::any(glm::greaterThan(glm::abs(a[c] - b[c]), glm::vec4(1e-4f))))
            return true;
    return false;
}

JPH::MotorSettings MotorFor(const JointMotor& m)
{
    JPH::MotorSettings motor(std::max(m.frequency, 0.01f), std::max(m.damping, 0.0f));
    motor.SetForceLimit(std::max(m.maxForce, 0.0f));
    motor.SetTorqueLimit(std::max(m.maxForce, 0.0f));
    return motor;
}

JPH::EMotorState MotorState(JointMotorMode mode)
{
    switch (mode) {
    case JointMotorMode::Velocity: return JPH::EMotorState::Velocity;
    case JointMotorMode::Position: return JPH::EMotorState::Position;
    case JointMotorMode::Off: break;
    }
    return JPH::EMotorState::Off;
}

// World anchor points of a joint (center of mass transform * constraint frame).
JPH::RMat44 FrameOn(const JPH::Body& body, const JPH::Mat44& constraintToBody)
{
    return body.GetCenterOfMassTransform() * constraintToBody;
}

} // namespace

void PhysicsWorld::Impl::SyncJoints(Scene& scene, bool stepping)
{
    stats.pendingJoints = 0;
    scene.GetRegistry().ViewOf<Joint>().Each([&](Entity e, Joint& joint) { SyncJoint(scene, e, joint, stepping); });
    for (auto it = joints.begin(); it != joints.end();) {
        if (it->second.visit != visit) {
            RemoveJoint(it->second);
            it = joints.erase(it);
        } else {
            ++it;
        }
    }
}

void PhysicsWorld::Impl::SyncJoint(Scene& scene, Entity e, const Joint& joint, bool stepping)
{
    JointRecord& record = joints[Key(e)];
    record.visit        = visit;
    record.entity       = e;
    if (!joint.enabled) {
        RemoveJoint(record);
        record.joint = joint;
        return;
    }
    const Registry& registry = scene.GetRegistry();
    const Entity    a        = joint.ownerBody ? scene.FindByUuid(joint.ownerBody) : e;
    const Entity    b        = joint.connectedBody ? scene.FindByUuid(joint.connectedBody) : NullEntity;
    const auto      ia       = a != NullEntity ? bodies.find(Key(a)) : bodies.end();
    const auto      ib       = b != NullEntity ? bodies.find(Key(b)) : bodies.end();
    if (ia == bodies.end() || (joint.connectedBody != 0 && ib == bodies.end())) {
        ++stats.pendingJoints; // a body is missing (loading mesh, not streamed in, no RigidBody)
        RemoveJoint(record);
        return;
    }
    if (a == b) {
        if (jointWarned.insert(Key(e)).second)
            ENGINE_WARN("Physics: joint on entity {} connects a body to itself", static_cast<std::uint64_t>(e));
        RemoveJoint(record);
        return;
    }
    const BodyRecord& ra  = ia->second;
    const JPH::BodyID idA = ra.id;
    const JPH::BodyID idB = ib != bodies.end() ? ib->second.id : JPH::BodyID();
    const Pose        frame    = JointFrame(registry.Get<WorldTransform>(e).matrix, joint);
    const glm::mat4   relative = glm::inverse(PoseMatrix(ra.last)) * PoseMatrix(frame);

    if (record.constraint) {
        Joint sameMotor = joint;
        sameMotor.motor = record.joint.motor;
        const bool keep = sameMotor == record.joint && record.a == idA && record.b == idB && ra.teleported != visit &&
                          (ib == bodies.end() || ib->second.teleported != visit) &&
                          (stepping || !Differs(record.relative, relative)); // anchors edited in edit mode
        if (keep) {
            if (record.joint.motor != joint.motor) { // motor targets apply live
                record.joint.motor = joint.motor;
                ApplyMotor(record);
            }
            return;
        }
        RemoveJoint(record);
    }
    record.joint    = joint;
    record.a        = idA;
    record.b        = idB;
    record.entityA  = a;
    record.entityB  = b;
    record.relative = relative;
    CreateJoint(record, frame);
}

bool PhysicsWorld::Impl::CreateJoint(JointRecord& record, const Pose& frame)
{
    const Joint&     j = record.joint;
    const JPH::RVec3 p(ToJolt(frame.position));
    const JPH::Vec3  x = ToJolt(glm::normalize(frame.rotation * glm::vec3(1.0f, 0.0f, 0.0f)));
    const JPH::Vec3  y = ToJolt(glm::normalize(frame.rotation * glm::vec3(0.0f, 1.0f, 0.0f)));
    constexpr float  kPi = 3.14159265f;
    const JPH::SpringSettings spring(JPH::ESpringMode::FrequencyAndDamping, std::max(j.limitSpring, 0.0f),
                                     std::max(j.limitDamping, 0.0f));
    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    switch (j.type) {
    case JointType::Fixed: {
        auto* f    = new JPH::FixedConstraintSettings();
        f->mPoint1 = f->mPoint2 = p;
        f->mAxisX1 = f->mAxisX2 = x;
        f->mAxisY1 = f->mAxisY2 = y;
        settings   = f;
        break;
    }
    case JointType::Hinge: {
        auto* h        = new JPH::HingeConstraintSettings();
        h->mPoint1     = h->mPoint2 = p;
        h->mHingeAxis1 = h->mHingeAxis2 = x;
        h->mNormalAxis1 = h->mNormalAxis2 = y;
        if (j.limits) {
            h->mLimitsMin            = std::clamp(j.minLimit, -kPi, 0.0f);
            h->mLimitsMax            = std::clamp(j.maxLimit, 0.0f, kPi);
            h->mLimitsSpringSettings = spring;
        }
        h->mMaxFrictionTorque = std::max(j.friction, 0.0f);
        h->mMotorSettings     = MotorFor(j.motor);
        settings              = h;
        break;
    }
    case JointType::Slider: {
        auto* sl         = new JPH::SliderConstraintSettings();
        sl->mPoint1      = sl->mPoint2 = p;
        sl->mSliderAxis1 = sl->mSliderAxis2 = x;
        sl->mNormalAxis1 = sl->mNormalAxis2 = y;
        if (j.limits) {
            sl->mLimitsMin            = std::min(j.minLimit, 0.0f);
            sl->mLimitsMax            = std::max(j.maxLimit, 0.0f);
            sl->mLimitsSpringSettings = spring;
        }
        sl->mMaxFrictionForce = std::max(j.friction, 0.0f);
        sl->mMotorSettings    = MotorFor(j.motor);
        settings              = sl;
        break;
    }
    case JointType::Ball: {
        auto* b    = new JPH::PointConstraintSettings();
        b->mPoint1 = b->mPoint2 = p;
        settings   = b;
        break;
    }
    case JointType::Distance: {
        // Between the anchor (on A) and B's origin; to the world: between A's origin and the anchor.
        // (Jolt's body 1 is B, body 2 is A.)
        auto* d = new JPH::DistanceConstraintSettings();
        if (!record.b.IsInvalid()) {
            d->mPoint1 = Bodies().GetPosition(record.b);
            d->mPoint2 = p;
        } else {
            d->mPoint1 = p;
            d->mPoint2 = Bodies().GetPosition(record.a);
        }
        if (j.limits) {
            d->mMinDistance = std::max(j.minLimit, 0.0f);
            d->mMaxDistance = std::max(j.maxLimit, d->mMinDistance);
        }
        d->mLimitsSpringSettings = spring;
        settings                 = d;
        break;
    }
    case JointType::Cone: {
        auto* c        = new JPH::ConeConstraintSettings();
        c->mPoint1     = c->mPoint2 = p;
        c->mTwistAxis1 = c->mTwistAxis2 = x;
        c->mHalfConeAngle = std::clamp(j.coneAngle, 0.0f, kPi);
        settings          = c;
        break;
    }
    case JointType::SwingTwist: {
        auto* st                 = new JPH::SwingTwistConstraintSettings();
        st->mPosition1           = st->mPosition2 = p;
        st->mTwistAxis1          = st->mTwistAxis2 = x;
        st->mPlaneAxis1          = st->mPlaneAxis2 = y;
        st->mNormalHalfConeAngle = std::clamp(j.coneAngle, 0.0f, kPi);
        st->mPlaneHalfConeAngle  = std::clamp(j.planeAngle, 0.0f, kPi);
        st->mTwistMinAngle       = std::clamp(j.minLimit, -kPi, 0.0f);
        st->mTwistMaxAngle       = std::clamp(j.maxLimit, 0.0f, kPi);
        st->mMaxFrictionTorque   = std::max(j.friction, 0.0f);
        st->mTwistMotorSettings  = MotorFor(j.motor);
        st->mSwingMotorSettings  = MotorFor(j.motor);
        settings                 = st;
        break;
    }
    case JointType::SixDof: {
        auto* six       = new JPH::SixDOFConstraintSettings();
        six->mPosition1 = six->mPosition2 = p;
        six->mAxisX1    = six->mAxisX2 = x;
        six->mAxisY1    = six->mAxisY2 = y;
        six->mSwingType = JPH::ESwingType::Pyramid; // asymmetric swing limits
        for (int i = 0; i < 6; ++i) {
            const auto axis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(i);
            switch (j.axes[static_cast<std::size_t>(i)]) {
            case JointAxisMode::Free: six->MakeFreeAxis(axis); break;
            case JointAxisMode::Locked: six->MakeFixedAxis(axis); break;
            case JointAxisMode::Limited: {
                float lo = j.axisMin[static_cast<std::size_t>(i)], hi = j.axisMax[static_cast<std::size_t>(i)];
                if (i >= 3) { // rotation limits within (-pi, pi), twist within its range
                    lo = std::clamp(lo, -kPi, 0.0f);
                    hi = std::clamp(hi, 0.0f, kPi);
                }
                six->SetLimitedAxis(axis, std::min(lo, hi), std::max(lo, hi));
                break;
            }
            }
            six->mMaxFriction[i] = std::max(j.friction, 0.0f);
        }
        for (int i = 0; i < 3; ++i)
            six->mLimitsSpringSettings[i] = spring;
        settings = six;
        break;
    }
    }
    // Body 1 = B (or the world), body 2 = A: angles, positions and motors are A relative to B.
    record.constraint = Bodies().CreateConstraint(settings, record.b, record.a);
    if (!record.constraint) {
        ENGINE_WARN("Physics: joint on entity {} could not be created", static_cast<std::uint64_t>(record.entity));
        return false;
    }
    system->AddConstraint(record.constraint);
    if (!j.collideConnected && !record.b.IsInvalid()) {
        for (const JPH::BodyID id : {record.a, record.b})
            Bodies().SetCollisionGroup(id, JPH::CollisionGroup(groupFilter, id.GetIndexAndSequenceNumber(), 0));
        ++groupFilter->disabled[JointGroupFilter::PairOf(record.a.GetIndexAndSequenceNumber(), record.b.GetIndexAndSequenceNumber())];
        record.noCollision = true;
    }
    ApplyMotor(record);
    Bodies().ActivateConstraint(record.constraint);
    return true;
}

void PhysicsWorld::Impl::ApplyMotor(JointRecord& record)
{
    if (!record.constraint)
        return;
    const JointMotor& m     = record.joint.motor;
    const auto        state = MotorState(m.mode);
    switch (record.joint.type) {
    case JointType::Hinge: {
        auto* h                = static_cast<JPH::HingeConstraint*>(record.constraint.GetPtr());
        h->GetMotorSettings()  = MotorFor(m);
        h->SetTargetAngularVelocity(m.target);
        h->SetTargetAngle(m.target);
        h->SetMotorState(state);
        break;
    }
    case JointType::Slider: {
        auto* sl               = static_cast<JPH::SliderConstraint*>(record.constraint.GetPtr());
        sl->GetMotorSettings() = MotorFor(m);
        sl->SetTargetVelocity(m.target);
        sl->SetTargetPosition(m.target);
        sl->SetMotorState(state);
        break;
    }
    case JointType::SwingTwist: {
        auto* st                    = static_cast<JPH::SwingTwistConstraint*>(record.constraint.GetPtr());
        st->GetTwistMotorSettings() = MotorFor(m);
        st->SetTargetAngularVelocityCS(JPH::Vec3(m.target, 0.0f, 0.0f));
        st->SetTargetOrientationCS(JPH::Quat::sRotation(JPH::Vec3::sAxisX(), m.target));
        st->SetTwistMotorState(state);
        break;
    }
    default: return; // no motor
    }
    Bodies().ActivateConstraint(record.constraint);
}

// Forces from the last sub step's impulses; joints over their break force / torque are removed.
void PhysicsWorld::Impl::CheckBrokenJoints(Scene& scene, float dt, int collisionSteps)
{
    const float subStep = dt / static_cast<float>(std::max(collisionSteps, 1));
    if (!(subStep > 0.0f))
        return;
    Registry& registry = scene.GetRegistry();
    for (auto& [key, record] : joints) {
        if (!record.constraint)
            continue;
        JPH::Constraint* c = record.constraint.GetPtr();
        glm::vec3 linear{0.0f}, angular{0.0f};
        switch (record.joint.type) {
        case JointType::Fixed: {
            auto* f = static_cast<JPH::FixedConstraint*>(c);
            linear  = ToGlm(f->GetTotalLambdaPosition());
            angular = ToGlm(f->GetTotalLambdaRotation());
            break;
        }
        case JointType::Hinge: {
            auto* h = static_cast<JPH::HingeConstraint*>(c);
            linear  = ToGlm(h->GetTotalLambdaPosition());
            const JPH::Vector<2> r = h->GetTotalLambdaRotation();
            angular = {r[0], r[1], h->GetTotalLambdaRotationLimits()};
            break;
        }
        case JointType::Slider: {
            auto* sl = static_cast<JPH::SliderConstraint*>(c);
            const JPH::Vector<2> l = sl->GetTotalLambdaPosition();
            linear  = {l[0], l[1], sl->GetTotalLambdaPositionLimits()};
            angular = ToGlm(sl->GetTotalLambdaRotation());
            break;
        }
        case JointType::Ball: linear = ToGlm(static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition()); break;
        case JointType::Distance: linear = {static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition(), 0.0f, 0.0f}; break;
        case JointType::Cone: {
            auto* cone = static_cast<JPH::ConeConstraint*>(c);
            linear     = ToGlm(cone->GetTotalLambdaPosition());
            angular    = {cone->GetTotalLambdaRotation(), 0.0f, 0.0f};
            break;
        }
        case JointType::SwingTwist: {
            auto* st = static_cast<JPH::SwingTwistConstraint*>(c);
            linear   = ToGlm(st->GetTotalLambdaPosition());
            angular  = {st->GetTotalLambdaTwist(), st->GetTotalLambdaSwingY(), st->GetTotalLambdaSwingZ()};
            break;
        }
        case JointType::SixDof: {
            auto* six = static_cast<JPH::SixDOFConstraint*>(c);
            linear    = ToGlm(six->GetTotalLambdaPosition());
            angular   = ToGlm(six->GetTotalLambdaRotation());
            break;
        }
        }
        record.force  = linear / subStep;
        record.torque = angular / subStep;
        const Joint& j = record.joint;
        const bool broken = (j.breakForce > 0.0f && glm::length(record.force) > j.breakForce) ||
                            (j.breakTorque > 0.0f && glm::length(record.torque) > j.breakTorque);
        if (!broken)
            continue;
        RemoveJoint(record);
        record.joint.enabled = false;
        if (registry.Valid(record.entity))
            if (auto* component = registry.TryGet<Joint>(record.entity))
                component->enabled = false;
        pendingBroken.push_back({record.entity, record.entityA, record.entityB});
    }
}

void PhysicsWorld::Impl::SyncAll(Scene& scene, bool stepping)
{
    Impl&      w     = *this;
    const auto start = std::chrono::steady_clock::now();
    scene.UpdateTransforms();
    w.stats.created = w.stats.removed = w.stats.pendingMeshes = 0;
    ++w.visit;
    w.CheckMaterials();

    Registry& registry = scene.GetRegistry();
    registry.ViewOf<CharacterController>().Each(
        [&](Entity e, CharacterController& cc) { w.SyncCharacter(scene, e, cc); });
    registry.ViewOf<RigidBody, Collider>().Each([&](Entity e, RigidBody& body, Collider& collider) {
        if (!registry.Has<CharacterController>(e)) // the character takes precedence
            w.SyncBody(scene, e, body, collider, stepping);
    });

    // Components or entities that are gone.
    for (auto it = w.bodies.begin(); it != w.bodies.end();) {
        if (it->second.visit != w.visit) {
            w.RemoveBody(it->second);
            it = w.bodies.erase(it);
            ++w.stats.removed;
        } else {
            ++it;
        }
    }
    for (auto it = w.characters.begin(); it != w.characters.end();) {
        if (it->second.visit != w.visit) {
            w.EndCharacterContacts(it->second);
            w.EndPairsOf(it->second.character->GetInnerBodyID());
            w.innerBodies.erase(it->second.character->GetInnerBodyID().GetIndexAndSequenceNumber());
            it = w.characters.erase(it);
            ++w.stats.removed;
        } else {
            ++it;
        }
    }
    w.EndRemovedPairs();
    w.SyncJoints(scene, stepping);
    if (w.stats.removed || w.stats.created) // drop mesh shapes no body uses any more
        std::erase_if(w.meshShapes, [](const auto& entry) { return entry.second->GetRefCount() == 1; });
    if (w.stats.created > 256)
        w.system->OptimizeBroadPhase(); // many new bodies (scene load, restore)

    w.stats.syncMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void PhysicsWorld::Sync(Scene& scene)
{
    m_Impl->layerMatrix = settings.layerCollision;
    m_Impl->SyncAll(scene, false);
    m_Impl->PublishEvents();
}

void PhysicsWorld::Step(Scene& scene, float dt)
{
    Impl& w = *m_Impl;
    w.layerMatrix = settings.layerCollision; // read by the pair filter during Update
    w.contacts.recordPersisted.store(settings.persistEvents, std::memory_order_relaxed);
    w.SyncAll(scene, true);
    if (!(dt > 0.0f)) {
        w.PublishEvents();
        return;
    }
    const auto start = std::chrono::steady_clock::now();

    // Kinematic bodies move to their target over this step (velocity -> they push dynamic bodies).
    for (auto& [key, r] : w.bodies) {
        if (r.type != BodyType::Kinematic || (!r.kinematicMove && !w.Bodies().IsActive(r.id)))
            continue;
        w.Bodies().MoveKinematic(r.id, JPH::RVec3(ToJolt(r.last.position)), ToJolt(r.last.rotation), dt);
        r.kinematicMove = false; // not moved again: the next step stops it
    }

    w.system->SetGravity(ToJolt(settings.gravity));
    w.StepCharacters(scene, dt, settings.gravity, settings.airControl);
    w.UpdateCharacterContacts(settings.persistEvents);
    const JPH::EPhysicsUpdateError error =
        w.system->Update(dt, std::max(settings.collisionSteps, 1), w.tempAllocator.get(), w.jobSystem.get());
    if (error != JPH::EPhysicsUpdateError::None)
        ENGINE_WARN("Physics: update error {:#x} (limits too small?)", static_cast<unsigned>(error));
    w.CheckBrokenJoints(scene, dt, settings.collisionSteps);

    // Write-back: moving dynamic bodies and characters (their previous poses kept for Interpolate).
    std::vector<Impl::PoseWrite> writes;
    for (auto& [key, r] : w.bodies)
        if (r.type == BodyType::Dynamic)
            r.simPrevious = r.simCurrent;
    JPH::BodyIDVector active;
    w.system->GetActiveBodies(JPH::EBodyType::RigidBody, active);
    for (const JPH::BodyID id : active) {
        const auto it = w.bodies.find(w.Bodies().GetUserData(id));
        if (it == w.bodies.end() || it->second.type != BodyType::Dynamic || it->second.id != id)
            continue;
        JPH::RVec3 position;
        JPH::Quat  rotation;
        w.Bodies().GetPositionAndRotation(id, position, rotation);
        const glm::quat q = glm::normalize(ToGlm(rotation));
        it->second.simCurrent.position = ToGlm(JPH::Vec3(position));
        it->second.simCurrent.rotation = q;
        it->second.between             = false;
        writes.push_back({.entity = it->second.entity, .position = it->second.simCurrent.position, .rotation = q});
    }
    for (auto& [key, r] : w.characters) {
        const glm::vec3 position = ToGlm(JPH::Vec3(r.character->GetPosition()));
        const bool      turns    = r.settings.rotation != CharacterRotation::None;
        r.simPrevious    = r.simCurrent;
        r.simCurrent     = position;
        r.simPreviousYaw = r.simCurrentYaw;
        r.simCurrentYaw  = r.yaw;
        if (Moved(position, r.lastPosition) || (turns && r.simPreviousYaw != r.simCurrentYaw)) {
            writes.push_back({.entity   = r.entity,
                              .position = position,
                              .rotation = turns ? std::optional<glm::quat>(YawRotation(r.yaw)) : std::nullopt});
            r.between = false;
        }
    }
    w.WritePoses(scene, writes);

    w.ProcessContacts(settings.persistEvents);
    w.stats.stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    w.PublishEvents();
}

void PhysicsWorld::Reset()
{
    m_Impl->Clear();
}

void PhysicsWorld::Interpolate(Scene& scene, float alpha)
{
    // Without interpolation (or after it was switched off) bodies are snapped to their last step.
    alpha   = settings.interpolate ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
    Impl& w = *m_Impl;
    std::vector<Impl::PoseWrite> writes;
    for (auto& [key, r] : w.bodies) {
        if (r.type != BodyType::Dynamic)
            continue;
        const bool moving = r.simPrevious.position != r.simCurrent.position || r.simPrevious.rotation != r.simCurrent.rotation;
        if (!moving && !r.between) // at rest and already shown at its last step
            continue;
        writes.push_back({.entity   = r.entity,
                          .position = glm::mix(r.simPrevious.position, r.simCurrent.position, alpha),
                          .rotation = glm::normalize(glm::slerp(r.simPrevious.rotation, r.simCurrent.rotation, alpha))});
        r.between = moving && alpha < 1.0f;
    }
    for (auto& [key, r] : w.characters) {
        const bool turns  = r.settings.rotation != CharacterRotation::None;
        const bool moving = r.simPrevious != r.simCurrent || (turns && r.simPreviousYaw != r.simCurrentYaw);
        if (!moving && !r.between)
            continue;
        const float yaw = r.simPreviousYaw + WrapAngle(r.simCurrentYaw - r.simPreviousYaw) * alpha;
        writes.push_back({.entity   = r.entity,
                          .position = glm::mix(r.simPrevious, r.simCurrent, alpha),
                          .rotation = turns ? std::optional<glm::quat>(YawRotation(yaw)) : std::nullopt});
        r.between = moving && alpha < 1.0f;
    }
    w.WritePoses(scene, writes);
}

std::optional<PhysicsHit> PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance,
                                                Entity ignore, std::uint16_t layerMask) const
{
    const float length = glm::length(dir);
    if (!(length > 0.0f) || !(maxDistance > 0.0f))
        return std::nullopt;
    const glm::vec3 direction = dir / length;
    const Impl&     w         = *m_Impl;

    const JPH::RRayCast   ray{JPH::RVec3(ToJolt(origin)), ToJolt(direction * maxDistance)};
    JPH::RayCastResult    result;
    const QueryBodyFilter filter(ignore);
    const LayerMaskFilter layers(layerMask);
    if (!w.system->GetNarrowPhaseQueryNoLock().CastRay(ray, result, {}, layers, filter))
        return std::nullopt;

    PhysicsHit hit;
    hit.distance = result.mFraction * maxDistance;
    hit.point    = origin + direction * hit.distance;
    hit.entity   = Entity{w.Bodies().GetUserData(result.mBodyID)};
    JPH::BodyLockRead lock(w.system->GetBodyLockInterfaceNoLock(), result.mBodyID);
    if (lock.Succeeded()) {
        hit.normal = ToGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, JPH::RVec3(ToJolt(hit.point))));
        if (glm::dot(hit.normal, direction) > 0.0f) // back face of a mesh
            hit.normal = -hit.normal;
        hit.surface = SurfaceName(lock.GetBody().GetShape()->GetMaterial(result.mSubShapeID2));
    }
    return hit;
}

std::optional<PhysicsHit> PhysicsWorld::SphereCast(const glm::vec3& origin, float radius, const glm::vec3& dir,
                                                   float maxDistance, Entity ignore, std::uint16_t layerMask) const
{
    const float length = glm::length(dir);
    if (!(length > 0.0f) || !(maxDistance > 0.0f) || !(radius > 0.0f))
        return std::nullopt;
    const glm::vec3 direction = dir / length;
    const Impl&     w         = *m_Impl;

    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded(); // on the stack: no reference counting
    const JPH::RShapeCast cast(&sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(JPH::RVec3(ToJolt(origin))),
                               ToJolt(direction * maxDistance));
    JPH::ShapeCastSettings                                       castSettings;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const QueryBodyFilter                                        filter(ignore);
    const LayerMaskFilter layers(layerMask);
    w.system->GetNarrowPhaseQueryNoLock().CastShape(cast, castSettings, JPH::RVec3::sZero(), collector, {}, layers, filter);
    if (!collector.HadHit())
        return std::nullopt;

    const JPH::ShapeCastResult& r = collector.mHit;
    PhysicsHit                  hit;
    hit.entity   = Entity{w.Bodies().GetUserData(r.mBodyID2)};
    hit.distance = r.mFraction * maxDistance;
    hit.point    = ToGlm(r.mContactPointOn2);
    const JPH::Vec3 axis = r.mPenetrationAxis;
    hit.normal   = axis.LengthSq() > 0.0f ? -ToGlm(axis.Normalized()) : -direction;
    JPH::BodyLockRead lock(w.system->GetBodyLockInterfaceNoLock(), r.mBodyID2);
    if (lock.Succeeded())
        hit.surface = SurfaceName(lock.GetBody().GetShape()->GetMaterial(r.mSubShapeID2));
    return hit;
}

void PhysicsWorld::AddImpulse(Entity entity, const glm::vec3& impulse)
{
    if (const auto it = m_Impl->bodies.find(Key(entity)); it != m_Impl->bodies.end() && it->second.type == BodyType::Dynamic)
        m_Impl->Bodies().AddImpulse(it->second.id, ToJolt(impulse));
}

void PhysicsWorld::AddImpulseAt(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPoint)
{
    if (const auto it = m_Impl->bodies.find(Key(entity)); it != m_Impl->bodies.end() && it->second.type == BodyType::Dynamic)
        m_Impl->Bodies().AddImpulse(it->second.id, ToJolt(impulse), JPH::RVec3(ToJolt(worldPoint)));
}

void PhysicsWorld::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
{
    if (const auto it = m_Impl->bodies.find(Key(entity)); it != m_Impl->bodies.end() && it->second.type == BodyType::Dynamic)
        m_Impl->Bodies().SetLinearVelocity(it->second.id, ToJolt(velocity));
}

glm::vec3 PhysicsWorld::LinearVelocity(Entity entity) const
{
    if (const auto it = m_Impl->bodies.find(Key(entity)); it != m_Impl->bodies.end())
        return ToGlm(m_Impl->Bodies().GetLinearVelocity(it->second.id));
    if (const auto it = m_Impl->characters.find(Key(entity)); it != m_Impl->characters.end())
        return ToGlm(it->second.character->GetLinearVelocity());
    return glm::vec3(0.0f);
}

void PhysicsWorld::SetCharacterInput(Entity entity, const glm::vec3& moveVelocity, bool jump)
{
    if (const auto it = m_Impl->characters.find(Key(entity)); it != m_Impl->characters.end()) {
        it->second.input = moveVelocity;
        it->second.jump  = it->second.jump || jump; // consumed by the next Step
    }
}

void PhysicsWorld::SetCharacterCrouch(Entity entity, bool crouch)
{
    if (const auto it = m_Impl->characters.find(Key(entity)); it != m_Impl->characters.end())
        it->second.wantCrouch = crouch;
}

void PhysicsWorld::SetCharacterYaw(Entity entity, float yaw)
{
    if (const auto it = m_Impl->characters.find(Key(entity)); it != m_Impl->characters.end() && std::isfinite(yaw))
        it->second.targetYaw = WrapAngle(yaw);
}

std::optional<CharacterState> PhysicsWorld::GetCharacterState(Entity entity) const
{
    const auto it = m_Impl->characters.find(Key(entity));
    if (it == m_Impl->characters.end())
        return std::nullopt;
    const JPH::CharacterVirtual& c = *it->second.character;
    GroundState ground = GroundState::InAir;
    switch (c.GetGroundState()) {
    case JPH::CharacterBase::EGroundState::OnGround: ground = GroundState::OnGround; break;
    case JPH::CharacterBase::EGroundState::OnSteepGround: ground = GroundState::OnSteepGround; break;
    case JPH::CharacterBase::EGroundState::NotSupported: ground = GroundState::NotSupported; break;
    case JPH::CharacterBase::EGroundState::InAir: break;
    }
    const JPH::BodyID groundBody = c.GetGroundBodyID();
    return CharacterState{.onGround       = ground == GroundState::OnGround,
                          .ground         = ground,
                          .velocity       = ToGlm(c.GetLinearVelocity()),
                          .groundNormal   = ToGlm(c.GetGroundNormal()),
                          .groundVelocity = ToGlm(c.GetGroundVelocity()),
                          .groundEntity   = !groundBody.IsInvalid() && m_Impl->Bodies().IsAdded(groundBody)
                                                ? Entity{m_Impl->Bodies().GetUserData(groundBody)}
                                                : NullEntity,
                          .crouching      = it->second.crouching,
                          .yaw            = it->second.yaw};
}

JointState PhysicsWorld::GetJointState(Entity jointEntity) const
{
    const auto it = m_Impl->joints.find(Key(jointEntity));
    if (it == m_Impl->joints.end() || !it->second.constraint)
        return {};
    const Impl::JointRecord& r = it->second;
    JointState state{.active = true, .force = r.force, .torque = r.torque};
    switch (r.joint.type) {
    case JointType::Hinge: state.angle = static_cast<const JPH::HingeConstraint*>(r.constraint.GetPtr())->GetCurrentAngle(); break;
    case JointType::Slider:
        state.position = static_cast<const JPH::SliderConstraint*>(r.constraint.GetPtr())->GetCurrentPosition();
        break;
    case JointType::Distance: {
        const JPH::TwoBodyConstraint& c = *r.constraint;
        const JPH::RVec3 a = FrameOn(*c.GetBody1(), c.GetConstraintToBody1Matrix()).GetTranslation();
        const JPH::RVec3 b = FrameOn(*c.GetBody2(), c.GetConstraintToBody2Matrix()).GetTranslation();
        state.position     = static_cast<float>((b - a).Length());
        break;
    }
    default: break;
    }
    return state;
}

void PhysicsWorld::ForEachJoint(const std::function<void(const JointDebugShape&)>& fn) const
{
    for (const auto& [key, r] : m_Impl->joints) {
        JointDebugShape shape{.entity = r.entity, .type = r.joint.type, .a = r.entityA, .b = r.entityB};
        if (r.constraint) {
            const JPH::TwoBodyConstraint& c = *r.constraint;
            shape.frameA = ToGlm(FrameOn(*c.GetBody2(), c.GetConstraintToBody2Matrix())); // body 2 = A
            shape.frameB = ToGlm(FrameOn(*c.GetBody1(), c.GetConstraintToBody1Matrix()));
            shape.active = true;
        }
        fn(shape);
    }
}

BodyActivity PhysicsWorld::Activity(Entity entity) const
{
    if (m_Impl->characters.contains(Key(entity)))
        return BodyActivity::Character;
    const auto it = m_Impl->bodies.find(Key(entity));
    if (it == m_Impl->bodies.end())
        return BodyActivity::None;
    switch (it->second.type) {
    case BodyType::Static: return BodyActivity::Static;
    case BodyType::Kinematic: return BodyActivity::Kinematic;
    case BodyType::Dynamic: break;
    }
    return m_Impl->Bodies().IsActive(it->second.id) ? BodyActivity::Active : BodyActivity::Sleeping;
}

bool PhysicsWorld::HasBody(Entity entity) const
{
    return m_Impl->bodies.contains(Key(entity)) || m_Impl->characters.contains(Key(entity));
}

const PhysicsStats& PhysicsWorld::Stats() const
{
    Impl& w              = *m_Impl;
    w.stats.bodies       = static_cast<std::uint32_t>(w.bodies.size());
    w.stats.characters   = static_cast<std::uint32_t>(w.characters.size());
    w.stats.activeBodies = w.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    w.stats.contactPairs = static_cast<std::uint32_t>(w.pairs.size());
    w.stats.joints       = static_cast<std::uint32_t>(
        std::ranges::count_if(w.joints, [](const auto& entry) { return entry.second.constraint != nullptr; }));
    return w.stats;
}

void PhysicsWorld::ForEachCollider(const std::function<void(const ColliderDebugShape&)>& fn) const
{
    const Impl& w = *m_Impl;
    for (const auto& [key, r] : w.bodies) {
        JPH::RVec3 position;
        JPH::Quat  rotation;
        w.Bodies().GetPositionAndRotation(r.id, position, rotation);
        const glm::quat      q = glm::normalize(ToGlm(rotation));
        const ScaledCollider s = Scaled(r.collider, r.scale);

        ColliderDebugShape shape;
        shape.entity   = r.entity;
        shape.shape    = r.collider.shape;
        shape.trigger  = r.collider.trigger;
        shape.activity = Activity(r.entity);
        if (r.collider.shape == ColliderShape::Mesh) {
            const JPH::AABox bounds = w.Bodies().GetTransformedShape(r.id).GetWorldSpaceBounds();
            shape.transform         = glm::translate(glm::mat4(1.0f), ToGlm(bounds.GetCenter()));
            shape.halfExtents       = ToGlm(bounds.GetExtent());
        } else {
            shape.transform   = glm::translate(glm::mat4(1.0f), ToGlm(JPH::Vec3(position)) + q * s.center) *
                              glm::mat4_cast(q * glm::normalize(r.collider.rotation));
            shape.halfExtents = s.halfExtents;
            shape.radius      = s.radius;
            shape.halfHeight  = r.collider.shape == ColliderShape::Capsule ? s.halfHeight : 0.0f;
        }
        fn(shape);
    }
    for (const auto& [key, r] : w.characters) {
        const float radius     = std::max(r.settings.radius, 0.01f);
        const float height     = r.crouching ? std::clamp(r.settings.crouchHeight, 2.0f * radius, r.settings.height) : r.settings.height;
        const float halfHeight = std::max(height * 0.5f - radius, 0.01f);
        const glm::vec3 feet   = ToGlm(JPH::Vec3(r.character->GetPosition()));
        fn({.entity     = r.entity,
            .shape      = ColliderShape::Capsule,
            .transform  = glm::translate(glm::mat4(1.0f), feet + glm::vec3(0.0f, halfHeight + radius, 0.0f)),
            .halfExtents = glm::vec3(0.0f),
            .radius     = radius,
            .halfHeight = halfHeight,
            .activity   = BodyActivity::Character,
            .trigger    = false});
    }
}

} // namespace Engine
