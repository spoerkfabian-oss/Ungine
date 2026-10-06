// Level streaming (CPU): scene files prepared off the main thread, sub-levels loaded and unloaded
// by requests and streaming volumes, scripts ending with their level, references into levels.
#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Scene/LevelStreaming.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptSystem.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / std::format("ungine_{}_{}", name, std::random_device{}());
    fs::create_directories(dir);
    return dir;
}

void WriteText(const fs::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; }

// A level file: `count` root entities "<prefix>0..", the first with a child.
void WriteLevel(const fs::path& file, const char* prefix, int count, std::uint64_t firstUuid)
{
    Scene scene;
    for (int i = 0; i < count; ++i) {
        const Entity e = scene.CreateEntity(std::format("{}{}", prefix, i), NullEntity, firstUuid + static_cast<std::uint64_t>(i));
        if (i == 0)
            scene.CreateEntity(std::format("{}Child", prefix), e, firstUuid + 100);
    }
    SaveSceneFile(file, scene, nullptr);
}

std::size_t CountNamed(const Scene& scene, const std::string& prefix)
{
    std::size_t n = 0;
    scene.GetRegistry().ViewOf<Name>().Each([&](Entity, const Name& name) { n += name.value.starts_with(prefix) ? 1 : 0; });
    return n;
}

