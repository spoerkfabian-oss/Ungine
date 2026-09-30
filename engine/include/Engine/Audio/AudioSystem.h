#pragma once
#include "Engine/Audio/AudioEngine.h"
#include "Engine/ECS/Entity.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>

namespace Engine {

class AssetManager;
class PhysicsWorld;
class Scene;
struct CameraData;

struct AudioSystemStats {
    std::uint32_t sources  = 0; // AudioSource components
    std::uint32_t playing  = 0; // of which have a voice
    std::uint32_t waiting  = 0; // want to play, sound still loading
    std::uint32_t oneShots = 0; // PlayAt / Play2D voices alive
    std::uint32_t rays     = 0; // occlusion raycasts last Update
    std::uint32_t occluded = 0;
    std::uint32_t zones    = 0; // reverb zones affecting the listener
    ReverbParams  reverb;       // blended
    glm::vec3     listener{0.0f};
};

// Plays a scene's audio (AudioSource, AudioListener, ReverbZone) on an AudioEngine. Main thread.
//
//   Begin(scene)   play mode: sources with playOnStart start (once their sound is loaded).
//   Update(...)    per frame: listener (AudioListener entity > primary camera > `view`), new /
//                  changed / removed sources (edits apply live), positions + doppler velocity,
//                  occlusion raycasts (PhysicsWorld), reverb zone blending, finished voices.
//   End(scene)     stops everything the scene started.
//
// Outside Begin/End only previews and one-shots play (editor). Sounds come from the AssetManager
// (asynchronous, cached; the system keeps one reference per sound until it is destroyed) or,
// without one, are loaded synchronously. Destroy the system before the AssetManager.
class AudioSystem {
public:
    AudioSystem(AudioEngine& engine, AssetManager* assets = nullptr, PhysicsWorld* physics = nullptr);
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void Begin(Scene& scene);
    void Update(Scene& scene, float dt, const CameraData* view = nullptr);
    void End(Scene& scene);
    [[nodiscard]] bool Running() const;
    // Pauses / resumes every voice the scene started (editor pause); previews keep playing.
    void SetPaused(bool paused);
    [[nodiscard]] bool Paused() const;

    // Gameplay / scripts (while running).
    bool Play(Entity entity);                             // (re)starts the entity's AudioSource
    void Stop(Entity entity, float fadeOutSeconds = 0.0f);
    [[nodiscard]] bool IsPlaying(Entity entity) const;    // also true while waiting for the sound
    // One-shots (also outside play mode): a 3D sound at a point / a 2D sound. False if the sound
    // failed or the voice limit was reached; a sound still loading plays once it is ready (within 1 s).
    bool PlayAt(const std::string& sound, const glm::vec3& position, float volume = 1.0f, float pitch = 1.0f,
                AudioBus bus = AudioBus::World);
    bool Play2D(const std::string& sound, float volume = 1.0f, AudioBus bus = AudioBus::Ui);

    // Editor: one sound at a time on the UI bus.
    void Preview(const std::filesystem::path& sound);
    void StopPreview();
    [[nodiscard]] bool Previewing() const;

    void Apply(const AudioSettings& settings); // bus volumes / mutes, occlusion
    [[nodiscard]] const AudioSettings&    Settings() const;
    [[nodiscard]] const AudioSystemStats& Stats() const;
    [[nodiscard]] AudioEngine&            Engine();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
