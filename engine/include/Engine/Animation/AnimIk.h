#pragma once
#include "Engine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Engine {

struct AnimIk;
struct Model;

// Model-space view of a local pose (one Transform per model node, parents before children):
// IK solvers read bone positions / rotations in model space and write local transforms back.
class PoseSpace {
public:
    PoseSpace(const Model& model, std::span<Transform> locals);

    [[nodiscard]] std::size_t Size() const { return m_Locals.size(); }
    [[nodiscard]] int         Parent(int node) const;
    [[nodiscard]] const glm::mat4& Global(int node) const;
    [[nodiscard]] glm::vec3   Position(int node) const { return glm::vec3(Global(node)[3]); }
    [[nodiscard]] glm::quat   Rotation(int node) const;
    [[nodiscard]] const Transform& Local(int node) const { return m_Locals[static_cast<std::size_t>(node)]; }

    void SetLocal(int node, const Transform& local);
    // Pre-multiplies the node's model rotation by `delta` (children follow).
    void Rotate(int node, const glm::quat& delta);
    // Moves the node by a model-space offset (children follow).
    void Translate(int node, const glm::vec3& delta);

private:
    void Update() const;

    std::span<Transform>           m_Locals;
    std::vector<int>               m_Parents;
    mutable std::vector<glm::mat4> m_Globals;
    mutable bool                   m_Dirty = true;
};

// Shortest rotation taking direction a to direction b (both non-zero).
[[nodiscard]] glm::quat RotationBetween(const glm::vec3& a, const glm::vec3& b);

// Analytic two-bone IK: the end node reaches `target` (model space) by rotating its grandparent
// and parent; the bend stays in the plane towards `pole` (else the current bend). Out of reach:
// the chain points straight at the target. Returns false for a chain without two ancestors.
bool SolveTwoBoneIk(PoseSpace& pose, int endNode, const glm::vec3& target, const glm::vec3* pole,
                    bool keepEndRotation, float weight);
// FABRIK over rootNode .. endNode (rootNode an ancestor of endNode); bone lengths stay.
bool SolveFabrikIk(PoseSpace& pose, int rootNode, int endNode, const glm::vec3& target, std::uint32_t iterations,
                   float tolerance, float weight);
// Turns the chain (root .. aiming bone) so that `axis` (in the aiming bone's space) points at
// `target`, at most maxAngle (radians) away from the animated direction; each bone takes a share.
bool SolveLookAtIk(PoseSpace& pose, std::span<const int> chain, const glm::vec3& axis, const glm::vec3& target,
                   float maxAngle, float weight);

struct GroundHit {
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};
// World-space ray from -> to; the closest hit.
using GroundRaycast = std::function<std::optional<GroundHit>(const glm::vec3& from, const glm::vec3& to)>;

// Smoothed per-instance state of a foot placement goal.
struct FootIkState {
    float                  pelvisOffset = 0.0f; // world units (<= 0)
    std::vector<float>     footOffsets;         // world units
    std::vector<glm::vec3> normals;             // world ground normals
    bool                   initialized = false;
};

// Places the feet (ends of two-bone legs) on the ground below them: each foot keeps its animated
// height above the entity origin relative to the ground under it; the pelvis lowers by the lowest
// foot (at most maxPelvisOffset), feet turn to the ground normal (at most maxFootAngle).
// worldFromModel: the model root's world matrix (up = +Y in world space).
void SolveFootPlacement(PoseSpace& pose, const glm::mat4& worldFromModel, const AnimIk& settings,
                        std::span<const int> feet, int pelvis, const GroundRaycast& raycast, FootIkState& state,
                        float deltaSeconds, float weight);

} // namespace Engine
