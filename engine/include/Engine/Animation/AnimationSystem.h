#pragma once
#include "Engine/Animation/AnimGraph.h"
#include "Engine/Animation/AnimGraphPlayer.h"
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

class AssetManager;
class AudioSystem;
class EventBus;
class ParticleWorld;
class PhysicsWorld;
class Scene;
struct Model;

// Published (EventBus::Publish) when a state passes a notify; also what On Anim Notify gets.
struct AnimNotifyEvent {
    Entity      entity = NullEntity; // the controller's entity
    std::string name;
    std::string state;
    std::string bone;
    glm::vec3   position{0.0f}; // world position of the bone (else the entity)
};

struct AnimationStats {
    std::uint32_t instances = 0; // controllers running a graph
    std::uint32_t graphs    = 0; // graph files loaded
    std::uint32_t notifies  = 0; // fired last Update
    std::uint32_t ikSolved  = 0; // goals applied last Update
    double        updateMs  = 0.0;
};

// What the inspector / editor shows of a running controller.
struct AnimationInstanceInfo {
    std::string                  graph;
    std::vector<AnimLayerStatus> layers;
    std::vector<std::pair<AnimParameter, float>> parameters; // definition, current value
};

// Runs AnimationController components: per instantiated model root an AnimGraphPlayer
// (parameters, layered state machines, blend spaces), then the graph's IK goals in model space
// (foot placement raycasts the PhysicsWorld), then writes the local poses of the bone entities.
// Notifies are published as AnimNotifyEvent and play their sound (AudioSystem) / particle effect
// (ParticleWorld) when `fireNotifyActions`. Graph files load through the VFS, cached by path and
// reloaded when they change (checked 1/s; instances rebind, parameters stay). Main thread.
class AnimationSystem {
public:
    using ModelResolver = std::function<const Model*(ModelHandle)>;

    explicit AnimationSystem(AssetManager* assets, PhysicsWorld* physics = nullptr, EventBus* events = nullptr);
    // Without an AssetManager (tests, tools): models come from `resolver`.
    AnimationSystem(ModelResolver resolver, PhysicsWorld* physics = nullptr, EventBus* events = nullptr);
    ~AnimationSystem();

    AnimationSystem(const AnimationSystem&)            = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    void SetAudio(AudioSystem* audio);
    void SetParticles(ParticleWorld* particles);
    // Sounds / particles of notifies (the editor turns them off outside play).
    bool fireNotifyActions = true;

    // Per frame after scripts (parameters) and before rendering; dt 0 re-poses without advancing.
    void Update(Scene& scene, float dt);
    // Forget every instance (scene cleared / play toggled); they start again at their entry states.
    void Reset();
    void SetPaused(bool paused); // poses stay, nothing advances
    // Binds the entity's controller now when it can (scripts before its first Update): parameter
    // and goal names are then checked. False while the model / graph is not ready.
    bool Prepare(Scene& scene, Entity entity);
    [[nodiscard]] bool Paused() const;

    // --- Gameplay / scripts (entity = the controller's entity). Values set before the first
    // Update of an entity are kept for it. False for unknown parameters / layers / goals.
    bool SetFloat(Entity entity, std::string_view parameter, float value);
    bool SetBool(Entity entity, std::string_view parameter, bool value);
    bool SetInt(Entity entity, std::string_view parameter, int value);
    bool SetTrigger(Entity entity, std::string_view parameter);
    [[nodiscard]] std::optional<float> Parameter(Entity entity, std::string_view parameter) const;
    bool SetLayerWeight(Entity entity, std::string_view layer, float weight);
    [[nodiscard]] std::string CurrentState(Entity entity, std::string_view layer = {}) const; // "" = first layer
    [[nodiscard]] float       StateTime(Entity entity, std::string_view layer = {}) const;   // normalized, unwrapped
    void Restart(Entity entity);

    // IK goals of the entity's graph by name. Targets in world space (or following an entity).
    bool SetIkTarget(Entity entity, std::string_view goal, const glm::vec3& target);
    bool SetIkTargetEntity(Entity entity, std::string_view goal, Entity target);
    bool ClearIkTarget(Entity entity, std::string_view goal);
    bool SetIkPole(Entity entity, std::string_view goal, const glm::vec3& pole); // TwoBone bend direction (world point)
    bool SetIkWeight(Entity entity, std::string_view goal, float weight);       // runtime multiplier (default 1)
    bool SetIkEnabled(Entity entity, std::string_view goal, bool enabled);

    [[nodiscard]] std::optional<AnimationInstanceInfo> Inspect(Entity entity) const;
    [[nodiscard]] bool HasInstance(Entity entity) const;

    // Loaded (cached) graph; nullptr if missing / broken (logged once per change).
    [[nodiscard]] std::shared_ptr<const AnimGraph> Graph(const std::string& path);
    // Editor: an unsaved version of a graph file (null: back to the file). Instances rebind.
    void Provide(const std::string& path, std::shared_ptr<const AnimGraph> graph);

    [[nodiscard]] const AnimationStats& Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
