// Lifecycle audits: randomized sequences of scene, prefab and script operations that must keep the
// scene consistent (hierarchy, UUIDs, transforms) and never touch destroyed entities. Run under
// ASan/UBSan to catch use-after-free; the checks catch logical corruption.
#include "Test.h"

#include "Engine/Events/EventBus.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptSystem.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

std::vector<Entity> AliveEntities(const Scene& scene)
{
    std::vector<Entity> alive;
    scene.GetRegistry().ViewOf<Hierarchy>().Each([&](Entity e, const Hierarchy&) { alive.push_back(e); });
    std::ranges::sort(alive, {}, [](Entity e) { return static_cast<std::uint64_t>(e); });
    return alive;
}

// Hierarchy links are symmetric and point at live entities, UUIDs are unique and found again,
// world transforms match a full recompute. Returns the number of problems (printed).
int CheckSceneInvariants(Scene& scene, const char* where)
{
    int             problems = 0;
    const Registry& r        = scene.GetRegistry();
    const auto      report   = [&](const std::string& what) {
        if (problems++ < 5)
            std::printf("    [%s] %s\n", where, what.c_str());
    };
    std::unordered_set<std::uint64_t> uuids;
    for (const Entity e : AliveEntities(scene)) {
        const Hierarchy& h = r.Get<Hierarchy>(e);
        if (!r.Has<Uuid>(e) || !r.Has<Transform>(e) || !r.Has<WorldTransform>(e) || !r.Has<Name>(e))
            report("entity without its base components");
        if (const Uuid* id = r.TryGet<Uuid>(e)) {
            if (id->value == 0 || !uuids.insert(id->value).second)
                report(std::format("uuid {} zero or duplicated", id->value));
            if (scene.FindByUuid(id->value) != e)
                report(std::format("FindByUuid({}) does not find its entity", id->value));
        }
        if (h.parent != NullEntity) {
            if (!r.Valid(h.parent))
                report("parent is not alive");
            else if (std::ranges::count(r.Get<Hierarchy>(h.parent).children, e) != 1)
                report("parent does not list the child exactly once");
            if (scene.IsAncestor(e, h.parent))
                report("cycle");
        }
        std::unordered_set<std::uint64_t> seen;
        for (const Entity c : h.children) {
            if (!r.Valid(c))
                report("child is not alive");
            else if (r.Get<Hierarchy>(c).parent != e)
                report("child names another parent");
            if (!seen.insert(static_cast<std::uint64_t>(c)).second)
                report("child listed twice");
        }
    }
    scene.UpdateTransforms();
    if (const std::size_t stale = scene.CountStaleTransforms(); stale != 0)
        report(std::format("{} stale world transforms", stale));
    return problems;
}

Entity Pick(std::mt19937& rng, const std::vector<Entity>& entities)
{
    if (entities.empty())
        return NullEntity;
    return entities[std::uniform_int_distribution<std::size_t>(0, entities.size() - 1)(rng)];
}

} // namespace

