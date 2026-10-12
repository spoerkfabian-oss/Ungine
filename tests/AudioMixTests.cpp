#include "Test.h"

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Audio/SoundCue.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsMaterial.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptSystem.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path MixTempDir(const char* name)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

AudioEngineDesc OfflineMix(std::uint32_t maxVoices = 64)
{
    AudioEngineDesc desc;
    desc.device    = false;
    desc.maxVoices = maxVoices;
    return desc;
}

std::shared_ptr<const SoundData> Shared(SoundData data) { return std::make_shared<const SoundData>(std::move(data)); }

std::vector<float> Render(AudioEngine& audio, double seconds)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * audio.SampleRate()) * audio.Channels());
    audio.Render(out);
    audio.Update();
    return out;
}

int Crossings(const std::vector<float>& buffer)
{
    int count = 0;
    for (std::size_t f = 1; f < buffer.size() / 2; ++f)
        count += (buffer[(f - 1) * 2] < 0.0f) != (buffer[f * 2] < 0.0f) ? 1 : 0;
    return count;
}

bool Close(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

VoiceDesc Desc(AudioBus bus, float volume = 1.0f, std::uint8_t priority = kDefaultVoicePriority)
{
    VoiceDesc d;
    d.bus      = bus;
    d.volume   = volume;
    d.priority = priority;
    return d;
}

} // namespace

TEST_CASE(Audio_SoundCueFileAndPicks)
{
    const fs::path dir = MixTempDir("audio_cue_file");
    SoundCue       cue;
    cue.variants = {{.sound = PathToUtf8(dir / "a.wav"), .weight = 3.0f, .volume = 0.5f, .pitch = 1.0f},
                    {.sound = PathToUtf8(dir / "Sub" / "b.wav"), .weight = 1.0f, .volume = 1.0f, .pitch = 1.5f},
                    {.sound = "", .weight = 5.0f},                                    // no sound: never picked
                    {.sound = PathToUtf8(dir / "c.wav"), .weight = 0.0f}};            // weight 0: never picked
    cue.volumeMin    = 0.8f;
    cue.volumeMax    = 1.0f;
    cue.pitchMin     = 0.9f;
    cue.pitchMax     = 1.1f;
    cue.noRepeat     = false;
    cue.priority     = 200;
    cue.maxInstances = 3;
    cue.limitMode    = VoiceLimitMode::Reject;
    std::string error;
    const fs::path file = dir / "Steps.ucue";
    CHECK(SaveSoundCue(file, cue, &error));
    CHECK(IsSoundCueFile(file) && IsSoundCueFile("X.UCUE") && !IsSoundCueFile("x.wav"));
    std::ifstream     in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"Sub/b.wav\"") != std::string::npos); // relative to the cue on disk
    const auto loaded = LoadSoundCue(file, &error);
    CHECK(loaded && *loaded == cue); // absolute again in memory
    std::ofstream(dir / "broken.ucue") << "{ not json";
    CHECK(!LoadSoundCue(dir / "broken.ucue", &error) && !error.empty());
    CHECK(!LoadSoundCue(dir / "missing.ucue"));

    // Weighted picks: 3 : 1, the empty and the zero-weight variants never.
    std::mt19937             random(7);
    std::array<int, 4>       counts{};
    for (int i = 0; i < 4000; ++i)
        if (const auto pick = PickCueVariant(cue, random))
            ++counts[*pick];
    CHECK(counts[2] == 0 && counts[3] == 0 && counts[0] + counts[1] == 4000);
    CHECK(Close(static_cast<float>(counts[0]) / 4000.0f, 0.75f, 0.03f));
    // No repeat: never the same variant twice in a row.
    cue.noRepeat = true;
    std::optional<std::size_t> last;
    bool                       repeated = false;
    for (int i = 0; i < 200; ++i) {
        const auto pick = PickCueVariant(cue, random, last);
        repeated        = repeated || pick == last;
        last            = pick;
    }
    CHECK(!repeated);
    CHECK(!PickCueVariant(SoundCue{}, random));
}

