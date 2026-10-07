// Phase 26: physics materials, contact data, joints, ragdolls, character tools, their Blueprint nodes.
#include "BlueprintTestUtil.h"
#include "Test.h"

#include "Engine/Assets/Model.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsMaterial.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Physics/Ragdoll.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <thread>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / std::format("ungine_{}_{}", name, std::random_device{}());
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

struct World {
    ThreadPool                         pool{2};
    EventBus                           bus;
    Scene                              scene;
    PhysicsWorld                       physics{pool, bus};
    std::vector<CollisionEvent>        events;
    std::vector<CollisionPersistEvent> persisted;
    std::vector<JointBrokenEvent>      broken;
    Subscription sub1 = bus.Subscribe<CollisionEvent>([this](const CollisionEvent& e) { events.push_back(e); return false; });
    Subscription sub2 = bus.Subscribe<CollisionPersistEvent>([this](const CollisionPersistEvent& e) {
        persisted.push_back(e);
        return false;
    });
    Subscription sub3 = bus.Subscribe<JointBrokenEvent>([this](const JointBrokenEvent& e) { broken.push_back(e); return false; });

    Entity Body(const char* name, glm::vec3 position, BodyType type, Collider collider, float mass = 1.0f)
    {
        const Entity e = scene.CreateEntity(name);
        scene.EditTransform(e).position = position;
        scene.GetRegistry().Emplace<RigidBody>(e, RigidBody{.type = type, .mass = mass});
        scene.GetRegistry().Emplace<Collider>(e, collider);
        return e;
    }
    static Collider Box(glm::vec3 half, std::string material = {})
    {
        Collider c;
        c.halfExtents = half;
        c.material    = std::move(material);
        return c;
    }
    static Collider Sphere(float radius, std::string material = {})
    {
        Collider c;
        c.shape    = ColliderShape::Sphere;
        c.radius   = radius;
        c.material = std::move(material);
        return c;
    }
    void Run(float seconds)
    {
        for (int i = 0, n = static_cast<int>(seconds * 60.0f); i < n; ++i)
            physics.Step(scene, 1.0f / 60.0f);
    }
    glm::vec3 Position(Entity e) const { return scene.GetRegistry().Get<WorldTransform>(e).matrix[3]; }
    std::uint64_t Uuid(Entity e) const { return scene.GetRegistry().Get<Engine::Uuid>(e).value; }
};

bool Near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

} // namespace

