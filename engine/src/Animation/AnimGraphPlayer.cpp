#include "Engine/Animation/AnimGraphPlayer.h"
#include "Engine/Assets/Animation.h"
#include "Engine/Assets/Model.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Engine {

namespace {

// Delaunay triangulation (Bowyer-Watson) of a handful of 2D points; degenerate input gives none.
std::vector<std::array<int, 3>> Triangulate(const std::vector<glm::vec2>& points)
{
    std::vector<std::array<int, 3>> result;
    const int n = static_cast<int>(points.size());
    if (n < 3)
        return result;
    glm::vec2 lo = points[0], hi = points[0];
    for (const glm::vec2& p : points)
        lo = glm::min(lo, p), hi = glm::max(hi, p);
    const float     span   = std::max(std::max(hi.x - lo.x, hi.y - lo.y), 1.0e-3f);
    const glm::vec2 center = (lo + hi) * 0.5f;
    std::vector<glm::vec2> v = points;
    v.push_back(center + glm::vec2(-20.0f * span, -10.0f * span));
    v.push_back(center + glm::vec2(20.0f * span, -10.0f * span));
    v.push_back(center + glm::vec2(0.0f, 20.0f * span));

    const auto inCircle = [&](const std::array<int, 3>& t, const glm::vec2& p) {
        glm::vec2 a = v[static_cast<std::size_t>(t[0])] - p, b = v[static_cast<std::size_t>(t[1])] - p,
                  c = v[static_cast<std::size_t>(t[2])] - p;
        const glm::vec2 ab = v[static_cast<std::size_t>(t[1])] - v[static_cast<std::size_t>(t[0])];
        const glm::vec2 ac = v[static_cast<std::size_t>(t[2])] - v[static_cast<std::size_t>(t[0])];
        if (ab.x * ac.y - ab.y * ac.x < 0.0f)
            std::swap(b, c); // counter-clockwise for the determinant's sign
        const double det = (double(a.x) * a.x + double(a.y) * a.y) * (double(b.x) * c.y - double(c.x) * b.y) -
                           (double(b.x) * b.x + double(b.y) * b.y) * (double(a.x) * c.y - double(c.x) * a.y) +
                           (double(c.x) * c.x + double(c.y) * c.y) * (double(a.x) * b.y - double(b.x) * a.y);
        return det > 1.0e-12;
    };

    std::vector<std::array<int, 3>> tris{{n, n + 1, n + 2}};
    for (int i = 0; i < n; ++i) {
        const glm::vec2&                 p = v[static_cast<std::size_t>(i)];
        std::vector<std::array<int, 2>> edges;
        std::vector<std::array<int, 3>> keep;
        for (const auto& t : tris) {
            if (!inCircle(t, p)) {
                keep.push_back(t);
                continue;
            }
            for (int e = 0; e < 3; ++e) {
                std::array<int, 2> edge{t[static_cast<std::size_t>(e)], t[static_cast<std::size_t>((e + 1) % 3)]};
                const auto shared = std::ranges::find_if(edges, [&](const std::array<int, 2>& o) {
                    return (o[0] == edge[0] && o[1] == edge[1]) || (o[0] == edge[1] && o[1] == edge[0]);
                });
                if (shared != edges.end())
                    edges.erase(shared);
                else
                    edges.push_back(edge);
            }
        }
        tris = std::move(keep);
        for (const auto& edge : edges)
            tris.push_back({edge[0], edge[1], i});
    }
    for (const auto& t : tris) {
        if (t[0] >= n || t[1] >= n || t[2] >= n)
            continue;
        const glm::vec2 ab = points[static_cast<std::size_t>(t[1])] - points[static_cast<std::size_t>(t[0])];
        const glm::vec2 ac = points[static_cast<std::size_t>(t[2])] - points[static_cast<std::size_t>(t[0])];
        if (std::abs(ab.x * ac.y - ab.y * ac.x) > 1.0e-6f * span * span)
            result.push_back(t);
    }
    return result;
}

float Wrap01(float t) { return t - std::floor(t); }

void AddWeighted(std::vector<Transform>& acc, std::span<const Transform> pose, float w, bool first)
{
    for (std::size_t i = 0; i < acc.size(); ++i) {
        glm::quat q = pose[i].rotation;
        if (first) {
            acc[i].position = pose[i].position * w;
            acc[i].scale    = pose[i].scale * w;
            acc[i].rotation = glm::quat(q.w * w, q.x * w, q.y * w, q.z * w);
            continue;
        }
        if (glm::dot(acc[i].rotation, q) < 0.0f)
            q = -q;
        acc[i].position += pose[i].position * w;
        acc[i].scale += pose[i].scale * w;
        acc[i].rotation = glm::quat(acc[i].rotation.w + q.w * w, acc[i].rotation.x + q.x * w,
                                    acc[i].rotation.y + q.y * w, acc[i].rotation.z + q.z * w);
    }
}

} // namespace

