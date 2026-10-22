#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Animation/AnimIk.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Model.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Particles/ParticleWorld.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace Engine {

namespace {

constexpr float kGraphCheckSeconds = 1.0f;

std::string Key(const std::string& path)
{
    return path.empty() ? path : PathToUtf8(PathFromUtf8(path).lexically_normal());
}

bool IsAncestorOrSelf(const Registry& registry, Entity ancestor, Entity entity)
{
    for (Entity e = entity; e != NullEntity && registry.Valid(e); e = registry.Get<Hierarchy>(e).parent)
        if (e == ancestor)
            return true;
    return false;
}

} // namespace

struct AnimationSystem::Impl {
    struct GraphRef {
        std::shared_ptr<const AnimGraph> graph, provided;
        std::filesystem::file_time_type  time{};
        bool                             failed  = false;
        std::uint64_t                    version = 1;
    };
    struct IkRuntime {
        std::string         name;
        bool                hasTarget = false;
        glm::vec3           target{0.0f};
        Entity              targetEntity = NullEntity;
        bool                hasPole = false;
        glm::vec3           pole{0.0f};
        float               weight = 1.0f;
        std::optional<bool> enabled{};
    };
    struct Instance {
        std::string     graphKey;
        std::uint64_t   version = 0;
        const Model*    model = nullptr;
        std::uint32_t   modelRevision = 0;
        AnimGraphPlayer player;
        std::vector<std::pair<std::string, float>> pending; // parameters set before binding
        std::vector<std::pair<std::string, float>> pendingLayers;
        std::vector<IkRuntime>   ik;
        std::vector<FootIkState> feet; // per graph goal
        std::vector<Transform>   pose;
        bool                     seen = false;
    };

    AssetManager*  assets = nullptr;
    ModelResolver  resolver;
    PhysicsWorld*  physics = nullptr;
    EventBus*      events  = nullptr;
    AudioSystem*   audio   = nullptr;
    ParticleWorld* particles = nullptr;
    std::unordered_map<std::string, GraphRef> graphs;
    std::unordered_map<Entity, Instance>      instances;
    float          clock = 0.0f, nextCheck = 0.0f;
    bool           paused = false;
    AnimationStats stats;

    const Model* ResolveModel(ModelHandle handle) const
    {
        if (assets)
            return assets->Get(handle);
        return resolver ? resolver(handle) : nullptr;
    }

    void Load(const std::string& key, GraphRef& ref)
    {
        const std::filesystem::path file = PathFromUtf8(key);
        ref.time = Vfs::ModifiedTime(file);
        std::string error;
        if (auto graph = LoadAnimGraph(file, &error)) {
            ref.failed = false;
            if (ref.graph && *ref.graph == *graph)
                return;
            ref.graph = std::make_shared<const AnimGraph>(std::move(*graph));
            if (!ref.provided)
                ++ref.version;
        } else {
            if (!ref.failed)
                ENGINE_WARN("Animation: {}", error);
            ref.failed = true; // a broken edit keeps the last good version
        }
    }

    GraphRef* Find(const std::string& key)
    {
        if (key.empty())
            return nullptr;
        auto [it, inserted] = graphs.try_emplace(key);
        if (inserted)
            Load(key, it->second);
        return it->second.provided || it->second.graph ? &it->second : nullptr;
    }

    static std::shared_ptr<const AnimGraph> Current(const GraphRef& ref) { return ref.provided ? ref.provided : ref.graph; }

    void CheckFiles()
    {
        if (clock < nextCheck)
            return;
        nextCheck = clock + kGraphCheckSeconds;
        for (auto& [key, ref] : graphs)
            if (Vfs::ModifiedTime(PathFromUtf8(key)) != ref.time)
                Load(key, ref);
    }

    Instance* FindInstance(Entity entity)
    {
        const auto it = instances.find(entity);
        return it == instances.end() ? nullptr : &it->second;
    }
    const Instance* FindInstance(Entity entity) const
    {
        const auto it = instances.find(entity);
        return it == instances.end() ? nullptr : &it->second;
    }

    // Unbound instances accept anything (checked once the graph binds).
    IkRuntime* Ik(Entity entity, std::string_view goal)
    {
        Instance& inst = instances[entity];
        if (const AnimGraph* graph = inst.player.Graph();
            graph && std::ranges::none_of(graph->ik, [&](const AnimIk& g) { return g.name == goal; }))
            return nullptr;
        const auto it = std::ranges::find(inst.ik, goal, &IkRuntime::name);
        if (it != inst.ik.end())
            return &*it;
        inst.ik.push_back(IkRuntime{.name = std::string(goal)});
        return &inst.ik.back();
    }

