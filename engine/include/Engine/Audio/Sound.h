#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Engine {

// How a sound file is kept: decoded to PCM in memory (short effects, many voices) or streamed
// from the file while it plays (music, long ambience). Auto streams long files.
enum class SoundLoadMode : std::uint8_t { Auto, Decode, Stream };

// PCM audio (32-bit float, interleaved) or, for streamed sounds, the file to decode while playing.
struct SoundData {
    std::uint32_t         sampleRate = 48000;
    std::uint32_t         channels   = 1;
    std::uint64_t         frames     = 0; // length (also known for streamed files; 0 if the format cannot tell)
    std::vector<float>    samples;        // frames * channels; empty when streamed
    std::filesystem::path streamFile;     // non-empty: streamed from here

    [[nodiscard]] bool        Streamed() const { return !streamFile.empty(); }
    [[nodiscard]] double      Duration() const { return sampleRate ? static_cast<double>(frames) / sampleRate : 0.0; }
    [[nodiscard]] std::size_t MemoryBytes() const { return samples.size() * sizeof(float); }
};

inline constexpr double kSoundStreamThresholdSeconds = 10.0; // SoundLoadMode::Auto

// WAV, MP3, FLAC and OGG Vorbis (miniaudio + stb_vorbis). Throws std::runtime_error.
[[nodiscard]] SoundData LoadSoundFile(const std::filesystem::path& file, SoundLoadMode mode = SoundLoadMode::Auto);
[[nodiscard]] SoundData DecodeSound(std::span<const std::byte> encoded); // a whole file in memory
[[nodiscard]] bool      IsSoundFile(const std::filesystem::path& file);  // by extension

// 16-bit PCM WAV (decoded sounds only). Throws std::runtime_error.
void WriteWav(const std::filesystem::path& file, const SoundData& sound);

// Generated sounds (tests, samples): sine tone, white noise, silence. Mono.
[[nodiscard]] SoundData MakeTone(float frequency, float seconds, std::uint32_t sampleRate = 48000, float amplitude = 0.5f);
[[nodiscard]] SoundData MakeNoise(float seconds, std::uint32_t sampleRate = 48000, float amplitude = 0.5f,
                                  std::uint32_t seed = 1);

} // namespace Engine