void AnimGraphPlayer::Bind(std::shared_ptr<const AnimGraph> graph, const Model* model)
{
    // Keep parameter values that survive (same name and type).
    std::vector<std::pair<AnimParameter, float>> old;
    if (m_Graph)
        for (std::size_t i = 0; i < m_Graph->parameters.size() && i < m_Values.size(); ++i)
            old.emplace_back(m_Graph->parameters[i], m_Values[i]);

    m_Graph = std::move(graph);
    m_Model = model;
    m_Layers.clear();
    m_Values.clear();
    m_Driven.clear();
    m_Rest.clear();
    m_RootMotionNode = -1;
    if (!Bound()) {
        m_Graph = nullptr, m_Model = nullptr;
        return;
    }

    for (const AnimParameter& p : m_Graph->parameters) {
        float value = p.defaultValue;
        for (const auto& [previous, previousValue] : old)
            if (previous.name == p.name && previous.type == p.type)
                value = previousValue;
        m_Values.push_back(value);
    }
    const std::size_t nodes = m_Model->nodes.size();
    m_Rest.resize(nodes);
    for (std::size_t i = 0; i < nodes; ++i)
        m_Rest[i] = m_Model->nodes[i].local;
    m_Driven.assign(nodes, 0);
    m_RootMotionNode = NodeIndex(m_Graph->rootMotionBone);

    for (const AnimLayer& source : m_Graph->layers) {
        Layer layer;
        layer.source = &source;
        for (const AnimState& state : source.states) {
            State resolved;
            ResolveState(resolved, state);
            layer.states.push_back(std::move(resolved));
        }
        const auto stateIndex = [&](const std::string& name) {
            for (std::size_t i = 0; i < source.states.size(); ++i)
                if (source.states[i].name == name)
                    return static_cast<int>(i);
            return -1;
        };
        for (const AnimTransition& t : source.transitions) {
            Transition transition{.source = &t, .from = t.from.empty() ? -1 : stateIndex(t.from), .to = stateIndex(t.to)};
            if (transition.to < 0 || (!t.from.empty() && transition.from < 0))
                continue;
            bool valid = true;
            for (const AnimCondition& c : t.conditions) {
                const auto param = std::ranges::find(m_Graph->parameters, c.parameter, &AnimParameter::name);
                if (param == m_Graph->parameters.end()) {
                    valid = false; // an unknown parameter never holds
                    break;
                }
                transition.conditions.push_back(Condition{
                    .param = static_cast<int>(param - m_Graph->parameters.begin()), .op = c.op, .value = c.value});
            }
            if (valid)
                layer.transitions.push_back(std::move(transition));
        }
        // Any-state transitions are checked first (in their order), then the current state's.
        std::ranges::stable_partition(layer.transitions, [](const Transition& t) { return t.from < 0; });
        layer.entry = stateIndex(source.entryState);
        if (layer.entry < 0 && !layer.states.empty())
            layer.entry = 0;
        const auto weightParam = std::ranges::find(m_Graph->parameters, source.weightParameter, &AnimParameter::name);
        layer.weightParam = source.weightParameter.empty() || weightParam == m_Graph->parameters.end()
                                ? -1
                                : static_cast<int>(weightParam - m_Graph->parameters.begin());
        layer.mask.assign(nodes, source.mask.empty() ? 1.0f : 0.0f);
        for (const AnimBoneMask& m : source.mask) {
            const int bone = NodeIndex(m.bone);
            if (bone < 0)
                continue;
            std::vector<std::uint8_t> inside(nodes, 0);
            inside[static_cast<std::size_t>(bone)] = 1;
            for (std::size_t i = static_cast<std::size_t>(bone) + 1; m.includeChildren && i < nodes; ++i) {
                const int parent = m_Model->nodes[i].parent;
                if (parent >= 0 && inside[static_cast<std::size_t>(parent)])
                    inside[i] = 1;
            }
            for (std::size_t i = 0; i < nodes; ++i)
                if (inside[i])
                    layer.mask[i] = 1.0f;
        }
        layer.current  = Play{.state = layer.entry};
        layer.lastPose = m_Rest;
        m_Layers.push_back(std::move(layer));
    }
    if (m_RootMotionNode >= 0)
        m_Driven[static_cast<std::size_t>(m_RootMotionNode)] = 1;
}

