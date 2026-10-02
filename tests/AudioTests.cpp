#include "Test.h"

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptSystem.h"

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path AudioTempDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

AudioEngineDesc Offline(std::uint32_t maxVoices = 64)
{
    AudioEngineDesc desc;
    desc.device    = false;
    desc.maxVoices = maxVoices;
    return desc;
}

std::shared_ptr<const SoundData> Share(SoundData data) { return std::make_shared<const SoundData>(std::move(data)); }

// Stereo output of `seconds`.
std::vector<float> Mix(AudioEngine& audio, double seconds)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * audio.SampleRate()) * audio.Channels());
    audio.Render(out);
    audio.Update();
    return out;
}

// RMS of one channel over [from, to) of a stereo buffer, as fractions of its length.
float Rms(const std::vector<float>& buffer, int channel, double from = 0.0, double to = 1.0)
{
    const std::size_t frames = buffer.size() / 2;
    const auto        a      = static_cast<std::size_t>(from * static_cast<double>(frames));
    const auto        b      = static_cast<std::size_t>(to * static_cast<double>(frames));
    double            sum    = 0.0;
    for (std::size_t f = a; f < b; ++f)
        sum += static_cast<double>(buffer[f * 2 + static_cast<std::size_t>(channel)]) * buffer[f * 2 + static_cast<std::size_t>(channel)];
    return b > a ? static_cast<float>(std::sqrt(sum / static_cast<double>(b - a))) : 0.0f;
}

// RMS of the first difference relative to the RMS: high-frequency content.
float Brightness(const std::vector<float>& buffer, double from = 0.0)
{
    const std::size_t frames = buffer.size() / 2;
    double            sum = 0.0, diff = 0.0;
    for (std::size_t f = static_cast<std::size_t>(from * static_cast<double>(frames)) + 1; f < frames; ++f) {
        const double x = buffer[f * 2], d = buffer[f * 2] - buffer[(f - 1) * 2];
        sum += x * x;
        diff += d * d;
    }
    return sum > 0.0 ? static_cast<float>(std::sqrt(diff / sum)) : 0.0f;
}

int ZeroCrossings(const std::vector<float>& buffer, int channel, double from = 0.0)
{
    const std::size_t frames = buffer.size() / 2;
    int               count  = 0;
    for (std::size_t f = static_cast<std::size_t>(from * static_cast<double>(frames)) + 1; f < frames; ++f) {
        const float a = buffer[(f - 1) * 2 + static_cast<std::size_t>(channel)], b = buffer[f * 2 + static_cast<std::size_t>(channel)];
        count += (a < 0.0f) != (b < 0.0f) ? 1 : 0;
    }
    return count;
}

bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

} // namespace

