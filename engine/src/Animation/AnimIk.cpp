#include "Engine/Animation/AnimIk.h"
#include "Engine/Animation/AnimGraph.h"
#include "Engine/Assets/Model.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Engine {

namespace {

constexpr float kEpsilon = 1.0e-6f;

glm::quat RotationOf(const glm::mat4& m)
{
    glm::mat3 basis(m);
    for (glm::length_t i = 0; i < 3; ++i) {
        const float length = glm::length(basis[i]);
        if (length > kEpsilon)
            basis[i] /= length;
    }
    return glm::normalize(glm::quat_cast(basis));
}

bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Blends the locals of `nodes` from their values in `before` towards the solved pose by weight.
void ApplyWeight(PoseSpace& pose, std::span<const int> nodes, std::span<const Transform> before, float weight)
{
    if (weight >= 1.0f)
        return;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        Transform solved = pose.Local(nodes[i]);
        const Transform& original = before[i];
        solved.position = glm::mix(original.position, solved.position, weight);
        solved.rotation = glm::normalize(glm::slerp(original.rotation, solved.rotation, weight));
        solved.scale    = glm::mix(original.scale, solved.scale, weight);
        pose.SetLocal(nodes[i], solved);
    }
}

std::vector<Transform> Snapshot(const PoseSpace& pose, std::span<const int> nodes)
{
    std::vector<Transform> out;
    out.reserve(nodes.size());
    for (const int node : nodes)
        out.push_back(pose.Local(node));
    return out;
}

} // namespace

glm::quat RotationBetween(const glm::vec3& aIn, const glm::vec3& bIn)
{
    const float la = glm::length(aIn), lb = glm::length(bIn);
    if (la < kEpsilon || lb < kEpsilon || !Finite(aIn) || !Finite(bIn))
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 a = aIn / la, b = bIn / lb;
    const float     d = glm::dot(a, b);
    if (d < -0.999999f) { // opposite: half turn about any perpendicular axis
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
        if (glm::dot(axis, axis) < 1.0e-6f)
            axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), a);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

PoseSpace::PoseSpace(const Model& model, std::span<Transform> locals)
    : m_Locals(locals), m_Parents(locals.size(), -1), m_Globals(locals.size(), glm::mat4(1.0f))
{
    for (std::size_t i = 0; i < locals.size() && i < model.nodes.size(); ++i) {
        const int parent = model.nodes[i].parent;
        m_Parents[i]     = parent >= 0 && static_cast<std::size_t>(parent) < i ? parent : -1;
    }
}

int PoseSpace::Parent(int node) const
{
    return node >= 0 && static_cast<std::size_t>(node) < m_Parents.size() ? m_Parents[static_cast<std::size_t>(node)] : -1;
}

void PoseSpace::Update() const
{
    if (!m_Dirty)
        return;
    for (std::size_t i = 0; i < m_Locals.size(); ++i) {
        const glm::mat4 local = m_Locals[i].LocalMatrix();
        m_Globals[i]          = m_Parents[i] >= 0 ? m_Globals[static_cast<std::size_t>(m_Parents[i])] * local : local;
    }
    m_Dirty = false;
}

const glm::mat4& PoseSpace::Global(int node) const
{
    Update();
    return m_Globals[static_cast<std::size_t>(node)];
}

glm::quat PoseSpace::Rotation(int node) const { return RotationOf(Global(node)); }

void PoseSpace::SetLocal(int node, const Transform& local)
{
    m_Locals[static_cast<std::size_t>(node)] = local;
    m_Dirty                                   = true;
}

void PoseSpace::Rotate(int node, const glm::quat& delta)
{
    const int       parent      = Parent(node);
    const glm::quat parentRot   = parent >= 0 ? Rotation(parent) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat modelRot    = Rotation(node);
    Transform       local       = Local(node);
    const glm::quat newLocal    = glm::inverse(parentRot) * (glm::normalize(delta) * modelRot);
    // Keep the local scale's handedness: only the rotation changes.
    local.rotation = glm::normalize(newLocal);
    SetLocal(node, local);
}