void AnimGraphPlayer::ResolveState(State& state, const AnimState& source)
{
    state.source = &source;
    const auto clipIndex = [&](const std::string& name) {
        for (std::size_t i = 0; i < m_Model->animations.size(); ++i)
            if (m_Model->animations[i].name == name)
                return static_cast<int>(i);
        return -1;
    };
    const auto param = [&](const std::string& name) {
        if (name.empty())
            return -1;
        for (std::size_t i = 0; i < m_Graph->parameters.size(); ++i)
            if (m_Graph->parameters[i].name == name)
                return static_cast<int>(i);
        return -1;
    };
    const auto drive = [&](int clip) {
        if (clip < 0)
            return;
        for (const AnimationTrack& track : m_Model->animations[static_cast<std::size_t>(clip)].tracks)
            if (track.node < m_Driven.size())
                m_Driven[track.node] = 1;
    };
    state.speedParam = param(source.speedParameter);
    if (source.kind == AnimStateKind::Clip) {
        state.clip = clipIndex(source.clip);
        drive(state.clip);
    } else {
        state.xParam = param(source.xParameter);
        state.yParam = source.kind == AnimStateKind::BlendSpace2D ? param(source.yParameter) : -1;
        std::vector<glm::vec2> points;
        for (const AnimBlendSample& s : source.samples) {
            const int clip = clipIndex(s.clip);
            if (clip < 0)
                continue;
            glm::vec2 position = s.position;
            if (source.kind == AnimStateKind::BlendSpace1D)
                position.y = 0.0f;
            if (std::ranges::any_of(points, [&](const glm::vec2& p) { return glm::length(p - position) < 1.0e-5f; }))
                continue; // duplicate position: the first sample wins
            state.samples.push_back(Sample{.clip = clip, .position = position, .speed = std::max(s.speed, 0.01f)});
            points.push_back(position);
            drive(clip);
        }
        if (source.kind == AnimStateKind::BlendSpace2D)
            state.triangles = Triangulate(points);
    }
    for (const AnimNotify& n : source.notifies)
        state.notifyBones.push_back(NodeIndex(n.bone));
}

void AnimGraphPlayer::Restart()
{
    for (Layer& layer : m_Layers) {
        layer.current  = Play{.state = layer.entry};
        layer.previous = {};
        layer.fading = layer.frozen = false;
        layer.lastPose = m_Rest;
    }
}

bool AnimGraphPlayer::SetParameter(std::string_view name, float value)
{
    if (!m_Graph || !std::isfinite(value))
        return false;
    for (std::size_t i = 0; i < m_Graph->parameters.size(); ++i) {
        if (m_Graph->parameters[i].name != name)
            continue;
        switch (m_Graph->parameters[i].type) {
        case AnimParamType::Float: m_Values[i] = value; break;
        case AnimParamType::Int: m_Values[i] = std::round(value); break;
        default: m_Values[i] = value != 0.0f ? 1.0f : 0.0f; break;
        }
        return true;
    }
    return false;
}

bool AnimGraphPlayer::SetTrigger(std::string_view name) { return SetParameter(name, 1.0f); }

std::optional<float> AnimGraphPlayer::Parameter(std::string_view name) const
{
    if (!m_Graph)
        return std::nullopt;
    for (std::size_t i = 0; i < m_Graph->parameters.size(); ++i)
        if (m_Graph->parameters[i].name == name)
            return m_Values[i];
    return std::nullopt;
}

bool AnimGraphPlayer::SetLayerWeight(std::string_view layerName, float weight)
{
    for (Layer& layer : m_Layers)
        if (layer.source->name == layerName) {
            layer.runtimeWeight = std::clamp(std::isfinite(weight) ? weight : 0.0f, 0.0f, 1.0f);
            return true;
        }
    return false;
}

