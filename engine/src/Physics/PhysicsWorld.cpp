#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
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
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
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

// --- Contacts: recorded on the physics threads, turned into events on the main thread ---

struct ContactDelta {
    JPH::BodyID a, b; // a < b
    int         delta   = 0;
    bool        trigger = false;
};

class ContactRecorder final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&, JPH::ContactSettings&) override
    {
        Record({a.GetID(), b.GetID(), +1, a.IsSensor() || b.IsSensor()});
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        Record({pair.GetBody1ID(), pair.GetBody2ID(), -1, false});
    }
    std::vector<ContactDelta> Take()
    {
        std::scoped_lock lock(m_Mutex);
        return std::exchange(m_Deltas, {});
    }

private:
    void Record(ContactDelta delta)
    {
        if (delta.b < delta.a)
            std::swap(delta.a, delta.b);
        std::scoped_lock lock(m_Mutex);
        m_Deltas.push_back(delta);
    }
    std::mutex                m_Mutex;
    std::vector<ContactDelta> m_Deltas;
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

bool Moved(const glm::vec3& a, const glm::vec3& b) { return glm::any(glm::greaterThan(glm::abs(a - b), glm::vec3(1e-5f))); }
bool Rotated(const glm::quat& a, const glm::quat& b) { return std::abs(glm::dot(a, b)) < 1.0f - 1e-6f; }
bool Rescaled(const glm::vec3& a, const glm::vec3& b)
{
    return glm::any(glm::greaterThan(glm::abs(a - b), glm::max(glm::abs(a), glm::abs(b)) * 1e-4f));
}

constexpr float kMinExtent = 1e-3f;

// Collider extents after scaling (shared by shape creation and debug drawing).
struct ScaledCollider {
    glm::vec3 halfExtents;
    float     radius;
    float     halfHeight;
    glm::vec3 center;
};

