#pragma once
#include "Engine/Audio/AudioTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

inline constexpr std::string_view kSoundCueExtension = ".ucue";

// One sound a cue can pick. `sound` is absolute in memory, relative to the cue file on disk.
struct SoundCueVariant {
    std::string sound;
    float       weight = 1.0f; // relative chance
    float       volume = 1.0f;
    float       pitch  = 1.0f;

    bool operator==(const SoundCueVariant&) const = default;
};

// A sound cue (.ucue): plays one of its variants, chosen by weight, with a random volume and
// pitch in [min, max] on top of the variant's and the caller's. Anything that takes a sound path
// (Audio Source, one-shots, Blueprint nodes) takes a cue path as well.
struct SoundCue {
    std::vector<SoundCueVariant> variants;
    float          volumeMin = 1.0f, volumeMax = 1.0f;
    float          pitchMin  = 1.0f, pitchMax  = 1.0f;
    bool           noRepeat  = true; // with 2+ variants, never the same one twice in a row
    std::uint8_t   priority  = kDefaultVoicePriority;
    std::uint32_t  maxInstances = 0; // voices of this cue at once, 0 = unlimited
    VoiceLimitMode limitMode    = VoiceLimitMode::Steal;

    bool operator==(const SoundCue&) const = default;
};

[[nodiscard]] bool IsSoundCueFile(const std::filesystem::path& file); // by extension
// Reads through the VFS; variant paths come back absolute (normalized).
[[nodiscard]] std::optional<SoundCue> LoadSoundCue(const std::filesystem::path& file, std::string* error = nullptr);
// Writes atomically (.tmp + rename); variant paths are stored relative to the cue's folder (absolute
// only without a relative path, e.g. another drive).
bool SaveSoundCue(const std::filesystem::path& file, const SoundCue& cue, std::string* error = nullptr);

// Variant index by weight (none: nullopt). `last` (if any) is skipped when noRepeat applies.
[[nodiscard]] std::optional<std::size_t> PickCueVariant(const SoundCue& cue, std::mt19937& random,
                                                         std::optional<std::size_t> last = std::nullopt);

} // namespace Engine
