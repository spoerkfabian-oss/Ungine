// Animation graphs (.uanim): file format, state machines, blend spaces, layers, notifies, IK and
// the AnimationSystem on scene entities (no renderer: models are built on the CPU).
#include "BlueprintTestUtil.h"
#include "Test.h"

#include "Engine/Animation/AnimGraph.h"
#include "Engine/Animation/AnimGraphPlayer.h"
#include "Engine/Animation/AnimIk.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Assets/Animation.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Assets/Model.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path AnimTempDir(const char* name)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }
bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance) { return glm::length(a - b) <= tolerance; }
bool Near(const glm::quat& a, const glm::quat& b, float tolerance) { return std::abs(glm::dot(a, b)) >= 1.0f - tolerance; }

Model ToModel(ModelData&& data)
{
    Model model;
    model.name       = data.name;
    model.nodes      = std::move(data.nodes);
    model.skins      = std::move(data.skins);
    model.animations = std::move(data.animations);
    model.meshes     = std::move(data.meshes);
    return model;
}

const Model& Fox()
{
    static const Model fox = ToModel(LoadGltf(fs::path(ENGINE_ASSET_DIR) / "models" / "Fox.glb"));
    return fox;
}

ModelNode Node(const char* name, int parent, glm::vec3 position)
{
    ModelNode node;
    node.name           = name;
    node.parent         = parent;
    node.local.position = position;
    return node;
}

AnimationTrack Track(std::uint32_t node, AnimationPath path, std::vector<float> times, std::vector<glm::vec4> values)
{
    AnimationTrack track;
    track.node   = node;
    track.path   = path;
    track.times  = std::move(times);
    track.values = std::move(values);
    return track;
}

glm::vec4 Rot(float degrees, glm::vec3 axis)
{
    const glm::quat q = glm::angleAxis(glm::radians(degrees), glm::normalize(axis));
    return {q.x, q.y, q.z, q.w};
}

// A chain along +Y: base, b1 .. b3, tip (1 unit apart). Clips: Bend (b1 0 -> 90 deg about Z over
// 1 s), Hold (b1 90 deg about X, b3 45 deg about Z), Move (base 0 -> (2, 0, 0) over 1 s).
Model Chain()
{
    Model m;
    m.name  = "Chain";
    m.nodes = {Node("base", -1, {0, 0, 0}), Node("b1", 0, {0, 1, 0}), Node("b2", 1, {0, 1, 0}), Node("b3", 2, {0, 1, 0}),
               Node("tip", 3, {0, 1, 0})};
    AnimationClip bend{.name = "Bend", .duration = 1.0f, .tracks = {}};
    bend.tracks.push_back(Track(1, AnimationPath::Rotation, {0.0f, 1.0f}, {Rot(0, {0, 0, 1}), Rot(90, {0, 0, 1})}));
    AnimationClip hold{.name = "Hold", .duration = 2.0f, .tracks = {}};
    hold.tracks.push_back(Track(1, AnimationPath::Rotation, {0.0f, 2.0f}, {Rot(90, {1, 0, 0}), Rot(90, {1, 0, 0})}));
    hold.tracks.push_back(Track(3, AnimationPath::Rotation, {0.0f, 2.0f}, {Rot(45, {0, 0, 1}), Rot(45, {0, 0, 1})}));
    AnimationClip move{.name = "Move", .duration = 1.0f, .tracks = {}};
    move.tracks.push_back(Track(0, AnimationPath::Translation, {0.0f, 1.0f}, {glm::vec4(0.0f), glm::vec4(2, 0, 0, 0)}));
    m.animations = {bend, hold, move};
    return m;
}

// Two legs under a pelvis 1 unit above the origin; ankles at y = 0.1, knees bent forward.
Model Biped()
{
    Model m;
    m.name  = "Biped";
    m.nodes = {Node("root", -1, {0, 0, 0}),          Node("pelvis", 0, {0, 1, 0}),
               Node("l_hip", 1, {-0.2f, 0, 0}),      Node("l_knee", 2, {0, -0.45f, 0.05f}),
               Node("l_ankle", 3, {0, -0.45f, -0.05f}), Node("r_hip", 1, {0.2f, 0, 0}),
               Node("r_knee", 5, {0, -0.45f, 0.05f}), Node("r_ankle", 6, {0, -0.45f, -0.05f})};
    AnimationClip idle{.name = "Idle", .duration = 1.0f, .tracks = {}};
    idle.tracks.push_back(Track(1, AnimationPath::Translation, {0.0f, 1.0f}, {glm::vec4(0, 1, 0, 0), glm::vec4(0, 1, 0, 0)}));
    m.animations = {idle};
    return m;
}

std::vector<Transform> Pose(const Model& model) { return std::vector<Transform>(model.nodes.size()); }