TEST_CASE(Audio_VoicePriorityAndLimits)
{
    const auto tone = Shared(MakeTone(440.0f, 2.0f));
    {
        // Engine limit 4: equal priority is refused, higher priority steals the quietest lower one.
        AudioEngine   audio(OfflineMix(4));
        const VoiceId quiet = audio.Play(tone, Desc(AudioBus::Ui, 0.1f));
        const VoiceId a     = audio.Play(tone, Desc(AudioBus::Ui));
        const VoiceId b     = audio.Play(tone, Desc(AudioBus::Ui));
        const VoiceId c     = audio.Play(tone, Desc(AudioBus::Ui));
        CHECK(quiet && a && b && c);
        CHECK(!audio.Play(tone, Desc(AudioBus::Ui)) && audio.Stats().dropped == 1);
        CHECK(!audio.Play(tone, Desc(AudioBus::Ui, 1.0f, 50)) && audio.Stats().dropped == 2);
        const VoiceId important = audio.Play(tone, Desc(AudioBus::Ui, 1.0f, 200));
        CHECK(important && !audio.IsPlaying(quiet) && audio.IsPlaying(a) && audio.Stats().stolen == 1);
        // Equal loudness: the oldest goes.
        const VoiceId important2 = audio.Play(tone, Desc(AudioBus::Ui, 1.0f, 200));
        CHECK(important2 && !audio.IsPlaying(a) && audio.IsPlaying(b));
        // Spatial voices: the farthest from the listener is the quietest.
        Render(audio, 0.05);
        CHECK(audio.Stats().voices == 4);
    }
    {
        AudioEngine audio(OfflineMix(16));
        // Group (a cue's instances): steal the least important instance, or reject.
        VoiceDesc group = Desc(AudioBus::World);
        group.group      = 7;
        group.groupLimit = 2;
        const VoiceId g1 = audio.Play(tone, group);
        const VoiceId g2 = audio.Play(tone, group);
        const VoiceId g3 = audio.Play(tone, group);
        CHECK(g1 && g2 && g3 && !audio.IsPlaying(g1) && audio.IsPlaying(g2) && audio.Stats().stolen == 1);
        group.groupLimitMode = VoiceLimitMode::Reject;
        CHECK(!audio.Play(tone, group) && audio.Stats().dropped == 1);
        CHECK(audio.Play(tone, Desc(AudioBus::World))); // not in the group

        // Bus limit.
        audio.SetBusVoiceLimit(AudioBus::Music, 1, VoiceLimitMode::Reject);
        const VoiceId m1 = audio.Play(tone, Desc(AudioBus::Music));
        CHECK(m1 && !audio.Play(tone, Desc(AudioBus::Music)));
        audio.SetBusVoiceLimit(AudioBus::Music, 1, VoiceLimitMode::Steal);
        const VoiceId m2 = audio.Play(tone, Desc(AudioBus::Music));
        CHECK(m2 && !audio.IsPlaying(m1));
        CHECK(!audio.Play(tone, Desc(AudioBus::Music, 1.0f, 10))); // nothing of lower or equal priority
        audio.SetBusVoiceLimit(AudioBus::Music, 0);
        CHECK(audio.Play(tone, Desc(AudioBus::Music)));

        // Spatial audibility: the far voice is stolen first.
        AudioEngine small(OfflineMix(2));
        small.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
        VoiceDesc near = Desc(AudioBus::World), far = Desc(AudioBus::World);
        near.spatial = far.spatial = true;
        near.position = {0.0f, 0.0f, -1.0f};
        far.position  = {0.0f, 0.0f, -40.0f};
        const VoiceId farVoice  = small.Play(tone, far);
        const VoiceId nearVoice = small.Play(tone, near);
        CHECK(small.Play(tone, Desc(AudioBus::World, 1.0f, 255)) && !small.IsPlaying(farVoice) && small.IsPlaying(nearVoice));
    }
}