int AnimGraphPlayer::NodeIndex(std::string_view bone) const
{
    if (!m_Model || bone.empty())
        return -1;
    for (std::size_t i = 0; i < m_Model->nodes.size(); ++i)
        if (m_Model->nodes[i].name == bone)
            return static_cast<int>(i);
    return -1;
}

float AnimGraphPlayer::Value(int param) const
{
    return param >= 0 && static_cast<std::size_t>(param) < m_Values.size() ? m_Values[static_cast<std::size_t>(param)] : 0.0f;
}

float AnimGraphPlayer::ClipDuration(int clip) const
{
    return clip >= 0 ? m_Model->animations[static_cast<std::size_t>(clip)].duration : 0.0f;
}

void AnimGraphPlayer::Weights(const State& state, std::vector<float>& out) const
{
    out.assign(state.samples.size(), 0.0f);
    if (state.samples.empty())
        return;
    if (state.samples.size() == 1) {
        out[0] = 1.0f;
        return;
    }
    const glm::vec2 p(Value(state.xParam), state.yParam >= 0 ? Value(state.yParam) : 0.0f);
    if (state.source->kind == AnimStateKind::BlendSpace1D) {
        std::vector<std::size_t> order(state.samples.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::ranges::sort(order, {}, [&](std::size_t i) { return state.samples[i].position.x; });
        if (p.x <= state.samples[order.front()].position.x) {
            out[order.front()] = 1.0f;
            return;
        }
        if (p.x >= state.samples[order.back()].position.x) {
            out[order.back()] = 1.0f;
            return;
        }
        for (std::size_t k = 0; k + 1 < order.size(); ++k) {
            const float a = state.samples[order[k]].position.x, b = state.samples[order[k + 1]].position.x;
            if (p.x >= a && p.x <= b) {
                const float t = b > a ? (p.x - a) / (b - a) : 0.0f;
                out[order[k]] = 1.0f - t, out[order[k + 1]] = t;
                return;
            }
        }
        return;
    }
    // 2D: the triangle containing the point, else the closest point on an edge.
    for (const auto& t : state.triangles) {
        const glm::vec2 a = state.samples[static_cast<std::size_t>(t[0])].position;
        const glm::vec2 b = state.samples[static_cast<std::size_t>(t[1])].position;
        const glm::vec2 c = state.samples[static_cast<std::size_t>(t[2])].position;
        const float     det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(det) < 1.0e-12f)
            continue;
        const float wa = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / det;
        const float wb = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / det;
        const float wc = 1.0f - wa - wb;
        if (wa >= -1.0e-5f && wb >= -1.0e-5f && wc >= -1.0e-5f) {
            const float sum = std::max(wa, 0.0f) + std::max(wb, 0.0f) + std::max(wc, 0.0f);
            out[static_cast<std::size_t>(t[0])] = std::max(wa, 0.0f) / sum;
            out[static_cast<std::size_t>(t[1])] = std::max(wb, 0.0f) / sum;
            out[static_cast<std::size_t>(t[2])] = std::max(wc, 0.0f) / sum;
            return;
        }
    }
    float best = std::numeric_limits<float>::max();
    std::size_t bestA = 0, bestB = 0;
    float bestT = 0.0f;
    const auto tryEdge = [&](std::size_t ia, std::size_t ib) {
        const glm::vec2 a = state.samples[ia].position, b = state.samples[ib].position;
        const glm::vec2 ab = b - a;
        const float     len2 = glm::dot(ab, ab);
        const float     t    = len2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const float     dist = glm::length(a + ab * t - p);
        if (dist < best)
            best = dist, bestA = ia, bestB = ib, bestT = t;
    };
    if (!state.triangles.empty()) {
        for (const auto& t : state.triangles)
            for (int e = 0; e < 3; ++e)
                tryEdge(static_cast<std::size_t>(t[static_cast<std::size_t>(e)]),
                        static_cast<std::size_t>(t[static_cast<std::size_t>((e + 1) % 3)]));
    } else {
        for (std::size_t a = 0; a < state.samples.size(); ++a)
            for (std::size_t b = a + 1; b < state.samples.size(); ++b)
                tryEdge(a, b);
    }
    out[bestA] += 1.0f - bestT;
    out[bestB] += bestT;
}

std::vector<float> AnimGraphPlayer::BlendWeights(std::size_t layer) const
{
    std::vector<float> out;
    if (layer < m_Layers.size()) {
        const Layer& l = m_Layers[layer];
        if (l.current.state >= 0)
            Weights(l.states[static_cast<std::size_t>(l.current.state)], out);
    }
    return out;
}