    bool SetParam(Entity entity, std::string_view name, float value, bool trigger)
    {
        Instance& inst = instances[entity];
        if (inst.player.Bound())
            return trigger ? inst.player.SetTrigger(name) : inst.player.SetParameter(name, value);
        std::erase_if(inst.pending, [&](const auto& p) { return p.first == name; });
        inst.pending.emplace_back(std::string(name), trigger ? 1.0f : value);
        return true;
    }

    void Bind(Instance& inst, const std::string& key, const GraphRef& ref, const Model* model, std::uint32_t revision)
    {
        inst.player.Bind(Current(ref), model);
        inst.graphKey      = key;
        inst.version       = ref.version;
        inst.model         = model;
        inst.modelRevision = revision;
        inst.pose.assign(model->nodes.size(), Transform{});
        inst.feet.assign(inst.player.Graph()->ik.size(), FootIkState{});
        for (const auto& [name, value] : inst.pending)
            inst.player.SetParameter(name, value);
        inst.pending.clear();
        for (const auto& [name, value] : inst.pendingLayers)
            inst.player.SetLayerWeight(name, value);
        inst.pendingLayers.clear();
    }

    // Binds (or rebinds after a graph / model change) when the model is ready and the graph loads.
    bool Ensure(Instance& inst, const AnimationController& controller, const ModelInstance& model)
    {
        const Model*      resolved = ResolveModel(model.model);
        const std::string key      = Key(controller.graph);
        GraphRef*         ref      = Find(key);
        if (!resolved || resolved->nodes.empty() || !ref)
            return false;
        const std::uint32_t revision = assets ? assets->Revision(model.model) : 0;
        if (inst.graphKey != key || inst.version != ref->version || inst.model != resolved || inst.modelRevision != revision ||
            !inst.player.Bound())
            Bind(inst, key, *ref, resolved, revision);
        return true;
    }

    // The character the controller belongs to: itself or the nearest ancestor with a
    // CharacterController or NavAgent; its subtree is not ground for the feet.
    static Entity Actor(const Registry& registry, Entity entity)
    {
        for (Entity e = entity; e != NullEntity && registry.Valid(e); e = registry.Get<Hierarchy>(e).parent)
            if (registry.TryGet<CharacterController>(e) || registry.TryGet<NavAgent>(e))
                return e;
        return entity;
    }

    void Update(Scene& scene, float dt, bool fireActions);
    void ApplyIk(Scene& scene, Entity root, Instance& inst, const glm::mat4& world, float dt, std::vector<std::uint8_t>& touched);
};