TEST_CASE(Audio_SnapshotsAndDucking)
{
    AudioEngine   audio(OfflineMix());
    AudioSystem   system(audio);
    Scene         scene;
    AudioSettings settings;
    settings.snapshots.push_back({.name = "Quiet", .volume = {1.0f, 1.0f, 0.25f, 1.0f, 1.0f}, .fadeSeconds = 0.5f});
    settings.ducking.push_back({.source = AudioBus::Ui, .target = AudioBus::Music, .depth = 0.75f, .threshold = 0.01f,
                                .attack = 0.01f, .release = 0.1f});
    settings.ducking.push_back({.source = AudioBus::Music, .target = AudioBus::Music, .depth = 1.0f}); // ignored
    system.Apply(settings);

    // Snapshot weights and the resulting bus mix.
    CHECK(!system.ActivateSnapshot("Missing"));
    CHECK(system.ActivateSnapshot("Quiet", 0.0f));
    CHECK(Close(system.SnapshotWeight("Quiet"), 1.0f, 1e-5f) && Close(audio.BusMix(AudioBus::Music), 0.25f, 1e-5f));
    CHECK(audio.BusMix(AudioBus::World) == 1.0f && system.ActiveSnapshots() == std::vector<std::string>{"Quiet"});
    system.DeactivateSnapshot("Quiet"); // its own fade: 0.5 s
    CHECK(system.ActiveSnapshots().empty());
    system.Update(scene, 0.25f);
    CHECK(Close(system.SnapshotWeight("Quiet"), 0.5f, 1e-4f) && Close(audio.BusMix(AudioBus::Music), 0.625f, 1e-4f));
    system.Update(scene, 0.3f);
    CHECK(system.SnapshotWeight("Quiet") == 0.0f && audio.BusMix(AudioBus::Music) == 1.0f && system.Stats().snapshots == 0);

    // The mix reaches the signal.
    const VoiceId music = audio.Play(Shared(MakeTone(300.0f, 4.0f)), Desc(AudioBus::Music));
    CHECK(music != 0);
    Render(audio, 0.2);
    const float level = audio.BusLevel(AudioBus::Music);
    CHECK(level > 0.1f);
    system.ActivateSnapshot("Quiet", 0.0f);
    Render(audio, 0.2);
    CHECK(Close(audio.BusLevel(AudioBus::Music), 0.25f * level, 0.03f * level));
    system.DeactivateAllSnapshots();
    Render(audio, 0.2);
    CHECK(Close(audio.BusLevel(AudioBus::Music), level, 0.03f * level));

    // Ducking: UI sound -> music at a quarter; released after the UI sound ends.
    CHECK(Close(audio.BusDuckGain(AudioBus::Music), 1.0f, 1e-3f));
    audio.Play(Shared(MakeTone(800.0f, 0.3f)), Desc(AudioBus::Ui));
    Render(audio, 0.2);
    CHECK(Close(audio.BusDuckGain(AudioBus::Music), 0.25f, 0.02f));
    CHECK(Close(audio.BusLevel(AudioBus::Music), 0.25f * level, 0.05f * level));
    CHECK(audio.BusDuckGain(AudioBus::World) == 1.0f); // not a target
    Render(audio, 1.0);
    CHECK(audio.BusDuckGain(AudioBus::Music) > 0.98f && audio.BusLevel(AudioBus::Ui) < 1e-4f);
    CHECK(Close(audio.BusLevel(AudioBus::Music), level, 0.03f * level));

    // A quiet source below the threshold does not duck.
    settings.ducking[0].threshold = 0.5f;
    system.Apply(settings);
    audio.Play(Shared(MakeTone(800.0f, 0.3f)), Desc(AudioBus::Ui, 0.2f));
    Render(audio, 0.2);
    CHECK(audio.BusDuckGain(AudioBus::Music) > 0.98f);

    // End returns to the user's mix.
    system.Begin(scene);
    system.ActivateSnapshot("Quiet", 0.0f);
    system.End(scene);
    CHECK(audio.BusMix(AudioBus::Music) == 1.0f && system.ActiveSnapshots().empty());
}

TEST_CASE(AudioSystem_CueSourcesOneShotsAndReload)
{
    const fs::path dir = MixTempDir("audio_cue_system");
    WriteWav(dir / "low.wav", MakeTone(300.0f, 0.5f));
    WriteWav(dir / "high.wav", MakeTone(1500.0f, 0.5f));
    const fs::path cueFile = dir / "Blip.ucue";
    SoundCue       cue;
    cue.variants     = {{.sound = PathToUtf8(dir / "low.wav")}, {.sound = PathToUtf8(dir / "high.wav")}};
    cue.noRepeat     = true;
    cue.maxInstances = 2;
    CHECK(SaveSoundCue(cueFile, cue));
    const std::string cuePath = PathToUtf8(cueFile);

    AudioEngine audio(OfflineMix());
    AudioSystem system(audio);
    system.SetRandomSeed(3);
    Scene        scene;
    Registry&    r       = scene.GetRegistry();
    const Entity speaker = scene.CreateEntity("Speaker");
    r.Emplace<AudioSource>(speaker, AudioSource{.sound = cuePath, .spatial = false});
    scene.UpdateTransforms();

    // Each play picks the other variant (two variants, no repeat).
    system.Begin(scene);
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.IsPlaying(speaker) && system.Stats().cues == 1);
    std::vector<bool> high;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) {
            CHECK(system.Play(speaker));
            system.Update(scene, 1.0f / 60.0f);
        }
        const int crossings = Crossings(Render(audio, 0.1)); // 300 Hz: ~60, 1500 Hz: ~300
        CHECK(crossings > 40 && crossings < 340);
        high.push_back(crossings > 150);
    }
    for (std::size_t i = 1; i < high.size(); ++i)
        CHECK(high[i] != high[i - 1]);
    system.End(scene);
    CHECK(audio.Stats().voices == 0);

    // One-shots honour the cue's instance limit (steal).
    const std::uint32_t stolen = audio.Stats().stolen;
    CHECK(system.Play2D(cuePath) && system.Play2D(cuePath) && system.Play2D(cuePath));
    CHECK(audio.Stats().stolen == stolen + 1);
    system.Update(scene, 1.0f / 60.0f);
    Render(audio, 0.05);
    CHECK(audio.Stats().voices == 2);
    CHECK(!system.Play2D(PathToUtf8(dir / "missing.ucue")));

    // Hot reload: the changed file is picked up within a second.
    cue.variants.pop_back();
    cue.volumeMin = cue.volumeMax = 0.5f;
    CHECK(SaveSoundCue(cueFile, cue));
    std::error_code ec;
    fs::last_write_time(cueFile, fs::last_write_time(cueFile, ec) + std::chrono::seconds(2), ec);
    system.Update(scene, 1.1f);
    const auto reloaded = system.Cue(cuePath);
    CHECK(reloaded && reloaded->variants.size() == 1 && reloaded->volumeMax == 0.5f);

    // Preview picks a variant too.
    system.Preview(cueFile);
    CHECK(system.Previewing());
    system.StopPreview();
}