TEST_CASE(Audio_WavRoundTripAndLoadModes)
{
    const fs::path dir  = AudioTempDir("audio_wav");
    const SoundData tone = MakeTone(440.0f, 0.5f, 44100, 0.5f);
    CHECK(tone.frames == 22050 && tone.samples.size() == 22050);
    WriteWav(dir / "tone.wav", tone);
    CHECK(IsSoundFile(dir / "tone.WAV") && IsSoundFile("x.ogg") && !IsSoundFile("x.txt"));

    const SoundData loaded = LoadSoundFile(dir / "tone.wav");
    CHECK(!loaded.Streamed());
    CHECK(loaded.sampleRate == 44100 && loaded.channels == 1 && loaded.frames == tone.frames);
    float maxError = 0.0f;
    for (std::size_t i = 0; i < std::min(loaded.samples.size(), tone.samples.size()); ++i)
        maxError = std::max(maxError, std::abs(loaded.samples[i] - tone.samples[i]));
    CHECK(maxError < 2.0f / 32767.0f);
    CHECK(Near(static_cast<float>(loaded.Duration()), 0.5f, 1e-4f));

    const SoundData streamed = LoadSoundFile(dir / "tone.wav", SoundLoadMode::Stream);
    CHECK(streamed.Streamed() && streamed.samples.empty() && streamed.frames == tone.frames && streamed.MemoryBytes() == 0);

    // Long files stream by default.
    WriteWav(dir / "long.wav", MakeTone(100.0f, static_cast<float>(kSoundStreamThresholdSeconds) + 1.0f, 8000));
    CHECK(LoadSoundFile(dir / "long.wav").Streamed());
    CHECK(!LoadSoundFile(dir / "long.wav", SoundLoadMode::Decode).Streamed());

    bool threw = false;
    try {
        (void)LoadSoundFile(dir / "missing.wav");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    CHECK(AudioBusFromString("music") == AudioBus::Music && AudioBusFromString("UI") == AudioBus::Ui);
    CHECK(!AudioBusFromString("nope"));
    for (std::size_t i = 0; i < kAudioBusCount; ++i)
        CHECK(AudioBusFromString(ToString(static_cast<AudioBus>(i))) == static_cast<AudioBus>(i));
}

TEST_CASE(Audio_MixBusesAndVoiceLimit)
{
    AudioEngine audio(Offline(3));
    CHECK(!audio.HasDevice() && audio.Channels() == 2 && audio.SampleRate() == 48000);
    const auto tone = Share(MakeTone(440.0f, 2.0f, 48000, 0.5f));
    const float full = 0.5f / std::sqrt(2.0f);

    const VoiceId voice = audio.Play(tone);
    CHECK(voice != 0 && audio.IsPlaying(voice));
    auto out = Mix(audio, 0.1);
    CHECK(Near(Rms(out, 0), full, 0.02f) && Near(Rms(out, 1), full, 0.02f));
    CHECK(audio.Stats().voices == 1 && audio.Stats().framesMixed == 4800);

    audio.SetBusVolume(AudioBus::World, 0.5f);
    out = Mix(audio, 0.1);
    CHECK(Near(Rms(out, 0, 0.5), full * 0.5f, 0.02f));
    audio.SetBusMuted(AudioBus::World, true);
    out = Mix(audio, 0.1);
    CHECK(Rms(out, 0, 0.5) < 0.001f && audio.BusMuted(AudioBus::World) && audio.BusVolume(AudioBus::World) == 0.5f);
    audio.SetBusMuted(AudioBus::World, false);
    audio.SetBusVolume(AudioBus::World, 1.0f);

    // Music is a separate bus; Master scales everything.
    VoiceDesc music;
    music.bus = AudioBus::Music;
    const VoiceId musicVoice = audio.Play(tone, music);
    audio.SetBusMuted(AudioBus::World, true);
    out = Mix(audio, 0.1);
    CHECK(Near(Rms(out, 0, 0.5), full, 0.02f));
    audio.SetBusVolume(AudioBus::Master, 0.0f);
    out = Mix(audio, 0.1);
    CHECK(Rms(out, 0, 0.5) < 0.001f);
    audio.SetBusVolume(AudioBus::Master, 1.0f);
    audio.SetBusMuted(AudioBus::World, false);

    // Voice limit.
    CHECK(audio.Play(tone) != 0);
    CHECK(audio.Play(tone) == 0 && audio.Stats().dropped == 1);
    audio.Stop(musicVoice);
    CHECK(!audio.IsPlaying(musicVoice) && audio.Play(tone) != 0);
    audio.StopAll();
    audio.Update();
    CHECK(audio.Stats().voices == 0 && !audio.IsPlaying(voice));
}

TEST_CASE(Audio_SpatialPanningDistanceDoppler)
{
    AudioEngine audio(Offline());
    audio.SetListener({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const auto tone = Share(MakeTone(500.0f, 3.0f, 48000, 0.5f));

    VoiceDesc right;
    right.spatial  = true;
    right.position = {3, 0, 0};
    right.doppler  = 0.0f;
    VoiceId voice = audio.Play(tone, right);
    auto    out   = Mix(audio, 0.2);
    CHECK(Rms(out, 1, 0.5) > 3.0f * Rms(out, 0, 0.5));
    audio.SetTransform(voice, {-3, 0, 0}, {0, 0, 0});
    out = Mix(audio, 0.2);
    CHECK(Rms(out, 0, 0.5) > 3.0f * Rms(out, 1, 0.5));
    audio.Stop(voice);

    // Inverse distance (min 1, rolloff 1): 10 m → 1/10.
    VoiceDesc ahead = right;
    ahead.position  = {0, 0, -1};
    voice           = audio.Play(tone, ahead);
    out             = Mix(audio, 0.2);
    const float near = Rms(out, 0, 0.5) + Rms(out, 1, 0.5);
    audio.SetTransform(voice, {0, 0, -10}, {0, 0, 0});
    out             = Mix(audio, 0.2);
    const float far = Rms(out, 0, 0.5) + Rms(out, 1, 0.5);
    CHECK(near > 0.1f && Near(far / near, 0.1f, 0.02f));
    audio.SetSpatialRange(voice, 20.0f, 100.0f, 1.0f); // inside min distance: full volume
    out = Mix(audio, 0.2);
    CHECK(Near(Rms(out, 0, 0.5) + Rms(out, 1, 0.5), near, 0.05f));
    audio.Stop(voice);

    // Pitch doubles the frequency; an approaching source sounds higher (Doppler).
    voice = audio.Play(tone, ahead);
    out   = Mix(audio, 0.5);
    const int base = ZeroCrossings(out, 0, 0.2);
    audio.SetPitch(voice, 2.0f);
    out = Mix(audio, 0.5);
    CHECK(Near(static_cast<float>(ZeroCrossings(out, 0, 0.2)) / static_cast<float>(base), 2.0f, 0.05f));
    audio.Stop(voice);

    VoiceDesc moving = ahead;
    moving.doppler   = 1.0f;
    moving.position  = {0, 0, -40};
    moving.velocity  = {0, 0, 50}; // towards the listener
    voice = audio.Play(tone, moving);
    out   = Mix(audio, 0.5);
    const float shift = static_cast<float>(ZeroCrossings(out, 0, 0.2)) / static_cast<float>(base);
    CHECK(shift > 1.1f && shift < 1.25f); // 343 / (343 - 50) = 1.17
}

TEST_CASE(Audio_LoopFadeStopAndStreaming)
{
    AudioEngine audio(Offline());
    const auto  shortTone = Share(MakeTone(300.0f, 0.05f, 48000, 0.5f));

    const VoiceId once = audio.Play(shortTone);
    VoiceDesc     looped;
    looped.loop = true;
    const VoiceId loop = audio.Play(shortTone, looped);
    Mix(audio, 0.2);
    CHECK(!audio.IsPlaying(once) && audio.IsPlaying(loop) && audio.Stats().voices == 1);

    // Fade-out: stopped at once for the game, audible until the fade ends.
    audio.Stop(loop, 0.05f);
    CHECK(!audio.IsPlaying(loop));
    auto out = Mix(audio, 0.02);
    CHECK(Rms(out, 0) > 0.05f && audio.Stats().voices == 1);
    out = Mix(audio, 0.1);
    CHECK(Rms(out, 0, 0.8) < 0.001f && audio.Stats().voices == 0);

    // Fade-in and volume ramps.
    const auto tone = Share(MakeTone(300.0f, 2.0f, 48000, 0.5f));
    VoiceDesc  fade;
    fade.fadeInSeconds = 0.2f;
    VoiceId voice      = audio.Play(tone, fade);
    out                = Mix(audio, 0.4);
    CHECK(Rms(out, 0, 0.0, 0.1) < 0.3f * Rms(out, 0, 0.6, 1.0));
    audio.SetVolume(voice, 0.25f, 0.1f);
    out = Mix(audio, 0.2);
    CHECK(Near(Rms(out, 0, 0.6), 0.25f * 0.5f / std::sqrt(2.0f), 0.01f));
    audio.SetPaused(voice, true);
    const double cursor = audio.Cursor(voice);
    out = Mix(audio, 0.1);
    CHECK(Rms(out, 0) < 0.001f && audio.IsPlaying(voice) && audio.Cursor(voice) == cursor);
    audio.SetPaused(voice, false);
    Mix(audio, 0.1);
    CHECK(audio.Cursor(voice) > cursor + 0.09);
    audio.Stop(voice);

    // Streaming from a file.
    const fs::path dir = AudioTempDir("audio_stream");
    WriteWav(dir / "music.wav", MakeTone(250.0f, 1.0f, 22050, 0.5f));
    const auto music = Share(LoadSoundFile(dir / "music.wav", SoundLoadMode::Stream));
    VoiceDesc  desc;
    desc.bus          = AudioBus::Music;
    desc.startSeconds = 0.5;
    voice             = audio.Play(music, desc);
    CHECK(voice != 0);
    out = Mix(audio, 0.2);
    CHECK(Near(Rms(out, 0, 0.2), 0.5f / std::sqrt(2.0f), 0.03f) && audio.Stats().streamed == 1);
    CHECK(audio.Cursor(voice) > 0.65 && audio.Cursor(voice) < 0.75);
    Mix(audio, 0.4);
    CHECK(!audio.IsPlaying(voice) && audio.Stats().voices == 0);

    // A vanished file cannot stream.
    fs::remove(dir / "music.wav");
    CHECK(audio.Play(music) == 0);
}

TEST_CASE(Audio_OcclusionAndReverb)
{
    AudioEngine audio(Offline());
    const auto  noise = Share(MakeNoise(3.0f, 48000, 0.5f, 7));

    const VoiceId voice = audio.Play(noise);
    auto          out   = Mix(audio, 0.2);
    const float   openRms = Rms(out, 0, 0.5), openBright = Brightness(out, 0.5);
    audio.SetOcclusion(voice, 1.0f);
    out = Mix(audio, 0.4);
    CHECK(Rms(out, 0, 0.6) < 0.3f * openRms);        // -12 dB and the low-pass
    CHECK(Brightness(out, 0.6) < 0.4f * openBright); // high frequencies removed
    audio.Update();
    CHECK(audio.Stats().occluded == 1);
    audio.SetOcclusion(voice, 0.0f);
    out = Mix(audio, 0.4);
    CHECK(Near(Rms(out, 0, 0.6), openRms, 0.03f));
    audio.Stop(voice);

    // Reverb on the World bus: a burst leaves a tail only with wet > 0; other buses stay dry.
    const auto burst = Share(MakeNoise(0.05f, 48000, 0.5f, 3));
    audio.Play(burst);
    out = Mix(audio, 0.3);
    CHECK(Rms(out, 0, 0.5) < 1e-4f);

    ReverbParams hall;
    hall.roomSize = 0.9f;
    hall.wet      = 1.0f;
    audio.SetReverb(hall);
    CHECK(audio.Reverb() == hall);
    audio.Play(burst);
    out = Mix(audio, 0.3);
    CHECK(Rms(out, 0, 0.5) > 0.005f && Rms(out, 1, 0.5) > 0.005f);
    Mix(audio, 5.0); // let the tail die out

    VoiceDesc ui;
    ui.bus = AudioBus::Ui;
    audio.Play(burst, ui);
    out = Mix(audio, 0.3);
    CHECK(Rms(out, 0, 0.5) < 1e-4f);
}

// --- AudioSystem (scene) ---------------------------------------------------------------------------

TEST_CASE(AudioSystem_SourcesListenerAndOneShots)
{
    const fs::path dir = AudioTempDir("audio_system");
    WriteWav(dir / "tone.wav", MakeTone(400.0f, 1.0f, 48000, 0.5f));
    const std::string tone = PathToUtf8(dir / "tone.wav");

    AudioEngine audio(Offline());
    AudioSystem system(audio);
    Scene       scene;
    Registry&   r = scene.GetRegistry();
    const Entity listener = scene.CreateEntity("Listener");
    r.Emplace<AudioListener>(listener);
    const Entity speaker = scene.CreateEntity("Speaker");
    scene.EditTransform(speaker).position = {3.0f, 0.0f, 0.0f};
    r.Emplace<AudioSource>(speaker, AudioSource{.sound = tone, .loop = true});
    scene.UpdateTransforms();

    // Edit mode: components do not play.
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().sources == 0 && !system.IsPlaying(speaker));

    system.Begin(scene);
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Running() && system.IsPlaying(speaker) && system.Stats().playing == 1);
    auto out = Mix(audio, 0.2);
    CHECK(Rms(out, 1, 0.5) > 3.0f * Rms(out, 0, 0.5)); // on the right

    // Moving the entity moves the sound; component edits apply live.
    scene.EditTransform(speaker).position = {-3.0f, 0.0f, 0.0f};
    scene.UpdateTransforms();
    system.Update(scene, 1.0f / 60.0f);
    out = Mix(audio, 0.2);
    CHECK(Rms(out, 0, 0.5) > 3.0f * Rms(out, 1, 0.5));
    const float loud = Rms(out, 0, 0.5);
    r.Get<AudioSource>(speaker).volume = 0.25f;
    system.Update(scene, 1.0f / 60.0f);
    out = Mix(audio, 0.2);
    CHECK(Near(Rms(out, 0, 0.5), 0.25f * loud, 0.02f));

    // Listener turned around: left and right swap.
    scene.EditTransform(listener).rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(0, 1, 0));
    scene.UpdateTransforms();
    system.Update(scene, 1.0f / 60.0f);
    out = Mix(audio, 0.2);
    CHECK(Rms(out, 1, 0.5) > 3.0f * Rms(out, 0, 0.5));

    system.Stop(speaker);
    CHECK(!system.IsPlaying(speaker));
    CHECK(system.Play(speaker) && system.IsPlaying(speaker));

    // A new sound restarts the voice; removing the component stops it.
    WriteWav(dir / "other.wav", MakeTone(800.0f, 1.0f, 48000, 0.5f));
    r.Get<AudioSource>(speaker).sound = PathToUtf8(dir / "other.wav");
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.IsPlaying(speaker));
    r.Remove<AudioSource>(speaker);
    system.Update(scene, 1.0f / 60.0f);
    Mix(audio, 0.1);
    CHECK(system.Stats().sources == 0 && audio.Stats().voices == 0);

    // One-shots; unknown files fail.
    CHECK(system.PlayAt(tone, {0.0f, 0.0f, -2.0f}) && system.Play2D(tone, 0.5f));
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().oneShots == 2);
    CHECK(!system.PlayAt(PathToUtf8(dir / "missing.wav"), {}));

    system.End(scene);
    CHECK(!system.Running() && audio.Stats().voices == 0);

    // Preview (editor) on the UI bus.
    system.Preview(dir / "tone.wav");
    CHECK(system.Previewing());
    system.StopPreview();
    CHECK(!system.Previewing());

    // Bus settings.
    AudioSettings settings;
    settings.volume[static_cast<std::size_t>(AudioBus::Music)] = 0.3f;
    settings.muted[static_cast<std::size_t>(AudioBus::Ui)]     = true;
    system.Apply(settings);
    CHECK(audio.BusVolume(AudioBus::Music) == 0.3f && audio.BusMuted(AudioBus::Ui) && system.Settings() == settings);
}

