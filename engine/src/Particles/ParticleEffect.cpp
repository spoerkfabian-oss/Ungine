#include "Engine/Particles/ParticleEffect.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Platform.h"
#include "Engine/Particles/ParticleFields.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// --- Curves ---

float ParticleCurve::Evaluate(float t) const
{
    if (keys.empty())
        return 1.0f;
    if (t <= keys.front().time)
        return keys.front().value;
    if (t >= keys.back().time)
        return keys.back().value;
    const auto next = std::ranges::upper_bound(keys, t, {}, &Key::time);
    const Key& b    = *next;
    const Key& a    = *(next - 1);
    if (interp == Interp::Constant)
        return a.value;
    const float span = b.time - a.time;
    float       f    = span > 0.0f ? (t - a.time) / span : 1.0f;
    if (interp == Interp::Smooth)
        f = f * f * (3.0f - 2.0f * f);
    return a.value + (b.value - a.value) * f;
}

ParticleCurve ParticleCurve::Constant(float value) { return ParticleCurve{.keys = {{0.0f, value}}}; }

ParticleCurve ParticleCurve::Ramp(float from, float to) { return ParticleCurve{.keys = {{0.0f, from}, {1.0f, to}}}; }

glm::vec4 ParticleGradient::Evaluate(float t) const
{
    if (keys.empty())
        return glm::vec4(1.0f);
    if (t <= keys.front().time)
        return keys.front().color;
    if (t >= keys.back().time)
        return keys.back().color;
    const auto  next = std::ranges::upper_bound(keys, t, {}, &Key::time);
    const Key&  b    = *next;
    const Key&  a    = *(next - 1);
    const float span = b.time - a.time;
    return glm::mix(a.color, b.color, span > 0.0f ? (t - a.time) / span : 1.0f);
}

ParticleGradient ParticleGradient::Fade(const glm::vec4& from, const glm::vec4& to)
{
    return ParticleGradient{.keys = {{0.0f, from}, {1.0f, to}}};
}

// --- Names ---

namespace {

constexpr std::array<ParticleModuleInfo, static_cast<std::size_t>(ParticleModuleType::Count)> kModules{{
    {"SpawnRate", "Spawn Rate", ParticleStage::Spawn, "Particles per second (scaled over the emitter's duration)."},
    {"SpawnBurst", "Spawn Burst", ParticleStage::Spawn, "A number of particles at once, optionally repeated."},
    {"SpawnPerDistance", "Spawn per Distance", ParticleStage::Spawn, "Particles per meter the emitter moves."},
    {"Shape", "Shape Location", ParticleStage::Initialize, "Start position inside / on a shape."},
    {"Velocity", "Initial Velocity", ParticleStage::Initialize, "Direction with spread, speed, radial push."},
    {"Lifetime", "Lifetime", ParticleStage::Initialize, "Seconds a particle lives (default 1)."},
    {"Size", "Initial Size", ParticleStage::Initialize, "Sprite size / mesh scale / ribbon width."},
    {"Color", "Initial Color", ParticleStage::Initialize, "Random color between two, times an intensity."},
    {"Rotation", "Initial Rotation", ParticleStage::Initialize, "Start angle and spin rate (degrees)."},
    {"SpriteFrame", "Sprite Frame", ParticleStage::Initialize, "Random start frame of the sprite sheet."},
    {"Gravity", "Gravity", ParticleStage::Update, "Constant acceleration (world space)."},
    {"Drag", "Drag", ParticleStage::Update, "Velocity decays exponentially."},
    {"Force", "Force / Wind", ParticleStage::Update, "Constant acceleration, world or emitter space."},
    {"CurlNoise", "Curl Noise", ParticleStage::Update, "Swirling turbulence."},
    {"Attractor", "Point Attractor", ParticleStage::Update, "Pulls particles towards a point."},
    {"SpeedLimit", "Speed Limit", ParticleStage::Update, "Clamps the speed."},
    {"SizeOverLife", "Size over Life", ParticleStage::Update, "Size multiplier over the particle's life."},
    {"ColorOverLife", "Color over Life", ParticleStage::Update, "Color multiplier over the particle's life."},
    {"SubUv", "Sprite Animation", ParticleStage::Update, "Sprite sheet frames over life or per second."},
    {"Collision", "Collision", ParticleStage::Update, "Bounces off physics geometry (raycasts)."},
    {"KillVolume", "Kill Volume", ParticleStage::Update, "Particles inside (or outside) a box / sphere die."},
    {"SubEmitter", "Sub Emitter", ParticleStage::Events, "Spawns another emitter's particles on an event."},
}};

template <class E, std::size_t N>
std::optional<E> FromNames(std::string_view name, const std::array<const char*, N>& names)
{
    for (std::size_t i = 0; i < N; ++i)
        if (name == names[i])
            return static_cast<E>(i);
    return std::nullopt;
}

constexpr std::array<const char*, 4> kStageNames{"Spawn", "Initialize", "Update", "Events"};
constexpr std::array<const char*, 6> kShapeNames{"point", "sphere", "hemisphere", "box", "cone", "circle"};
constexpr std::array<const char*, 4> kEventNames{"spawn", "death", "collision", "trail"};
constexpr std::array<const char*, 4> kRenderNames{"sprite", "mesh", "ribbon", "none"};
constexpr std::array<const char*, 2> kBlendNames{"alpha", "additive"};
constexpr std::array<const char*, 3> kFacingNames{"camera", "velocity", "axis"};
constexpr std::array<const char*, 2> kRibbonUvNames{"stretch", "tile"};
constexpr std::array<const char*, 3> kInterpNames{"linear", "smooth", "constant"};

template <class E, std::size_t N>
const char* Name(E value, const std::array<const char*, N>& names)
{
    const auto i = static_cast<std::size_t>(value);
    return i < N ? names[i] : names[0];
}

} // namespace