TEST_CASE(Lifecycle_SceneHierarchyFuzz)
{
    // Create / reparent (cycles refused) / destroy subtrees / edit transforms / snapshot + restore
    // (original and duplicate) / clear, in random order.
    std::mt19937 rng(1234);
    Scene        scene;
    int          problems = 0;
    std::string  snapshot;
    std::vector<std::uint64_t> snapshotUuids;
    for (int step = 0; step < 4000 && problems == 0; ++step) {
        const std::vector<Entity> alive = AliveEntities(scene);
        const int                 op    = std::uniform_int_distribution<int>(0, 99)(rng);
        if (op < 30 || alive.size() < 4) {
            const Entity parent = op % 2 == 0 ? Pick(rng, alive) : NullEntity;
            const Entity e      = scene.CreateEntity(std::format("E{}", step), parent);
            scene.EditTransform(e).position = glm::vec3(static_cast<float>(step % 7), 1.0f, 0.0f);
        } else if (op < 50) {
            const Entity child = Pick(rng, alive), parent = op % 5 == 0 ? NullEntity : Pick(rng, alive);
            const bool   cycle = parent != NullEntity && (parent == child || scene.IsAncestor(child, parent));
            const bool   ok    = scene.SetParent(child, parent, std::uniform_int_distribution<std::size_t>(0, 3)(rng));
            if (ok == cycle) {
                std::printf("    SetParent returned %d for a %s\n", ok, cycle ? "cycle" : "valid parent");
                ++problems;
            }
        } else if (op < 65) {
            scene.DestroyEntity(Pick(rng, alive));
        } else if (op < 80) {
            const Entity e = Pick(rng, alive);
            scene.EditTransform(e).rotation = glm::angleAxis(0.1f * static_cast<float>(step), glm::vec3(0.0f, 1.0f, 0.0f));
            scene.EditTransform(e).scale    = glm::vec3(1.0f + 0.01f * static_cast<float>(step % 10));
        } else if (op < 88) {
            // Snapshot some roots (with their subtrees).
            std::vector<Entity> roots;
            for (const Entity e : alive)
                if (scene.GetRegistry().Get<Hierarchy>(e).parent == NullEntity && roots.size() < 3)
                    roots.push_back(e);
            snapshot = SnapshotEntities(scene, roots);
            snapshotUuids.clear();
            for (const Entity e : roots)
                snapshotUuids.push_back(scene.GetRegistry().Get<Uuid>(e).value);
        } else if (op < 94 && !snapshot.empty()) {
            // Undo-like: the snapshotted roots are destroyed, then restored with the same UUIDs.
            for (const std::uint64_t uuid : snapshotUuids)
                scene.DestroyEntity(scene.FindByUuid(uuid));
            const auto restored = RestoreEntities(scene, snapshot, RestoreMode::Original);
            if (restored.size() != snapshotUuids.size()) {
                std::printf("    restored %zu of %zu roots\n", restored.size(), snapshotUuids.size());
                ++problems;
            }
        } else if (op < 98 && !snapshot.empty()) {
            (void)RestoreEntities(scene, snapshot, RestoreMode::Duplicate);
        } else {
            scene.Clear();
            snapshot.clear();
        }
        if (step % 50 == 0 || op >= 80)
            problems += CheckSceneInvariants(scene, std::format("step {}", step).c_str());
        (void)scene.TakeChanges();
    }
    problems += CheckSceneInvariants(scene, "end");
    CHECK(problems == 0);
}