TEST_CASE(AudioSystem_OcclusionAndReverbZones)
{
    const fs::path dir = AudioTempDir("audio_occlusion");
    WriteWav(dir / "noise.wav", MakeNoise(2.0f, 48000, 0.5f, 5));

    ThreadPool   pool(2);
    EventBus     bus;
    PhysicsWorld physics(pool, bus);
    AudioEngine  audio(Offline());
    AudioSystem  system(audio, nullptr, &physics);
    Scene        scene;
    Registry&    r = scene.GetRegistry();

    const Entity listener = scene.CreateEntity("Listener");
    r.Emplace<AudioListener>(listener);
    const Entity source = scene.CreateEntity("Source");
    scene.EditTransform(source).position = {0.0f, 0.0f, -8.0f};
    r.Emplace<AudioSource>(source, AudioSource{.sound = PathToUtf8(dir / "noise.wav"), .loop = true});
    // The source sits inside its own (static) body: that must not count as occlusion.
    r.Emplace<RigidBody>(source, RigidBody{.type = BodyType::Static});
    r.Emplace<Collider>(source, Collider{.halfExtents = glm::vec3(0.3f)});
    const Entity wall = scene.CreateEntity("Wall");
    scene.EditTransform(wall).position = {0.0f, 0.0f, -4.0f};
    r.Emplace<RigidBody>(wall, RigidBody{.type = BodyType::Static});
    r.Emplace<Collider>(wall, Collider{.halfExtents = {5.0f, 5.0f, 0.2f}});
    physics.Sync(scene);

    system.Begin(scene);
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().rays >= 1 && system.Stats().occluded == 1);
    auto out = Mix(audio, 0.4);
    const float blockedRms = Rms(out, 0, 0.6), blockedBright = Brightness(out, 0.6);

    r.Remove<Collider>(wall); // path free
    physics.Sync(scene);
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().occluded == 0);
    out = Mix(audio, 0.4);
    CHECK(blockedRms < 0.5f * Rms(out, 0, 0.6) && blockedBright < 0.6f * Brightness(out, 0.6));

    // Occlusion off in the settings.
    r.Emplace<Collider>(wall, Collider{.halfExtents = {5.0f, 5.0f, 0.2f}});
    physics.Sync(scene);
    AudioSettings settings;
    settings.occlusion = false;
    system.Apply(settings);
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().occluded == 0 && system.Stats().rays == 0);

    // Reverb zones: inside = full, in the blend band = partly, outside = dry.
    const Entity zone = scene.CreateEntity("Hall");
    r.Emplace<ReverbZone>(zone, ReverbZone{.halfExtents = glm::vec3(5.0f), .blendDistance = 2.0f,
                                           .reverb = {.roomSize = 0.9f, .damping = 0.2f, .wet = 0.8f, .width = 1.0f}});
    scene.UpdateTransforms();
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().zones == 1 && Near(system.Stats().reverb.wet, 0.8f, 1e-4f) && audio.Reverb().roomSize > 0.89f);
    scene.EditTransform(listener).position = {6.0f, 0.0f, 0.0f}; // 1 m outside
    scene.UpdateTransforms();
    system.Update(scene, 1.0f / 60.0f);
    CHECK(Near(system.Stats().reverb.wet, 0.4f, 1e-3f));
    scene.EditTransform(listener).position = {20.0f, 0.0f, 0.0f};
    scene.UpdateTransforms();
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().zones == 0 && audio.Reverb().wet == 0.0f);

    system.End(scene);
    CHECK(audio.Stats().voices == 0);
}