TEST_CASE(AudioSystem_MultiRayOcclusionAbsorption)
{
    const fs::path dir = MixTempDir("audio_multiray");
    WriteWav(dir / "noise.wav", MakeNoise(2.0f, 48000, 0.5f, 3));
    CHECK(SavePhysicsMaterial(dir / "Thin.uphysmat", {.friction = 0.5f, .surface = "Thin", .absorption = 0.5f}));
    const std::string thin = PathToUtf8(dir / "Thin.uphysmat");

    ThreadPool   pool(2);
    EventBus     bus;
    PhysicsWorld physics(pool, bus);
    AudioEngine  audio(OfflineMix());
    AudioSystem  system(audio, nullptr, &physics);
    AudioSettings settings;
    settings.occlusionRaysPerSource = 3;
    settings.occlusionSpread        = 0.5f;
    settings.occlusionRays          = 64;
    system.Apply(settings);

    Scene        scene;
    Registry&    r        = scene.GetRegistry();
    const Entity listener = scene.CreateEntity("Listener");
    r.Emplace<AudioListener>(listener);
    const Entity source = scene.CreateEntity("Source");
    scene.EditTransform(source).position = {0.0f, 0.0f, -8.0f};
    r.Emplace<AudioSource>(source, AudioSource{.sound = PathToUtf8(dir / "noise.wav"), .loop = true});
    const auto wall = [&](const char* name, float z, glm::vec3 half, const std::string& material) {
        const Entity e = scene.CreateEntity(name);
        scene.EditTransform(e).position = {0.0f, 0.0f, z};
        r.Emplace<RigidBody>(e, RigidBody{.type = BodyType::Static});
        Collider collider{.halfExtents = half};
        collider.material = material;
        r.Emplace<Collider>(e, collider);
        return e;
    };
    const auto update = [&] {
        physics.Sync(scene);
        system.Update(scene, 1.0f / 60.0f);
        return system.Occlusion(source);
    };

    // One wide wall of a material absorbing half: every ray loses half.
    const Entity front = wall("Front", -4.0f, {5.0f, 5.0f, 0.2f}, thin);
    system.Begin(scene);
    CHECK(Close(update(), 0.5f, 0.01f) && system.Stats().rays >= 3);
    // Two walls: a quarter gets through.
    const Entity back = wall("Back", -6.0f, {5.0f, 5.0f, 0.2f}, thin);
    CHECK(Close(update(), 0.75f, 0.01f));
    // A narrow post without material (blocks fully) only on the straight path: one ray of three.
    r.Remove<Collider>(front);
    r.Remove<Collider>(back);
    wall("Post", -4.0f, {0.1f, 5.0f, 0.2f}, {});
    CHECK(Close(update(), 1.0f / 3.0f, 0.01f));
    // Strength scales it; one ray per source: blocked completely.
    settings.occlusionStrength = 0.5f;
    system.Apply(settings);
    CHECK(Close(update(), 1.0f / 6.0f, 0.01f));
    settings.occlusionStrength      = 1.0f;
    settings.occlusionRaysPerSource = 1;
    system.Apply(settings);
    CHECK(Close(update(), 1.0f, 0.01f));
    system.End(scene);
}