const ParticleModuleInfo& ModuleInfo(ParticleModuleType type)
{
    return kModules[std::min(static_cast<std::size_t>(type), kModules.size() - 1)];
}
const char* ToString(ParticleStage stage) { return Name(stage, kStageNames); }
const char* ToString(ParticleShape shape) { return Name(shape, kShapeNames); }
const char* ToString(ParticleEvent event) { return Name(event, kEventNames); }
const char* ToString(ParticleRenderType type) { return Name(type, kRenderNames); }
const char* ToString(ParticleBlend blend) { return Name(blend, kBlendNames); }
const char* ToString(ParticleFacing facing) { return Name(facing, kFacingNames); }
const char* ToString(RibbonUv uv) { return Name(uv, kRibbonUvNames); }

ParticleModule MakeParticleModule(ParticleModuleType type)
{
    ParticleModule module;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((I == static_cast<std::size_t>(type) ? (module.params.emplace<I>(), 0) : 0), ...);
    }(std::make_index_sequence<std::variant_size_v<ParticleModuleParams>>{});
    return module;
}

std::optional<ParticleModuleType> ParticleModuleTypeFromString(std::string_view name)
{
    for (std::size_t i = 0; i < kModules.size(); ++i)
        if (name == kModules[i].name)
            return static_cast<ParticleModuleType>(i);
    return std::nullopt;
}

static_assert(std::variant_size_v<ParticleModuleParams> == static_cast<std::size_t>(ParticleModuleType::Count));

const ParticleEmitter* ParticleEffect::Find(std::string_view name) const
{
    const auto it = std::ranges::find(emitters, name, &ParticleEmitter::name);
    return it == emitters.end() ? nullptr : &*it;
}

std::optional<std::size_t> ParticleEffect::IndexOf(std::string_view name) const
{
    const auto it = std::ranges::find(emitters, name, &ParticleEmitter::name);
    return it == emitters.end() ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(it - emitters.begin()));
}

ParticleEmitter MakeDefaultParticleEmitter(std::string name)
{
    ParticleEmitter emitter;
    emitter.name = std::move(name);
    emitter.modules.push_back(MakeParticleModule(ParticleModuleType::SpawnRate));
    emitter.modules.push_back(MakeParticleModule(ParticleModuleType::Shape));
    emitter.modules.push_back(MakeParticleModule(ParticleModuleType::Velocity));
    emitter.modules.push_back(MakeParticleModule(ParticleModuleType::Lifetime));
    emitter.modules.push_back(MakeParticleModule(ParticleModuleType::Size));
    emitter.modules.push_back(MakeParticleModule(ParticleModuleType::Color));
    ParticleModule gravity = MakeParticleModule(ParticleModuleType::Gravity);
    std::get<GravityModule>(gravity.params).acceleration = glm::vec3(0.0f, -1.0f, 0.0f);
    emitter.modules.push_back(std::move(gravity));
    emitter.modules.push_back(MakeParticleModule(ParticleModuleType::ColorOverLife));
    return emitter;
}