TEST_CASE(Lifecycle_PrefabInstancesFuzz)
{
    // Prefab instances under random edits: members destroyed / re-parented / renamed, overrides
    // reverted, instances duplicated, unlinked and destroyed; the prefab file rewritten.
    const fs::path dir = fs::temp_directory_path() / std::format("ungine_lifecycle_prefab_{}", std::random_device{}());
    fs::create_directories(dir);
    const fs::path file = dir / "Thing.uprefab";

    Scene scene;
    {
        const Entity root  = scene.CreateEntity("Thing");
        const Entity arm   = scene.CreateEntity("Arm", root);
        const Entity hand  = scene.CreateEntity("Hand", arm);
        scene.GetRegistry().Emplace<Tags>(hand, Tags{.values = {"grab"}});
        scene.EditTransform(arm).position = {1.0f, 0.0f, 0.0f};
        CreatePrefab(file, scene, nullptr, root);
        scene.DestroyEntity(root);
    }
    std::vector<ModelHandle> models; // none (no assets), required by the API
    std::mt19937             rng(99);
    int                      problems = 0;
    // Instantiate / duplicate copy whole subtrees and Apply adds an instance's extra children to
    // the prefab (so every instance grows): unbounded, the scene grows exponentially within a few
    // hundred steps (how fast depends on prefab file mtimes, i.e. timing). Past the cap the scene is
    // cleared instead, and only small instances are applied.
    constexpr std::size_t kMaxEntities  = 300;
    constexpr std::size_t kMaxApplied   = 24;
    const auto            subtreeSize   = [&](Entity root) {
        std::size_t         count = 0;
        std::vector<Entity> stack{root};
        while (!stack.empty()) {
            const Entity e = stack.back();
            stack.pop_back();
            ++count;
            for (const Entity child : scene.GetRegistry().Get<Hierarchy>(e).children)
                stack.push_back(child);
        }
        return count;
    };
    for (int step = 0; step < 1500 && problems == 0; ++step) {
        const std::vector<Entity> alive = AliveEntities(scene);
        std::vector<Entity>       roots;
        for (const Entity e : alive)
            if (scene.GetRegistry().Has<PrefabInstance>(e))
                roots.push_back(e);
        int op = std::uniform_int_distribution<int>(0, 99)(rng);
        if (alive.size() > kMaxEntities)
            op = 99; // clear
        try {
            if (op < 20 || roots.empty()) {
                const Entity parent = op % 3 == 0 ? Pick(rng, alive) : NullEntity;
                (void)InstantiatePrefab(scene, nullptr, file, parent, Transform{.position = {0.0f, static_cast<float>(step % 5), 0.0f}}, models);
            } else if (op < 35) {
                const Entity e = Pick(rng, alive);
                scene.GetRegistry().Get<Name>(e).value = std::format("Renamed{}", step);
                scene.EditTransform(e).position.x += 0.5f;
            } else if (op < 45) {
                scene.DestroyEntity(Pick(rng, alive)); // a member, a root or anything else
            } else if (op < 55) {
                const Entity child = Pick(rng, alive), parent = Pick(rng, alive);
                (void)scene.SetParent(child, parent);
            } else if (op < 65) {
                // Reverting the whole instance rebuilds it: re-parented members come back under it.
                const Entity root = Pick(rng, roots);
                RevertPrefabOverrides(scene, nullptr, root, {}, models);
                const std::uint64_t               uuid = scene.GetRegistry().Get<Uuid>(root).value;
                std::unordered_set<std::uint64_t> sources;
                scene.GetRegistry().ViewOf<PrefabLink>().Each([&](Entity e, const PrefabLink& link) {
                    if (link.instance != uuid)
                        return;
                    if (e != root && !scene.IsAncestor(root, e) && problems++ < 5)
                        std::printf("    step %d: reverted member outside its instance\n", step);
                    if (!sources.insert(link.source).second && problems++ < 5)
                        std::printf("    step %d: two members for one prefab entity\n", step);
                });
            } else if (op < 72) {
                const Entity e = Pick(rng, alive);
                if (const Entity root = PrefabInstanceRoot(scene, e); root != NullEntity)
                    RevertPrefabOverrides(scene, nullptr, e, {}, models);
            } else if (op < 80) {
                const Entity root = Pick(rng, roots);
                const std::string snapshot = SnapshotEntities(scene, std::span<const Entity>(&root, 1));
                (void)RestoreEntities(scene, snapshot, RestoreMode::Duplicate);
            } else if (op < 85) {
                UnlinkPrefabInstance(scene, Pick(rng, roots));
            } else if (op < 90) {
                // The prefab file changes (another instance applied): every instance rebuilds.
                if (const Entity root = Pick(rng, roots); subtreeSize(root) <= kMaxApplied)
                    ApplyPrefabInstance(scene, nullptr, root, models);
            } else if (op < 95) {
                (void)RefreshPrefabInstances(scene, nullptr, models);
            } else {
                scene.Clear();
            }
        } catch (const std::exception& e) {
            std::printf("    step %d (op %d): %s\n", step, op, e.what());
            ++problems;
        }
        problems += CheckSceneInvariants(scene, std::format("prefab step {} op {}", step, op).c_str());
    }
    CHECK(problems == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

namespace {
// Graph helper: nodes by type, defaults by pin name, links that must connect.
struct GraphBuilder {
    ScriptGraph graph;
    int         failures = 0;

    std::uint32_t Node(const char* type, std::string param = {}) { return graph.AddNode(type, {}, std::move(param)); }
    void          Set(std::uint32_t node, const char* pin, ScriptValue value) { graph.FindNode(node)->defaults[pin] = std::move(value); }
    void          Link(std::uint32_t from, const char* fromPin, std::uint32_t to, const char* toPin)
    {
        if (const std::string error = graph.Connect(from, fromPin, to, toPin); !error.empty()) {
            std::printf("    connect %s -> %s: %s\n", fromPin, toPin, error.c_str());
            ++failures;
        }
    }
};
} // namespace

TEST_CASE(Lifecycle_ScriptsDestroyAndSpawnWhileRunning)
{
    // Every actor: a looping timer calls "Hit" on another actor (which destroys itself inside the
    // call), spawns entities, waits on Delays; EndPlay calls into other actors. Meanwhile the test
    // destroys, re-parents and adds actors from outside and restarts play. Nothing may run on a
    // destroyed entity, crash or leave a broken scene.
    GraphBuilder b;
    const auto   begin = b.Node("Event.BeginPlay");
    const auto   timer = b.Node("Timer.Set");
    b.Set(timer, "Event", std::string("Pulse"));
    b.Set(timer, "Time", 0.05f);
    b.Set(timer, "Looping", true);
    const auto delay = b.Node("Flow.Delay");
    b.Set(delay, "Duration", 0.2f);
    const auto spawnLate = b.Node("Entity.SpawnEmpty");
    b.Set(spawnLate, "Name", std::string("Late"));
    b.Link(begin, "Out", timer, "In");
    b.Link(timer, "Then", delay, "In");
    b.Link(delay, "Completed", spawnLate, "In");

    const auto pulse  = b.Node("Event.Custom", "Pulse");
    const auto other  = b.Node("Entity.FindByName");
    b.Set(other, "Name", std::string("Actor"));
    const auto call = b.Node("Script.CallEvent");
    b.Set(call, "Event", std::string("Hit"));
    const auto spawn = b.Node("Entity.SpawnEmpty");
    b.Set(spawn, "Name", std::string("Spawn"));
    const auto findSpawn = b.Node("Entity.FindByName");
    b.Set(findSpawn, "Name", std::string("Spawn"));
    const auto killSpawn = b.Node("Entity.Destroy");
    b.Link(pulse, "Out", call, "In");
    b.Link(other, "Entity", call, "Target");
    b.Link(call, "Then", spawn, "In");
    b.Link(spawn, "Then", killSpawn, "In");
    b.Link(findSpawn, "Entity", killSpawn, "Target");

    const auto hit     = b.Node("Event.Custom", "Hit");
    const auto suicide = b.Node("Entity.Destroy"); // Target unconnected: self
    b.Link(hit, "Out", suicide, "In");

    const auto end      = b.Node("Event.EndPlay");
    const auto endOther = b.Node("Entity.FindByName");
    b.Set(endOther, "Name", std::string("Actor"));
    const auto endCall = b.Node("Script.CallEvent");
    b.Set(endCall, "Event", std::string("Hit"));
    b.Link(end, "Out", endCall, "In");
    b.Link(endOther, "Entity", endCall, "Target");
    CHECK(b.failures == 0);

    EventBus     bus;
    Scene        scene;
    ScriptSystem scripts(bus);
    scripts.Provide("actor.ugraph", b.graph);
    const auto addActor = [&](Entity parent) {
        const Entity e = scene.CreateEntity("Actor", parent);
        scene.GetRegistry().Emplace<ScriptComponent>(e, ScriptComponent{"actor.ugraph"});
        return e;
    };
    for (int i = 0; i < 12; ++i)
        addActor(NullEntity);
    scripts.Begin(scene);

    std::mt19937 rng(7);
    int          problems = 0;
    for (int frame = 0; frame < 1200 && problems == 0; ++frame) {
        scripts.Update(scene, 1.0f / 60.0f);
        // After an update, running instances belong to live entities.
        if (scripts.Stats().instances > AliveEntities(scene).size() && problems++ < 5)
            std::printf("    frame %d: %u instances for %zu entities\n", frame, scripts.Stats().instances,
                        AliveEntities(scene).size());
        const std::vector<Entity> alive = AliveEntities(scene);
        const int                 op    = std::uniform_int_distribution<int>(0, 99)(rng);
        if (op < 15)
            addActor(op % 2 ? Pick(rng, alive) : NullEntity); // started on the next update
        else if (op < 25)
            scene.DestroyEntity(Pick(rng, alive));
        else if (op < 32)
            (void)scene.SetParent(Pick(rng, alive), Pick(rng, alive));
        else if (op < 33) {
            scripts.End(scene); // EndPlay calls into actors that are being ended
            scripts.Begin(scene);
        } else if (op < 34) {
            scripts.End(scene);
            scene.Clear();
            for (int i = 0; i < 8; ++i)
                addActor(NullEntity);
            scripts.Begin(scene);
        }
        if (frame % 20 == 0)
            problems += CheckSceneInvariants(scene, std::format("frame {}", frame).c_str());
    }
    scripts.End(scene);
    problems += CheckSceneInvariants(scene, "end");
    CHECK(problems == 0);
    CHECK(scripts.Stats().instances == 0);
}

TEST_CASE(Lifecycle_PrefabMemberMovedOutSurvivesSave)
{
    // A member dragged out of its instance becomes an ordinary entity; reverting the instance
    // brings the prefab entity back, and both must survive saving even after the detached one is
    // moved back below the instance (it used to be skipped as a "member").
    const fs::path dir = fs::temp_directory_path() / std::format("ungine_lifecycle_detach_{}", std::random_device{}());
    fs::create_directories(dir);
    std::vector<ModelHandle> models;
    Scene                    scene;
    Registry&                r = scene.GetRegistry();
    {
        const Entity root = scene.CreateEntity("Thing");
        scene.CreateEntity("Arm", root);
        CreatePrefab(dir / "Thing.uprefab", scene, nullptr, root);
        scene.DestroyEntity(root);
    }
    const Entity instance = InstantiatePrefab(scene, nullptr, dir / "Thing.uprefab", NullEntity, Transform{}, models);
    const auto   named    = [&](const char* name) {
        std::vector<Entity> found;
        r.ViewOf<Name>().Each([&](Entity e, const Name& n) {
            if (n.value == name)
                found.push_back(e);
        });
        return found;
    };
    CHECK(named("Arm").size() == 1);
    if (named("Arm").size() != 1)
        return;
    const Entity detached = named("Arm").front();
    CHECK(scene.SetParent(detached, NullEntity));
    CHECK(PrefabInstanceRoot(scene, detached) == NullEntity);
    RevertPrefabOverrides(scene, nullptr, instance, {}, models); // the instance gets its Arm back
    CHECK(named("Arm").size() == 2);
    CHECK(!r.Has<PrefabLink>(detached)); // an ordinary entity now
    CHECK(scene.SetParent(detached, instance)); // back below the instance, still not a member

    SaveSceneFile(dir / "Level.scene.json", scene, nullptr);
    Scene loaded;
    (void)LoadSceneFile(dir / "Level.scene.json", loaded, static_cast<AssetManager*>(nullptr));
    std::size_t arms = 0;
    loaded.GetRegistry().ViewOf<Name>().Each([&](Entity, const Name& n) {
        if (n.value == "Arm")
            ++arms;
    });
    CHECK(arms == 2);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Lifecycle_DestroyedEntityGetsNoMoreEvents)
{
    // X and Y each destroy the other on Tick: whichever ticks first wins, the other must not tick
    // (or fire anything else) in the same update.
    EventBus     bus;
    Scene        scene;
    ScriptSystem scripts(bus);
    for (const auto& [self, other] : {std::pair{"X", "Y"}, std::pair{"Y", "X"}}) {
        GraphBuilder b;
        const auto   tick  = b.Node("Event.Tick");
        const auto   print = b.Node("Debug.Print");
        b.Set(print, "Text", std::string("tick"));
        b.Set(print, "Duration", 100.0f);
        const auto find = b.Node("Entity.FindByName");
        b.Set(find, "Name", std::string(other));
        const auto destroy = b.Node("Entity.Destroy");
        b.Link(tick, "Out", print, "In");
        b.Link(print, "Then", destroy, "In");
        b.Link(find, "Entity", destroy, "Target");
        CHECK(b.failures == 0);
        const std::string file = std::string(self) + ".ugraph";
        scripts.Provide(file, b.graph);
        const Entity e = scene.CreateEntity(self);
        scene.GetRegistry().Emplace<ScriptComponent>(e, ScriptComponent{file});
    }
    scripts.Begin(scene);
    scripts.Update(scene, 1.0f / 60.0f);
    const auto messages = scripts.Messages();
    CHECK(std::ranges::count_if(messages, [](const ScriptMessage& m) { return m.text == "tick"; }) == 1);
    CHECK(std::ranges::none_of(messages, [](const ScriptMessage& m) { return m.error; }));
    CHECK(scripts.Stats().instances == 1);
    scripts.End(scene);
}