TEST_CASE(Physics_MaterialsAndSurfaces)
{
    const fs::path dir = TempDir("physmat");
    const auto     ice = dir / "Ice.uphysmat", rubber = dir / "Rubber.uphysmat", stone = dir / "Stone.uphysmat";
    CHECK(SavePhysicsMaterial(ice, {.friction = 0.0f, .restitution = 0.0f, .surface = "Ice"}));
    CHECK(SavePhysicsMaterial(rubber, {.friction = 1.0f, .restitution = 0.9f, .surface = "Rubber"}));
    CHECK(SavePhysicsMaterial(stone, {.friction = 0.8f, .restitution = 0.0f, .surface = "Stone"}));
    CHECK(LoadPhysicsMaterial(rubber) == (PhysicsMaterialData{1.0f, 0.9f, "Rubber"}));
    std::string error;
    CHECK(!LoadPhysicsMaterial(dir / "missing.uphysmat", &error) && !error.empty());
    std::ofstream(dir / "broken.uphysmat") << "{ nope";
    CHECK(!LoadPhysicsMaterial(dir / "broken.uphysmat", &error));

    // Friction: a box slides far on ice, briefly on rubber (material friction replaces the collider's).
    const auto slide = [&](const fs::path& material) {
        World w;
        w.Body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({50.0f, 0.5f, 50.0f}, PathToUtf8(material)));
        const Entity box = w.Body("Box", {0.0f, 0.25f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.25f), PathToUtf8(material)));
        w.physics.Sync(w.scene);
        w.physics.SetLinearVelocity(box, {5.0f, 0.0f, 0.0f});
        w.Run(1.5f);
        return w.Position(box).x;
    };
    CHECK(slide(ice) > 6.0f);
    CHECK(slide(rubber) < 3.0f);

    // Restitution (max of both), surfaces in raycasts and contacts; a missing material falls back.
    World w;
    const Entity floor = w.Body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({20.0f, 0.5f, 20.0f}, PathToUtf8(rubber)));
    const Entity ball  = w.Body("Ball", {0.0f, 2.0f, 0.0f}, BodyType::Dynamic, World::Sphere(0.5f, PathToUtf8(stone)));
    w.Body("Plain", {5.0f, 0.0f, 0.0f}, BodyType::Static, World::Box(glm::vec3(0.5f), PathToUtf8(dir / "missing.uphysmat")));
    w.physics.Sync(w.scene);
    float maxUp = 0.0f;
    for (int i = 0; i < 90; ++i) {
        w.physics.Step(w.scene, 1.0f / 60.0f);
        maxUp = std::max(maxUp, w.physics.LinearVelocity(ball).y);
    }
    CHECK(maxUp > 3.0f); // bounced
    const auto down = w.physics.Raycast({0.0f, 5.0f, 3.0f}, {0.0f, -1.0f, 0.0f}, 10.0f);
    CHECK(down && down->entity == floor && down->surface == "Rubber");
    const auto plain = w.physics.Raycast({5.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f);
    CHECK(plain && plain->surface.empty());
    const auto begin = std::ranges::find_if(w.events, [&](const CollisionEvent& e) { return e.begin; });
    CHECK(begin != w.events.end());
    if (begin != w.events.end()) {
        const bool ballIsA = begin->a == ball;
        CHECK((ballIsA ? begin->contact.surfaceA : begin->contact.surfaceB) == "Stone");
        CHECK((ballIsA ? begin->contact.surfaceB : begin->contact.surfaceA) == "Rubber");
    }

    // Hot reload: the file changes, the next Sync (checks once a second) uses the new values.
    World h;
    h.Body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({50.0f, 0.5f, 50.0f}, PathToUtf8(ice)));
    const Entity box = h.Body("Box", {0.0f, 0.25f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.25f), PathToUtf8(ice)));
    h.physics.Sync(h.scene);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(SavePhysicsMaterial(ice, {.friction = 2.0f, .restitution = 0.0f, .surface = "Ice"}));
    h.physics.Sync(h.scene);
    h.physics.SetLinearVelocity(box, {5.0f, 0.0f, 0.0f});
    h.Run(1.5f);
    CHECK(h.Position(box).x < 3.0f);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Physics_ContactDataPersistAndCharacters)
{
    World w;
    w.Body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({20.0f, 0.5f, 20.0f}));
    const Entity ball = w.Body("Ball", {0.0f, 2.0f, 0.0f}, BodyType::Dynamic, World::Sphere(0.5f));
    w.physics.Sync(w.scene);
    w.Run(1.0f);
    const auto begin = std::ranges::find_if(w.events, [](const CollisionEvent& e) { return e.begin; });
    CHECK(begin != w.events.end());
    if (begin != w.events.end()) {
        const ContactInfo& c        = begin->contact;
        const glm::vec3    fromBall = begin->a == ball ? c.normal : -c.normal; // normal: from a towards b
        CHECK(Near(c.point.y, 0.0f, 0.1f) && fromBall.y < -0.9f);
        CHECK(c.approachSpeed > 4.0f && c.approachSpeed < 7.0f); // ~5.4 m/s after 1.5 m
        CHECK(c.impulse > 3.0f && c.impulse < 9.0f);              // 1 kg
    }
    CHECK(!w.persisted.empty()); // resting contact, every step until it sleeps
    const std::size_t persisted = w.persisted.size();

    // The End event carries the last contact; no persist events when switched off.
    w.scene.EditTransform(ball).position = {0.0f, 5.0f, 0.0f};
    w.Run(0.1f);
    const auto end = std::ranges::find_if(w.events, [](const CollisionEvent& e) { return !e.begin; });
    CHECK(end != w.events.end() && end != w.events.end() && Near(end->contact.point.y, 0.0f, 0.1f));
    w.physics.settings.persistEvents = false;
    w.persisted.clear();
    w.Run(2.0f);
    CHECK(w.persisted.empty() && persisted > 5);

    // Characters: contacts with point / normal towards the other body.
    World c;
    c.Body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({20.0f, 0.5f, 20.0f}));
    const Entity wall      = c.Body("Wall", {2.0f, 1.0f, 0.0f}, BodyType::Static, World::Box({0.2f, 1.0f, 2.0f}));
    const Entity character = c.scene.CreateEntity("Character");
    c.scene.GetRegistry().Emplace<CharacterController>(character);
    c.physics.Sync(c.scene);
    c.physics.SetCharacterInput(character, {3.0f, 0.0f, 0.0f}, false);
    c.Run(1.5f);
    const auto hitWall = std::ranges::find_if(c.events, [&](const CollisionEvent& e) { return e.begin && e.b == wall; });
    CHECK(hitWall != c.events.end() && hitWall->a == character);
    if (hitWall != c.events.end())
        CHECK(hitWall->contact.normal.x > 0.9f && Near(hitWall->contact.point.x, 1.8f, 0.1f));
}