void PoseSpace::Translate(int node, const glm::vec3& delta)
{
    const int parent = Parent(node);
    Transform local  = Local(node);
    if (parent >= 0) {
        const glm::mat3 basis(Global(parent));
        const float     det = glm::determinant(basis);
        if (!std::isfinite(det) || std::abs(det) < 1.0e-12f)
            return;
        local.position += glm::inverse(basis) * delta;
    } else
        local.position += delta;
    SetLocal(node, local);
}

bool SolveTwoBoneIk(PoseSpace& pose, int endNode, const glm::vec3& target, const glm::vec3* pole, bool keepEndRotation,
                    float weight)
{
    weight = std::clamp(std::isfinite(weight) ? weight : 0.0f, 0.0f, 1.0f);
    if (weight <= 0.0f || endNode < 0 || static_cast<std::size_t>(endNode) >= pose.Size() || !Finite(target))
        return false;
    const int mid   = pose.Parent(endNode);
    const int upper = mid >= 0 ? pose.Parent(mid) : -1;
    if (upper < 0)
        return false;
    const std::array<int, 3> nodes{upper, mid, endNode};
    const std::vector<Transform> before = Snapshot(pose, nodes);

    const glm::vec3 a = pose.Position(upper), b = pose.Position(mid), c = pose.Position(endNode);
    const float     l1 = glm::length(b - a), l2 = glm::length(c - b);
    const glm::vec3 toTarget = target - a;
    const float     reach    = glm::length(toTarget);
    if (l1 < kEpsilon || l2 < kEpsilon || reach < kEpsilon)
        return false;
    const glm::vec3 axis = toTarget / reach;
    const float     minReach = std::abs(l1 - l2) + 1.0e-4f * (l1 + l2);
    const float     maxReach = (l1 + l2) * (1.0f - 1.0e-4f);
    const float     d        = std::clamp(reach, std::min(minReach, maxReach), maxReach);

    // Bend plane: towards the pole, else the current knee / elbow direction.
    glm::vec3 hint = pole ? *pole - a : b - a;
    hint -= axis * glm::dot(hint, axis);
    if (glm::dot(hint, hint) < 1.0e-10f) {
        hint = (b - a) - axis * glm::dot(b - a, axis);
        if (glm::dot(hint, hint) < 1.0e-10f)
            hint = glm::cross(axis, std::abs(axis.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
    }
    const glm::vec3 bendDir = glm::normalize(hint);
    const float     cosA    = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
    const float     sinA    = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    const glm::vec3 midGoal = a + axis * (l1 * cosA) + bendDir * (l1 * sinA);
    const glm::vec3 endGoal = a + axis * d;

    const glm::quat endRotation = pose.Rotation(endNode);
    pose.Rotate(upper, RotationBetween(b - a, midGoal - a));
    const glm::vec3 midNow = pose.Position(mid);
    pose.Rotate(mid, RotationBetween(pose.Position(endNode) - midNow, endGoal - midNow));
    if (keepEndRotation)
        pose.Rotate(endNode, endRotation * glm::inverse(pose.Rotation(endNode)));
    ApplyWeight(pose, nodes, before, weight);
    return true;
}

bool SolveFabrikIk(PoseSpace& pose, int rootNode, int endNode, const glm::vec3& target, std::uint32_t iterations,
                   float tolerance, float weight)
{
    weight = std::clamp(std::isfinite(weight) ? weight : 0.0f, 0.0f, 1.0f);
    if (weight <= 0.0f || !Finite(target) || rootNode < 0 || endNode < 0 ||
        static_cast<std::size_t>(rootNode) >= pose.Size() || static_cast<std::size_t>(endNode) >= pose.Size())
        return false;
    std::vector<int> chain;
    for (int node = endNode; node >= 0; node = pose.Parent(node)) {
        chain.push_back(node);
        if (node == rootNode)
            break;
    }
    if (chain.back() != rootNode || chain.size() < 2)
        return false;
    std::ranges::reverse(chain);
    const std::vector<Transform> before = Snapshot(pose, chain);

    const std::size_t      n = chain.size();
    std::vector<glm::vec3> p(n);
    std::vector<float>     lengths(n - 1);
    float                  total = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
        p[i] = pose.Position(chain[i]);
    for (std::size_t i = 0; i + 1 < n; ++i)
        total += lengths[i] = glm::length(p[i + 1] - p[i]);
    if (total < kEpsilon)
        return false;

    const glm::vec3 base = p[0];
    if (glm::length(target - base) >= total) { // out of reach: straight line towards the target
        const glm::vec3 dir = glm::normalize(target - base);
        for (std::size_t i = 0; i + 1 < n; ++i)
            p[i + 1] = p[i] + dir * lengths[i];
    } else {
        tolerance = std::max(tolerance, 1.0e-6f);
        for (std::uint32_t it = 0; it < std::max(iterations, 1u); ++it) {
            p[n - 1] = target; // backward
            for (std::size_t i = n - 1; i-- > 0;) {
                const glm::vec3 dir = p[i] - p[i + 1];
                const float     len = glm::length(dir);
                p[i] = p[i + 1] + (len > kEpsilon ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f)) * lengths[i];
            }
            p[0] = base; // forward
            for (std::size_t i = 0; i + 1 < n; ++i) {
                const glm::vec3 dir = p[i + 1] - p[i];
                const float     len = glm::length(dir);
                p[i + 1] = p[i] + (len > kEpsilon ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f)) * lengths[i];
            }
            if (glm::length(p[n - 1] - target) <= tolerance)
                break;
        }
    }
    const glm::quat endRotation = pose.Rotation(endNode);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const glm::vec3 at = pose.Position(chain[i]);
        pose.Rotate(chain[i], RotationBetween(pose.Position(chain[i + 1]) - at, p[i + 1] - at));
    }
    pose.Rotate(endNode, endRotation * glm::inverse(pose.Rotation(endNode))); // the tip keeps its orientation
    ApplyWeight(pose, chain, before, weight);
    return true;
}

