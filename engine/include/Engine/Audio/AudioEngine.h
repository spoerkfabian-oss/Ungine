#pragma once
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Audio/Sound.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace Engine {

struct AudioEngineDesc {
    bool          device     = true;  // false: no output device, mix with Render() (tests, servers)
    std::uint32_t sampleRate = 48000; // mixing rate (a device may use its own)
    std::uint32_t channels   = 2;
    std::uint32_t maxVoices  = 256;
};

struct VoiceDesc {
    AudioBus    bus    = AudioBus::World;
    float       volume = 1.0f;
    float       pitch  = 1.0f; // playback rate
    bool        loop   = false;
    bool        paused = false;
    float       fadeInSeconds = 0.0f;
    double      startSeconds  = 0.0; // seek into the sound
    // 3D: positioned relative to the listener (distance attenuation, panning, doppler).
    bool        spatial = false;
    glm::vec3   position{0.0f};
    glm::vec3   velocity{0.0f};
    Attenuation attenuation = Attenuation::Inverse;
    float       minDistance = 1.0f;  // full volume up to here
    float       maxDistance = 50.0f; // no further attenuation beyond
    float       rolloff     = 1.0f;
    float       doppler     = 1.0f;  // 0: off
};

using VoiceId = std::uint64_t; // 0: none

struct AudioStats {
    std::uint32_t voices      = 0; // playing or fading
    std::uint32_t streamed    = 0; // of which read from files
    std::uint32_t dropped     = 0; // Play() calls refused (voice limit)
    std::uint32_t occluded    = 0; // voices with occlusion > 0
    std::uint64_t framesMixed = 0; // Render() only
};

// Real-time mixing on miniaudio: voices play decoded or streamed sounds through a bus, spatial
// voices are placed relative to the listener. Each voice has an occlusion filter (low-pass +
// gain, set by the AudioSystem from raycasts); the World bus runs through a reverb.
// Main thread only; the mixing itself runs on the audio device's thread (or in Render()).
// Without an output device (none available, or desc.device false) everything works the same and
// Render() pulls the mix.
class AudioEngine {
public:
    explicit AudioEngine(const AudioEngineDesc& desc = {});
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool               HasDevice() const;
    [[nodiscard]] const std::string& DeviceName() const;
    [[nodiscard]] std::uint32_t      SampleRate() const;
    [[nodiscard]] std::uint32_t      Channels() const;

    // Voices. The sound's data stays alive while a voice uses it. 0 if refused (voice limit,
    // stream cannot be opened).
    VoiceId Play(std::shared_ptr<const SoundData> sound, const VoiceDesc& desc = {});
    void    Stop(VoiceId voice, float fadeOutSeconds = 0.0f);
    void    StopAll(float fadeOutSeconds = 0.0f);
    [[nodiscard]] bool IsPlaying(VoiceId voice) const; // false once finished or stopped
    void SetVolume(VoiceId voice, float volume, float fadeSeconds = 0.0f);
    void SetPitch(VoiceId voice, float pitch);
    void SetPaused(VoiceId voice, bool paused);
    void SetLooping(VoiceId voice, bool loop);
    void SetTransform(VoiceId voice, const glm::vec3& position, const glm::vec3& velocity);
    void SetSpatialRange(VoiceId voice, float minDistance, float maxDistance, float rolloff);
    // 0 = free path, 1 = fully blocked: low-pass (down to ~800 Hz) and -12 dB, smoothed.
    void SetOcclusion(VoiceId voice, float occlusion);
    [[nodiscard]] double Cursor(VoiceId voice) const; // playback position in seconds

    // World-up is +Y; `up` only disambiguates a listener looking straight up or down.
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up,
                     const glm::vec3& velocity = glm::vec3(0.0f));

    // Mixer.
    void SetBusVolume(AudioBus bus, float volume);
    [[nodiscard]] float BusVolume(AudioBus bus) const;
    void SetBusMuted(AudioBus bus, bool muted);
    [[nodiscard]] bool BusMuted(AudioBus bus) const;
    void SetReverb(const ReverbParams& params); // taken over by the audio thread without locks
    [[nodiscard]] const ReverbParams& Reverb() const;

    // Once per frame: frees finished voices.
    void Update();
    // No device: mixes the next frames (interleaved float, Channels() per frame).
    void Render(std::span<float> out);

    [[nodiscard]] const AudioStats& Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