TEST_CASE(AudioComponents_SerializeAndProjectSettings)
{
    Scene     scene;
    Registry& r = scene.GetRegistry();
    const Entity e = scene.CreateEntity("Speaker");
    const AudioSource source{.sound = "sounds/hum.wav", .bus = AudioBus::Ambient, .volume = 0.5f, .pitch = 1.5f,
                             .loop = true, .playOnStart = false, .stream = true, .fadeIn = 2.0f, .spatial = false,
                             .attenuation = Attenuation::Linear, .minDistance = 2.0f, .maxDistance = 30.0f,
                             .rolloff = 0.5f, .doppler = 0.0f, .occlusion = false};
    const ReverbZone zone{.halfExtents = {1.0f, 2.0f, 3.0f}, .blendDistance = 0.5f,
                          .reverb = {.roomSize = 0.2f, .damping = 0.9f, .wet = 0.3f, .width = 0.5f}};
    r.Emplace<AudioSource>(e, source);
    r.Emplace<AudioListener>(e);
    r.Emplace<ReverbZone>(e, zone);

    const std::string state = SnapshotEntityState(scene, e);
    r.Remove<AudioSource>(e);
    r.Remove<AudioListener>(e);
    r.Get<ReverbZone>(e).blendDistance = 9.0f;
    ApplyEntityState(scene, e, state);
    CHECK(r.Has<AudioSource>(e) && r.Get<AudioSource>(e) == source);
    CHECK(r.Has<AudioListener>(e) && r.Has<ReverbZone>(e) && r.Get<ReverbZone>(e) == zone);

    // Project audio settings.
    const fs::path dir = AudioTempDir("audio_project");
    std::string    error;
    auto templates = ProjectTemplates();
    CHECK(!templates.empty());
    if (templates.empty())
        return;
    auto project = Project::Create(dir, "AudioGame", templates.front(), &error);
    CHECK(project.has_value());
    if (!project)
        return;
    project->settings.audio.volume[static_cast<std::size_t>(AudioBus::Music)] = 0.25f;
    project->settings.audio.muted[static_cast<std::size_t>(AudioBus::World)]  = true;
    project->settings.audio.occlusionStrength                                 = 0.5f;
    CHECK(project->Save(&error));
    const auto loaded = Project::Load(project->File(), &error);
    CHECK(loaded && loaded->settings.audio == project->settings.audio);
}