bool SolveLookAtIk(PoseSpace& pose, std::span<const int> chain, const glm::vec3& axisIn, const glm::vec3& target,
                   float maxAngle, float weight)
{
    weight = std::clamp(std::isfinite(weight) ? weight : 0.0f, 0.0f, 1.0f);
    if (weight <= 0.0f || chain.empty() || !Finite(target) || glm::length(axisIn) < kEpsilon)
        return false;
    for (const int node : chain)
        if (node < 0 || static_cast<std::size_t>(node) >= pose.Size())
            return false;
    const int       tip  = chain.back();
    const glm::vec3 axis = glm::normalize(axisIn);
    const glm::vec3 animated = glm::normalize(pose.Rotation(tip) * axis);
    // The aim direction from where the tip is now, at most maxAngle away from the animated one.
    const auto desiredDirection = [&]() -> std::optional<glm::vec3> {
        const glm::vec3 toTarget = target - pose.Position(tip);
        if (glm::length(toTarget) < kEpsilon)
            return std::nullopt;
        const glm::vec3 wanted = glm::normalize(toTarget);
        const float     angle  = std::acos(std::clamp(glm::dot(animated, wanted), -1.0f, 1.0f));
        if (angle <= maxAngle || angle <= kEpsilon)
            return wanted;
        return glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), RotationBetween(animated, wanted), maxAngle / angle) * animated;
    };
    if (!desiredDirection())
        return false;

    const std::vector<Transform> before = Snapshot(pose, chain);
    for (std::size_t k = 0; k < chain.size(); ++k) {
        const auto desired = desiredDirection(); // ancestors moved the tip: aim from its new place
        if (!desired)
            break;
        const glm::vec3 current = pose.Rotation(tip) * axis;
        const glm::quat full    = RotationBetween(current, *desired);
        const float     share   = 1.0f / static_cast<float>(chain.size() - k);
        pose.Rotate(chain[k], glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), full, share));
    }
    ApplyWeight(pose, chain, before, weight);
    return true;
}