// The locomotion graph used by several tests (Fox clips).
AnimGraph Locomotion()
{
    AnimGraph g;
    g.parameters = {{.name = "Speed", .type = AnimParamType::Float},
                    {.name = "Jump", .type = AnimParamType::Trigger},
                    {.name = "Grounded", .type = AnimParamType::Bool, .defaultValue = 1.0f}};
    AnimLayer base;
    base.entryState = "Idle";
    base.states.push_back(AnimState{.name = "Idle", .clip = "Survey"});
    AnimState loco{.name = "Locomotion", .kind = AnimStateKind::BlendSpace1D, .xParameter = "Speed"};
    loco.samples = {{.clip = "Survey", .position = {0, 0}}, {.clip = "Walk", .position = {1, 0}}, {.clip = "Run", .position = {3, 0}}};
    loco.notifies = {{.name = "Step", .time = 0.5f, .bone = "b_LeftFoot01_017"}};
    base.states.push_back(loco);
    base.states.push_back(AnimState{.name = "Jump", .clip = "Survey", .loop = false});
    base.transitions = {
        {.from = "Idle", .to = "Locomotion", .duration = 0.25f, .conditions = {{.parameter = "Speed", .op = AnimCompare::Greater, .value = 0.1f}}},
        {.from = "Locomotion", .to = "Idle", .duration = 0.25f, .conditions = {{.parameter = "Speed", .op = AnimCompare::Less, .value = 0.1f}}},
        {.from = "", .to = "Jump", .duration = 0.0f, .conditions = {{.parameter = "Jump", .op = AnimCompare::Triggered}}},
        {.from = "Jump", .to = "Idle", .duration = 0.1f, .hasExitTime = true, .exitTime = 1.0f}};
    g.layers.push_back(base);
    return g;
}

} // namespace

TEST_CASE(Animation_GraphFileRoundtripAndValidation)
{
    const fs::path dir = AnimTempDir("anim_files");
    AnimGraph      g   = Locomotion();
    g.layers[0].states[1].notifies[0].sound  = PathToUtf8(dir / "Sounds" / "step.wav");
    g.layers[0].states[1].notifies[0].effect = PathToUtf8(dir / "Effects" / "dust.uparticle");
    g.layers[0].states[1].position          = {120.0f, 40.0f};
    AnimState space2d{.name = "Strafe", .kind = AnimStateKind::BlendSpace2D, .xParameter = "Speed", .yParameter = "Speed"};
    space2d.samples = {{.clip = "Walk", .position = {0, 0}}, {.clip = "Run", .position = {1, 0}}, {.clip = "Survey", .position = {0, 1}, .speed = 0.5f}};
    g.layers[0].states.push_back(space2d);
    AnimLayer upper{.name = "Upper", .mode = AnimLayerMode::Additive, .weight = 0.5f, .weightParameter = "Speed",
                    .mask = {{.bone = "b_Spine02_03"}, {.bone = "b_Tail01_012", .includeChildren = false}}, .entryState = "Look"};
    upper.states.push_back(AnimState{.name = "Look", .clip = "Survey"});
    g.layers.push_back(upper);
    g.ik = {{.name = "Head", .kind = AnimIkKind::LookAt, .bones = {"b_Neck_04", "b_Head_05"}, .forwardAxis = {1, 0, 0}, .maxAngle = 60.0f},
            {.name = "Feet", .kind = AnimIkKind::FootPlacement, .feet = {"b_LeftFoot01_017", "b_RightFoot01_021"},
             .pelvisBone = "b_Hip_01", .rayHeight = 0.4f, .layerMask = 3},
            {.name = "Tail", .kind = AnimIkKind::Fabrik, .endBone = "b_Tail03_014", .rootBone = "b_Tail01_012", .iterations = 8},
            {.name = "Paw", .kind = AnimIkKind::TwoBone, .weightParameter = "Speed", .endBone = "b_LeftHand_011", .keepEndRotation = false}};
    g.rootMotionBone = "b_Root_00";
    g.previewModel   = PathToUtf8(dir / "Models" / "Fox.glb");

    const fs::path file = dir / "Anim" / "Fox.uanim";
    std::string    error;
    CHECK(SaveAnimGraph(file, g, &error));
    const auto loaded = LoadAnimGraph(file, &error);
    CHECK(loaded.has_value());
    CHECK(loaded && *loaded == g);
    std::ifstream in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"../Sounds/step.wav\"") != std::string::npos); // relative to the graph
    CHECK(text.find("\"../Models/Fox.glb\"") != std::string::npos);
    CHECK(IsAnimGraphFile(file) && !IsAnimGraphFile(dir / "x.ugraph"));

    // Validation against the Fox: the graph is clean; broken references are reported.
    CHECK(ValidateAnimGraph(g, &Fox()).empty());
    AnimGraph broken = g;
    broken.layers[0].states[0].clip                     = "Dance";
    broken.layers[0].transitions[0].conditions[0].parameter = "Velocity";
    broken.layers[0].transitions.push_back({.from = "Idle", .to = "Nowhere", .hasExitTime = true});
    broken.ik[0].bones.push_back("b_Ear");
    broken.layers[0].states.push_back(AnimState{.name = "Line", .kind = AnimStateKind::BlendSpace2D, .xParameter = "Speed", .yParameter = "Speed",
                                                .samples = {{.clip = "Walk", .position = {0, 0}}, {.clip = "Run", .position = {1, 1}},
                                                            {.clip = "Survey", .position = {2, 2}}}});
    const auto problems = ValidateAnimGraph(broken, &Fox());
    const auto has = [&](const char* text) {
        return std::ranges::any_of(problems, [&](const std::string& p) { return p.find(text) != std::string::npos; });
    };
    CHECK(has("clip 'Dance' not in the model"));
    CHECK(has("unknown parameter 'Velocity'"));
    CHECK(has("unknown target state"));
    CHECK(has("bone 'b_Ear' not in the model"));
    CHECK(has("three samples that are not on one line"));

    // Sanitizing: junk is rejected, odd values are clamped, duplicate names made unique.
    CHECK(!ParseAnimGraph("[1, 2]", dir).has_value());
    const auto odd = ParseAnimGraph(R"({"parameters": [{"name": "A", "type": "int", "default": 2.6}, {"name": "A"}],
                                        "layers": [{"weight": 7, "states": [{"name": "S", "kind": "nope", "clip": "Walk"}]}]})",
                                    dir);
    CHECK(odd.has_value());
    CHECK(odd && odd->parameters.size() == 2 && odd->parameters[1].name == "A 2" && odd->parameters[0].defaultValue == 3.0f);
    CHECK(odd && odd->layers[0].weight == 1.0f && odd->layers[0].entryState == "S" &&
          odd->layers[0].states[0].kind == AnimStateKind::Clip);
}

