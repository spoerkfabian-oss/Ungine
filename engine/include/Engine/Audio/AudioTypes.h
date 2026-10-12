#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

// Mixer buses. World (3D effects, the reverb goes here), Music, Ui and Ambient feed Master.
enum class AudioBus : std::uint8_t { Master, World, Music, Ui, Ambient, Count };
[[nodiscard]] const char*             ToString(AudioBus bus);
[[nodiscard]] std::optional<AudioBus> AudioBusFromString(std::string_view name); // case-insensitive
inline constexpr std::size_t          kAudioBusCount = static_cast<std::size_t>(AudioBus::Count);

// Distance attenuation of 3D sounds (OpenAL's clamped models).
enum class Attenuation : std::uint8_t { Inverse, Linear, Exponential };
[[nodiscard]] const char*                ToString(Attenuation attenuation);
[[nodiscard]] std::optional<Attenuation> AttenuationFromString(std::string_view name);

// Room reverb on the World bus (reverb zones blend these). wet 0 = off.
struct ReverbParams {
    float roomSize = 0.5f; // 0..1
    float damping  = 0.5f; // 0..1, high frequencies die faster
    float wet      = 0.0f; // 0..1
    float width    = 1.0f; // stereo width 0..1

    bool operator==(const ReverbParams&) const = default;
};

// What happens when a voice limit (per cue / per bus) is reached.
enum class VoiceLimitMode : std::uint8_t {
    Steal,  // the least important playing voice makes room (lower priority, then quieter, then older)
    Reject, // the new sound does not play
};
[[nodiscard]] const char*                   ToString(VoiceLimitMode mode);
[[nodiscard]] std::optional<VoiceLimitMode> VoiceLimitModeFromString(std::string_view name);

inline constexpr std::uint8_t kDefaultVoicePriority = 128; // 0 = least, 255 = most important

// A named mix (e.g. "Pause", "Underwater"): bus gains on top of the user's volumes. Active
// snapshots fade in / out and multiply.
struct AudioSnapshot {
    std::string                       name;
    std::array<float, kAudioBusCount> volume{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // gain per bus (Master too)
    float                             fadeSeconds = 0.5f; // default fade in / out

    bool operator==(const AudioSnapshot&) const = default;
};

// Sidechain ducking: while `source` is louder than `threshold` (RMS, linear), `target` is
// attenuated by `depth` (0..1: 0.5 = half volume), reaching it within `attack` and recovering
// within `release` seconds. Runs on the audio thread.
struct DuckingRule {
    AudioBus source    = AudioBus::Ui;
    AudioBus target    = AudioBus::Music;
    float    depth     = 0.5f;
    float    threshold = 0.01f;
    float    attack    = 0.05f;
    float    release   = 0.5f;

    bool operator==(const DuckingRule&) const = default;
};

// Mixer and effect settings of a project (.ungineproj "audio").
struct AudioSettings {
    std::array<float, kAudioBusCount> volume{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // per AudioBus
    std::array<bool, kAudioBusCount>  muted{};
    std::array<std::uint32_t, kAudioBusCount> voiceLimit{}; // per bus, 0 = only the engine limit (Master: ignored)
    VoiceLimitMode voiceLimitMode = VoiceLimitMode::Steal;  // for the bus limits
    bool          occlusion         = true; // raycasts against physics geometry
    float         occlusionStrength = 1.0f; // 0..1: how much a blocked path muffles
    std::uint32_t occlusionRays     = 32;   // raycasts per frame (round-robin over the sources)
    std::uint32_t occlusionRaysPerSource = 3;    // 1 = straight path only; more: spread around it
    float         occlusionSpread        = 0.5f; // m, offset of the extra rays at the source
    std::vector<AudioSnapshot> snapshots;
    std::vector<DuckingRule>   ducking;

    bool operator==(const AudioSettings&) const = default;
};

} // namespace Engine