void SolveFootPlacement(PoseSpace& pose, const glm::mat4& worldFromModel, const AnimIk& settings, std::span<const int> feet,
                        int pelvis, const GroundRaycast& raycast, FootIkState& state, float deltaSeconds, float weight)
{
    weight = std::clamp(std::isfinite(weight) ? weight : 0.0f, 0.0f, 1.0f);
    if (!raycast || feet.empty())
        return;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 origin(worldFromModel[3]);
    const glm::mat3 modelBasis(worldFromModel);
    const float     det = glm::determinant(modelBasis);
    if (!std::isfinite(det) || std::abs(det) < 1.0e-12f)
        return;
    const glm::mat3 modelFromWorld = glm::inverse(modelBasis);
    const glm::quat worldRotation  = RotationOf(worldFromModel);

    // Ground under each foot (target values before smoothing).
    std::vector<float>     offsets(feet.size(), 0.0f);
    std::vector<glm::vec3> normals(feet.size(), up);
    std::vector<glm::vec3> ankles(feet.size());
    for (std::size_t i = 0; i < feet.size(); ++i) {
        if (feet[i] < 0 || static_cast<std::size_t>(feet[i]) >= pose.Size())
            continue;
        ankles[i] = glm::vec3(worldFromModel * glm::vec4(pose.Position(feet[i]), 1.0f));
        const glm::vec3 from(ankles[i].x, origin.y + settings.rayHeight, ankles[i].z);
        const glm::vec3 to(ankles[i].x, origin.y - settings.rayDepth, ankles[i].z);
        if (const auto hit = raycast(from, to); hit && Finite(hit->point)) {
            offsets[i] = std::clamp(hit->point.y - origin.y, -settings.rayDepth, settings.rayHeight);
            if (Finite(hit->normal) && glm::length(hit->normal) > kEpsilon && hit->normal.y > 0.0f)
                normals[i] = glm::normalize(hit->normal);
        }
    }
    float pelvisTarget = 0.0f;
    for (const float offset : offsets)
        pelvisTarget = std::min(pelvisTarget, offset);
    pelvisTarget = std::max(pelvisTarget, -settings.maxPelvisOffset);

    const bool  snap  = !state.initialized || settings.smoothing <= 0.0f || state.footOffsets.size() != feet.size();
    const float alpha = snap ? 1.0f : 1.0f - std::exp(-settings.smoothing * std::max(deltaSeconds, 0.0f));
    if (snap) {
        state.footOffsets.assign(feet.size(), 0.0f);
        state.normals.assign(feet.size(), up);
        state.pelvisOffset = 0.0f;
    }
    state.initialized   = true;
    state.pelvisOffset += (pelvisTarget - state.pelvisOffset) * alpha;
    for (std::size_t i = 0; i < feet.size(); ++i) {
        state.footOffsets[i] += (offsets[i] - state.footOffsets[i]) * alpha;
        state.normals[i] = glm::normalize(glm::mix(state.normals[i], normals[i], alpha));
    }
    if (weight <= 0.0f)
        return;

    if (pelvis >= 0 && static_cast<std::size_t>(pelvis) < pose.Size() && std::abs(state.pelvisOffset) > 1.0e-6f)
        pose.Translate(pelvis, modelFromWorld * (up * (state.pelvisOffset * weight)));

    const float maxFoot = glm::radians(settings.maxFootAngle);
    for (std::size_t i = 0; i < feet.size(); ++i) {
        if (feet[i] < 0 || static_cast<std::size_t>(feet[i]) >= pose.Size())
            continue;
        const glm::vec3 goalWorld = ankles[i] + up * state.footOffsets[i];
        const glm::vec3 goalModel = glm::vec3(glm::inverse(worldFromModel) * glm::vec4(goalWorld, 1.0f));
        SolveTwoBoneIk(pose, feet[i], goalModel, nullptr, true, weight);
        if (!settings.alignToNormal)
            continue;
        const glm::vec3 normal = state.normals[i];
        const float     angle  = std::acos(std::clamp(glm::dot(up, normal), -1.0f, 1.0f));
        if (angle < 1.0e-4f)
            continue;
        glm::quat tilt = RotationBetween(up, normal);
        if (angle > maxFoot)
            tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), tilt, maxFoot / angle);
        const glm::quat modelTilt = glm::inverse(worldRotation) * tilt * worldRotation;
        pose.Rotate(feet[i], glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), modelTilt, weight));
    }
}

} // namespace Engine