TEST_CASE(Animation_StateMachineTransitionsAndBlendSpaces)
{
    const Model&    fox = Fox();
    AnimGraphPlayer player;
    player.Bind(std::make_shared<const AnimGraph>(Locomotion()), &fox);
    CHECK(player.Bound());
    std::vector<Transform> pose = Pose(fox), expected = Pose(fox);
    const auto clip = [&](const char* name) -> const AnimationClip& {
        return *std::ranges::find(fox.animations, std::string(name), &AnimationClip::name);
    };
    const auto samePose = [&](float tolerance) {
        for (std::size_t i = 0; i < pose.size(); ++i)
            if (!Near(pose[i].position, expected[i].position, tolerance * 100.0f) || !Near(pose[i].rotation, expected[i].rotation, tolerance))
                return false;
        return true;
    };

    // Idle plays Survey.
    player.Update(0.5f, pose);
    CHECK(player.LayerStatus(0).state == "Idle");
    EvaluateAnimation(fox, clip("Survey"), 0.5f, AnimationPlayback::Loop, expected);
    CHECK(samePose(1.0e-4f));

    // Speed > 0.1 crossfades into the blend space over 0.25 s.
    CHECK(player.SetParameter("Speed", 1.0f));
    player.Update(0.1f, pose);
    AnimLayerStatus status = player.LayerStatus(0);
    CHECK(status.state == "Locomotion" && status.fromState == "Idle" && Near(status.transition, 0.4f, 1.0e-4f));
    player.Update(0.2f, pose);
    status = player.LayerStatus(0);
    CHECK(status.state == "Locomotion" && status.fromState.empty() && status.transition < 0.0f);
    // Speed 1: Walk only, phase-synchronized normalized time.
    const float before = status.normalizedTime;
    player.Update(0.1f, pose);
    CHECK(Near(player.LayerStatus(0).normalizedTime - before, 0.1f / clip("Walk").duration, 1.0e-4f));
    EvaluateAnimation(fox, clip("Walk"), std::fmod(player.LayerStatus(0).normalizedTime, 1.0f) * clip("Walk").duration,
                      AnimationPlayback::Loop, expected);
    CHECK(samePose(1.0e-4f));
    // Speed 2: half Walk, half Run; the phase advances at the blended rate.
    player.SetParameter("Speed", 2.0f);
    const std::vector<float> weights = player.BlendWeights(0);
    CHECK(weights.size() == 3 && Near(weights[0], 0.0f, 1e-6f) && Near(weights[1], 0.5f, 1e-6f) && Near(weights[2], 0.5f, 1e-6f));
    const float t0 = player.LayerStatus(0).normalizedTime;
    player.Update(0.05f, pose);
    const float rate = 0.5f / clip("Walk").duration + 0.5f / clip("Run").duration;
    CHECK(Near(player.LayerStatus(0).normalizedTime - t0, 0.05f * rate, 1.0e-4f));
    // Speed 5 clamps to Run, below the first sample to Survey.
    player.SetParameter("Speed", 5.0f);
    CHECK(Near(player.BlendWeights(0)[2], 1.0f, 1e-6f));

    // Notifies: Step at the middle of each loop, with the bone's node.
    std::vector<AnimFiredNotify> fired;
    player.SetParameter("Speed", 1.0f);
    const float start = player.LayerStatus(0).normalizedTime;
    player.Update(clip("Walk").duration * 2.0f, pose, &fired);
    const int expectedCount = static_cast<int>(std::floor(start + 2.0f - 0.5f) - std::ceil(start - 0.5f) + 1.0f);
    CHECK(static_cast<int>(fired.size()) == expectedCount);
    CHECK(!fired.empty() && fired[0].notify.name == "Step" && fired[0].state == "Locomotion" &&
          fired[0].boneNode == player.NodeIndex("b_LeftFoot01_017"));

    // Trigger: any state -> Jump at once, the trigger is consumed; exit time returns to Idle.
    player.SetParameter("Speed", 0.0f);
    CHECK(player.SetTrigger("Jump"));
    player.Update(0.01f, pose);
    CHECK(player.LayerStatus(0).state == "Jump" && player.Parameter("Jump") == 0.0f);
    player.Update(clip("Survey").duration, pose); // reaches the end (exit time 1)
    player.Update(0.01f, pose);
    CHECK(player.LayerStatus(0).state == "Idle");
    CHECK(!player.SetParameter("Nope", 1.0f));

    // Interruption: a new transition during a fade blends from the frozen pose.
    player.Update(1.0f, pose);
    player.SetParameter("Speed", 2.0f);
    player.Update(0.05f, pose); // Idle -> Locomotion fading
    CHECK(player.LayerStatus(0).fromState == "Idle");
    player.SetParameter("Speed", 0.0f);
    player.Update(0.05f, pose); // interrupted: Locomotion -> Idle from the frozen pose
    status = player.LayerStatus(0);
    CHECK(status.state == "Idle" && status.fromState == "(frozen)");

    // Rebinding keeps parameter values; Restart goes back to the entry state.
    player.SetParameter("Speed", 0.7f);
    player.Bind(std::make_shared<const AnimGraph>(Locomotion()), &fox);
    CHECK(player.Parameter("Speed") == 0.7f && player.LayerStatus(0).state == "Idle");
}