ParticleEffect MakeDefaultParticleEffect()
{
    ParticleEffect effect;
    effect.emitters.push_back(MakeDefaultParticleEmitter("Emitter"));
    return effect;
}

bool IsParticleEffectFile(const fs::path& file)
{
    std::string ext = PathToUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == kParticleEffectExtension;
}

// --- JSON ---

namespace {

float Finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

json ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json ToJson(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

template <class Vec>
void ReadVec(const json& j, Vec& out)
{
    if (!j.is_array() || j.size() != static_cast<std::size_t>(Vec::length()))
        return;
    for (int i = 0; i < Vec::length(); ++i)
        if (j[static_cast<std::size_t>(i)].is_number())
            out[i] = Finite(j[static_cast<std::size_t>(i)].get<float>(), out[i]);
}

float Clamp(float value, const ParticleFieldHint& hint) { return std::clamp(value, hint.min, hint.max); }

struct Writer {
    json& out;
    void  operator()(const char* name, const float& value, const ParticleFieldHint&) { out[name] = value; }
    void  operator()(const char* name, const std::uint32_t& value, const ParticleFieldHint&) { out[name] = value; }
    void  operator()(const char* name, const std::uint16_t& value, const ParticleFieldHint&) { out[name] = value; }
    void  operator()(const char* name, const bool& value, const ParticleFieldHint&) { out[name] = value; }
    void  operator()(const char* name, const glm::vec3& value, const ParticleFieldHint&) { out[name] = ToJson(value); }
    void  operator()(const char* name, const glm::vec4& value, const ParticleFieldHint&) { out[name] = ToJson(value); }
    void  operator()(const char* name, const std::string& value, const ParticleFieldHint&) { out[name] = value; }
    void  operator()(const char* name, const ParticleShape& value, const ParticleFieldHint&) { out[name] = ToString(value); }
    void  operator()(const char* name, const ParticleEvent& value, const ParticleFieldHint&) { out[name] = ToString(value); }
    void  operator()(const char* name, const FloatRange& value, const ParticleFieldHint&)
    {
        out[name] = json::array({value.min, value.max});
    }
    void operator()(const char* name, const ParticleCurve& curve, const ParticleFieldHint&)
    {
        json keys = json::array();
        for (const ParticleCurve::Key& k : curve.keys)
            keys.push_back(json::array({k.time, k.value}));
        out[name] = {{"interp", Name(curve.interp, kInterpNames)}, {"keys", std::move(keys)}};
    }
    void operator()(const char* name, const ParticleGradient& gradient, const ParticleFieldHint&)
    {
        json keys = json::array();
        for (const ParticleGradient::Key& k : gradient.keys)
            keys.push_back(json::array({k.time, k.color.r, k.color.g, k.color.b, k.color.a}));
        out[name] = std::move(keys);
    }
};

struct Reader {
    const json& in;
    void operator()(const char* name, float& value, const ParticleFieldHint& hint)
    {
        if (const auto it = in.find(name); it != in.end() && it->is_number())
            value = Clamp(Finite(it->get<float>(), value), hint);
    }
    void operator()(const char* name, std::uint32_t& value, const ParticleFieldHint& hint)
    {
        if (const auto it = in.find(name); it != in.end() && it->is_number())
            value = static_cast<std::uint32_t>(std::clamp(Finite(it->get<float>(), static_cast<float>(value)), 0.0f,
                                                          std::min(hint.max, 4.0e9f)));
    }
    void operator()(const char* name, std::uint16_t& value, const ParticleFieldHint&)
    {
        if (const auto it = in.find(name); it != in.end() && it->is_number_integer())
            value = static_cast<std::uint16_t>(std::clamp<std::int64_t>(it->get<std::int64_t>(), 0, 0xFFFF));
    }
    void operator()(const char* name, bool& value, const ParticleFieldHint&)
    {
        if (const auto it = in.find(name); it != in.end() && it->is_boolean())
            value = it->get<bool>();
    }
    void operator()(const char* name, glm::vec3& value, const ParticleFieldHint& hint)
    {
        if (const auto it = in.find(name); it != in.end()) {
            ReadVec(*it, value);
            value = glm::clamp(value, glm::vec3(hint.min), glm::vec3(hint.max));
        }
    }
    void operator()(const char* name, glm::vec4& value, const ParticleFieldHint& hint)
    {
        if (const auto it = in.find(name); it != in.end()) {
            ReadVec(*it, value);
            value = glm::clamp(value, glm::vec4(hint.min), glm::vec4(hint.max));
        }
    }
    void operator()(const char* name, std::string& value, const ParticleFieldHint&)
    {
        if (const auto it = in.find(name); it != in.end() && it->is_string())
            value = it->get<std::string>();
    }
    void operator()(const char* name, ParticleShape& value, const ParticleFieldHint&)
    {
        if (const auto it = in.find(name); it != in.end() && it->is_string())
            value = FromNames<ParticleShape>(it->get<std::string>(), kShapeNames).value_or(value);
    }
    void operator()(const char* name, ParticleEvent& value, const ParticleFieldHint&)
    {
        if (const auto it = in.find(name); it != in.end() && it->is_string())
            value = FromNames<ParticleEvent>(it->get<std::string>(), kEventNames).value_or(value);
    }
    void operator()(const char* name, FloatRange& value, const ParticleFieldHint& hint)
    {
        const auto it = in.find(name);
        if (it == in.end())
            return;
        if (it->is_number()) // a single value
            value.min = value.max = Clamp(Finite(it->get<float>(), value.min), hint);
        else if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
            value.min = Clamp(Finite((*it)[0].get<float>(), value.min), hint);
            value.max = Clamp(Finite((*it)[1].get<float>(), value.max), hint);
        }
        if (value.max < value.min)
            std::swap(value.min, value.max);
    }
    void operator()(const char* name, ParticleCurve& curve, const ParticleFieldHint&)
    {
        const auto it = in.find(name);
        if (it == in.end() || !it->is_object())
            return;
        curve.keys.clear();
        curve.interp = FromNames<ParticleCurve::Interp>(it->value("interp", std::string("linear")), kInterpNames)
                           .value_or(ParticleCurve::Interp::Linear);
        for (const json& k : it->value("keys", json::array()))
            if (k.is_array() && k.size() == 2 && k[0].is_number() && k[1].is_number())
                curve.keys.push_back({std::clamp(Finite(k[0].get<float>(), 0.0f), 0.0f, 1.0f), Finite(k[1].get<float>(), 1.0f)});
        std::ranges::stable_sort(curve.keys, {}, &ParticleCurve::Key::time);
    }
    void operator()(const char* name, ParticleGradient& gradient, const ParticleFieldHint&)
    {
        const auto it = in.find(name);
        if (it == in.end() || !it->is_array())
            return;
        gradient.keys.clear();
        for (const json& k : *it) {
            if (!k.is_array() || k.size() != 5 || !std::ranges::all_of(k, [](const json& x) { return x.is_number(); }))
                continue;
            ParticleGradient::Key key;
            key.time = std::clamp(Finite(k[0].get<float>(), 0.0f), 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c)
                key.color[c] = std::max(Finite(k[static_cast<std::size_t>(c + 1)].get<float>(), 1.0f), 0.0f);
            gradient.keys.push_back(key);
        }
        std::ranges::stable_sort(gradient.keys, {}, &ParticleGradient::Key::time);
    }
};

// Relative to the effect file (also "../Textures/x.png", like scene files: the project moves and
// packages as a whole); absolute only when there is no relative path (another drive).
std::string StorePath(const std::string& path, const fs::path& folder)
{
    if (path.empty())
        return path;
    const fs::path absolute = PathFromUtf8(path).lexically_normal();
    const fs::path relative = absolute.lexically_relative(folder);
    if (!absolute.is_absolute() || relative.empty())
        return path;
    const std::u8string generic = relative.generic_u8string();
    return {generic.begin(), generic.end()};
}

std::string LoadPath(const std::string& path, const fs::path& folder)
{
    if (path.empty())
        return path;
    const fs::path p = PathFromUtf8(path);
    return PathToUtf8((p.is_absolute() ? p : folder / p).lexically_normal());
}

} // namespace

