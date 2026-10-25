#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

struct Model;

// Animation graph asset (.uanim): parameters, layered state machines whose states play a clip or
// a 1D / 2D blend space, transitions with conditions and crossfades, anim notifies and IK goals.
// Clips and bones are referenced by name and resolved against the model the graph drives
// (AnimationController on an instantiated model root).

enum class AnimParamType : std::uint8_t { Float, Int, Bool, Trigger };

struct AnimParameter {
    std::string   name = "Speed";
    AnimParamType type = AnimParamType::Float;
    float         defaultValue = 0.0f; // Int / Bool / Trigger stored as numbers (0 / 1)
    bool operator==(const AnimParameter&) const = default;
};

enum class AnimCompare : std::uint8_t { Greater, Less, GreaterEqual, LessEqual, Equal, NotEqual, IsTrue, IsFalse, Triggered };

struct AnimCondition {
    std::string parameter{};
    AnimCompare op    = AnimCompare::Greater;
    float       value = 0.0f; // ignored by IsTrue / IsFalse / Triggered
    bool operator==(const AnimCondition&) const = default;
};

enum class AnimStateKind : std::uint8_t { Clip, BlendSpace1D, BlendSpace2D };

// One clip of a blend space at its parameter position (1D: x only).
struct AnimBlendSample {
    std::string clip{};
    glm::vec2   position{0.0f};
    float       speed = 1.0f; // playback rate of this clip
    bool operator==(const AnimBlendSample&) const = default;
};

// Fired when the state's normalized time passes `time` (0..1, every loop). The event reaches
// blueprints (On Anim Notify); a sound / particle effect plays at the bone (else the entity).
struct AnimNotify {
    std::string name = "Notify";
    float       time = 0.0f;
    std::string sound{};  // sound file or cue (absolute in memory, relative to the graph in files)
    std::string effect{}; // .uparticle
    std::string bone{};
    float       volume = 1.0f;
    bool operator==(const AnimNotify&) const = default;
};

struct AnimState {
    std::string   name = "State";
    AnimStateKind kind = AnimStateKind::Clip;
    std::string   clip{};            // Clip
    float         speed = 1.0f;
    std::string   speedParameter{};  // multiplies the speed (Float / Int parameter)
    bool          loop  = true;
    std::string   xParameter{}, yParameter{}; // blend spaces
    std::vector<AnimBlendSample> samples{};
    std::vector<AnimNotify>      notifies{};
    glm::vec2     position{0.0f};  // editor canvas
    bool operator==(const AnimState&) const = default;
};

struct AnimTransition {
    std::string from{};               // empty: any state
    std::string to{};
    float       duration = 0.2f;    // crossfade (s)
    std::vector<AnimCondition> conditions{}; // all must hold
    bool        hasExitTime = false;
    float       exitTime    = 1.0f; // normalized time of `from` (unwrapped: 1.5 = halfway through the 2nd loop)
    bool        canInterrupt = true; // other transitions out of the target may start during the fade
    bool operator==(const AnimTransition&) const = default;
};

enum class AnimLayerMode : std::uint8_t { Override, Additive };

struct AnimBoneMask {
    std::string bone{};
    bool        includeChildren = true;
    bool operator==(const AnimBoneMask&) const = default;
};

struct AnimLayer {
    std::string   name = "Base";
    AnimLayerMode mode = AnimLayerMode::Override;
    float         weight = 1.0f;
    std::string   weightParameter{}; // multiplies the weight
    std::vector<AnimBoneMask> mask{}; // empty: every bone
    std::string   entryState{};
    std::vector<AnimState>      states{};
    std::vector<AnimTransition> transitions{};
    bool operator==(const AnimLayer&) const = default;

    [[nodiscard]] const AnimState* FindState(std::string_view name) const;
};

enum class AnimIkKind : std::uint8_t { TwoBone, FootPlacement, LookAt, Fabrik };

// IK goal applied after the layers (model pose -> world). Targets of TwoBone / LookAt / Fabrik
// come at runtime (AnimationSystem::SetIkTarget, blueprint nodes); without a target they rest.
struct AnimIk {
    std::string name = "IK";
    AnimIkKind  kind = AnimIkKind::TwoBone;
    bool        enabled = true;
    float       weight  = 1.0f;
    std::string weightParameter{};
    // TwoBone: endBone with its parent and grandparent. Fabrik: chain rootBone .. endBone.
    std::string endBone{};
    std::string rootBone{};
    bool        keepEndRotation = true; // TwoBone: the end keeps its world orientation
    std::uint32_t iterations = 12;      // Fabrik
    float       tolerance  = 0.001f;    // Fabrik (model units)
    // LookAt: bones from the root of the chain to the one that aims (shares the rotation).
    std::vector<std::string> bones{};
    glm::vec3   forwardAxis{0.0f, 0.0f, 1.0f}; // aiming axis in the last bone's space
    float       maxAngle = 70.0f;              // degrees
    // FootPlacement: each foot is the end of a two-bone leg; rays go down from rayHeight above
    // the entity origin to rayDepth below it (world units); the pelvis lowers for the lowest foot.
    std::vector<std::string> feet{};
    std::string pelvisBone{};
    float       rayHeight = 0.5f;
    float       rayDepth  = 0.75f;
    float       maxPelvisOffset = 0.4f;
    bool        alignToNormal = true;
    float       maxFootAngle  = 30.0f;  // degrees
    float       smoothing     = 12.0f;  // 1/s (0: instant)
    std::uint32_t layerMask   = 0xFFFF; // physics layers the rays hit
    bool operator==(const AnimIk&) const = default;
};

struct AnimGraph {
    std::vector<AnimParameter> parameters{};
    std::vector<AnimLayer>     layers{};
    std::vector<AnimIk>        ik{};
    std::string                rootMotionBone{}; // applied when the controller enables root motion
    std::string                previewModel{};   // editor preview (absolute in memory)
    bool operator==(const AnimGraph&) const = default;

    [[nodiscard]] const AnimParameter* FindParameter(std::string_view name) const;
};

// A starting point: one layer with an Idle clip state (first clip of the preview model).
[[nodiscard]] AnimGraph MakeDefaultAnimGraph();

[[nodiscard]] bool IsAnimGraphFile(const std::filesystem::path& file); // by extension (.uanim)
// Through the VFS; paths (preview model, notify sounds / effects) come back absolute.
[[nodiscard]] std::optional<AnimGraph> LoadAnimGraph(const std::filesystem::path& file, std::string* error = nullptr);
[[nodiscard]] std::optional<AnimGraph> ParseAnimGraph(std::string_view json, const std::filesystem::path& folder,
                                                      std::string* error = nullptr);
// Atomic write; paths relative to the graph's folder.
bool SaveAnimGraph(const std::filesystem::path& file, const AnimGraph& graph, std::string* error = nullptr);
[[nodiscard]] std::string AnimGraphToJson(const AnimGraph& graph, const std::filesystem::path& folder);
// Problems an author should know about (one line each): unknown parameters / states, duplicate
// names, empty blend spaces, degenerate 2D spaces; with a model also unknown clips and bones.
[[nodiscard]] std::vector<std::string> ValidateAnimGraph(const AnimGraph& graph, const Model* model = nullptr);

[[nodiscard]] const char* AnimParamTypeName(AnimParamType type);
[[nodiscard]] const char* AnimCompareName(AnimCompare op);
[[nodiscard]] const char* AnimStateKindName(AnimStateKind kind);
[[nodiscard]] const char* AnimIkKindName(AnimIkKind kind);

} // namespace Engine