TEST_CASE(Animation_LayersMasksAdditiveAndBlendSpace2D)
{
    const Model chain = Chain();
    AnimGraph   g;
    g.parameters = {{.name = "X"}, {.name = "Y"}, {.name = "UpperWeight", .defaultValue = 0.5f}};
    AnimLayer base{.name = "Base", .entryState = "Bend"};
    base.states.push_back(AnimState{.name = "Bend", .clip = "Bend"});
    AnimLayer upper{.name = "Upper", .weightParameter = "UpperWeight", .mask = {{.bone = "b3"}}, .entryState = "Hold"};
    upper.states.push_back(AnimState{.name = "Hold", .clip = "Hold"});
    g.layers = {base, upper};

    AnimGraphPlayer player;
    player.Bind(std::make_shared<const AnimGraph>(g), &chain);
    std::vector<Transform> pose = Pose(chain);
    player.Update(1.0f, pose); // Bend fully (looping: 1 s wraps to the start, so use 0.5 s steps)
    player.Restart();
    player.Update(0.5f, pose);
    // b1 from Bend only (mask excludes it), 45 deg about Z; b3 halfway to Hold's 45 deg.
    CHECK(Near(pose[1].rotation, glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1)), 1.0e-5f));
    CHECK(Near(pose[3].rotation, glm::angleAxis(glm::radians(22.5f), glm::vec3(0, 0, 1)), 1.0e-5f));
    CHECK(Near(player.LayerStatus(1).weight, 0.5f, 1e-6f));
    CHECK(player.SetLayerWeight("Upper", 0.0f) && !player.SetLayerWeight("Nope", 1.0f));
    player.Update(0.0f, pose);
    CHECK(Near(pose[3].rotation, glm::quat(1, 0, 0, 0), 1.0e-6f));
    player.SetLayerWeight("Upper", 1.0f);

    // Additive: Hold's deltas (relative to the rest pose) on top of Bend, everywhere.
    g.layers[1].mode = AnimLayerMode::Additive;
    g.layers[1].mask.clear();
    g.parameters[2].defaultValue = 1.0f;
    player.Bind(std::make_shared<const AnimGraph>(g), &chain);
    player.SetParameter("UpperWeight", 1.0f);
    player.Update(0.5f, pose);
    const glm::quat expected = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1)) * glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
    CHECK(Near(pose[1].rotation, expected, 1.0e-5f));

    // 2D blend space over three samples: barycentric inside, nearest edge outside.
    AnimGraph s;
    s.parameters = {{.name = "X"}, {.name = "Y"}};
    AnimLayer layer{.entryState = "Space"};
    layer.states.push_back(AnimState{.name = "Space", .kind = AnimStateKind::BlendSpace2D, .xParameter = "X", .yParameter = "Y",
                                     .samples = {{.clip = "Bend", .position = {0, 0}}, {.clip = "Hold", .position = {1, 0}},
                                                 {.clip = "Move", .position = {0, 1}}}});
    s.layers = {layer};
    player.Bind(std::make_shared<const AnimGraph>(s), &chain);
    player.SetParameter("X", 0.25f), player.SetParameter("Y", 0.25f);
    std::vector<float> w = player.BlendWeights(0);
    CHECK(w.size() == 3 && Near(w[0], 0.5f, 1e-5f) && Near(w[1], 0.25f, 1e-5f) && Near(w[2], 0.25f, 1e-5f));
    player.SetParameter("X", 2.0f), player.SetParameter("Y", 2.0f);
    w = player.BlendWeights(0);
    CHECK(Near(w[0], 0.0f, 1e-5f) && Near(w[1], 0.5f, 1e-5f) && Near(w[2], 0.5f, 1e-5f));
    player.SetParameter("X", -1.0f), player.SetParameter("Y", -1.0f);
    w = player.BlendWeights(0);
    CHECK(Near(w[0], 1.0f, 1e-5f));
    // Four samples in a square: the centre is shared by two triangles (sums to one either way).
    s.layers[0].states[0].samples.push_back({.clip = "Bend", .position = {1, 1}});
    player.Bind(std::make_shared<const AnimGraph>(s), &chain);
    player.SetParameter("X", 0.5f), player.SetParameter("Y", 0.5f);
    w = player.BlendWeights(0);
    CHECK(w.size() == 4 && Near(w[0] + w[1] + w[2] + w[3], 1.0f, 1e-5f));

    // Root motion: the bone's movement is reported; the bone stays at its rest position.
    AnimGraph rm;
    rm.rootMotionBone = "base";
    AnimLayer moveLayer{.entryState = "Move"};
    moveLayer.states.push_back(AnimState{.name = "Move", .clip = "Move"});
    rm.layers = {moveLayer};
    player.Bind(std::make_shared<const AnimGraph>(rm), &chain);
    glm::vec3 delta(0.0f), total(0.0f);
    for (int i = 0; i < 15; ++i) { // 1.5 s: one full loop (2 units) and half of the next (1 unit)
        player.Update(0.1f, pose, nullptr, &delta);
        total += delta;
    }
    CHECK(Near(total, glm::vec3(3.0f, 0.0f, 0.0f), 1.0e-3f));
    CHECK(Near(pose[0].position, glm::vec3(0.0f), 1e-6f));
}