std::string ParticleEffectToJson(const ParticleEffect& effect, const fs::path& folderIn)
{
    const fs::path folder   = folderIn.lexically_normal();
    json           emitters = json::array();
    for (const ParticleEmitter& e : effect.emitters) {
        json modules = json::array();
        for (const ParticleModule& m : e.modules) {
            json entry{{"type", ModuleInfo(m.Type()).name}};
            if (!m.enabled)
                entry["enabled"] = false;
            std::visit([&](const auto& params) { VisitParticleFields(params, Writer{entry}); }, m.params);
            modules.push_back(std::move(entry));
        }
        const ParticleRender& r = e.render;
        json render{{"type", ToString(r.type)},
                    {"blend", ToString(r.blend)},
                    {"texture", StorePath(r.texture, folder)},
                    {"columns", r.columns},
                    {"rows", r.rows},
                    {"facing", ToString(r.facing)},
                    {"axis", ToJson(r.axis)},
                    {"stretch", r.stretch},
                    {"softness", r.softness},
                    {"cameraFade", r.cameraFade},
                    {"sort", r.sort},
                    {"mesh", StorePath(r.mesh, folder)},
                    {"meshIndex", r.meshIndex},
                    {"alignToVelocity", r.alignToVelocity},
                    {"shade", r.shade},
                    {"ribbonUv", ToString(r.ribbonUv)},
                    {"ribbonTile", r.ribbonTile}};
        emitters.push_back({{"name", e.name},
                            {"enabled", e.enabled},
                            {"maxParticles", e.maxParticles},
                            {"localSpace", e.localSpace},
                            {"duration", e.duration},
                            {"loop", e.loop},
                            {"delay", e.delay},
                            {"modules", std::move(modules)},
                            {"render", std::move(render)}});
    }
    const json root{{"version", 1}, {"warmup", effect.warmup}, {"emitters", std::move(emitters)}};
    return root.dump(2) + "\n";
}