TEST_CASE(Audio_DeviceThreadParameterChanges)
{
    // With an output device (or miniaudio's null device) the mix runs on the device thread while
    // the main thread changes parameters: exercised for ThreadSanitizer.
    AudioEngine audio;
    const auto  tone = Share(MakeTone(300.0f, 0.2f, 44100, 0.3f));
    VoiceDesc   desc;
    desc.loop    = true;
    desc.spatial = true;
    const VoiceId voice = audio.Play(tone, desc);
    CHECK(voice != 0);
    for (int i = 0; i < 40; ++i) {
        const float t = static_cast<float>(i) / 40.0f;
        audio.SetOcclusion(voice, t);
        audio.SetVolume(voice, 1.0f - t * 0.5f, 0.01f);
        audio.SetTransform(voice, {std::sin(t * 6.0f), 0.0f, -2.0f}, {1.0f, 0.0f, 0.0f});
        audio.SetListener({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
        audio.SetReverb({.roomSize = t, .damping = 0.5f, .wet = t, .width = 1.0f});
        audio.SetBusVolume(AudioBus::World, 1.0f - t * 0.5f);
        audio.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    audio.Stop(voice, 0.02f);
    for (int i = 0; i < 200 && audio.Stats().voices > 0; ++i) {
        if (!audio.HasDevice()) // offline: nothing mixes on its own
            Mix(audio, 0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        audio.Update();
    }
    CHECK(audio.Stats().voices == 0);
}

TEST_CASE(AudioTemplates_SoundsAndGraphsValid)
{
    // Every template's audio sources and blueprint sound paths point at loadable sounds.
    int sources = 0, scriptSounds = 0;
    for (const ProjectTemplate& t : ProjectTemplates()) {
        const fs::path scenePath = t.directory / PathFromUtf8(t.startScene);
        std::ifstream  file(scenePath);
        CHECK(file.good());
        if (!file)
            continue;
        const nlohmann::json scene = nlohmann::json::parse(file);
        for (const auto& e : scene.at("entities")) {
            if (e.contains("audioSource")) {
                const fs::path sound = (scenePath.parent_path() / e["audioSource"]["sound"].get<std::string>()).lexically_normal();
                CHECK(LoadSoundFile(sound, SoundLoadMode::Decode).frames > 0);
                ++sources;
            }
            if (e.contains("script")) {
                const fs::path graphFile = (scenePath.parent_path() / e["script"]["graph"].get<std::string>()).lexically_normal();
                const ScriptGraph graph = LoadScriptGraph(graphFile);
                CHECK(std::ranges::none_of(ValidateScriptGraph(graph), [](const ScriptDiagnostic& d) { return d.error; }));
                for (const ScriptNode& n : graph.nodes)
                    if (n.type.starts_with("Audio.PlaySound"))
                        if (const auto it = n.defaults.find("Sound"); it != n.defaults.end()) {
                            // Relative to the project root (the working directory in the editor and player).
                            CHECK(fs::exists(t.directory / PathFromUtf8(std::get<std::string>(it->second))));
                            ++scriptSounds;
                        }
            }
        }
    }
    CHECK(sources >= 2 && scriptSounds >= 1);
    for (const char* name : {"impact.wav", "hum.wav", "chime.wav", "music_loop.wav"})
        CHECK(LoadSoundFile(fs::path(ENGINE_ASSET_DIR) / "sounds" / name).frames > 0);
}

TEST_CASE(AudioSystem_BlueprintNodes)
{
    const fs::path dir = AudioTempDir("audio_script");
    WriteWav(dir / "tone.wav", MakeTone(400.0f, 1.0f, 48000, 0.5f));
    const std::string tone = PathToUtf8(dir / "tone.wav");

    EventBus     bus;
    Scene        scene;
    AudioEngine  audio(Offline());
    AudioSystem  system(audio);
    ScriptSystem scripts(bus, nullptr, nullptr, nullptr, &system);
    const Entity actor = scene.CreateEntity("Actor");
    scene.GetRegistry().Emplace<AudioSource>(actor, AudioSource{.sound = tone, .loop = true, .playOnStart = false, .spatial = false});

    // BeginPlay -> Play Audio Source (self) -> Play Sound at Location -> Set Audio Volume -> Set Bus Volume (music).
    ScriptGraph graph;
    const auto  node = [&](const char* type, std::string param = {}) { return graph.AddNode(type, {}, std::move(param)); };
    const auto  link = [&](std::uint32_t a, const char* pa, std::uint32_t b, const char* pb) { CHECK(graph.Connect(a, pa, b, pb).empty()); };
    const std::uint32_t begin  = node("Event.BeginPlay");
    const std::uint32_t play   = node("Audio.Play");
    const std::uint32_t at     = node("Audio.PlaySoundAt", "world");
    const std::uint32_t volume = node("Audio.SetVolume");
    const std::uint32_t busVol = node("Audio.SetBusVolume", "music");
    graph.FindNode(at)->defaults["Sound"]        = tone;
    graph.FindNode(at)->defaults["Location"]     = glm::vec3(0.0f, 0.0f, -2.0f);
    graph.FindNode(volume)->defaults["Volume"]   = 0.5f;
    graph.FindNode(busVol)->defaults["Volume"]   = 0.2f;
    link(begin, "Out", play, "In");
    link(play, "Then", at, "In");
    link(at, "Then", volume, "In");
    link(volume, "Then", busVol, "In");
    // Tick: Is Audio Playing -> Branch -> Print "playing".
    const std::uint32_t tick    = node("Event.Tick");
    const std::uint32_t playing = node("Audio.IsPlaying");
    const std::uint32_t branch  = node("Flow.Branch");
    const std::uint32_t print   = node("Debug.Print");
    graph.FindNode(print)->defaults["Text"] = std::string("playing");
    link(tick, "Out", branch, "In");
    link(playing, "Playing", branch, "Condition");
    link(branch, "True", print, "In");
    CHECK(ValidateScriptGraph(graph).empty());

    scene.GetRegistry().Emplace<ScriptComponent>(actor, ScriptComponent{"audio.ugraph"});
    scripts.Provide("audio.ugraph", graph);
    system.Begin(scene);
    scripts.Begin(scene);
    CHECK(system.IsPlaying(actor) && system.Stats().oneShots == 0);
    scripts.Update(scene, 1.0f / 60.0f);
    system.Update(scene, 1.0f / 60.0f);
    CHECK(system.Stats().playing == 1 && system.Stats().oneShots == 1);
    CHECK(scene.GetRegistry().Get<AudioSource>(actor).volume == 0.5f && audio.BusVolume(AudioBus::Music) == 0.2f);
    CHECK(!scripts.Messages().empty() && scripts.Messages().back().text == "playing" && scripts.Stats().errors == 0);

    scripts.End(scene);
    system.End(scene);
    CHECK(audio.Stats().voices == 0);
}