TEST_CASE(Animation_IkSolvers)
{
    const Model            chain = Chain();
    std::vector<Transform> pose  = Pose(chain);
    for (std::size_t i = 0; i < pose.size(); ++i)
        pose[i] = chain.nodes[i].local;
    pose[2].rotation = glm::angleAxis(glm::radians(10.0f), glm::vec3(0, 0, 1)); // a slight bend at b2

    // Two-bone (b1 - b2 - b3): reachable target, bone lengths kept, pole decides the side.
    {
        std::vector<Transform> p = pose;
        PoseSpace              space(chain, p);
        const glm::vec3        target(1.2f, 1.6f, 0.0f);
        const glm::vec3        pole(-5.0f, 2.0f, 0.0f);
        CHECK(SolveTwoBoneIk(space, 3, target, &pole, true, 1.0f));
        CHECK(Near(space.Position(3), target, 1.0e-3f));
        CHECK(Near(glm::length(space.Position(2) - space.Position(1)), 1.0f, 1.0e-4f));
        CHECK(Near(glm::length(space.Position(3) - space.Position(2)), 1.0f, 1.0e-4f));
        CHECK(space.Position(2).x < 0.6f); // knee towards the pole (left of the line base -> target)
        const glm::vec3 otherPole(5.0f, 0.0f, 0.0f);
        CHECK(SolveTwoBoneIk(space, 3, target, &otherPole, true, 1.0f));
        CHECK(Near(space.Position(3), target, 1.0e-3f) && space.Position(2).x > 0.6f);
        // Out of reach: straight towards the target.
        CHECK(SolveTwoBoneIk(space, 3, glm::vec3(0.0f, 9.0f, 0.0f), nullptr, true, 1.0f));
        CHECK(Near(space.Position(3), glm::vec3(0.0f, 3.0f, 0.0f), 2.0e-3f));
        // Half weight: between the animated and the solved pose; no grandparent: refused.
        std::vector<Transform> q = pose;
        PoseSpace              half(chain, q);
        const glm::vec3        animated = half.Position(3);
        CHECK(SolveTwoBoneIk(half, 3, target, &pole, true, 0.5f));
        const float d = glm::length(half.Position(3) - target);
        CHECK(d > 1.0e-2f && d < glm::length(animated - target));
        CHECK(!SolveTwoBoneIk(half, 1, target, nullptr, true, 1.0f));
    }
    // FABRIK over b1 .. tip (3 bones): reaches, lengths kept; out of reach points at the target.
    {
        std::vector<Transform> p = pose;
        PoseSpace              space(chain, p);
        const glm::vec3        target(1.5f, 2.5f, 0.8f);
        CHECK(SolveFabrikIk(space, 1, 4, target, 32, 1.0e-4f, 1.0f));
        CHECK(Near(space.Position(4), target, 2.0e-3f));
        for (int i = 1; i < 4; ++i)
            CHECK(Near(glm::length(space.Position(i + 1) - space.Position(i)), 1.0f, 1.0e-3f));
        CHECK(!SolveFabrikIk(space, 4, 1, target, 8, 1.0e-3f, 1.0f)); // root below the end
        CHECK(SolveFabrikIk(space, 1, 4, glm::vec3(0.0f, 10.0f, 0.0f), 8, 1.0e-3f, 1.0f));
        CHECK(Near(space.Position(4), glm::vec3(0.0f, 4.0f, 0.0f), 2.0e-3f));
    }
    // Look-at: the tip's +Y axis aims at the target; limited by maxAngle.
    {
        std::vector<Transform> p = pose;
        PoseSpace              space(chain, p);
        const std::array<int, 2> bones{2, 3};
        const glm::vec3        target = space.Position(3) + glm::vec3(5.0f, 0.0f, 0.0f);
        CHECK(SolveLookAtIk(space, bones, {0, 1, 0}, target, glm::radians(170.0f), 1.0f));
        const glm::vec3 aim = space.Rotation(3) * glm::vec3(0, 1, 0);
        CHECK(glm::dot(aim, glm::normalize(target - space.Position(3))) > 0.999f);
        std::vector<Transform> q = pose;
        PoseSpace              limited(chain, q);
        const glm::vec3        before = limited.Rotation(3) * glm::vec3(0, 1, 0);
        CHECK(SolveLookAtIk(limited, bones, {0, 1, 0}, limited.Position(3) + glm::vec3(0, 0, -5), glm::radians(30.0f), 1.0f));
        const glm::vec3 after = limited.Rotation(3) * glm::vec3(0, 1, 0);
        CHECK(Near(glm::degrees(std::acos(std::clamp(glm::dot(before, after), -1.0f, 1.0f))), 30.0f, 0.5f));
    }
    // Foot placement on a synthetic ground: left foot on a 0.2 step, then the whole body 0.25
    // above the ground (pelvis lowers), feet tilt to the normal.
    {
        const Model biped = Biped();
        std::vector<Transform> p(biped.nodes.size());
        for (std::size_t i = 0; i < p.size(); ++i)
            p[i] = biped.nodes[i].local;
        const std::vector<Transform> rest = p;
        AnimIk feet{.kind = AnimIkKind::FootPlacement, .feet = {"l_ankle", "r_ankle"}, .pelvisBone = "pelvis", .alignToNormal = false,
                    .smoothing = 0.0f};
        const std::array<int, 2> ankles{4, 7};
        float     stepHeight = 0.2f;
        glm::vec3 normal(0, 1, 0);
        const GroundRaycast ground = [&](const glm::vec3& from, const glm::vec3&) -> std::optional<GroundHit> {
            return GroundHit{.point = {from.x, from.x < 0.0f ? stepHeight : 0.0f, from.z}, .normal = normal};
        };
        FootIkState state;
        {
            PoseSpace space(biped, p);
            SolveFootPlacement(space, glm::mat4(1.0f), feet, ankles, 1, ground, state, 0.016f, 1.0f);
            CHECK(Near(space.Position(4).y, 0.3f, 2.0e-3f));
            CHECK(Near(space.Position(7).y, 0.1f, 2.0e-3f));
            CHECK(Near(space.Position(1).y, 1.0f, 1.0e-4f)); // the lowest foot is on the ground: pelvis stays
        }
        p = rest;
        stepHeight = 0.0f;
        glm::mat4 lifted = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.25f, 0.0f));
        state = {};
        {
            PoseSpace space(biped, p);
            SolveFootPlacement(space, lifted, feet, ankles, 1, ground, state, 0.016f, 1.0f);
            CHECK(Near(state.pelvisOffset, -0.25f, 1.0e-4f));
            CHECK(Near(space.Position(1).y + 0.25f, 1.0f, 1.0e-3f)); // world pelvis
            CHECK(Near(space.Position(4).y + 0.25f, 0.1f, 2.0e-3f)); // world ankles on the ground again
            CHECK(Near(space.Position(7).y + 0.25f, 0.1f, 2.0e-3f));
        }
        // Smoothing: the offsets approach their targets over time.
        p = rest;
        feet.smoothing = 10.0f;
        state          = {};
        state.initialized = true, state.footOffsets = {0.0f, 0.0f}, state.normals = {normal, normal};
        stepHeight = 0.2f;
        {
            PoseSpace space(biped, p);
            SolveFootPlacement(space, glm::mat4(1.0f), feet, ankles, 1, ground, state, 0.05f, 1.0f);
            CHECK(state.footOffsets[0] > 0.05f && state.footOffsets[0] < 0.15f);
        }
        // Tilt: a 20 degree slope turns the feet (limit 30).
        p = rest;
        feet.smoothing     = 0.0f;
        feet.alignToNormal = true;
        normal             = glm::angleAxis(glm::radians(20.0f), glm::vec3(1, 0, 0)) * glm::vec3(0, 1, 0);
        state              = {};
        stepHeight         = 0.0f;
        {
            PoseSpace       space(biped, p);
            const glm::quat footBefore = space.Rotation(7);
            SolveFootPlacement(space, glm::mat4(1.0f), feet, ankles, 1, ground, state, 0.016f, 1.0f);
            const glm::quat tilt = space.Rotation(7) * glm::inverse(footBefore);
            CHECK(Near(glm::degrees(glm::angle(tilt)), 20.0f, 0.5f));
        }
    }
}

