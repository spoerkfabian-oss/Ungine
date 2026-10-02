#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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

// Mixer and effect settings of a project (.ungineproj "audio").
struct AudioSettings {
    std::array<float, kAudioBusCount> volume{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // per AudioBus
    std::array<bool, kAudioBusCount>  muted{};
    bool          occlusion         = true; // raycasts against physics geometry
    float         occlusionStrength = 1.0f; // 0..1: how much a blocked path muffles
    std::uint32_t occlusionRays     = 32;   // per frame (round-robin over the sources)

    bool operator==(const AudioSettings&) const = default;
};

} // namespace Engine
