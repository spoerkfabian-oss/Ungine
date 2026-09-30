#include "Engine/Audio/Sound.h"
#include "Engine/Core/Platform.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>

namespace Engine {

namespace fs = std::filesystem;

namespace {

std::string ResultText(ma_result result) { return ma_result_description(result); }

// Owns an initialized decoder.
struct Decoder {
    ma_decoder decoder{};
    bool       open = false;
    ~Decoder()
    {
        if (open)
            ma_decoder_uninit(&decoder);
    }
};

void OpenFile(Decoder& d, const fs::path& file)
{
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0); // native channels / rate
#ifdef _WIN32
    const ma_result result = ma_decoder_init_file_w(file.c_str(), &config, &d.decoder);
#else
    const ma_result result = ma_decoder_init_file(file.c_str(), &config, &d.decoder);
#endif
    if (result != MA_SUCCESS)
        throw std::runtime_error("'" + PathToUtf8(file) + "': cannot decode (" + ResultText(result) + ")");
    d.open = true;
}

// Reads the whole stream (the length is not always known up front).
void ReadAll(ma_decoder& decoder, SoundData& out)
{
    ma_format   format     = ma_format_f32;
    ma_uint32   channels   = 0;
    ma_uint32   sampleRate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &sampleRate, nullptr, 0);
    if (channels == 0 || sampleRate == 0)
        throw std::runtime_error("unsupported audio format");
    out.channels   = channels;
    out.sampleRate = sampleRate;
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0)
        out.samples.reserve(static_cast<std::size_t>(length * channels));
    std::array<float, 4096> chunk{};
    const ma_uint64 chunkFrames = chunk.size() / channels;
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunkFrames, &read);
        out.samples.insert(out.samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read * channels));
        if (read < chunkFrames || result != MA_SUCCESS)
            break;
    }
    out.frames = out.samples.size() / channels;
}

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

SoundData LoadSoundFile(const fs::path& file, SoundLoadMode mode)
{
    Decoder d;
    OpenFile(d, file);
    SoundData sound;
    ma_format format = ma_format_f32;
    ma_decoder_get_data_format(&d.decoder, &format, &sound.channels, &sound.sampleRate, nullptr, 0);
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&d.decoder, &length) != MA_SUCCESS)
        length = 0;
    sound.frames = length;
    const bool stream = mode == SoundLoadMode::Stream ||
                        (mode == SoundLoadMode::Auto && sound.sampleRate > 0 &&
                         static_cast<double>(length) / sound.sampleRate > kSoundStreamThresholdSeconds);
    if (stream) {
        std::error_code ec;
        sound.streamFile = fs::absolute(file, ec).lexically_normal();
        return sound;
    }
    try {
        ReadAll(d.decoder, sound);
    } catch (const std::exception& e) {
        throw std::runtime_error("'" + PathToUtf8(file) + "': " + e.what());
    }
    return sound;
}

SoundData DecodeSound(std::span<const std::byte> encoded)
{
    Decoder                 d;
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    const ma_result result = ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &d.decoder);
    if (result != MA_SUCCESS)
        throw std::runtime_error("cannot decode sound (" + ResultText(result) + ")");
    d.open = true;
    SoundData sound;
    ReadAll(d.decoder, sound);
    return sound;
}

bool IsSoundFile(const fs::path& file)
{
    const std::string ext = Lower(PathToUtf8(file.extension()));
    return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg";
}

void WriteWav(const fs::path& file, const SoundData& sound)
{
    if (sound.Streamed() || sound.channels == 0)
        throw std::runtime_error("WriteWav: needs a decoded sound");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write '" + PathToUtf8(file) + "'");
    const auto u32 = [&](std::uint32_t v) {
        const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        out.write(b, 4);
    };
    const auto u16 = [&](std::uint16_t v) {
        const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
        out.write(b, 2);
    };
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(sound.samples.size() * 2);
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(static_cast<std::uint16_t>(sound.channels));
    u32(sound.sampleRate);
    u32(sound.sampleRate * sound.channels * 2);
    u16(static_cast<std::uint16_t>(sound.channels * 2));
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    for (float s : sound.samples)
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f))));
    if (!out)
        throw std::runtime_error("write failed: '" + PathToUtf8(file) + "'");
}

SoundData MakeTone(float frequency, float seconds, std::uint32_t sampleRate, float amplitude)
{
    SoundData sound;
    sound.sampleRate = sampleRate;
    sound.channels   = 1;
    sound.frames     = static_cast<std::uint64_t>(std::max(seconds, 0.0f) * static_cast<float>(sampleRate));
    sound.samples.resize(static_cast<std::size_t>(sound.frames));
    const double step = 2.0 * std::numbers::pi * frequency / sampleRate;
    for (std::size_t i = 0; i < sound.samples.size(); ++i)
        sound.samples[i] = amplitude * static_cast<float>(std::sin(step * static_cast<double>(i)));
    return sound;
}

SoundData MakeNoise(float seconds, std::uint32_t sampleRate, float amplitude, std::uint32_t seed)
{
    SoundData sound;
    sound.sampleRate = sampleRate;
    sound.channels   = 1;
    sound.frames     = static_cast<std::uint64_t>(std::max(seconds, 0.0f) * static_cast<float>(sampleRate));
    sound.samples.resize(static_cast<std::size_t>(sound.frames));
    std::mt19937                          rng(seed);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    for (float& s : sound.samples)
        s = dist(rng);
    return sound;
}

} // namespace Engine