float AnimGraphPlayer::StateRate(const State& state) const
{
    float multiplier = state.source->speed;
    if (state.speedParam >= 0)
        multiplier *= Value(state.speedParam);
    if (state.source->kind == AnimStateKind::Clip) {
        const float duration = ClipDuration(state.clip);
        return duration > 0.0f ? multiplier / duration : 0.0f;
    }
    // Synchronized phase: the weighted rate of the samples.
    Weights(state, m_WeightScratch);
    float rate = 0.0f;
    for (std::size_t i = 0; i < state.samples.size(); ++i) {
        const float duration = ClipDuration(state.samples[i].clip);
        if (duration > 0.0f)
            rate += m_WeightScratch[i] * state.samples[i].speed / duration;
    }
    return rate * multiplier;
}

void AnimGraphPlayer::EvaluateState(const State& state, float time, std::span<Transform> pose)
{
    const bool loop     = state.source->loop;
    const auto clipTime = [&](int clip) {
        const float duration = ClipDuration(clip);
        float       t        = time * duration;
        if (loop && duration > 0.0f)
            t = Wrap01(time) * duration;
        return std::isfinite(t) ? std::max(t, 0.0f) : 0.0f;
    };
    const AnimationPlayback playback = loop ? AnimationPlayback::Loop : AnimationPlayback::Once;
    if (state.source->kind == AnimStateKind::Clip) {
        if (state.clip < 0) {
            std::ranges::copy(m_Rest, pose.begin());
            return;
        }
        EvaluateAnimation(*m_Model, m_Model->animations[static_cast<std::size_t>(state.clip)], clipTime(state.clip),
                          playback, pose);
        return;
    }
    Weights(state, m_WeightScratch);
    std::size_t active = 0, last = 0;
    for (std::size_t i = 0; i < m_WeightScratch.size(); ++i)
        if (m_WeightScratch[i] > 1.0e-5f)
            ++active, last = i;
    if (active == 0) {
        std::ranges::copy(m_Rest, pose.begin());
        return;
    }
    if (active == 1) {
        const int clip = state.samples[last].clip;
        EvaluateAnimation(*m_Model, m_Model->animations[static_cast<std::size_t>(clip)], clipTime(clip), playback, pose);
        return;
    }
    const std::vector<float> weights = m_WeightScratch; // EvaluateAnimation does not touch it, but stay safe
    m_Scratch.resize(pose.size());
    m_Accum.resize(pose.size());
    bool  first = true;
    float total = 0.0f;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] <= 1.0e-5f)
            continue;
        const int clip = state.samples[i].clip;
        EvaluateAnimation(*m_Model, m_Model->animations[static_cast<std::size_t>(clip)], clipTime(clip), playback, m_Scratch);
        AddWeighted(m_Accum, m_Scratch, weights[i], first);
        first = false;
        total += weights[i];
    }
    for (std::size_t i = 0; i < pose.size(); ++i) {
        const float inv = total > 0.0f ? 1.0f / total : 1.0f;
        pose[i].position = m_Accum[i].position * inv;
        pose[i].scale    = m_Accum[i].scale * inv;
        const glm::quat q = m_Accum[i].rotation;
        pose[i].rotation  = glm::dot(q, q) > 1.0e-12f ? glm::normalize(q) : m_Rest[i].rotation;
    }
}

glm::vec3 AnimGraphPlayer::RootDelta(const State& state, float from, float to) const
{
    const auto delta = [&](int clip, float weight) {
        if (clip < 0 || weight <= 0.0f)
            return glm::vec3(0.0f);
        const AnimationClip& c        = m_Model->animations[static_cast<std::size_t>(clip)];
        const auto           node     = static_cast<std::uint32_t>(m_RootMotionNode);
        const bool           loop     = state.source->loop;
        return weight * (SampleRootMotion(c, node, std::max(to, 0.0f) * c.duration, loop) -
                         SampleRootMotion(c, node, std::max(from, 0.0f) * c.duration, loop));
    };
    if (state.source->kind == AnimStateKind::Clip)
        return delta(state.clip, 1.0f);
    std::vector<float> weights;
    Weights(state, weights);
    glm::vec3 sum(0.0f);
    for (std::size_t i = 0; i < weights.size(); ++i)
        sum += delta(state.samples[i].clip, weights[i]);
    return sum;
}