void AnimationSystem::Impl::ApplyIk(Scene& scene, Entity root, Instance& inst, const glm::mat4& world, float dt,
                                    std::vector<std::uint8_t>& touched)
{
    const AnimGraph& graph   = *inst.player.Graph();
    const Registry&  registry = scene.GetRegistry();
    PoseSpace        pose(*inst.model, inst.pose);
    const glm::mat4  modelFromWorld = glm::inverse(world);
    const auto toModel = [&](const glm::vec3& p) { return glm::vec3(modelFromWorld * glm::vec4(p, 1.0f)); };
    const auto mark    = [&](int node) {
        if (node >= 0 && static_cast<std::size_t>(node) < touched.size())
            touched[static_cast<std::size_t>(node)] = 1;
    };
    const auto markChain = [&](int node, int depth) {
        for (int i = 0; i < depth && node >= 0; ++i, node = pose.Parent(node))
            mark(node);
    };

    const Entity  actor = Actor(registry, root);
    std::uint16_t layerMask = 0xFFFF;
    const GroundRaycast ground = [&](const glm::vec3& from, const glm::vec3& to) -> std::optional<GroundHit> {
        if (!physics)
            return std::nullopt;
        glm::vec3   origin = from;
        const float total  = glm::length(to - from);
        if (total <= 1.0e-6f)
            return std::nullopt;
        const glm::vec3 dir = (to - from) / total;
        float           left = total;
        for (int attempt = 0; attempt < 4 && left > 0.0f; ++attempt) {
            const auto hit = physics->Raycast(origin, dir, left, actor, layerMask); // rays start inside its capsule
            if (!hit)
                return std::nullopt;
            const bool own = registry.Valid(hit->entity) &&
                             (IsAncestorOrSelf(registry, actor, hit->entity) || IsAncestorOrSelf(registry, hit->entity, root));
            if (!own)
                return GroundHit{.point = hit->point, .normal = hit->normal};
            const float advance = hit->distance + 1.0e-3f;
            origin += dir * advance;
            left -= advance;
        }
        return std::nullopt;
    };

    for (std::size_t g = 0; g < graph.ik.size(); ++g) {
        const AnimIk& goal = graph.ik[g];
        const auto    rt   = std::ranges::find(inst.ik, goal.name, &IkRuntime::name);
        const IkRuntime* runtime = rt == inst.ik.end() ? nullptr : &*rt;
        if (!(runtime && runtime->enabled ? *runtime->enabled : goal.enabled))
            continue;
        float weight = goal.weight * (runtime ? runtime->weight : 1.0f);
        if (!goal.weightParameter.empty())
            weight *= std::clamp(inst.player.Parameter(goal.weightParameter).value_or(1.0f), 0.0f, 1.0f);
        std::optional<glm::vec3> target;
        if (runtime && runtime->targetEntity != NullEntity && registry.Valid(runtime->targetEntity))
            target = glm::vec3(registry.Get<WorldTransform>(runtime->targetEntity).matrix[3]);
        else if (runtime && runtime->hasTarget)
            target = runtime->target;

        bool solved = false;
        switch (goal.kind) {
        case AnimIkKind::TwoBone: {
            const int end = inst.player.NodeIndex(goal.endBone);
            if (!target || end < 0)
                break;
            const glm::vec3 pole = runtime && runtime->hasPole ? toModel(runtime->pole) : glm::vec3(0.0f);
            solved = SolveTwoBoneIk(pose, end, toModel(*target), runtime && runtime->hasPole ? &pole : nullptr,
                                    goal.keepEndRotation, weight);
            if (solved)
                markChain(end, 3);
            break;
        }
        case AnimIkKind::Fabrik: {
            const int end = inst.player.NodeIndex(goal.endBone), base = inst.player.NodeIndex(goal.rootBone);
            if (!target || end < 0 || base < 0)
                break;
            solved = SolveFabrikIk(pose, base, end, toModel(*target), goal.iterations, goal.tolerance, weight);
            if (solved)
                for (int node = end; node >= 0; node = pose.Parent(node)) {
                    mark(node);
                    if (node == base)
                        break;
                }
            break;
        }
        case AnimIkKind::LookAt: {
            if (!target)
                break;
            std::vector<int> chain;
            for (const std::string& bone : goal.bones)
                if (const int node = inst.player.NodeIndex(bone); node >= 0)
                    chain.push_back(node);
            solved = SolveLookAtIk(pose, chain, goal.forwardAxis, toModel(*target), glm::radians(goal.maxAngle), weight);
            if (solved)
                for (const int node : chain)
                    mark(node);
            break;
        }
        case AnimIkKind::FootPlacement: {
            if (!physics)
                break;
            std::vector<int> feet;
            for (const std::string& bone : goal.feet)
                if (const int node = inst.player.NodeIndex(bone); node >= 0)
                    feet.push_back(node);
            if (feet.empty())
                break;
            const int pelvis = inst.player.NodeIndex(goal.pelvisBone);
            layerMask        = static_cast<std::uint16_t>(goal.layerMask);
            SolveFootPlacement(pose, world, goal, feet, pelvis, ground, inst.feet[g], dt, weight);
            solved = weight > 0.0f;
            if (solved) {
                for (const int foot : feet)
                    markChain(foot, 3);
                mark(pelvis);
            }
            break;
        }
        }
        if (solved)
            ++stats.ikSolved;
    }
}