TEST_CASE(Animation_SystemDrivesEntitiesNotifiesAndIk)
{
    const fs::path dir   = AnimTempDir("anim_system");
    const Model&   fox   = Fox();
    const Model    chain = Chain();
    const Model    biped = Biped();
    const fs::path foxGraph = dir / "Fox.uanim";
    CHECK(SaveAnimGraph(foxGraph, Locomotion()));

    EventBus bus;
    Scene    scene;
    const ModelHandle foxHandle{.index = 1, .generation = 1}, chainHandle{.index = 2, .generation = 1},
                      bipedHandle{.index = 3, .generation = 1};
    AnimationSystem anim([&](ModelHandle h) -> const Model* {
        return h == foxHandle ? &fox : h == chainHandle ? &chain : h == bipedHandle ? &biped : nullptr;
    }, nullptr, &bus);
    std::vector<AnimNotifyEvent> notifies;
    auto sub = bus.Subscribe<AnimNotifyEvent>([&](const AnimNotifyEvent& e) {
        notifies.push_back(e);
        return false;
    });

    const Entity character = scene.CreateEntity("Character");
    const Entity foxRoot   = InstantiateModel(scene, foxHandle, fox, character);
    scene.GetRegistry().Emplace<AnimationController>(foxRoot, AnimationController{.graph = PathToUtf8(foxGraph)});
    CHECK(anim.SetFloat(foxRoot, "Speed", 1.0f)); // before the first update: kept
    anim.Update(scene, 0.0f);
    CHECK(anim.HasInstance(foxRoot) && anim.Parameter(foxRoot, "Speed") == 1.0f);
    CHECK(!anim.SetFloat(foxRoot, "Nope", 1.0f));
    for (int i = 0; i < 30; ++i)
        anim.Update(scene, 1.0f / 30.0f);
    CHECK(anim.CurrentState(foxRoot) == "Locomotion");
    CHECK(!notifies.empty() && notifies[0].entity == foxRoot && notifies[0].name == "Step" && notifies[0].bone == "b_LeftFoot01_017");
    // The bone entities carry the player's pose.
    Entity head = NullEntity;
    scene.GetRegistry().ViewOf<ModelNodeRef>().Each([&](Entity e, ModelNodeRef& ref) {
        if (ref.node == 7 && FindModelInstanceRoot(scene.GetRegistry(), e) == foxRoot)
            head = e;
    });
    CHECK(head != NullEntity);
    AnimGraphPlayer reference;
    reference.Bind(anim.Graph(PathToUtf8(foxGraph)), &fox);
    CHECK(anim.Inspect(foxRoot).has_value() && anim.Inspect(foxRoot)->layers[0].state == "Locomotion");

    // Disabled controller: the instance goes away; the Animator path would take over again.
    scene.GetRegistry().Get<AnimationController>(foxRoot).enabled = false;
    anim.Update(scene, 0.1f);
    CHECK(!anim.HasInstance(foxRoot));
    scene.GetRegistry().Get<AnimationController>(foxRoot).enabled = true;
    anim.Update(scene, 0.1f);
    CHECK(anim.CurrentState(foxRoot) == "Idle"); // starts over at the entry state

    // Editor override and hot reload: instances rebind, parameters stay.
    AnimGraph provided = Locomotion();
    provided.layers[0].entryState = "Jump";
    anim.Provide(PathToUtf8(foxGraph), std::make_shared<const AnimGraph>(provided));
    anim.SetFloat(foxRoot, "Speed", 0.0f);
    anim.Update(scene, 0.01f);
    CHECK(anim.CurrentState(foxRoot) == "Jump");
    anim.Provide(PathToUtf8(foxGraph), nullptr);
    anim.Update(scene, 0.01f);
    CHECK(anim.CurrentState(foxRoot) == "Idle");
    AnimGraph edited = Locomotion();
    edited.parameters.push_back({.name = "Extra"});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(SaveAnimGraph(foxGraph, edited));
    fs::last_write_time(foxGraph, fs::file_time_type::clock::now() + std::chrono::seconds(2));
    anim.Update(scene, 1.1f); // the check runs once a second
    CHECK(anim.Parameter(foxRoot, "Extra").has_value());

    // Root motion moves the controller's entity (no character); the bone stays.
    const fs::path moveGraph = dir / "Move.uanim";
    AnimGraph      rm;
    rm.rootMotionBone = "base";
    rm.layers.push_back(AnimLayer{.entryState = "Move", .states = {AnimState{.name = "Move", .clip = "Move"}}});
    CHECK(SaveAnimGraph(moveGraph, rm));
    const Entity mover = InstantiateModel(scene, chainHandle, chain);
    scene.GetRegistry().Emplace<AnimationController>(mover, AnimationController{.graph = PathToUtf8(moveGraph), .applyRootMotion = true});
    anim.Update(scene, 0.0f);
    for (int i = 0; i < 5; ++i)
        anim.Update(scene, 0.1f);
    CHECK(Near(scene.GetTransform(mover).position, glm::vec3(1.0f, 0.0f, 0.0f), 1.0e-3f));

    // Two-bone IK through the system: the end entity reaches a world target (instance moved).
    const fs::path ikGraph = dir / "Reach.uanim";
    AnimGraph      reach;
    reach.layers.push_back(AnimLayer{.entryState = "Hold", .states = {AnimState{.name = "Hold", .clip = "Bend", .speed = 0.0f}}});
    reach.ik.push_back(AnimIk{.name = "Arm", .kind = AnimIkKind::TwoBone, .endBone = "b3"});
    CHECK(SaveAnimGraph(ikGraph, reach));
    const Entity arm = InstantiateModel(scene, chainHandle, chain);
    scene.EditTransform(arm).position = {10.0f, 0.0f, 0.0f};
    scene.GetRegistry().Emplace<AnimationController>(arm, AnimationController{.graph = PathToUtf8(ikGraph)});
    scene.UpdateTransforms();
    CHECK(anim.SetIkTarget(arm, "Arm", {11.0f, 1.8f, 0.5f}));
    anim.Update(scene, 0.0f);
    CHECK(!anim.SetIkTarget(arm, "Leg", {0, 0, 0})); // bound now: unknown goal
    anim.Update(scene, 0.016f);
    scene.UpdateTransforms();
    Entity b3 = NullEntity;
    scene.GetRegistry().ViewOf<ModelNodeRef>().Each([&](Entity e, ModelNodeRef& ref) {
        if (ref.node == 3 && FindModelInstanceRoot(scene.GetRegistry(), e) == arm)
            b3 = e;
    });
    CHECK(b3 != NullEntity && Near(glm::vec3(scene.GetRegistry().Get<WorldTransform>(b3).matrix[3]), glm::vec3(11.0f, 1.8f, 0.5f), 2.0e-3f));
    CHECK(anim.Stats().ikSolved >= 1);
    CHECK(anim.SetIkEnabled(arm, "Arm", false));
    anim.Update(scene, 0.016f);
    scene.UpdateTransforms();
    CHECK(!Near(glm::vec3(scene.GetRegistry().Get<WorldTransform>(b3).matrix[3]), glm::vec3(11.0f, 1.8f, 0.5f), 0.05f));

    // Foot placement with physics: a step under the left foot lifts it, the right stays.
    ThreadPool   pool{2};
    PhysicsWorld physics(pool, bus);
    AnimationSystem grounded([&](ModelHandle h) -> const Model* { return h == bipedHandle ? &biped : nullptr; }, &physics, &bus);
    Scene        world;
    const auto box = [&](glm::vec3 position, glm::vec3 half) {
        const Entity e = world.CreateEntity("Box");
        world.EditTransform(e).position = position;
        world.GetRegistry().Emplace<Collider>(e, Collider{.shape = ColliderShape::Box, .halfExtents = half});
        world.GetRegistry().Emplace<RigidBody>(e, RigidBody{.type = BodyType::Static});
    };
    box({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f});
    box({-0.6f, 0.1f, 0.0f}, {0.5f, 0.1f, 0.5f}); // top at 0.2 under x < -0.1
    const fs::path feetGraph = dir / "Feet.uanim";
    AnimGraph      feet;
    feet.layers.push_back(AnimLayer{.entryState = "Idle", .states = {AnimState{.name = "Idle", .clip = "Idle"}}});
    feet.ik.push_back(AnimIk{.name = "Feet", .kind = AnimIkKind::FootPlacement, .feet = {"l_ankle", "r_ankle"}, .pelvisBone = "pelvis",
                             .alignToNormal = false, .smoothing = 0.0f});
    CHECK(SaveAnimGraph(feetGraph, feet));
    const Entity walker = world.CreateEntity("Walker");
    world.GetRegistry().Emplace<Collider>(walker, Collider{.shape = ColliderShape::Capsule, .radius = 0.3f, .halfHeight = 0.6f,
                                                           .center = {0.0f, 0.9f, 0.0f}}); // its own body: ignored
    world.GetRegistry().Emplace<CharacterController>(walker);
    const Entity body = InstantiateModel(world, bipedHandle, biped, walker);
    world.GetRegistry().Emplace<AnimationController>(body, AnimationController{.graph = PathToUtf8(feetGraph)});
    world.UpdateTransforms();
    physics.Sync(world);
    grounded.Update(world, 0.016f);
    world.UpdateTransforms();
    const auto worldY = [&](std::uint32_t node) {
        float y = -1.0f;
        world.GetRegistry().ViewOf<ModelNodeRef>().Each([&](Entity e, ModelNodeRef& ref) {
            if (ref.node == node && FindModelInstanceRoot(world.GetRegistry(), e) == body)
                y = world.GetRegistry().Get<WorldTransform>(e).matrix[3].y;
        });
        return y;
    };
    CHECK(Near(worldY(4), 0.3f, 0.01f));
    CHECK(Near(worldY(7), 0.1f, 0.01f));
    CHECK(Near(worldY(1), 1.0f, 0.01f));
    // Character 0.25 above the ground: the pelvis lowers, both feet reach the ground.
    world.EditTransform(walker).position = {2.0f, 0.25f, 0.0f};
    world.UpdateTransforms();
    physics.Sync(world);
    grounded.Update(world, 0.016f);
    world.UpdateTransforms();
    CHECK(Near(worldY(4), 0.1f, 0.01f) && Near(worldY(7), 0.1f, 0.01f));
    CHECK(Near(worldY(1), 1.0f, 0.01f));
}