void AnimGraphPlayer::Fire(const Layer& layer, std::uint32_t index, const State& state, float from, float to,
                           std::vector<AnimFiredNotify>& out) const
{
    if (!(to > from))
        return;
    for (std::size_t n = 0; n < state.source->notifies.size(); ++n) {
        const AnimNotify& notify = state.source->notifies[n];
        int               count  = 0;
        if (state.source->loop) { // every loop passed in this update (a long frame may pass several)
            for (float k = std::ceil(from - notify.time); k + notify.time < to && count < 8; k += 1.0f)
                ++count;
        } else
            count = (notify.time >= from && notify.time < to) || (notify.time >= 1.0f && from < 1.0f && to >= 1.0f) ? 1 : 0;
        for (int i = 0; i < count; ++i)
            out.push_back(AnimFiredNotify{
                .notify = notify, .layer = index, .state = state.source->name, .boneNode = state.notifyBones[n]});
    }
    (void)layer;
}

bool AnimGraphPlayer::Holds(const Transition& transition, const Layer& layer) const
{
    if (transition.source->hasExitTime && layer.current.time < transition.source->exitTime)
        return false;
    for (const Condition& c : transition.conditions) {
        const float v = Value(c.param);
        bool        ok = false;
        switch (c.op) {
        case AnimCompare::Greater: ok = v > c.value; break;
        case AnimCompare::Less: ok = v < c.value; break;
        case AnimCompare::GreaterEqual: ok = v >= c.value; break;
        case AnimCompare::LessEqual: ok = v <= c.value; break;
        case AnimCompare::Equal: ok = std::abs(v - c.value) < 1.0e-4f; break;
        case AnimCompare::NotEqual: ok = std::abs(v - c.value) >= 1.0e-4f; break;
        case AnimCompare::IsTrue:
        case AnimCompare::Triggered: ok = v != 0.0f; break;
        case AnimCompare::IsFalse: ok = v == 0.0f; break;
        }
        if (!ok)
            return false;
    }
    return true;
}

void AnimGraphPlayer::StartTransition(Layer& layer, const Transition& transition)
{
    for (const Condition& c : transition.conditions) // triggers are consumed by the transition that uses them
        if (m_Graph->parameters[static_cast<std::size_t>(c.param)].type == AnimParamType::Trigger)
            m_Values[static_cast<std::size_t>(c.param)] = 0.0f;
    const float duration = transition.source->duration;
    if (duration <= 0.0f) {
        layer.current = Play{.state = transition.to};
        layer.fading = layer.frozen = false;
        return;
    }
    if (layer.fading) { // interrupted: fade from the pose shown last
        layer.frozen     = true;
        layer.frozenPose = layer.lastPose;
    } else {
        layer.frozen   = false;
        layer.previous = layer.current;
    }
    layer.current           = Play{.state = transition.to};
    layer.fading            = true;
    layer.fadeElapsed       = 0.0f;
    layer.fadeDuration      = duration;
    layer.fadeInterruptible = transition.source->canInterrupt;
}