ScaledCollider Scaled(const Collider& c, const glm::vec3& scale)
{
    const float uniform = std::max({scale.x, scale.y, scale.z});
    return {.halfExtents = glm::max(c.halfExtents * scale, glm::vec3(kMinExtent)),
            .radius      = std::max(c.radius * (c.shape == ColliderShape::Capsule ? std::max(scale.x, scale.z) : uniform), kMinExtent),
            .halfHeight  = std::max(c.halfHeight * scale.y, 0.0f),
            .center      = c.center * scale};
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
    };
    struct CharacterRecord {
        Entity                          entity = NullEntity;
        JPH::Ref<JPH::CharacterVirtual> character;
        CharacterController             settings;
        glm::vec3                       lastPosition{0.0f};
        glm::vec3                       input{0.0f};
        glm::vec3                       simPrevious{0.0f}, simCurrent{0.0f}; // Interpolate
        bool                            between = false;
        bool                            jump  = false;
        std::uint32_t                   visit = 0;
        // Bodies it touches (BodyID index + sequence -> other entity, trigger): Begin / End events.
        std::unordered_map<std::uint32_t, std::pair<Entity, bool>> touching;
    };
    struct Pair {
        Entity a, b;
        int    count     = 0;
        bool   trigger   = false;
        bool   suspended = false; // a body fell asleep: Jolt removed the contacts, they still touch
    };

    Impl(ThreadPool& threads, EventBus& bus, const AssetManager* assetManager) : events(bus), assets(assetManager)
    {
        AcquireJolt();
        tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(32u << 20);
        jobSystem     = std::make_unique<EngineJobSystem>(threads, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
        system        = std::make_unique<JPH::PhysicsSystem>();
        system->Init(65536, 0, 65536, 20480, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        system->SetContactListener(&contacts);
    }

    ~Impl()
    {
        Clear();
        system.reset();
        jobSystem.reset();
        tempAllocator.reset();
        ReleaseJolt();
    }

    JPH::BodyInterface&       Bodies() { return system->GetBodyInterfaceNoLock(); } // main thread, outside Update
    const JPH::BodyInterface& Bodies() const { return system->GetBodyInterfaceNoLock(); }

    void Clear()
    {
        for (auto& [key, record] : bodies)
            DestroyBody(record.id);
        bodies.clear();
        characters.clear(); // removes the inner bodies
        innerBodies.clear();
        pairs.clear();
        meshShapes.clear();
        removedBodies.clear();
        contacts.Take();
        pendingEvents.clear();
    }

    void DestroyBody(JPH::BodyID id)
    {
        Bodies().RemoveBody(id);
        Bodies().DestroyBody(id);
    }

    // --- Shapes ---

    JPH::RefConst<JPH::Shape> MeshShapeFor(const Model& model, ModelHandle handle, std::uint32_t meshIndex,
                                           std::uint32_t revision)
    {
        const MeshKey key{handle.index, handle.generation, meshIndex, revision};
        if (const auto it = meshShapes.find(key); it != meshShapes.end())
            return it->second;

        JPH::VertexList vertices;
        vertices.reserve(model.collisionPositions.size());
        for (const glm::vec3& p : model.collisionPositions)
            vertices.push_back(JPH::Float3(p.x, p.y, p.z));
        JPH::IndexedTriangleList triangles;
        for (const Submesh& sm : model.meshes[meshIndex].submeshes) {
            for (std::uint32_t i = 0; i + 2 < sm.indexCount; i += 3) {
                const auto index = [&](std::uint32_t k) {
                    return static_cast<JPH::uint32>(static_cast<std::int64_t>(model.collisionIndices[sm.firstIndex + i + k]) +
                                                    sm.vertexOffset);
                };
                triangles.push_back(JPH::IndexedTriangle(index(0), index(1), index(2)));
            }
        }
        JPH::MeshShapeSettings meshSettings(std::move(vertices), std::move(triangles));
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
        const ScaledCollider    s = Scaled(c, scale);
        JPH::RefConst<JPH::Shape> shape;
        switch (c.shape) {
        case ColliderShape::Box: {
            const float convex = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({s.halfExtents.x, s.halfExtents.y, s.halfExtents.z}));
            shape = new JPH::BoxShape(ToJolt(s.halfExtents), convex);
            break;
        }
        case ColliderShape::Sphere: shape = new JPH::SphereShape(s.radius); break;
        case ColliderShape::Capsule:
            shape = s.halfHeight > kMinExtent ? JPH::RefConst<JPH::Shape>(new JPH::CapsuleShape(s.halfHeight, s.radius))
                                              : JPH::RefConst<JPH::Shape>(new JPH::SphereShape(s.radius));
            break;
        case ColliderShape::Mesh: {
            if (!model)
                return nullptr;
            shape = MeshShapeFor(*model, handle, meshIndex, revision);
            if (!shape)
                return nullptr;
            if (Rescaled(scale, glm::vec3(1.0f)))
                shape = new JPH::ScaledShape(shape, ToJolt(scale));
            break;
        }
        }
        if (glm::any(glm::notEqual(s.center, glm::vec3(0.0f))))
            shape = new JPH::RotatedTranslatedShape(ToJolt(s.center), JPH::Quat::sIdentity(), shape);
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
        bodySettings.mFriction       = r.collider.friction;
        bodySettings.mRestitution    = r.collider.restitution;
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
        return true;
    }

    // Removes the body; its contacts end in EndRemovedPairs (events are sent even if the entity is gone).
    void RemoveBody(const BodyRecord& r)
    {
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
                pendingEvents.push_back({it->second.a, it->second.b, false, it->second.trigger});
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

        const MeshRenderer* renderer = collider.shape == ColliderShape::Mesh ? registry.TryGet<MeshRenderer>(e) : nullptr;
        const Model*        model    = renderer && assets ? assets->Get(renderer->model) : nullptr;
        if (model && renderer->meshIndex >= model->meshes.size())
            model = nullptr;
        const ModelHandle   handle    = renderer ? renderer->model : ModelHandle{};
        const std::uint32_t meshIndex = renderer ? renderer->meshIndex : 0;
        const std::uint32_t revision  = renderer && assets ? assets->Revision(handle) : 0;
        const BodyType type = collider.shape == ColliderShape::Mesh && body.type == BodyType::Dynamic ? BodyType::Static : body.type;

        auto it = bodies.find(Key(e));
        if (it != bodies.end()) {
            BodyRecord& r      = it->second;
            const bool  remake = r.body != body || r.collider != collider || r.model != handle ||
                                r.meshIndex != meshIndex || r.revision != revision || Rescaled(r.scale, pose.scale) ||
                                (collider.shape == ColliderShape::Mesh && !model);
            if (!remake) {
                r.visit = visit;
                if (Moved(pose.position, r.last.position) || Rotated(pose.rotation, r.last.rotation))
                    Teleport(r, pose, stepping);
                return;
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
            bodies.emplace(Key(e), r);
            ++stats.created;
        }
    }

    // The entity was moved from outside (editor, game code).
    void Teleport(BodyRecord& r, const Pose& pose, bool stepping)
    {
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

    void SyncCharacter(Scene& scene, Entity e, const CharacterController& cc)
    {
        const glm::vec3 position = scene.GetRegistry().Get<WorldTransform>(e).matrix[3];
        auto            it       = characters.find(Key(e));
        if (it != characters.end() && it->second.settings == cc) {
            CharacterRecord& r = it->second;
            r.visit            = visit;
            if (Moved(position, r.lastPosition)) {
                r.character->SetPosition(JPH::RVec3(ToJolt(position)));
                r.character->SetLinearVelocity(JPH::Vec3::sZero());
                r.lastPosition = r.simPrevious = r.simCurrent = position;
                r.between = false;
            }
            return;
        }
        glm::vec3 input{0.0f};
        if (it != characters.end()) {
            input = it->second.input;
            EndCharacterContacts(it->second);
            EndPairsOf(it->second.character->GetInnerBodyID());
            innerBodies.erase(it->second.character->GetInnerBodyID().GetIndexAndSequenceNumber());
            characters.erase(it);
            ++stats.removed;
        }

        const float radius     = std::max(cc.radius, 0.01f);
        const float halfHeight = std::max(cc.height * 0.5f - radius, 0.01f); // cylinder part
        const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(halfHeight, radius);

        JPH::CharacterVirtualSettings characterSettings;
        characterSettings.mShape             = capsule;
        characterSettings.mShapeOffset       = JPH::Vec3(0.0f, halfHeight + radius, 0.0f); // feet at the entity position
        characterSettings.mMaxSlopeAngle     = cc.maxSlope;
        characterSettings.mSupportingVolume  = JPH::Plane(JPH::Vec3::sAxisY(), -radius); // contacts below the lower hemisphere center
        characterSettings.mInnerBodyShape    = capsule; // lets rigid bodies and queries see the character
        characterSettings.mInnerBodyLayer    = Layers::kMoving; // user layer 0

        CharacterRecord r;
        r.entity       = e;
        r.settings     = cc;
        r.lastPosition = r.simPrevious = r.simCurrent = position;
        r.between = false;
        r.input        = input;
        r.visit        = visit;
        r.character = new JPH::CharacterVirtual(&characterSettings, JPH::RVec3(ToJolt(position)), JPH::Quat::sIdentity(), Key(e),
                                                system.get());
        innerBodies.insert(r.character->GetInnerBodyID().GetIndexAndSequenceNumber());
        characters.emplace(Key(e), std::move(r));
        ++stats.created;
    }

    // Character contacts (CharacterVirtual does not go through the contact listener): diff of the
    // touched bodies after each update.
    void UpdateCharacterContacts()
    {
        for (auto& [key, r] : characters) {
            std::unordered_map<std::uint32_t, std::pair<Entity, bool>> now;
            for (const JPH::CharacterContact& c : r.character->GetActiveContacts()) {
                if (c.mBodyB.IsInvalid() || !(c.mHadCollision || c.mIsSensorB))
                    continue;
                now.try_emplace(c.mBodyB.GetIndexAndSequenceNumber(), Entity{c.mUserData}, c.mIsSensorB);
            }
            for (const auto& [id, other] : now)
                if (!r.touching.contains(id))
                    pendingEvents.push_back({r.entity, other.first, true, other.second});
            for (const auto& [id, other] : r.touching)
                if (!now.contains(id))
                    pendingEvents.push_back({r.entity, other.first, false, other.second});
            r.touching = std::move(now);
        }
    }

    void EndCharacterContacts(CharacterRecord& r)
    {
        for (const auto& [id, other] : r.touching)
            pendingEvents.push_back({r.entity, other.first, false, other.second});
        r.touching.clear();
    }

    void StepCharacters(float dt, const glm::vec3& gravity, float airControl)
    {
        const JPH::Vec3 g = ToJolt(gravity);
        for (auto& [key, r] : characters) {
            JPH::CharacterVirtual& c = *r.character;
            c.UpdateGroundVelocity();
            const JPH::Vec3 current = c.GetLinearVelocity();
            const JPH::Vec3 desired(r.input.x, 0.0f, r.input.z);
            const bool      grounded = c.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                                  (current - c.GetGroundVelocity()).GetY() < 0.1f; // not moving away (jump start)
            JPH::Vec3 velocity;
            if (grounded) {
                velocity = c.GetGroundVelocity() + desired;
                if (r.jump)
                    velocity += JPH::Vec3(0.0f, r.settings.jumpSpeed, 0.0f);
            } else {
                const JPH::Vec3 horizontal(current.GetX(), 0.0f, current.GetZ());
                velocity = horizontal + (desired - horizontal) * std::min(airControl * dt, 1.0f) +
                           JPH::Vec3(0.0f, current.GetY(), 0.0f);
            }
            r.jump = false;
            velocity += g * dt;
            c.SetLinearVelocity(velocity);

            JPH::CharacterVirtual::ExtendedUpdateSettings update;
            update.mWalkStairsStepUp     = JPH::Vec3(0.0f, r.settings.stepHeight, 0.0f);
            update.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(r.settings.stepHeight, 0.1f), 0.0f);
            c.ExtendedUpdate(dt, g, update, system->GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
                             system->GetDefaultLayerFilter(Layers::kMoving), {}, {}, *tempAllocator);
        }
    }

    // --- Events ---

    void ProcessContacts()
    {
        struct Accum {
            JPH::BodyID a, b;
            int         delta   = 0;
            bool        trigger = false;
        };
        std::unordered_map<std::uint64_t, Accum> accum;
        for (const ContactDelta& d : contacts.Take()) {
            Accum& acc = accum.try_emplace(PairKey(d.a, d.b), Accum{d.a, d.b}).first->second;
            acc.delta += d.delta;
            acc.trigger |= d.trigger;
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
                pairs.emplace(key, Pair{a, b, acc.delta, acc.trigger, false});
                pendingEvents.push_back({a, b, true, acc.trigger});
                continue;
            }
            Pair& pair = it->second;
            pair.count += acc.delta;
            pair.trigger |= acc.trigger;
            if (pair.count > 0) {
                pair.suspended = false; // woke up and still touching: no new Begin
            } else if (sleeping(acc.a) || sleeping(acc.b)) {
                pair.count     = 0;
                pair.suspended = true; // contacts dropped because the bodies sleep
            } else {
                pendingEvents.push_back({pair.a, pair.b, false, pair.trigger});
                pairs.erase(it);
            }
        }
        // Suspended pairs whose bodies woke up without touching again.
        for (auto it = pairs.begin(); it != pairs.end();) {
            Pair& pair = it->second;
            if (pair.suspended && !accum.contains(it->first)) {
                const JPH::BodyID a(static_cast<JPH::uint32>(it->first >> 32)), b(static_cast<JPH::uint32>(it->first));
                if (!sleeping(a) && !sleeping(b)) {
                    pendingEvents.push_back({pair.a, pair.b, false, pair.trigger});
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
            const glm::mat4  parentWorld = parent != NullEntity ? worldOf(parent) : glm::mat4(1.0f);
            Transform&       t           = scene.EditTransform(w.entity);
            t.position = glm::vec3(glm::inverse(parentWorld) * glm::vec4(w.position, 1.0f));
            if (w.rotation)
                t.rotation = parent != NullEntity ? glm::normalize(glm::conjugate(Decompose(parentWorld).rotation) * *w.rotation)
                                                  : *w.rotation;
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
    using MeshKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>; // model index, generation, mesh, revision
    std::map<MeshKey, JPH::RefConst<JPH::Shape>> meshShapes; // unscaled
    std::unordered_set<JPH::uint32>              removedBodies; // since the last EndRemovedPairs
    std::vector<CollisionEvent>                  pendingEvents;
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

void PhysicsWorld::Impl::SyncAll(Scene& scene, bool stepping)
{
    Impl&      w     = *this;
    const auto start = std::chrono::steady_clock::now();
    scene.UpdateTransforms();
    w.stats.created = w.stats.removed = w.stats.pendingMeshes = 0;
    ++w.visit;

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
    w.StepCharacters(dt, settings.gravity, settings.airControl);
    w.UpdateCharacterContacts();
    const JPH::EPhysicsUpdateError error =
        w.system->Update(dt, std::max(settings.collisionSteps, 1), w.tempAllocator.get(), w.jobSystem.get());
    if (error != JPH::EPhysicsUpdateError::None)
        ENGINE_WARN("Physics: update error {:#x} (limits too small?)", static_cast<unsigned>(error));

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
        r.simPrevious = r.simCurrent;
        r.simCurrent  = position;
        if (Moved(position, r.lastPosition)) {
            writes.push_back({.entity = r.entity, .position = position, .rotation = std::nullopt});
            r.between = false;
        }
    }
    w.WritePoses(scene, writes);

    w.ProcessContacts();
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
        const bool moving = r.simPrevious != r.simCurrent;
        if (!moving && !r.between)
            continue;
        writes.push_back({.entity = r.entity, .position = glm::mix(r.simPrevious, r.simCurrent, alpha), .rotation = std::nullopt});
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

std::optional<CharacterState> PhysicsWorld::GetCharacterState(Entity entity) const
{
    const auto it = m_Impl->characters.find(Key(entity));
    if (it == m_Impl->characters.end())
        return std::nullopt;
    const JPH::CharacterVirtual& c = *it->second.character;
    return CharacterState{.onGround     = c.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround,
                          .velocity     = ToGlm(c.GetLinearVelocity()),
                          .groundNormal = ToGlm(c.GetGroundNormal())};
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
            shape.transform   = glm::translate(glm::mat4(1.0f), ToGlm(JPH::Vec3(position)) + q * s.center) * glm::mat4_cast(q);
            shape.halfExtents = s.halfExtents;
            shape.radius      = s.radius;
            shape.halfHeight  = r.collider.shape == ColliderShape::Capsule ? s.halfHeight : 0.0f;
        }
        fn(shape);
    }
    for (const auto& [key, r] : w.characters) {
        const float radius     = std::max(r.settings.radius, 0.01f);
        const float halfHeight = std::max(r.settings.height * 0.5f - radius, 0.01f);
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