std::optional<ParticleEffect> ParseParticleEffect(std::string_view text, const fs::path& folderIn, std::string* error)
{
    const fs::path folder = folderIn.lexically_normal();
    try {
        const json     root = json::parse(text);
        ParticleEffect effect;
        effect.warmup = std::clamp(Finite(root.value("warmup", 0.0f), 0.0f), 0.0f, 30.0f);
        std::set<std::string> names;
        for (const json& je : root.value("emitters", json::array())) {
            if (!je.is_object())
                continue;
            ParticleEmitter e;
            e.name = je.value("name", std::string("Emitter"));
            for (std::string base = e.name, candidate = base; ; ) { // unique names (sub-emitter targets)
                if (names.insert(candidate).second) {
                    e.name = candidate;
                    break;
                }
                candidate = base + " " + std::to_string(names.size() + 1);
            }
            e.enabled      = je.value("enabled", true);
            e.maxParticles = std::clamp(je.value("maxParticles", e.maxParticles), 1u, kMaxParticlesPerEmitter);
            e.localSpace   = je.value("localSpace", false);
            e.duration     = std::max(Finite(je.value("duration", 0.0f), 0.0f), 0.0f);
            e.loop         = je.value("loop", true);
            e.delay        = std::max(Finite(je.value("delay", 0.0f), 0.0f), 0.0f);
            for (const json& jm : je.value("modules", json::array())) {
                if (!jm.is_object())
                    continue;
                const auto type = ParticleModuleTypeFromString(jm.value("type", std::string()));
                if (!type)
                    continue; // unknown module (newer file): skipped
                ParticleModule m = MakeParticleModule(*type);
                m.enabled        = jm.value("enabled", true);
                std::visit([&](auto& params) { VisitParticleFields(params, Reader{jm}); }, m.params);
                e.modules.push_back(std::move(m));
            }
            const json  jr = je.value("render", json::object());
            ParticleRender& r = e.render;
            r.type     = FromNames<ParticleRenderType>(jr.value("type", std::string("sprite")), kRenderNames).value_or(r.type);
            r.blend    = FromNames<ParticleBlend>(jr.value("blend", std::string("alpha")), kBlendNames).value_or(r.blend);
            r.texture  = LoadPath(jr.value("texture", std::string()), folder);
            r.columns  = std::clamp(jr.value("columns", 1u), 1u, 64u);
            r.rows     = std::clamp(jr.value("rows", 1u), 1u, 64u);
            r.facing   = FromNames<ParticleFacing>(jr.value("facing", std::string("camera")), kFacingNames).value_or(r.facing);
            if (const auto it = jr.find("axis"); it != jr.end())
                ReadVec(*it, r.axis);
            r.stretch    = std::max(Finite(jr.value("stretch", r.stretch), r.stretch), 0.0f);
            r.softness   = std::max(Finite(jr.value("softness", r.softness), r.softness), 0.0f);
            r.cameraFade = std::max(Finite(jr.value("cameraFade", 0.0f), 0.0f), 0.0f);
            r.sort       = jr.value("sort", true);
            r.mesh       = LoadPath(jr.value("mesh", std::string()), folder);
            r.meshIndex  = jr.value("meshIndex", 0u);
            r.alignToVelocity = jr.value("alignToVelocity", false);
            r.shade      = jr.value("shade", true);
            r.ribbonUv   = FromNames<RibbonUv>(jr.value("ribbonUv", std::string("stretch")), kRibbonUvNames).value_or(r.ribbonUv);
            r.ribbonTile = std::max(Finite(jr.value("ribbonTile", 1.0f), 1.0f), 0.01f);
            effect.emitters.push_back(std::move(e));
        }
        return effect;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

std::optional<ParticleEffect> LoadParticleEffect(const fs::path& file, std::string* error)
{
    const std::optional<std::string> text = Vfs::ReadText(file);
    if (!text) {
        if (error)
            *error = "'" + PathToUtf8(file) + "': cannot read";
        return std::nullopt;
    }
    std::string parseError;
    auto        effect = ParseParticleEffect(*text, file.parent_path(), &parseError);
    if (!effect && error)
        *error = "'" + PathToUtf8(file) + "': " + parseError;
    return effect;
}

bool SaveParticleEffect(const fs::path& file, const ParticleEffect& effect, std::string* error)
{
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << ParticleEffectToJson(effect, file.parent_path());
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

std::vector<std::string> ValidateParticleEffect(const ParticleEffect& effect)
{
    std::vector<std::string> problems;
    // Sub-emitter graph: edges emitter -> target; a cycle would spawn forever.
    const std::size_t                     n = effect.emitters.size();
    std::vector<std::vector<std::size_t>> edges(n);
    std::vector<bool>                     spawnedByEvent(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        const ParticleEmitter& e = effect.emitters[i];
        for (const ParticleModule& m : e.modules) {
            if (!m.enabled)
                continue;
            if (const auto* sub = std::get_if<SubEmitterModule>(&m.params)) {
                const auto target = effect.IndexOf(sub->emitter);
                if (!target)
                    problems.push_back("'" + e.name + "': sub emitter target '" + sub->emitter + "' does not exist");
                else {
                    edges[i].push_back(*target);
                    spawnedByEvent[*target] = true;
                }
            }
        }
        if (e.render.type == ParticleRenderType::Mesh && e.render.mesh.empty())
            problems.push_back("'" + e.name + "': mesh renderer without a mesh");
    }
    for (std::size_t i = 0; i < n; ++i) {
        const ParticleEmitter& e      = effect.emitters[i];
        const bool             spawns = std::ranges::any_of(e.modules, [](const ParticleModule& m) {
            return m.enabled && ModuleInfo(m.Type()).stage == ParticleStage::Spawn;
        });
        if (e.enabled && !spawns && !spawnedByEvent[i])
            problems.push_back("'" + e.name + "': never spawns (no spawn module and no sub emitter targets it)");
    }
    // Cycle detection (iterative DFS colors).
    std::vector<int> color(n, 0);
    for (std::size_t start = 0; start < n; ++start) {
        if (color[start] != 0)
            continue;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{start, 0}};
        color[start] = 1;
        while (!stack.empty()) {
            auto& [node, next] = stack.back();
            if (next < edges[node].size()) {
                const std::size_t to = edges[node][next++];
                if (color[to] == 1) {
                    problems.push_back("'" + effect.emitters[to].name + "': sub emitters form a cycle");
                } else if (color[to] == 0) {
                    color[to] = 1;
                    stack.push_back({to, 0});
                }
            } else {
                color[node] = 2;
                stack.pop_back();
            }
        }
    }
    return problems;
}

} // namespace Engine