TEST_CASE(Audio_MixSettingsSerializeAndNodes)
{
    // AudioSource priority, physics material absorption.
    Scene        scene;
    Registry&    r = scene.GetRegistry();
    const Entity e = scene.CreateEntity("Speaker");
    r.Emplace<AudioSource>(e, AudioSource{.sound = "Sounds/Steps.ucue", .priority = 42});
    const std::string state = SnapshotEntityState(scene, e);
    r.Get<AudioSource>(e).priority = 1;
    ApplyEntityState(scene, e, state);
    CHECK(r.Get<AudioSource>(e).priority == 42);
    const fs::path dir = MixTempDir("audio_mix_settings");
    CHECK(SavePhysicsMaterial(dir / "Felt.uphysmat", {.surface = "Felt", .absorption = 0.9f}));
    const auto felt = LoadPhysicsMaterial(dir / "Felt.uphysmat");
    CHECK(felt && felt->absorption == 0.9f);

    // Project: limits, occlusion rays, snapshots, ducking.
    auto templates = ProjectTemplates();
    CHECK(!templates.empty());
    if (templates.empty())
        return;
    std::string error;
    auto        project = Project::Create(dir, "MixGame", templates.front(), &error);
    CHECK(project.has_value());
    if (!project)
        return;
    AudioSettings& audioSettings = project->settings.audio;
    audioSettings.voiceLimit[static_cast<std::size_t>(AudioBus::World)] = 24;
    audioSettings.voiceLimitMode                                         = VoiceLimitMode::Reject;
    audioSettings.occlusionRaysPerSource                                 = 5;
    audioSettings.occlusionSpread                                        = 0.75f;
    audioSettings.snapshots.push_back({.name = "Pause", .volume = {1.0f, 0.2f, 0.5f, 1.0f, 0.2f}, .fadeSeconds = 0.25f});
    audioSettings.ducking.push_back({.source = AudioBus::Ui, .target = AudioBus::Ambient, .depth = 0.6f, .threshold = 0.02f,
                                     .attack = 0.1f, .release = 1.5f});
    CHECK(project->Save(&error));
    const auto loaded = Project::Load(project->File(), &error);
    CHECK(loaded && loaded->settings.audio == project->settings.audio);

    // Blueprint: BeginPlay -> Activate Audio Snapshot "Pause" (no fade); Tick prints the weight.
    EventBus     events;
    AudioEngine  audio(OfflineMix());
    AudioSystem  system(audio);
    system.Apply(audioSettings);
    ScriptSystem scripts(events, nullptr, nullptr, nullptr, &system);
    ScriptGraph  graph;
    const auto   node = [&](const char* type) { return graph.AddNode(type, {}, {}); };
    const auto   link = [&](std::uint32_t a, const char* pa, std::uint32_t b, const char* pb) { CHECK(graph.Connect(a, pa, b, pb).empty()); };
    const std::uint32_t begin    = node("Event.BeginPlay");
    const std::uint32_t activate = node("Audio.ActivateSnapshot");
    const std::uint32_t missing  = node("Audio.ActivateSnapshot");
    graph.FindNode(activate)->defaults["Snapshot"] = std::string("Pause");
    graph.FindNode(activate)->defaults["Fade"]     = 0.0f;
    graph.FindNode(missing)->defaults["Snapshot"]  = std::string("Nope");
    link(begin, "Out", activate, "In");
    link(activate, "Then", missing, "In");
    const std::uint32_t tick   = node("Event.Tick");
    const std::uint32_t weight = node("Audio.SnapshotWeight");
    const std::uint32_t print  = node("Debug.Print");
    graph.FindNode(weight)->defaults["Snapshot"] = std::string("Pause");
    link(tick, "Out", print, "In");
    link(weight, "Weight", print, "Text");
    CHECK(ValidateScriptGraph(graph).empty());
    const Entity actor = scene.CreateEntity("Actor");
    r.Emplace<ScriptComponent>(actor, ScriptComponent{"mix.ugraph"});
    scripts.Provide("mix.ugraph", graph);
    system.Begin(scene);
    scripts.Begin(scene);
    CHECK(system.SnapshotWeight("Pause") == 1.0f && Close(audio.BusMix(AudioBus::Music), 0.5f, 1e-5f));
    CHECK(scripts.Stats().errors == 1); // "Nope"
    scripts.Update(scene, 1.0f / 60.0f);
    CHECK(!scripts.Messages().empty() && scripts.Messages().back().text.starts_with("1"));
    scripts.End(scene);
    system.End(scene);
}