void AnimGraphPlayer::Update(float dt, std::span<Transform> pose, std::vector<AnimFiredNotify>* notifies, glm::vec3* rootMotion)
{
    if (rootMotion)
        *rootMotion = glm::vec3(0.0f);
    if (!Bound() || pose.size() != m_Rest.size())
        return;
    dt = std::isfinite(dt) ? std::max(dt, 0.0f) : 0.0f;
    std::ranges::copy(m_Rest, pose.begin());
    std::vector<Transform> current(pose.size()), source;
    glm::vec3              root(0.0f);

    for (std::size_t li = 0; li < m_Layers.size(); ++li) {
        Layer& layer = m_Layers[li];
        if (layer.current.state < 0) {
            layer.effectiveWeight = 0.0f;
            continue;
        }
        // 1. Transitions (from the state shown; during a fade only when interruptible).
        if (!layer.fading || layer.fadeInterruptible)
            for (const Transition& t : layer.transitions) {
                if (t.from >= 0 ? t.from != layer.current.state : t.to == layer.current.state)
                    continue;
                if (Holds(t, layer)) {
                    StartTransition(layer, t);
                    break;
                }
            }
        // 2. Advance.
        const State& state    = layer.states[static_cast<std::size_t>(layer.current.state)];
        const float  from     = layer.current.time;
        layer.current.time   += dt * StateRate(state);
        float previousFrom    = 0.0f;
        const bool liveSource = layer.fading && !layer.frozen && layer.previous.state >= 0;
        if (liveSource) {
            previousFrom = layer.previous.time;
            layer.previous.time += dt * StateRate(layer.states[static_cast<std::size_t>(layer.previous.state)]);
        }
        float alpha = 1.0f;
        if (layer.fading) {
            layer.fadeElapsed += dt;
            alpha = layer.fadeDuration > 0.0f ? std::clamp(layer.fadeElapsed / layer.fadeDuration, 0.0f, 1.0f) : 1.0f;
        }
        // 3. Evaluate.
        EvaluateState(state, layer.current.time, current);
        if (layer.fading && alpha < 1.0f) {
            if (liveSource) {
                source.resize(pose.size());
                EvaluateState(layer.states[static_cast<std::size_t>(layer.previous.state)], layer.previous.time, source);
            } else
                source = layer.frozenPose;
            BlendAnimationPoses(source, current, alpha);
            layer.lastPose = source;
        } else
            layer.lastPose = current;
        if (layer.fading && alpha >= 1.0f)
            layer.fading = layer.frozen = false;

        // 4. Combine.
        float weight = layer.source->weight * layer.runtimeWeight;
        if (layer.weightParam >= 0)
            weight *= std::clamp(Value(layer.weightParam), 0.0f, 1.0f);
        layer.effectiveWeight = weight;
        if (weight > 0.0f) {
            for (std::size_t n = 0; n < pose.size(); ++n) {
                const float w = layer.mask[n] * weight;
                if (w <= 0.0f)
                    continue;
                const Transform& l = layer.lastPose[n];
                if (layer.source->mode == AnimLayerMode::Additive) {
                    const Transform& rest  = m_Rest[n];
                    const glm::quat  delta = glm::inverse(rest.rotation) * l.rotation;
                    pose[n].position += (l.position - rest.position) * w;
                    pose[n].rotation =
                        glm::normalize(pose[n].rotation * glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), delta, w));
                    pose[n].scale *= glm::mix(glm::vec3(1.0f), l.scale / glm::max(glm::abs(rest.scale), glm::vec3(1.0e-6f)), w);
                } else if (w >= 1.0f)
                    pose[n] = l;
                else {
                    pose[n].position = glm::mix(pose[n].position, l.position, w);
                    pose[n].rotation = glm::normalize(glm::slerp(pose[n].rotation, l.rotation, w));
                    pose[n].scale    = glm::mix(pose[n].scale, l.scale, w);
                }
            }
        }
        // 5. Notifies (current state only) and root motion (first layer).
        if (notifies && weight > 0.0f && dt > 0.0f)
            Fire(layer, static_cast<std::uint32_t>(li), state, from, layer.current.time, *notifies);
        if (li == 0 && m_RootMotionNode >= 0) {
            glm::vec3 delta = RootDelta(state, from, layer.current.time);
            if (liveSource && alpha < 1.0f)
                delta = glm::mix(RootDelta(layer.states[static_cast<std::size_t>(layer.previous.state)], previousFrom,
                                           layer.previous.time),
                                 delta, alpha);
            root = delta;
        }
    }
    if (m_RootMotionNode >= 0) {
        pose[static_cast<std::size_t>(m_RootMotionNode)].position = m_Rest[static_cast<std::size_t>(m_RootMotionNode)].position;
        if (rootMotion)
            *rootMotion = root;
    }
}

AnimLayerStatus AnimGraphPlayer::LayerStatus(std::size_t index) const
{
    AnimLayerStatus status;
    if (index >= m_Layers.size())
        return status;
    const Layer& layer = m_Layers[index];
    if (layer.current.state >= 0)
        status.state = layer.states[static_cast<std::size_t>(layer.current.state)].source->name;
    status.normalizedTime = layer.current.time;
    if (layer.fading) {
        status.fromState = layer.frozen || layer.previous.state < 0
                               ? std::string("(frozen)")
                               : layer.states[static_cast<std::size_t>(layer.previous.state)].source->name;
        status.transition = layer.fadeDuration > 0.0f ? std::clamp(layer.fadeElapsed / layer.fadeDuration, 0.0f, 1.0f) : 1.0f;
    }
    status.weight = layer.effectiveWeight;
    return status;
}

} // namespace Engine
