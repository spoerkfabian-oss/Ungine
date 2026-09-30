// Generates the sample sounds shipped with the engine (assets/sounds, project templates):
//   MakeSounds <output directory>
// Everything is synthesized, so the files carry no third-party license. Deterministic output.
#include "Engine/Audio/Sound.h"
#include "Engine/Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <random>

namespace {

using Engine::SoundData;
constexpr double kTau = 2.0 * std::numbers::pi;

SoundData Silence(double seconds, std::uint32_t rate)
{
    SoundData s;
    s.sampleRate = rate;
    s.channels   = 1;
    s.frames     = static_cast<std::uint64_t>(seconds * rate);
    s.samples.assign(static_cast<std::size_t>(s.frames), 0.0f);
    return s;
}

void Normalize(SoundData& s, float peak)
{
    float max = 0.0f;
    for (float v : s.samples)
        max = std::max(max, std::abs(v));
    if (max > 0.0f)
        for (float& v : s.samples)
            v *= peak / max;
}

// Something solid hitting the ground: a low thump plus a short, low-passed crack.
SoundData Impact()
{
    constexpr std::uint32_t rate = 22050;
    SoundData               s    = Silence(0.35, rate);
    std::mt19937            rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    float lp = 0.0f;
    for (std::size_t i = 0; i < s.samples.size(); ++i) {
        const double t     = static_cast<double>(i) / rate;
        const double pitch = 70.0 + 60.0 * std::exp(-t * 30.0); // falls quickly
        const double thump = std::sin(kTau * pitch * t) * std::exp(-t * 16.0);
        lp += 0.35f * (noise(rng) - lp);
        const double crack = lp * std::exp(-t * 45.0);
        const double attack = std::min(1.0, t / 0.002);
        s.samples[i] = static_cast<float>(attack * (0.8 * thump + 0.9 * crack));
    }
    Normalize(s, 0.9f);
    return s;
}

// Seamless 2 s loop: a machine-like drone (whole periods of every partial).
SoundData Hum()
{
    constexpr std::uint32_t rate = 22050;
    SoundData               s    = Silence(2.0, rate);
    for (std::size_t i = 0; i < s.samples.size(); ++i) {
        const double t   = static_cast<double>(i) / rate;
        const double wob = 1.0 + 0.15 * std::sin(kTau * 1.0 * t); // 2 wobbles per loop
        s.samples[i]     = static_cast<float>(wob * (0.5 * std::sin(kTau * 55.0 * t) + 0.3 * std::sin(kTau * 110.0 * t) +
                                                     0.12 * std::sin(kTau * 165.0 * t) + 0.05 * std::sin(kTau * 440.0 * t)));
    }
    Normalize(s, 0.7f);
    return s;
}

// UI confirmation: a small bell.
SoundData Chime()
{
    constexpr std::uint32_t rate = 22050;
    SoundData               s    = Silence(0.9, rate);
    for (std::size_t i = 0; i < s.samples.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double e = std::min(1.0, t / 0.004) * std::exp(-t * 5.5);
        s.samples[i]   = static_cast<float>(e * (std::sin(kTau * 880.0 * t) + 0.5 * std::sin(kTau * 1320.0 * t) +
                                                0.25 * std::sin(kTau * 2640.0 * t) * std::exp(-t * 8.0)));
    }
    Normalize(s, 0.8f);
    return s;
}

// 8 s loop for the Music bus: arpeggios over Am - F - C - G with a bass line.
SoundData Music()
{
    constexpr std::uint32_t rate = 16000;
    SoundData               s    = Silence(8.0, rate);
    const auto midi = [](int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); };
    const std::array<std::array<int, 4>, 4> chords = {{{57, 60, 64, 69}, {53, 57, 60, 65}, {48, 52, 55, 60}, {55, 59, 62, 67}}};
    constexpr double step = 0.125; // 16 steps per 2 s chord
    for (std::size_t i = 0; i < s.samples.size(); ++i) {
        const double t      = static_cast<double>(i) / rate;
        const int    chord  = static_cast<int>(t / 2.0) % 4;
        const int    index  = static_cast<int>(t / step);
        const double local  = t - index * step;
        const int    pattern[8] = {0, 1, 2, 3, 2, 1, 2, 3};
        const double f      = midi(chords[chord][pattern[index % 8]] + 12);
        // Soft square-ish tone (odd harmonics), plucked envelope.
        const double env   = std::min(1.0, local / 0.005) * std::exp(-local * 14.0);
        const double lead  = env * (std::sin(kTau * f * t) + std::sin(kTau * 3.0 * f * t) / 6.0 + std::sin(kTau * 5.0 * f * t) / 15.0);
        const double bassF = midi(chords[chord][0] - 12);
        const double beat  = std::fmod(t, 0.5);
        const double bass  = std::min(1.0, beat / 0.01) * std::exp(-beat * 4.0) * std::sin(kTau * bassF * t);
        const double fade  = std::min(1.0, std::min(t, 8.0 - t) / 0.01); // no clicks at the loop point
        s.samples[i]       = static_cast<float>(fade * (0.45 * lead + 0.55 * bass));
    }
    Normalize(s, 0.8f);
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: MakeSounds <output directory>\n");
        return 1;
    }
    const std::filesystem::path dir = Engine::PathFromUtf8(argv[1]);
    std::error_code              ec;
    std::filesystem::create_directories(dir, ec);
    try {
        Engine::WriteWav(dir / "impact.wav", Impact());
        Engine::WriteWav(dir / "hum.wav", Hum());
        Engine::WriteWav(dir / "chime.wav", Chime());
        Engine::WriteWav(dir / "music_loop.wav", Music());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    std::printf("wrote impact, hum, chime, music_loop to %s\n", argv[1]);
    return 0;
}
