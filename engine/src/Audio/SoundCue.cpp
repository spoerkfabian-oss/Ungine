#include "Engine/Audio/SoundCue.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

const char* ToString(VoiceLimitMode mode) { return mode == VoiceLimitMode::Reject ? "reject" : "steal"; }

std::optional<VoiceLimitMode> VoiceLimitModeFromString(std::string_view name)
{
    if (name == "steal")
        return VoiceLimitMode::Steal;
    if (name == "reject")
        return VoiceLimitMode::Reject;
    return std::nullopt;
}

bool IsSoundCueFile(const fs::path& file)
{
    std::string ext = PathToUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == kSoundCueExtension;
}

namespace {

float Finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

} // namespace

std::optional<SoundCue> LoadSoundCue(const fs::path& file, std::string* error)
{
    const auto fail = [&](const std::string& what) -> std::optional<SoundCue> {
        if (error)
            *error = "'" + PathToUtf8(file) + "': " + what;
        return std::nullopt;
    };
    const std::optional<std::string> text = Vfs::ReadText(file);
    if (!text)
        return fail("cannot read");
    try {
        const json root = json::parse(*text);
        SoundCue   cue;
        const fs::path folder = file.parent_path();
        for (const json& v : root.value("variants", json::array())) {
            SoundCueVariant variant;
            const std::string sound = v.value("sound", std::string());
            if (!sound.empty()) {
                const fs::path path = PathFromUtf8(sound);
                variant.sound       = PathToUtf8((path.is_absolute() ? path : folder / path).lexically_normal());
            }
            variant.weight = std::max(Finite(v.value("weight", 1.0f), 1.0f), 0.0f);
            variant.volume = std::max(Finite(v.value("volume", 1.0f), 1.0f), 0.0f);
            variant.pitch  = std::clamp(Finite(v.value("pitch", 1.0f), 1.0f), 0.01f, 16.0f);
            cue.variants.push_back(std::move(variant));
        }
        cue.volumeMin = std::max(Finite(root.value("volumeMin", 1.0f), 1.0f), 0.0f);
        cue.volumeMax = std::max(Finite(root.value("volumeMax", cue.volumeMin), cue.volumeMin), cue.volumeMin);
        cue.pitchMin  = std::clamp(Finite(root.value("pitchMin", 1.0f), 1.0f), 0.01f, 16.0f);
        cue.pitchMax  = std::clamp(Finite(root.value("pitchMax", cue.pitchMin), cue.pitchMin), cue.pitchMin, 16.0f);
        cue.noRepeat  = root.value("noRepeat", cue.noRepeat);
        cue.priority  = static_cast<std::uint8_t>(std::clamp(root.value("priority", static_cast<int>(cue.priority)), 0, 255));
        cue.maxInstances = std::min(root.value("maxInstances", 0u), 1024u);
        cue.limitMode    = VoiceLimitModeFromString(root.value("limitMode", std::string("steal"))).value_or(VoiceLimitMode::Steal);
        return cue;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

bool SaveSoundCue(const fs::path& file, const SoundCue& cue, std::string* error)
{
    const fs::path folder = file.parent_path().lexically_normal();
    json           variants = json::array();
    for (const SoundCueVariant& v : cue.variants) {
        std::string sound = v.sound;
        if (!sound.empty()) {
            const fs::path path     = PathFromUtf8(sound).lexically_normal();
            const fs::path relative = path.lexically_relative(folder);
            if (path.is_absolute() && !relative.empty() && *relative.begin() != "..")
                sound = relative.generic_string();
        }
        variants.push_back({{"sound", sound}, {"weight", v.weight}, {"volume", v.volume}, {"pitch", v.pitch}});
    }
    const json root{{"version", 1},
                    {"variants", std::move(variants)},
                    {"volumeMin", cue.volumeMin},
                    {"volumeMax", cue.volumeMax},
                    {"pitchMin", cue.pitchMin},
                    {"pitchMax", cue.pitchMax},
                    {"noRepeat", cue.noRepeat},
                    {"priority", cue.priority},
                    {"maxInstances", cue.maxInstances},
                    {"limitMode", ToString(cue.limitMode)}};
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << root.dump(2) << '\n';
        if (!out) {
            if (error)
                *error = "cannot write '" + PathToUtf8(temp) + "'";
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec && error)
        *error = "cannot write '" + PathToUtf8(file) + "': " + ec.message();
    return !ec;
}

std::optional<std::size_t> PickCueVariant(const SoundCue& cue, std::mt19937& random, std::optional<std::size_t> last)
{
    const auto usable = [&](std::size_t i) { return !cue.variants[i].sound.empty() && cue.variants[i].weight > 0.0f; };
    std::size_t candidates = 0;
    for (std::size_t i = 0; i < cue.variants.size(); ++i)
        candidates += usable(i) ? 1u : 0u;
    if (candidates == 0)
        return std::nullopt;
    const bool skipLast = cue.noRepeat && candidates > 1 && last && *last < cue.variants.size() && usable(*last);
    float      total    = 0.0f;
    for (std::size_t i = 0; i < cue.variants.size(); ++i)
        if (usable(i) && !(skipLast && i == *last))
            total += cue.variants[i].weight;
    float pick = std::uniform_real_distribution<float>(0.0f, total)(random);
    std::optional<std::size_t> chosen;
    for (std::size_t i = 0; i < cue.variants.size(); ++i) {
        if (!usable(i) || (skipLast && i == *last))
            continue;
        chosen = i; // the last usable one catches rounding
        if (pick < cue.variants[i].weight)
            break;
        pick -= cue.variants[i].weight;
    }
    return chosen;
}

} // namespace Engine