// Updates until `done` (the parse runs on the pool) or 5 s.
bool Pump(LevelStreamer& streamer, Scene& scene, const std::function<bool()>& done,
          std::optional<glm::vec3> camera = std::nullopt)
{
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        streamer.Update(scene, camera);
        scene.UpdateTransforms();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE(Streaming_PrepareAndInstantiateSceneFile)
{
    // Parsing happens without Scene / AssetManager (any thread); instantiating adds the entities
    // (model preloading: GPU test Streaming_LevelModelsAndEditor).
    const fs::path dir = TempDir("prepare");
    fs::create_directories(dir / "Prefabs");
    WriteText(dir / "Prefabs" / "Crate.uprefab", R"({"version": 1, "prefab": true, "root": 1, "entities": [
        {"uuid": 1, "parent": 0, "name": "Crate", "mesh": {"model": {"file": "../Models/crate.glb"}, "index": 0}}]})");
    WriteText(dir / "Level.scene.json", R"({"version": 1, "entities": [
        {"uuid": 10, "parent": 0, "name": "Floor", "mesh": {"model": {"primitive": {"shape": "plane", "size": 4}}, "index": 0}},
        {"uuid": 11, "parent": 0, "name": "Statue", "modelInstance": {"model": {"file": "Models/statue.glb"}}},
        {"uuid": 12, "parent": 11, "name": "Base"},
        {"uuid": 13, "parent": 0, "name": "Crate", "prefab": {"file": "Prefabs/Crate.uprefab", "members": [],
            "overrides": {"1": {"mesh": {"model": {"file": "Models/other.glb"}}}}}}]})");
    const auto prepared = PrepareSceneFile(dir / "Level.scene.json");
    CHECK(prepared != nullptr);
    CHECK(PreparedSceneFile(*prepared) == dir / "Level.scene.json");

    // Without an asset manager nothing is acquired; the entities still come in (models skipped).
    Scene       scene;
    SceneModels models;
    const auto  roots = InstantiatePreparedScene(*prepared, scene, nullptr, models);
    CHECK(roots.size() == 3); // Floor, Statue, Crate (Base is a child)
    CHECK(models.empty());
    CHECK(scene.FindByUuid(12) != NullEntity && scene.GetRegistry().Get<Hierarchy>(scene.FindByUuid(12)).parent == scene.FindByUuid(11));

    // Broken files fail in the preparation, with the file named.
    WriteText(dir / "Broken.scene.json", "{ not json");
    bool threw = false;
    try {
        (void)PrepareSceneFile(dir / "Broken.scene.json");
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("Broken.scene.json") != std::string::npos;
    }
    CHECK(threw);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Streaming_VolumesRequestsAndHysteresis)
{
    const fs::path dir = TempDir("streaming");
    WriteLevel(dir / "A.scene.json", "A", 2, 1000);
    WriteLevel(dir / "B.scene.json", "B", 1, 2000);

    ThreadPool jobs(2);
    EventBus   bus;
    std::vector<LevelStreamedEvent> events;
    Subscription sub = bus.Subscribe<LevelStreamedEvent>([&](const LevelStreamedEvent& e) {
        events.push_back(e);
        return false;
    });
    Scene          scene;
    LevelStreamer  streamer(jobs, nullptr, &bus);
    std::vector<std::size_t> hooked;
    streamer.SetUnloadHook([&](Scene&, std::span<const Entity> roots) { hooked.push_back(roots.size()); });

    // A volume (box 5, load margin 1, unload margin 2) around the origin streams A for the player.
    const Entity volume = scene.CreateEntity("Volume");
    scene.GetRegistry().Emplace<LevelStreamingVolume>(volume, LevelStreamingVolume{.level = PathToUtf8(dir / "A.scene.json"),
                                                                                  .halfExtents = glm::vec3(5.0f),
                                                                                  .loadMargin = 1.0f,
                                                                                  .unloadMargin = 2.0f});
    const Entity player = scene.CreateEntity("Player");
    scene.GetRegistry().Emplace<StreamingSource>(player);
    const auto place = [&](float x) {
        scene.EditTransform(player).position = {x, 0.0f, 0.0f};
        scene.UpdateTransforms();
    };
    place(20.0f);
    streamer.Update(scene);
    CHECK(streamer.State(dir / "A.scene.json") == LevelState::Unloaded);

    place(5.5f); // inside the load margin
    CHECK(Pump(streamer, scene, [&] { return streamer.IsLoaded(dir / "A.scene.json"); }));
    CHECK(CountNamed(scene, "A") == 3); // A0, A1, AChild
    CHECK(events.size() == 1 && events.back().loaded && events.back().level.find("A.scene.json") != std::string::npos);
    std::size_t streamedRoots = 0;
    scene.GetRegistry().ViewOf<StreamedLevel>().Each([&](Entity, const StreamedLevel&) { ++streamedRoots; });
    CHECK(streamedRoots == 2);

    // The level is not saved with the persistent scene.
    SaveSceneFile(dir / "Persistent.scene.json", scene, nullptr);
    {
        Scene check;
        (void)LoadSceneFile(dir / "Persistent.scene.json", check, static_cast<AssetManager*>(nullptr));
        CHECK(CountNamed(check, "A") == 0 && check.FindByUuid(scene.GetRegistry().Get<Uuid>(volume).value) != NullEntity);
    }

    // Hysteresis: 7.5 is outside the load box (6) but inside the keep box (8).
    place(7.5f);
    for (int i = 0; i < 5; ++i)
        streamer.Update(scene);
    CHECK(streamer.IsLoaded(dir / "A.scene.json"));
    place(9.0f);
    streamer.Update(scene);
    CHECK(streamer.State(dir / "A.scene.json") == LevelState::Unloaded && CountNamed(scene, "A") == 0);
    CHECK(hooked.size() == 1 && hooked.back() == 2);
    CHECK(events.size() == 2 && !events.back().loaded && !events.back().failed);
    // Back at 7.5 (not in the load box): it stays unloaded.
    place(7.5f);
    streamer.Update(scene);
    CHECK(streamer.State(dir / "A.scene.json") == LevelState::Unloaded);

    // Requests: B by name, kept until Unload; the camera is the source without StreamingSource.
    streamer.Load(dir / "B.scene.json");
    CHECK(Pump(streamer, scene, [&] { return streamer.IsLoaded(dir / "B.scene.json"); }));
    CHECK(CountNamed(scene, "B") == 2);
    streamer.Load(dir / "B.scene.json"); // idempotent
    streamer.Update(scene);
    CHECK(CountNamed(scene, "B") == 2);
    streamer.Unload(dir / "B.scene.json");
    streamer.Update(scene);
    CHECK(CountNamed(scene, "B") == 0);

    scene.GetRegistry().Remove<StreamingSource>(player);
    CHECK(Pump(streamer, scene, [&] { return streamer.IsLoaded(dir / "A.scene.json"); }, glm::vec3(0.0f)));

    // A missing file fails once (event, no retries every frame); a new request tries again.
    events.clear();
    streamer.Load(dir / "Missing.scene.json");
    CHECK(Pump(streamer, scene, [&] { return streamer.State(dir / "Missing.scene.json") == LevelState::Failed; },
               glm::vec3(0.0f)));
    for (int i = 0; i < 5; ++i)
        streamer.Update(scene, glm::vec3(0.0f));
    CHECK(events.size() == 1 && events.back().failed && !events.back().error.empty());
    streamer.Load(dir / "Missing.scene.json");
    CHECK(Pump(streamer, scene, [&] { return events.size() == 2; }, glm::vec3(0.0f)));

    // A request dropped while the level is still being prepared: nothing is created.
    WriteLevel(dir / "C.scene.json", "C", 1, 3000);
    streamer.Load(dir / "C.scene.json");
    streamer.Update(scene, glm::vec3(0.0f));
    streamer.Unload(dir / "C.scene.json");
    for (int i = 0; i < 20; ++i) {
        streamer.Update(scene, glm::vec3(0.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(CountNamed(scene, "C") == 0 && streamer.State(dir / "C.scene.json") == LevelState::Unloaded);

    // UnloadAll empties the scene of streamed levels at once.
    streamer.UnloadAll(scene);
    CHECK(CountNamed(scene, "A") == 0 && streamer.Levels().empty());

    // Destroying a streamer while a level is being parsed is fine.
    {
        LevelStreamer other(jobs, nullptr, &bus);
        other.Load(dir / "A.scene.json");
        other.Update(scene);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Streaming_ScriptsEndPlayAndCrossLevelReferences)
{
    // The persistent actor references an entity of level A by UUID (null until A loads), loads A
    // with Load Stream Level and reacts to Level Loaded; A's own script ends with the level.
    const fs::path dir = TempDir("streaming_scripts");
    {
        ScriptGraph inLevel;
        const auto  end   = inLevel.AddNode("Event.EndPlay", {});
        const auto  print = inLevel.AddNode("Debug.Print", {});
        inLevel.FindNode(print)->defaults["Text"]     = std::string("level script ended");
        inLevel.FindNode(print)->defaults["Duration"] = 100.0f;
        CHECK(inLevel.Connect(end, "Out", print, "In").empty());
        SaveScriptGraph(dir / "InLevel.ugraph", inLevel);

        Scene        level;
        const Entity target = level.CreateEntity("Target", NullEntity, 5000);
        level.GetRegistry().Emplace<ScriptComponent>(target, ScriptComponent{PathToUtf8(dir / "InLevel.ugraph")});
        SaveSceneFile(dir / "A.scene.json", level, nullptr);
    }
    ScriptGraph actor;
    actor.variables.push_back({.name = "Target", .type = PinType::Entity, .value = NullEntity, .exposed = true});
    const auto print = [&](const char* text) {
        const auto n = actor.AddNode("Debug.Print", {});
        actor.FindNode(n)->defaults["Text"]     = std::string(text);
        actor.FindNode(n)->defaults["Duration"] = 100.0f;
        return n;
    };
    const auto begin = actor.AddNode("Event.BeginPlay", {});
    const auto load  = actor.AddNode("Level.LoadStream", {});
    actor.FindNode(load)->defaults["Level"] = PathToUtf8(dir / "A.scene.json");
    CHECK(actor.Connect(begin, "Out", load, "In").empty());
    CHECK(actor.Connect(load, "Completed", print("loaded via node"), "In").empty());
    const auto loaded = actor.AddNode("Event.LevelLoaded", {});
    const auto valid  = actor.AddNode("Entity.IsValid", {});
    const auto get    = actor.AddNode("Variable.Get", {}, "Target");
    const auto branch = actor.AddNode("Flow.Branch", {});
    CHECK(actor.Connect(get, "Value", valid, "Entity").empty());
    CHECK(actor.Connect(loaded, "Out", branch, "In").empty());
    CHECK(actor.Connect(valid, "Valid", branch, "Condition").empty());
    CHECK(actor.Connect(branch, "True", print("target resolved"), "In").empty());
    const auto unloaded = actor.AddNode("Event.LevelUnloaded", {});
    CHECK(actor.Connect(unloaded, "Out", print("level gone"), "In").empty());

    ThreadPool    jobs(2);
    EventBus      bus;
    Scene         scene;
    ScriptSystem  scripts(bus);
    LevelStreamer streamer(jobs, nullptr, &bus);
    scripts.SetLevelStreamer(&streamer);
    streamer.SetUnloadHook([&](Scene& s, std::span<const Entity> roots) { scripts.EndPlayFor(s, roots); });
    scripts.Provide(dir / "Actor.ugraph", actor);

    const Entity self = scene.CreateEntity("Actor");
    ScriptComponent component{PathToUtf8(dir / "Actor.ugraph")};
    component.variables["Target"] = ScriptVariableOverride{.value = NullEntity, .entityUuid = 5000};
    scene.GetRegistry().Emplace<ScriptComponent>(self, std::move(component));
    scripts.Begin(scene);

    const auto printed = [&](std::string_view text) {
        return std::ranges::count_if(scripts.Messages(), [&](const ScriptMessage& m) { return m.text == text; });
    };
    const auto start = std::chrono::steady_clock::now();
    while (printed("target resolved") == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        scripts.Update(scene, 1.0f / 60.0f);
        streamer.Update(scene);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(printed("target resolved") == 1);
    CHECK(printed("loaded via node") == 1);
    CHECK(scripts.Stats().instances == 2); // the level's script runs too
    const auto errors = std::ranges::count_if(scripts.Messages(), [](const ScriptMessage& m) { return m.error; });
    CHECK(errors == 0);

    streamer.Unload(dir / "A.scene.json");
    streamer.Update(scene);
    CHECK(printed("level script ended") == 1); // EndPlay ran before the entity went
    scripts.Update(scene, 1.0f / 60.0f);
    CHECK(printed("level gone") == 1);
    CHECK(scripts.Stats().instances == 1);
    scripts.End(scene);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