TEST_CASE(Animation_BlueprintNodesAndSerialization)
{
    using namespace BlueprintTest;
    const fs::path dir   = AnimTempDir("anim_blueprint");
    const Model&   fox   = Fox();
    const fs::path graph = dir / "Graphs" / "Fox.uanim";
    AnimGraph      loco  = Locomotion();
    loco.ik.push_back(AnimIk{.name = "Head", .kind = AnimIkKind::LookAt, .bones = {"b_Neck_04", "b_Head_05"}, .forwardAxis = {1, 0, 0}});
    CHECK(SaveAnimGraph(graph, loco));

    // Scene file: relative path, roundtrip.
    {
        Scene        scene;
        const Entity e = scene.CreateEntity("Fox");
        scene.GetRegistry().Emplace<AnimationController>(
            e, AnimationController{.graph = PathToUtf8(graph), .enabled = false, .speed = 1.5f, .applyRootMotion = true, .ik = false});
        const fs::path file = dir / "Levels" / "A.scene.json";
        fs::create_directories(file.parent_path());
        SaveSceneFile(file, scene, nullptr);
        std::ifstream in(file);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"../Graphs/Fox.uanim\"") != std::string::npos);
        Scene loaded;
        CHECK(LoadSceneFile(file, loaded, nullptr).empty());
        const Entity back = loaded.FindByUuid(scene.GetRegistry().Get<Uuid>(e).value);
        const auto*  c    = back != NullEntity ? loaded.GetRegistry().TryGet<AnimationController>(back) : nullptr;
        CHECK(c && *c == scene.GetRegistry().Get<AnimationController>(e));
    }

    // Blueprint on the character, the controller on its model child.
    Runner r;
    const ModelHandle handle{.index = 1, .generation = 1};
    AnimationSystem anim([&](ModelHandle h) -> const Model* { return h == handle ? &fox : nullptr; }, nullptr, &r.bus);
    r.scripts.SetAnimationSystem(&anim);
    Graph b;
    const auto begin    = b.Node("Event.BeginPlay");
    const auto setSpeed = b.Node("Anim.SetFloat");
    b.Set(setSpeed, "Parameter", std::string("Speed"));
    b.Set(setSpeed, "Value", 2.0f);
    const auto look = b.Node("Anim.SetIkTarget");
    b.Set(look, "Goal", std::string("Head"));
    b.Set(look, "Location", glm::vec3(0.0f, 50.0f, 300.0f));
    const auto bad = b.Node("Anim.SetTrigger");
    b.Set(bad, "Parameter", std::string("Missing"));
    b.Link(begin, "Out", setSpeed, "In");
    b.Link(setSpeed, "Then", look, "In");
    b.Link(look, "Then", bad, "In");
    const auto onNotify = b.Node("Event.AnimNotify");
    const auto print    = b.Node("Debug.Print");
    b.Link(onNotify, "Out", print, "In");
    b.Link(onNotify, "Name", print, "Text");
    const auto tick  = b.Node("Event.Tick");
    const auto state = b.Node("Anim.GetState");
    const auto print2 = b.Node("Debug.Print");
    b.Link(tick, "Out", print2, "In");
    b.Link(state, "State", print2, "Text");
    CHECK(b.Valid());
    const Entity character = r.Add("Character", "fox_bp.ugraph", b.g);
    const Entity foxRoot   = InstantiateModel(r.scene, handle, fox, character);
    r.scene.GetRegistry().Emplace<AnimationController>(foxRoot, AnimationController{.graph = PathToUtf8(graph)});
    r.scripts.Begin(r.scene);
    for (int i = 0; i < 40; ++i) {
        r.scripts.Update(r.scene, 0.05f);
        anim.Update(r.scene, 0.05f);
    }
    CHECK(anim.Parameter(foxRoot, "Speed") == 2.0f);
    CHECK(r.Printed("Locomotion"));
    CHECK(r.Printed("Step"));
    CHECK(anim.Stats().ikSolved >= 1); // the head looks at the target
    CHECK(r.scripts.Stats().errors >= 1); // unknown trigger reported
}