void AnimationSystem::Impl::Update(Scene& scene, float dt, bool fireActions)
{
    const auto started = std::chrono::steady_clock::now();
    dt = std::isfinite(dt) ? std::max(dt, 0.0f) : 0.0f;
    clock += dt;
    CheckFiles();
    stats.notifies = stats.ikSolved = 0;
    Registry& registry = scene.GetRegistry();

    std::unordered_map<Entity, std::vector<Entity>> nodeEntities;
    bool collected = false;
    const auto collect = [&] {
        if (collected)
            return;
        collected = true;
        registry.ViewOf<ModelNodeRef>().Each([&](Entity entity, ModelNodeRef& reference) {
            const Entity root = FindModelInstanceRoot(registry, entity);
            if (root == NullEntity || !instances.contains(root))
                return;
            auto& nodes = nodeEntities[root];
            if (nodes.size() <= reference.node)
                nodes.resize(static_cast<std::size_t>(reference.node) + 1, NullEntity);
            nodes[reference.node] = entity;
        });
    };

    for (auto& [entity, inst] : instances)
        inst.seen = false;
    struct Pending {
        Entity entity;
        std::vector<AnimFiredNotify> notifies;
        glm::vec3 rootMotion;
        bool      applyRoot;
        float     dt;
    };
    std::vector<Pending> work;
    registry.ViewOf<AnimationController, ModelInstance>().Each([&](Entity root, AnimationController& controller, ModelInstance& model) {
        if (!controller.enabled || controller.graph.empty())
            return;
        Instance& inst = instances[root];
        inst.seen      = true;
        if (const auto* ragdoll = registry.TryGet<Ragdoll>(root); ragdoll && ragdoll->simulate)
            return; // the physics moves the bones
        if (!Ensure(inst, controller, model))
            return;
        work.push_back(Pending{.entity = root, .notifies = {}, .rootMotion = glm::vec3(0.0f),
                               .applyRoot = controller.applyRootMotion,
                               .dt = paused ? 0.0f : dt * (std::isfinite(controller.speed) ? std::max(controller.speed, 0.0f) : 0.0f)});
    });
    std::erase_if(instances, [](const auto& entry) { return !entry.second.seen; });

    for (Pending& item : work) {
        Instance& inst = instances[item.entity];
        if (!inst.player.Bound())
            continue;
        collect();
        inst.player.Update(item.dt, inst.pose, &item.notifies, &item.rootMotion);
        const glm::mat4 world = registry.Get<WorldTransform>(item.entity).matrix;
        std::vector<std::uint8_t> touched = inst.player.DrivenNodes();
        const auto* controller = registry.TryGet<AnimationController>(item.entity);
        if (controller && controller->ik && !inst.player.Graph()->ik.empty())
            ApplyIk(scene, item.entity, inst, world, item.dt, touched);

        auto& nodes = nodeEntities[item.entity];
        if (nodes.size() < inst.pose.size())
            nodes.resize(inst.pose.size(), NullEntity);
        for (std::size_t i = 0; i < inst.pose.size(); ++i)
            if (touched[i] && nodes[i] != NullEntity && registry.Valid(nodes[i]) && scene.GetTransform(nodes[i]) != inst.pose[i])
                scene.SetTransform(nodes[i], inst.pose[i]);

        // Root motion: the bone's movement (parent space of the bone) -> model -> world.
        const int rootNode = inst.player.RootMotionNode();
        if (item.applyRoot && rootNode >= 0 && glm::length(item.rootMotion) > 1.0e-7f) {
            PoseSpace       space(*inst.model, inst.pose);
            const int       parent = space.Parent(rootNode);
            const glm::vec3 modelDelta = parent >= 0 ? glm::mat3(space.Global(parent)) * item.rootMotion : item.rootMotion;
            const glm::vec3 worldDelta = glm::mat3(world) * modelDelta;
            Entity          mover      = Actor(registry, item.entity);
            if (physics && registry.TryGet<CharacterController>(mover) && item.dt > 0.0f) {
                physics->SetCharacterInput(mover, glm::vec3(worldDelta.x, 0.0f, worldDelta.z) / item.dt, false);
            } else {
                mover = item.entity;
                Transform transform = scene.GetTransform(mover);
                glm::vec3 delta     = worldDelta;
                const Entity parentEntity = registry.Get<Hierarchy>(mover).parent;
                if (parentEntity != NullEntity && registry.Valid(parentEntity)) {
                    const glm::mat3 parentBasis(registry.Get<WorldTransform>(parentEntity).matrix);
                    const float     det = glm::determinant(parentBasis);
                    if (std::isfinite(det) && std::abs(det) > 1.0e-12f)
                        delta = glm::inverse(parentBasis) * delta;
                }
                transform.position += delta;
                scene.SetTransform(mover, transform);
            }
        }

        // Notifies.
        for (const AnimFiredNotify& fired : item.notifies) {
            glm::vec3 position(world[3]);
            if (fired.boneNode >= 0) {
                PoseSpace space(*inst.model, inst.pose);
                position = glm::vec3(world * glm::vec4(space.Position(fired.boneNode), 1.0f));
            }
            ++stats.notifies;
            if (events)
                events->Publish(AnimNotifyEvent{.entity = item.entity, .name = fired.notify.name, .state = fired.state,
                                                .bone = fired.notify.bone, .position = position});
            if (!fireActions)
                continue;
            if (audio && !fired.notify.sound.empty())
                audio->PlayAt(fired.notify.sound, position, fired.notify.volume);
            if (particles && !fired.notify.effect.empty())
                particles->SpawnAt(fired.notify.effect, position);
        }
    }

    stats.instances = static_cast<std::uint32_t>(std::ranges::count_if(instances, [](const auto& e) { return e.second.player.Bound(); }));
    stats.graphs    = static_cast<std::uint32_t>(std::ranges::count_if(graphs, [](const auto& e) { return e.second.graph != nullptr; }));
    stats.updateMs  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

AnimationSystem::AnimationSystem(AssetManager* assets, PhysicsWorld* physics, EventBus* events)
    : m_Impl(std::make_unique<Impl>())
{
    m_Impl->assets  = assets;
    m_Impl->physics = physics;
    m_Impl->events  = events;
}

AnimationSystem::AnimationSystem(ModelResolver resolver, PhysicsWorld* physics, EventBus* events)
    : m_Impl(std::make_unique<Impl>())
{
    m_Impl->resolver = std::move(resolver);
    m_Impl->physics  = physics;
    m_Impl->events   = events;
}

AnimationSystem::~AnimationSystem() = default;

void AnimationSystem::SetAudio(AudioSystem* audio) { m_Impl->audio = audio; }
void AnimationSystem::SetParticles(ParticleWorld* particles) { m_Impl->particles = particles; }
void AnimationSystem::Update(Scene& scene, float dt) { m_Impl->Update(scene, dt, fireNotifyActions); }
void AnimationSystem::Reset() { m_Impl->instances.clear(); }

bool AnimationSystem::Prepare(Scene& scene, Entity entity)
{
    const Registry& registry = scene.GetRegistry();
    if (!registry.Valid(entity))
        return false;
    const auto* controller = registry.TryGet<AnimationController>(entity);
    const auto* model      = registry.TryGet<ModelInstance>(entity);
    if (!controller || !model || !controller->enabled || controller->graph.empty())
        return false;
    return m_Impl->Ensure(m_Impl->instances[entity], *controller, *model);
}
void AnimationSystem::SetPaused(bool paused) { m_Impl->paused = paused; }
bool AnimationSystem::Paused() const { return m_Impl->paused; }

bool AnimationSystem::SetFloat(Entity entity, std::string_view parameter, float value)
{
    return m_Impl->SetParam(entity, parameter, value, false);
}
bool AnimationSystem::SetBool(Entity entity, std::string_view parameter, bool value)
{
    return m_Impl->SetParam(entity, parameter, value ? 1.0f : 0.0f, false);
}
bool AnimationSystem::SetInt(Entity entity, std::string_view parameter, int value)
{
    return m_Impl->SetParam(entity, parameter, static_cast<float>(value), false);
}
bool AnimationSystem::SetTrigger(Entity entity, std::string_view parameter) { return m_Impl->SetParam(entity, parameter, 1.0f, true); }

std::optional<float> AnimationSystem::Parameter(Entity entity, std::string_view parameter) const
{
    const Impl::Instance* inst = m_Impl->FindInstance(entity);
    if (!inst)
        return std::nullopt;
    if (inst->player.Bound())
        return inst->player.Parameter(parameter);
    for (const auto& [name, value] : inst->pending)
        if (name == parameter)
            return value;
    return std::nullopt;
}

bool AnimationSystem::SetLayerWeight(Entity entity, std::string_view layer, float weight)
{
    Impl::Instance& inst = m_Impl->instances[entity];
    if (inst.player.Bound())
        return inst.player.SetLayerWeight(layer, weight);
    inst.pendingLayers.emplace_back(std::string(layer), weight);
    return true;
}

std::string AnimationSystem::CurrentState(Entity entity, std::string_view layer) const
{
    const Impl::Instance* inst = m_Impl->FindInstance(entity);
    if (!inst || !inst->player.Bound())
        return {};
    const auto& layers = inst->player.Graph()->layers;
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (layer.empty() ? i == 0 : layers[i].name == layer)
            return inst->player.LayerStatus(i).state;
    return {};
}

float AnimationSystem::StateTime(Entity entity, std::string_view layer) const
{
    const Impl::Instance* inst = m_Impl->FindInstance(entity);
    if (!inst || !inst->player.Bound())
        return 0.0f;
    const auto& layers = inst->player.Graph()->layers;
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (layer.empty() ? i == 0 : layers[i].name == layer)
            return inst->player.LayerStatus(i).normalizedTime;
    return 0.0f;
}

void AnimationSystem::Restart(Entity entity)
{
    if (Impl::Instance* inst = m_Impl->FindInstance(entity))
        inst->player.Restart();
}

bool AnimationSystem::SetIkTarget(Entity entity, std::string_view goal, const glm::vec3& target)
{
    Impl::IkRuntime* ik = m_Impl->Ik(entity, goal);
    if (!ik)
        return false;
    ik->hasTarget = true, ik->target = target, ik->targetEntity = NullEntity;
    return true;
}

bool AnimationSystem::SetIkTargetEntity(Entity entity, std::string_view goal, Entity target)
{
    Impl::IkRuntime* ik = m_Impl->Ik(entity, goal);
    if (!ik)
        return false;
    ik->targetEntity = target, ik->hasTarget = false;
    return true;
}

bool AnimationSystem::ClearIkTarget(Entity entity, std::string_view goal)
{
    Impl::IkRuntime* ik = m_Impl->Ik(entity, goal);
    if (!ik)
        return false;
    ik->hasTarget = false, ik->targetEntity = NullEntity;
    return true;
}

bool AnimationSystem::SetIkPole(Entity entity, std::string_view goal, const glm::vec3& pole)
{
    Impl::IkRuntime* ik = m_Impl->Ik(entity, goal);
    if (!ik)
        return false;
    ik->hasPole = true, ik->pole = pole;
    return true;
}

bool AnimationSystem::SetIkWeight(Entity entity, std::string_view goal, float weight)
{
    Impl::IkRuntime* ik = m_Impl->Ik(entity, goal);
    if (!ik)
        return false;
    ik->weight = std::clamp(std::isfinite(weight) ? weight : 0.0f, 0.0f, 1.0f);
    return true;
}

bool AnimationSystem::SetIkEnabled(Entity entity, std::string_view goal, bool enabled)
{
    Impl::IkRuntime* ik = m_Impl->Ik(entity, goal);
    if (!ik)
        return false;
    ik->enabled = enabled;
    return true;
}

std::optional<AnimationInstanceInfo> AnimationSystem::Inspect(Entity entity) const
{
    const Impl::Instance* inst = m_Impl->FindInstance(entity);
    if (!inst || !inst->player.Bound())
        return std::nullopt;
    AnimationInstanceInfo info;
    info.graph = inst->graphKey;
    for (std::size_t i = 0; i < inst->player.LayerCount(); ++i)
        info.layers.push_back(inst->player.LayerStatus(i));
    for (const AnimParameter& p : inst->player.Graph()->parameters)
        info.parameters.emplace_back(p, inst->player.Parameter(p.name).value_or(0.0f));
    return info;
}

bool AnimationSystem::HasInstance(Entity entity) const
{
    const Impl::Instance* inst = m_Impl->FindInstance(entity);
    return inst && inst->player.Bound();
}

std::shared_ptr<const AnimGraph> AnimationSystem::Graph(const std::string& path)
{
    Impl::GraphRef* ref = m_Impl->Find(Key(path));
    return ref ? Impl::Current(*ref) : nullptr;
}

void AnimationSystem::Provide(const std::string& path, std::shared_ptr<const AnimGraph> graph)
{
    const std::string key = Key(path);
    if (key.empty())
        return;
    auto [it, inserted] = m_Impl->graphs.try_emplace(key);
    if (inserted)
        it->second.time = Vfs::ModifiedTime(PathFromUtf8(key));
    const bool same = graph && it->second.provided ? *graph == *it->second.provided
                    : !graph && it->second.provided && it->second.graph ? *it->second.provided == *it->second.graph
                                                                          : false;
    it->second.provided = std::move(graph);
    if (!same)
        ++it->second.version;
}

const AnimationStats& AnimationSystem::Stats() const { return m_Impl->stats; }

} // namespace Engine