TEST_CASE(Physics_JointsLimitsMotorsBreaking)
{
    constexpr float kPi = 3.14159265f;
    const glm::quat axisZ = glm::angleAxis(-0.5f * kPi, glm::vec3(0.0f, 1.0f, 0.0f)); // joint X -> world +Z

    // Hinge pendulum to the world (anchor 2 m beside the box): keeps its distance, swings down.
    {
        World      w;
        const Entity box = w.Body("Box", {2.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        w.scene.GetRegistry().Emplace<Joint>(box, Joint{.type = JointType::Hinge, .anchor = {-2.0f, 0.0f, 0.0f},
                                                         .anchorRotation = axisZ, .limits = false});
        w.physics.Sync(w.scene);
        CHECK(w.physics.Stats().joints == 1 && w.physics.GetJointState(box).active);
        w.Run(0.6f);
        CHECK(Near(glm::length(w.Position(box)), 2.0f, 0.05f) && w.Position(box).y < -1.0f);
        bool frames = false;
        w.physics.ForEachJoint([&](const JointDebugShape& s) {
            frames = s.active && glm::length(glm::vec3(s.frameA[3]) - glm::vec3(s.frameB[3])) < 0.05f;
        });
        CHECK(frames);
    }
    // Limited hinge stops at +-30 degrees; a velocity motor (no gravity) turns it.
    {
        World      w; // a 2 m bar hinged at one end
        const Entity box = w.Body("Bar", {1.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box({1.0f, 0.1f, 0.1f}));
        Joint        joint{.type = JointType::Hinge, .anchor = {-1.0f, 0.0f, 0.0f}, .anchorRotation = axisZ};
        joint.minLimit = glm::radians(-30.0f);
        joint.maxLimit = glm::radians(30.0f);
        w.scene.GetRegistry().Emplace<Joint>(box, joint);
        w.physics.Sync(w.scene);
        w.Run(2.0f);
        const float angle = w.physics.GetJointState(box).angle;
        CHECK(std::abs(angle) < glm::radians(34.0f) && std::abs(angle) > glm::radians(20.0f));

        World      m;
        m.physics.settings.gravity = glm::vec3(0.0f);
        const Entity wheel = m.Body("Wheel", {2.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        Joint        motor{.type = JointType::Hinge, .anchor = {-2.0f, 0.0f, 0.0f}, .anchorRotation = axisZ, .limits = false};
        m.scene.GetRegistry().Emplace<Joint>(wheel, motor);
        m.physics.Sync(m.scene);
        m.scene.GetRegistry().Get<Joint>(wheel).motor = {.mode = JointMotorMode::Velocity, .target = 1.0f, .maxForce = 1e5f};
        m.Run(1.0f); // the motor change applies live (no remake: the angle keeps counting)
        CHECK(std::abs(m.physics.GetJointState(wheel).angle) > 0.7f);
    }
    // Slider with limits under sideways gravity; distance rope; fixed pair falling together.
    {
        World      w;
        w.physics.settings.gravity = {5.0f, 0.0f, 0.0f};
        const Entity carriage = w.Body("Carriage", {0.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        Joint        slider{.type = JointType::Slider, .minLimit = -1.0f, .maxLimit = 0.5f};
        w.scene.GetRegistry().Emplace<Joint>(carriage, slider);
        w.physics.Sync(w.scene);
        w.Run(2.0f);
        CHECK(Near(w.Position(carriage).x, 0.5f, 0.05f) && Near(w.physics.GetJointState(carriage).position, 0.5f, 0.05f));
        CHECK(Near(w.Position(carriage).y, 0.0f, 0.01f));
    }
    {
        World      w;
        const Entity weight = w.Body("Weight", {0.0f, -1.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        // Rope to the world: the anchor (in the weight's space) is the hook 1 m above it.
        w.scene.GetRegistry().Emplace<Joint>(weight, Joint{.type = JointType::Distance, .anchor = {0.0f, 1.0f, 0.0f},
                                                            .minLimit = 0.0f, .maxLimit = 2.0f});
        const Entity a = w.Body("A", {5.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        const Entity b = w.Body("B", {6.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        w.scene.GetRegistry().Emplace<Joint>(a, Joint{.type = JointType::Fixed, .connectedBody = w.Uuid(b)});
        w.physics.Sync(w.scene);
        w.Run(1.5f);
        CHECK(Near(w.Position(weight).y, -2.0f, 0.05f));
        CHECK(glm::all(glm::epsilonEqual(w.Position(b) - w.Position(a), glm::vec3(1.0f, 0.0f, 0.0f), 0.02f)) && w.Position(a).y < -5.0f);
    }
    // Breaking: a fixed joint to the world cannot hold 10 kg with a 50 N limit.
    {
        World      w;
        const Entity box = w.Body("Heavy", {0.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.2f)), 10.0f);
        w.scene.GetRegistry().Emplace<Joint>(box, Joint{.type = JointType::Fixed, .breakForce = 50.0f});
        w.physics.Sync(w.scene);
        w.Run(0.5f);
        CHECK(w.broken.size() == 1 && w.broken[0].joint == box && w.broken[0].b == NullEntity);
        CHECK(!w.scene.GetRegistry().Get<Joint>(box).enabled && !w.physics.GetJointState(box).active);
        CHECK(w.Position(box).y < -0.3f);
    }
    // Connected bodies do not collide (no contact events) unless asked; 6DOF with a limited axis.
    {
        World      w;
        w.physics.settings.gravity = glm::vec3(0.0f);
        const Entity a = w.Body("A", {0.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Sphere(0.5f));
        const Entity b = w.Body("B", {0.3f, 0.0f, 0.0f}, BodyType::Dynamic, World::Sphere(0.5f));
        w.scene.GetRegistry().Emplace<Joint>(a, Joint{.type = JointType::Ball, .connectedBody = w.Uuid(b), .anchor = {0.15f, 0.0f, 0.0f}});
        w.physics.Sync(w.scene);
        w.Run(0.5f);
        CHECK(w.events.empty() && Near(glm::length(w.Position(b) - w.Position(a)), 0.3f, 0.02f));

        World      s;
        const Entity box = s.Body("Box", {0.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        Joint        six{.type = JointType::SixDof};
        six.axes    = {JointAxisMode::Locked, JointAxisMode::Limited, JointAxisMode::Locked,
                       JointAxisMode::Locked, JointAxisMode::Locked,  JointAxisMode::Locked};
        six.axisMin = {0.0f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f};
        six.axisMax = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        s.scene.GetRegistry().Emplace<Joint>(box, six);
        s.physics.Sync(s.scene);
        s.Run(1.0f);
        CHECK(Near(s.Position(box).y, -0.5f, 0.05f) && Near(s.Position(box).x, 0.0f, 0.01f));
    }
    // Waiting for a body; serialization; duplicates point into the copy.
    {
        World      w;
        const Entity a     = w.Body("A", {0.0f, 0.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
        const Entity other = w.scene.CreateEntity("Not yet a body");
        Joint        joint{.type = JointType::SwingTwist, .connectedBody = w.Uuid(other), .breakTorque = 5.0f};
        joint.motor = {.mode = JointMotorMode::Position, .target = 0.3f};
        w.scene.GetRegistry().Emplace<Joint>(a, joint);
        w.physics.Sync(w.scene);
        CHECK(w.physics.Stats().pendingJoints == 1 && !w.physics.GetJointState(a).active);
        w.scene.GetRegistry().Emplace<RigidBody>(other, RigidBody{.type = BodyType::Static});
        w.scene.GetRegistry().Emplace<Collider>(other, World::Box(glm::vec3(0.1f)));
        w.physics.Sync(w.scene);
        CHECK(w.physics.Stats().pendingJoints == 0 && w.physics.GetJointState(a).active);

        const std::string state = SnapshotEntityState(w.scene, a);
        w.scene.GetRegistry().Get<Joint>(a).minLimit = 0.0f;
        ApplyEntityState(w.scene, a, state);
        CHECK(w.scene.GetRegistry().Get<Joint>(a) == joint);

        const Entity parent = w.scene.CreateEntity("Pair");
        w.scene.SetParent(a, parent);
        w.scene.SetParent(other, parent);
        const std::vector<Entity> copies = RestoreEntities(w.scene, SnapshotEntities(w.scene, std::vector<Entity>{parent}),
                                                           RestoreMode::Duplicate);
        CHECK(copies.size() == 1);
        if (copies.size() == 1) {
            const auto& children = w.scene.GetRegistry().Get<Hierarchy>(copies[0]).children;
            CHECK(children.size() == 2);
            if (children.size() == 2) {
                const Joint& copied = w.scene.GetRegistry().Get<Joint>(children[0]);
                CHECK(copied.connectedBody == w.Uuid(children[1]) && copied.connectedBody != joint.connectedBody);
            }
        }
    }
}

TEST_CASE(Physics_CharacterRotationCrouchPlatformsSlopes)
{
    const auto makeWorld = [](World& w, CharacterController cc, glm::vec3 position = glm::vec3(0.0f)) {
        w.Body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({30.0f, 0.5f, 30.0f}));
        const Entity e = w.scene.CreateEntity("Character");
        w.scene.EditTransform(e).position = position;
        w.scene.GetRegistry().Emplace<CharacterController>(e, cc);
        w.physics.Sync(w.scene);
        return e;
    };
    const auto forward = [](World& w, Entity e) {
        return glm::vec3(w.scene.GetRegistry().Get<WorldTransform>(e).matrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    };
    {
        World      w;
        const Entity e = makeWorld(w, {.rotation = CharacterRotation::Movement, .turnSpeed = 0.0f});
        w.physics.SetCharacterInput(e, {2.0f, 0.0f, 0.0f}, false);
        w.Run(0.1f);
        CHECK(forward(w, e).x > 0.99f); // faces where it walks
        w.physics.SetCharacterInput(e, {0.0f, 0.0f, 0.0f}, false);
        w.Run(0.1f);
        CHECK(forward(w, e).x > 0.99f); // keeps its heading when it stops
    }
    {
        World      w;
        const Entity e = makeWorld(w, {.rotation = CharacterRotation::Script, .turnSpeed = 90.0f});
        w.physics.SetCharacterYaw(e, glm::radians(90.0f));
        w.Run(0.5f);
        const auto state = w.physics.GetCharacterState(e);
        CHECK(state && Near(glm::degrees(state->yaw), 45.0f, 4.0f));
        // Turned from outside (editor / script): it continues from there.
        w.scene.EditTransform(e).rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        w.physics.SetCharacterYaw(e, glm::radians(-90.0f));
        w.Run(0.2f);
        CHECK(Near(glm::degrees(w.physics.GetCharacterState(e)->yaw), -90.0f, 1.0f));
    }
    // Crouch; standing up waits for room above.
    {
        World        w;
        const Entity e = makeWorld(w, {.crouchHeight = 1.0f});
        w.physics.SetCharacterCrouch(e, true);
        w.Run(0.1f);
        CHECK(w.physics.GetCharacterState(e)->crouching);
        const Entity ceiling = w.Body("Ceiling", {0.0f, 1.5f, 0.0f}, BodyType::Static, World::Box({2.0f, 0.2f, 2.0f}));
        w.physics.SetCharacterCrouch(e, false);
        w.Run(0.2f);
        CHECK(w.physics.GetCharacterState(e)->crouching);
        w.scene.DestroyEntity(ceiling);
        w.Run(0.1f);
        CHECK(!w.physics.GetCharacterState(e)->crouching);
    }
    // A moving platform carries it along (or not).
    const auto ride = [&](bool platforms) {
        World      w;
        const Entity platform = w.Body("Platform", {0.0f, 0.75f, 0.0f}, BodyType::Kinematic, World::Box({2.0f, 0.25f, 2.0f}));
        const Entity e        = makeWorld(w, {.movingPlatforms = platforms}, {0.0f, 1.0f, 0.0f});
        w.Run(0.3f); // settle
        const float start = w.Position(e).x;
        for (int i = 0; i < 60; ++i) {
            w.scene.EditTransform(platform).position.x += 1.0f / 60.0f;
            w.physics.Step(w.scene, 1.0f / 60.0f);
        }
        return w.Position(e).x - start;
    };
    CHECK(Near(ride(true), 1.0f, 0.15f));
    CHECK(ride(false) < 0.3f);
    // Steep slopes: slide down or hold.
    const auto slope = [&](bool slide) {
        World      w;
        const Entity ramp = w.Body("Ramp", {0.0f, 0.0f, 0.0f}, BodyType::Static, World::Box({4.0f, 0.2f, 4.0f}));
        w.scene.EditTransform(ramp).rotation = glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        const Entity e = w.scene.CreateEntity("Character");
        w.scene.EditTransform(e).position = {0.0f, 0.7f, 0.0f};
        w.scene.GetRegistry().Emplace<CharacterController>(e, CharacterController{.slideOnSteepSlopes = slide});
        w.physics.Sync(w.scene);
        w.Run(0.4f);
        const glm::vec3 start = w.Position(e);
        w.Run(1.0f);
        return glm::length(w.Position(e) - start);
    };
    const float slid = slope(true), held = slope(false);
    CHECK(slid > held + 0.5f && held < 0.2f);
    // Pushing a dynamic box.
    {
        World      w;
        const Entity crate = w.Body("Crate", {1.2f, 0.5f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.5f)), 5.0f);
        const Entity e     = makeWorld(w, {.pushStrength = 200.0f});
        w.physics.SetCharacterInput(e, {2.0f, 0.0f, 0.0f}, false);
        w.Run(1.0f);
        CHECK(w.Position(crate).x > 1.5f);
    }
}

TEST_CASE(Physics_RagdollFromSkeleton)
{
    // A three-bone chain standing up: armature node 0, bones 1-3 one metre apart.
    Model model;
    model.nodes.resize(4);
    for (std::int32_t i = 1; i < 4; ++i) {
        model.nodes[static_cast<std::size_t>(i)].parent         = i - 1;
        model.nodes[static_cast<std::size_t>(i)].local.position = {0.0f, i == 1 ? 0.5f : 1.0f, 0.0f};
    }
    model.skins.push_back(Skin{.name = "Skin", .skeletonRoot = 1, .joints = {{1, glm::mat4(1.0f)}, {2, glm::mat4(1.0f)}, {3, glm::mat4(1.0f)}}});

    World      w;
    w.Body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({20.0f, 0.5f, 20.0f}));
    Registry&  r    = w.scene.GetRegistry();
    const Entity root = w.scene.CreateEntity("Model");
    r.Emplace<ModelInstance>(root);
    Entity parent = root;
    std::vector<Entity> nodes;
    for (std::int32_t i = 0; i < 4; ++i) {
        const Entity node = w.scene.CreateEntity(std::format("Node{}", i), parent);
        w.scene.EditTransform(node).position = model.nodes[static_cast<std::size_t>(i)].local.position;
        r.Emplace<ModelNodeRef>(node, ModelNodeRef{.node = static_cast<std::uint32_t>(i), .instanceRoot = root});
        nodes.push_back(node);
        parent = node;
    }
    CHECK(CreateRagdoll(w.scene, root, model, {.mass = 30.0f}) == 3);
    CHECK(RagdollBones(w.scene, root).size() == 3 && r.Has<Ragdoll>(root) && !r.Has<RigidBody>(nodes[0]));
    CHECK(!r.Has<Joint>(nodes[1]) && r.Has<Joint>(nodes[2]) && r.Get<Joint>(nodes[3]).connectedBody == w.Uuid(nodes[2]));
    float mass = 0.0f;
    for (const Entity bone : RagdollBones(w.scene, root))
        mass += r.Get<RigidBody>(bone).mass;
    CHECK(Near(mass, 30.0f, 0.01f) && r.Get<Collider>(nodes[1]).shape == ColliderShape::Capsule);

    w.physics.Sync(w.scene);
    CHECK(w.physics.Activity(nodes[2]) == BodyActivity::Kinematic && w.physics.Stats().joints == 2);
    SetRagdollSimulating(w.scene, root, true);
    w.Run(2.0f);
    CHECK(w.physics.Activity(nodes[2]) != BodyActivity::Kinematic && w.Position(nodes[3]).y < 2.0f); // collapsed
    CHECK(Near(glm::length(w.Position(nodes[3]) - w.Position(nodes[2])), 1.0f, 0.1f));               // joints held
    SetRagdollSimulating(w.scene, root, false);
    w.physics.Sync(w.scene);
    CHECK(w.physics.Activity(nodes[2]) == BodyActivity::Kinematic);

    // Saved and restored with the scene state; removing leaves the skeleton.
    const std::string state = SnapshotEntityState(w.scene, nodes[2]);
    CHECK(state.find("ragdollBone") != std::string::npos && state.find("swingTwist") != std::string::npos);
    RemoveRagdoll(w.scene, root);
    CHECK(RagdollBones(w.scene, root).empty() && !r.Has<Ragdoll>(root) && !r.Has<Collider>(nodes[2]));
}

TEST_CASE(Physics_SceneFilesKeepMaterialsRelative)
{
    const fs::path dir = TempDir("physscene");
    CHECK(SavePhysicsMaterial(dir / "Materials" / "Metal.uphysmat", {.friction = 0.3f, .surface = "Metal"}));
    Scene        scene;
    const Entity e = scene.CreateEntity("Plate");
    Collider     c;
    c.material                 = PathToUtf8(dir / "Materials" / "Metal.uphysmat");
    c.meshMaterials["Painted"] = PathToUtf8(dir / "Materials" / "Metal.uphysmat");
    c.rotation                 = glm::angleAxis(0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
    scene.GetRegistry().Emplace<Collider>(e, c);
    scene.GetRegistry().Emplace<CharacterController>(e, CharacterController{.rotation = CharacterRotation::Camera, .crouchHeight = 0.9f});
    fs::create_directories(dir / "Scenes");
    SaveSceneFile(dir / "Scenes" / "Test.scene.json", scene, nullptr);
    std::ifstream                in(dir / "Scenes" / "Test.scene.json");
    const nlohmann::json         root = nlohmann::json::parse(in);
    const nlohmann::json&        saved = root["entities"][0]["collider"];
    CHECK(saved["material"] == "../Materials/Metal.uphysmat" && saved["meshMaterials"]["Painted"] == "../Materials/Metal.uphysmat");
    Scene loaded;
    (void)LoadSceneFile(dir / "Scenes" / "Test.scene.json", loaded, nullptr);
    Entity found = NullEntity;
    loaded.GetRegistry().ViewOf<Collider>().Each([&](Entity x, Collider&) { found = x; });
    CHECK(found != NullEntity);
    if (found != NullEntity) {
        const Collider& back = loaded.GetRegistry().Get<Collider>(found);
        CHECK(fs::equivalent(PathFromUtf8(back.material), dir / "Materials" / "Metal.uphysmat"));
        CHECK(Near(glm::dot(back.rotation, c.rotation), 1.0f, 1e-4f));
        CHECK(loaded.GetRegistry().Get<CharacterController>(found).rotation == CharacterRotation::Camera);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Blueprint_PhysicsToolsNodes)
{
    using namespace BlueprintTest;
    const fs::path dir   = TempDir("physbp");
    const auto     stone = dir / "Stone.uphysmat";
    CHECK(SavePhysicsMaterial(stone, {.friction = 0.8f, .surface = "Stone"}));

    ThreadPool   pool{2};
    EventBus     bus;
    Scene        scene;
    PhysicsWorld physics{pool, bus};
    ScriptSystem scripts{bus, nullptr, &physics, nullptr};
    const auto   body = [&](const char* name, glm::vec3 p, BodyType type, Collider c) {
        const Entity e = scene.CreateEntity(name);
        scene.EditTransform(e).position = p;
        scene.GetRegistry().Emplace<RigidBody>(e, RigidBody{.type = type});
        scene.GetRegistry().Emplace<Collider>(e, c);
        return e;
    };
    body("Floor", {0.0f, -0.5f, 0.0f}, BodyType::Static, World::Box({20.0f, 0.5f, 20.0f}, PathToUtf8(stone)));
    const Entity ball = body("Ball", {0.0f, 2.0f, 0.0f}, BodyType::Dynamic, World::Sphere(0.5f));

    // Hit (min impulse 2: once; min 100: never) printing the other surface; Collision Stay sets a flag.
    Graph g;
    const auto hit    = g.Node("Event.Hit", "2");
    const auto hard   = g.Node("Event.Hit", "100");
    const auto stay   = g.Node("Event.CollisionStay");
    const auto pHit   = Print(g); // Print: messages stay on screen (and in Messages) for long
    const auto pHard  = Print(g, "hard");
    const auto pStay  = Print(g, "stay");
    g.Link(hit, "Out", pHit, "In");
    g.Link(hit, "Other Surface", pHit, "Text");

    g.Link(hard, "Out", pHard, "In");
    const auto once   = g.Node("Flow.DoOnce"); // Stay fires every step: print once (64 messages are kept)
    g.Link(stay, "Out", once, "In");
    g.Link(once, "Completed", pStay, "In");
    // Raycast surface below on BeginPlay.
    const auto begin = g.Node("Event.BeginPlay");
    const auto ray   = g.Node("Physics.Raycast");
    g.Set(ray, "Start", glm::vec3(3.0f, 5.0f, 0.0f));
    const auto pRay = Print(g);
    g.Link(begin, "Out", ray, "In");
    g.Link(ray, "Then", pRay, "In");
    g.Link(ray, "Surface", pRay, "Text");
    CHECK(g.Valid());
    scene.GetRegistry().Emplace<ScriptComponent>(ball, ScriptComponent{"ball.ugraph"});
    scripts.Provide("ball.ugraph", g.g);

    // A hinge with a motor driven by a Blueprint; a character turned and crouched by one.
    const Entity wheel = body("Wheel", {5.0f, 3.0f, 0.0f}, BodyType::Dynamic, World::Box(glm::vec3(0.1f)));
    scene.GetRegistry().Get<RigidBody>(wheel).gravityFactor = 0.0f;
    scene.GetRegistry().Emplace<Joint>(wheel, Joint{.type = JointType::Hinge, .anchor = {-1.0f, 0.0f, 0.0f},
                                                    .anchorRotation = glm::angleAxis(-1.5707963f, glm::vec3(0.0f, 1.0f, 0.0f)),
                                                    .limits = false});
    const Entity character = scene.CreateEntity("Character");
    scene.EditTransform(character).position = {-5.0f, 0.0f, 0.0f};
    scene.GetRegistry().Emplace<CharacterController>(character, CharacterController{.rotation = CharacterRotation::Script, .turnSpeed = 0.0f});
    Graph wg; // on the wheel: its own joint (Target unconnected = self)
    const auto wBegin = wg.Node("Event.BeginPlay");
    const auto motor  = wg.Node("Joint.SetMotor", "velocity");
    wg.Set(motor, "Value", 90.0f); // deg/s
    wg.Set(motor, "Max Force", 1e5f);
    wg.Link(wBegin, "Out", motor, "In");
    CHECK(wg.Valid());
    scene.GetRegistry().Emplace<ScriptComponent>(wheel, ScriptComponent{"wheel.ugraph"});
    scripts.Provide("wheel.ugraph", wg.g);
    Graph cg; // on the character
    const auto cBegin = cg.Node("Event.BeginPlay");
    const auto yaw    = cg.Node("Physics.CharacterYaw");
    cg.Set(yaw, "Yaw", 90.0f);
    const auto crouch = cg.Node("Physics.CharacterCrouch");
    cg.Set(crouch, "Crouch", true);
    cg.Link(cBegin, "Out", yaw, "In");
    cg.Link(yaw, "Then", crouch, "In");
    CHECK(cg.Valid());
    scene.GetRegistry().Emplace<ScriptComponent>(character, ScriptComponent{"character.ugraph"});
    scripts.Provide("character.ugraph", cg.g);

    physics.Sync(scene);
    scripts.Begin(scene);
    for (int i = 0; i < 120; ++i) {
        physics.Step(scene, 1.0f / 60.0f);
        scripts.Update(scene, 1.0f / 60.0f);
    }
    const auto count = [&](std::string_view text) {
        return std::ranges::count_if(scripts.Messages(), [&](const ScriptMessage& m) { return m.text == text; });
    };
    CHECK(count("Stone") == 2); // Hit once (other surface) + the raycast below
    CHECK(count("hard") == 0 && count("stay") > 0);
    CHECK(std::abs(physics.GetJointState(wheel).angle) > glm::radians(45.0f));
    const auto state = physics.GetCharacterState(character);
    CHECK(state && state->crouching && Near(glm::degrees(state->yaw), 90.0f, 1.0f));
    scripts.End(scene);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
