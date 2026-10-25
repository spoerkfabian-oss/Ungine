#pragma once
#include "Engine/Animation/AnimGraph.h"
#include "Engine/Scene/Components.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

struct Model;

// A notify that fired during an update.
struct AnimFiredNotify {
    AnimNotify    notify;
    std::uint32_t layer = 0;
    std::string   state;
    int           boneNode = -1; // model node of notify.bone (-1: none / unknown)
};

struct AnimLayerStatus {
    std::string state;             // current state (the target while fading)
    float       normalizedTime = 0.0f; // unwrapped (1.25 = a quarter into the second loop)
    std::string fromState;         // fading out ("" when not fading; "(frozen)" after an interruption)
    float       transition = -1.0f; // fade progress 0..1, -1 = none
    float       weight = 0.0f;      // effective layer weight
};

// Runs an AnimGraph on one model: parameters, a state machine per layer, crossfades, blend
// spaces (phase-synchronized), layer masks / additive layers, notifies and root motion. The
// output is a local pose (one Transform per model node) before IK. Main thread, no ECS.
class AnimGraphPlayer {
public:
    // Resolves clips / bones / parameters by name and starts every layer in its entry state.
    // Parameter values whose name and type stay are kept. A null graph or model unbinds.
    void Bind(std::shared_ptr<const AnimGraph> graph, const Model* model);
    [[nodiscard]] bool              Bound() const { return m_Graph && m_Model; }
    [[nodiscard]] const AnimGraph*  Graph() const { return m_Graph.get(); }
    [[nodiscard]] const Model*      BoundModel() const { return m_Model; }
    // Every layer back to its entry state (parameters stay).
    void Restart();

    // Parameters by name; false for unknown names. Int rounds, Bool / Trigger set when != 0.
    bool SetParameter(std::string_view name, float value);
    bool SetTrigger(std::string_view name);
    [[nodiscard]] std::optional<float> Parameter(std::string_view name) const;
    // Runtime multiplier of a layer's weight (1 by default); false for unknown layers.
    bool SetLayerWeight(std::string_view layer, float weight);

    // Advances by dt (seconds, >= 0) and writes the pose. `rootMotion`: movement of the graph's
    // root motion bone since the last update (in the bone's parent space; the bone's own
    // translation is reset to the rest pose then).
    void Update(float dt, std::span<Transform> pose, std::vector<AnimFiredNotify>* notifies = nullptr,
                glm::vec3* rootMotion = nullptr);

    [[nodiscard]] std::size_t     LayerCount() const { return m_Layers.size(); }
    [[nodiscard]] AnimLayerStatus LayerStatus(std::size_t layer) const;
    // Model nodes any clip of the graph animates (plus the root motion bone).
    [[nodiscard]] const std::vector<std::uint8_t>& DrivenNodes() const { return m_Driven; }
    [[nodiscard]] int  NodeIndex(std::string_view bone) const;
    [[nodiscard]] int  RootMotionNode() const { return m_RootMotionNode; }

    // Blend space weights of the current state of a layer at the current parameters (editor).
    [[nodiscard]] std::vector<float> BlendWeights(std::size_t layer) const;

private:
    struct Sample {
        int       clip = -1;
        glm::vec2 position{0.0f};
        float     speed = 1.0f;
    };
    struct State {
        const AnimState*   source = nullptr;
        int                clip = -1;
        int                speedParam = -1, xParam = -1, yParam = -1;
        std::vector<Sample> samples{};
        std::vector<std::array<int, 3>> triangles{}; // 2D: Delaunay triangulation of the samples
        std::vector<int>   notifyBones{};
    };
    struct Condition {
        int         param = -1;
        AnimCompare op = AnimCompare::Greater;
        float       value = 0.0f;
    };
    struct Transition {
        const AnimTransition* source = nullptr;
        int                   from = -1, to = -1; // from -1: any state
        std::vector<Condition> conditions{};
    };
    struct Play {
        int   state = -1;
        float time  = 0.0f; // unwrapped normalized time
    };
    struct Layer {
        const AnimLayer*        source = nullptr;
        std::vector<State>      states{};
        std::vector<Transition> transitions{};
        int                     entry = -1;
        int                     weightParam = -1;
        float                   runtimeWeight = 1.0f;
        std::vector<float>      mask{}; // per node
        Play                    current, previous;
        bool                    fading = false, frozen = false;
        float                   fadeElapsed = 0.0f, fadeDuration = 0.0f;
        bool                    fadeInterruptible = true;
        std::vector<Transform>  frozenPose{}, lastPose{};
        float                   effectiveWeight = 0.0f;
    };

    void  ResolveState(State& state, const AnimState& source);
    [[nodiscard]] float Value(int param) const;
    [[nodiscard]] bool  Holds(const Transition& transition, const Layer& layer) const;
    [[nodiscard]] float StateRate(const State& state) const; // normalized time per second
    [[nodiscard]] float ClipDuration(int clip) const;
    void  Weights(const State& state, std::vector<float>& out) const;
    void  EvaluateState(const State& state, float time, std::span<Transform> pose);
    [[nodiscard]] glm::vec3 RootDelta(const State& state, float from, float to) const;
    void  Fire(const Layer& layer, std::uint32_t index, const State& state, float from, float to,
               std::vector<AnimFiredNotify>& out) const;
    void  StartTransition(Layer& layer, const Transition& transition);

    std::shared_ptr<const AnimGraph> m_Graph;
    const Model*                     m_Model = nullptr;
    std::vector<float>               m_Values;
    std::vector<Layer>               m_Layers;
    std::vector<std::uint8_t>        m_Driven;
    std::vector<Transform>           m_Rest;
    int                              m_RootMotionNode = -1;
    std::vector<Transform>           m_Scratch, m_Accum;
    mutable std::vector<float>       m_WeightScratch;
};

} // namespace Engine
