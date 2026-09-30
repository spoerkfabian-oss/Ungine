#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

using json = nlohmann::json;

namespace {

constexpr int kSceneVersion = 1;

// --- Values -----------------------------------------------------------------------------------

json ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json ToJson(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }
json ToJson(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

// Missing or mistyped fields keep their current value (older or hand-edited files).
template <class T>
void Read(const json& j, const char* key, T& value)
{
    if (const auto it = j.find(key); it != j.end() && !it->is_null())
        value = it->get<T>();
}
void Read(const json& j, const char* key, glm::vec3& v)
{
    if (const auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 3)
        v = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}
void Read(const json& j, const char* key, glm::vec4& v)
{
    if (const auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 4)
        v = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>(), (*it)[3].get<float>()};
}
void Read(const json& j, const char* key, glm::quat& q)
{
    if (const auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 4)
        q = glm::normalize(glm::quat((*it)[3].get<float>(), (*it)[0].get<float>(), (*it)[1].get<float>(),
                                     (*it)[2].get<float>()));
}
template <class E>
void ReadEnum(const json& j, const char* key, E& value)
{
    std::string name;
    Read(j, key, name);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(E::Count); ++i)
        if (name == ToString(static_cast<E>(i)))
            value = static_cast<E>(i);
}

std::string ToUtf8(const std::filesystem::path& path)
{
    const std::u8string s = path.generic_u8string();
    return {s.begin(), s.end()};
}
std::filesystem::path FromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// --- Components -------------------------------------------------------------------------------

json LightToJson(const Light& l)
{
    return {{"type", l.type == LightType::Spot ? "spot" : "point"},
            {"color", ToJson(l.color)},
            {"intensity", l.intensity},
            {"range", l.range},
            {"innerCone", l.innerConeAngle},
            {"outerCone", l.outerConeAngle},
            {"castShadows", l.castShadows}};
}

Light LightFromJson(const json& j)
{
    Light       l;
    std::string type = "point";
    Read(j, "type", type);
    l.type = type == "spot" ? LightType::Spot : LightType::Point;
    Read(j, "color", l.color);
    Read(j, "intensity", l.intensity);
    Read(j, "range", l.range);
    Read(j, "innerCone", l.innerConeAngle);
    Read(j, "outerCone", l.outerConeAngle);
    Read(j, "castShadows", l.castShadows);
    return l;
}

template <class E, std::size_t N>
json EnumToJson(E value, const char* const (&names)[N])
{
    return names[static_cast<std::size_t>(value)];
}
template <class E, std::size_t N>
void ReadEnum(const json& j, const char* key, E& value, const char* const (&names)[N])
{
    std::string name;
    Read(j, key, name);
    for (std::size_t i = 0; i < N; ++i)
        if (name == names[i])
            value = static_cast<E>(i);
}

constexpr const char* kBodyTypes[]      = {"static", "kinematic", "dynamic"};
constexpr const char* kColliderShapes[] = {"box", "sphere", "capsule", "mesh"};

json RigidBodyToJson(const RigidBody& b)
{
    return {{"type", EnumToJson(b.type, kBodyTypes)}, {"mass", b.mass},
            {"linearDamping", b.linearDamping},       {"angularDamping", b.angularDamping},
            {"gravityFactor", b.gravityFactor},       {"allowSleeping", b.allowSleeping},
            {"continuous", b.continuous}};
}

RigidBody RigidBodyFromJson(const json& j)
{
    RigidBody b;
    ReadEnum(j, "type", b.type, kBodyTypes);
    Read(j, "mass", b.mass);
    Read(j, "linearDamping", b.linearDamping);
    Read(j, "angularDamping", b.angularDamping);
    Read(j, "gravityFactor", b.gravityFactor);
    Read(j, "allowSleeping", b.allowSleeping);
    Read(j, "continuous", b.continuous);
    return b;
}

json ColliderToJson(const Collider& c)
{
    return {{"shape", EnumToJson(c.shape, kColliderShapes)},
            {"halfExtents", ToJson(c.halfExtents)},
            {"radius", c.radius},
            {"halfHeight", c.halfHeight},
            {"center", ToJson(c.center)},
            {"friction", c.friction},
            {"restitution", c.restitution},
            {"trigger", c.trigger},
            {"layer", c.layer}};
}

Collider ColliderFromJson(const json& j)
{
    Collider c;
    ReadEnum(j, "shape", c.shape, kColliderShapes);
    Read(j, "halfExtents", c.halfExtents);
    Read(j, "radius", c.radius);
    Read(j, "halfHeight", c.halfHeight);
    Read(j, "center", c.center);
    Read(j, "friction", c.friction);
    Read(j, "restitution", c.restitution);
    Read(j, "trigger", c.trigger);
    Read(j, "layer", c.layer);
    c.layer = static_cast<std::uint8_t>(std::min<unsigned>(c.layer, 15u));
    return c;
}

json PhysicsSettingsToJson(const PhysicsSettings& p)
{
    return {{"gravity", ToJson(p.gravity)},
            {"collisionSteps", p.collisionSteps},
            {"airControl", p.airControl},
            {"interpolate", p.interpolate},
            {"layerCollision", p.layerCollision}};
}

void PhysicsSettingsFromJson(const json& j, PhysicsSettings& p)
{
    Read(j, "gravity", p.gravity);
    Read(j, "collisionSteps", p.collisionSteps);
    p.collisionSteps = std::clamp(p.collisionSteps, 1, 16);
    Read(j, "airControl", p.airControl);
    Read(j, "interpolate", p.interpolate);
    if (const auto it = j.find("layerCollision"); it != j.end() && it->is_array() && it->size() == kPhysicsLayers)
        for (std::uint32_t a = 0; a < kPhysicsLayers; ++a)
            for (std::uint32_t b = 0; b < kPhysicsLayers; ++b) // symmetric by construction
                p.SetLayerCollision(a, b, ((it->at(a).get<std::uint32_t>() >> b) & 1u) != 0 &&
                                              ((it->at(b).get<std::uint32_t>() >> a) & 1u) != 0);
}

json CharacterToJson(const CharacterController& c)
{
    return {{"radius", c.radius},         {"height", c.height},         {"maxSlope", c.maxSlope},
            {"stepHeight", c.stepHeight}, {"jumpSpeed", c.jumpSpeed}};
}

CharacterController CharacterFromJson(const json& j)
{
    CharacterController c;
    Read(j, "radius", c.radius);
    Read(j, "height", c.height);
    Read(j, "maxSlope", c.maxSlope);
    Read(j, "stepHeight", c.stepHeight);
    Read(j, "jumpSpeed", c.jumpSpeed);
    return c;
}

json AudioSourceToJson(const AudioSource& a, const std::string& sound)
{
    return {{"sound", sound},
            {"bus", ToString(a.bus)},
            {"volume", a.volume},
            {"pitch", a.pitch},
            {"loop", a.loop},
            {"playOnStart", a.playOnStart},
            {"stream", a.stream},
            {"fadeIn", a.fadeIn},
            {"spatial", a.spatial},
            {"attenuation", ToString(a.attenuation)},
            {"minDistance", a.minDistance},
            {"maxDistance", a.maxDistance},
            {"rolloff", a.rolloff},
            {"doppler", a.doppler},
            {"occlusion", a.occlusion}};
}

AudioSource AudioSourceFromJson(const json& j)
{
    AudioSource a;
    Read(j, "sound", a.sound);
    if (const auto it = j.find("bus"); it != j.end() && it->is_string())
        a.bus = AudioBusFromString(it->get<std::string>()).value_or(AudioBus::World);
    if (a.bus == AudioBus::Master)
        a.bus = AudioBus::World;
    Read(j, "volume", a.volume);
    Read(j, "pitch", a.pitch);
    Read(j, "loop", a.loop);
    Read(j, "playOnStart", a.playOnStart);
    Read(j, "stream", a.stream);
    Read(j, "fadeIn", a.fadeIn);
    Read(j, "spatial", a.spatial);
    if (const auto it = j.find("attenuation"); it != j.end() && it->is_string())
        a.attenuation = AttenuationFromString(it->get<std::string>()).value_or(Attenuation::Inverse);
    Read(j, "minDistance", a.minDistance);
    Read(j, "maxDistance", a.maxDistance);
    Read(j, "rolloff", a.rolloff);
    Read(j, "doppler", a.doppler);
    Read(j, "occlusion", a.occlusion);
    a.volume      = std::max(a.volume, 0.0f);
    a.pitch       = std::clamp(a.pitch, 0.01f, 16.0f);
    a.minDistance = std::max(a.minDistance, 0.01f);
    a.maxDistance = std::max(a.maxDistance, a.minDistance);
    return a;
}

json ReverbToJson(const ReverbParams& p)
{
    return {{"roomSize", p.roomSize}, {"damping", p.damping}, {"wet", p.wet}, {"width", p.width}};
}

ReverbParams ReverbFromJson(const json& j, ReverbParams p)
{
    Read(j, "roomSize", p.roomSize);
    Read(j, "damping", p.damping);
    Read(j, "wet", p.wet);
    Read(j, "width", p.width);
    return p;
}

json ReverbZoneToJson(const ReverbZone& z)
{
    return {{"halfExtents", ToJson(z.halfExtents)}, {"blendDistance", z.blendDistance}, {"reverb", ReverbToJson(z.reverb)}};
}

ReverbZone ReverbZoneFromJson(const json& j)
{
    ReverbZone z;
    Read(j, "halfExtents", z.halfExtents);
    Read(j, "blendDistance", z.blendDistance);
    if (const auto it = j.find("reverb"); it != j.end())
        z.reverb = ReverbFromJson(*it, z.reverb);
    z.halfExtents   = glm::max(z.halfExtents, glm::vec3(0.0f));
    z.blendDistance = std::max(z.blendDistance, 0.0f);
    return z;
}

json PrimitiveToJson(const PrimitiveDesc& p)
{
    return {{"shape", ToString(p.shape)}, {"size", p.size},          {"baseColor", ToJson(p.baseColor)},
            {"metallic", p.metallic},     {"roughness", p.roughness}};
}

PrimitiveDesc PrimitiveFromJson(const json& j)
{
    PrimitiveDesc p;
    std::string   shape = ToString(p.shape);
    Read(j, "shape", shape);
    p.shape = PrimitiveShapeFromString(shape).value_or(PrimitiveShape::Box);
    Read(j, "size", p.size);
    Read(j, "baseColor", p.baseColor);
    Read(j, "metallic", p.metallic);
    Read(j, "roughness", p.roughness);
    return p;
}

// How MeshRenderer models are written and read.
struct ModelRefs {
    AssetManager*       assets = nullptr; // null: raw handles (in-memory snapshots)
    const AssetManager* constAssets = nullptr;
    std::filesystem::path baseDir;        // scene file directory
    std::unordered_map<std::string, ModelHandle> acquired; // file / primitive key -> handle (loading)
    std::unordered_set<std::uint32_t>            warned;   // unsaveable models (saving)

    [[nodiscard]] bool Memory() const { return !assets && !constAssets; }

    json Write(ModelHandle handle)
    {
        if (Memory())
            return {{"handle", {handle.index, handle.generation}}};
        const ModelSource source = constAssets->Source(handle);
        if (source.primitive)
            return {{"primitive", PrimitiveToJson(*source.primitive)}};
        if (!source.file.empty()) {
            std::error_code ec;
            const std::filesystem::path relative = std::filesystem::relative(source.file, baseDir, ec);
            return {{"file", ToUtf8(ec || relative.empty() ? source.file : relative)}};
        }
        if (warned.insert(handle.index).second)
            ENGINE_WARN("Scene save: a generated model (AssetManager::CreateModel) cannot be saved - its mesh "
                        "renderers are skipped");
        return nullptr;
    }

    // Other files (scripts): relative to the scene file when saving, back to absolute on load.
    [[nodiscard]] std::string WritePath(const std::string& path) const
    {
        if (Memory() || path.empty())
            return path;
        std::error_code             ec;
        const std::filesystem::path absolute = std::filesystem::absolute(FromUtf8(path), ec);
        const std::filesystem::path relative = std::filesystem::relative(absolute, baseDir, ec);
        return ToUtf8(ec || relative.empty() ? absolute : relative);
    }
    [[nodiscard]] std::string ReadPath(const std::string& path) const
    {
        std::filesystem::path file = FromUtf8(path);
        if (Memory() || path.empty() || !file.is_relative())
            return path;
        return ToUtf8((baseDir / file).lexically_normal());
    }

    ModelHandle Read(const json& j)
    {
        if (Memory()) {
            const json& h = j.at("handle");
            return {h.at(0).get<std::uint32_t>(), h.at(1).get<std::uint32_t>()};
        }
        if (const auto it = j.find("primitive"); it != j.end()) {
            const PrimitiveDesc desc = PrimitiveFromJson(*it);
            auto [slot, added]       = acquired.try_emplace(PrimitiveKey(desc));
            if (added)
                slot->second = assets->CreatePrimitive(desc);
            return slot->second;
        }
        std::filesystem::path file = FromUtf8(j.at("file").get<std::string>());
        if (file.is_relative())
            file = baseDir / file;
        auto [slot, added] = acquired.try_emplace(ToUtf8(file.lexically_normal()));
        if (added)
            slot->second = assets->LoadModel(file);
        return slot->second;
    }
};

// UUID + components (the hierarchy is added by the callers).
json EntityToJson(const Registry& r, Entity e, ModelRefs& models)
{
    const Transform& t = r.Get<Transform>(e);
    json             j{{"uuid", r.Get<Uuid>(e).value},
                       {"name", r.Get<Name>(e).value},
                       {"transform",
                        {{"position", ToJson(t.position)}, {"rotation", ToJson(t.rotation)}, {"scale", ToJson(t.scale)}}}};
    if (const auto* mesh = r.TryGet<MeshRenderer>(e)) {
        json model = models.Write(mesh->model);
        if (!model.is_null())
            j["mesh"] = {{"model", std::move(model)}, {"index", mesh->meshIndex}};
    }
    if (const auto* light = r.TryGet<Light>(e))
        j["light"] = LightToJson(*light);
    if (const auto* instance = r.TryGet<ModelInstance>(e)) {
        json model = models.Write(instance->model);
        if (!model.is_null())
            j["modelInstance"] = {{"model", std::move(model)}};
    }
    if (const auto* node = r.TryGet<ModelNodeRef>(e))
        j["modelNode"] = node->node;
    if (const auto* body = r.TryGet<RigidBody>(e))
        j["rigidBody"] = RigidBodyToJson(*body);
    if (const auto* collider = r.TryGet<Collider>(e))
        j["collider"] = ColliderToJson(*collider);
    if (const auto* character = r.TryGet<CharacterController>(e))
        j["character"] = CharacterToJson(*character);
    if (const auto* cam = r.TryGet<CameraComponent>(e))
        j["cameraComponent"] = {{"fovY", cam->fovY}, {"nearPlane", cam->nearPlane}, {"primary", cam->primary}};
    if (const auto* script = r.TryGet<ScriptComponent>(e))
        j["script"] = {{"graph", models.WritePath(script->graph)}};
    if (const auto* source = r.TryGet<AudioSource>(e))
        j["audioSource"] = AudioSourceToJson(*source, models.WritePath(source->sound));
    if (r.Has<AudioListener>(e))
        j["audioListener"] = json::object();
    if (const auto* zone = r.TryGet<ReverbZone>(e))
        j["reverbZone"] = ReverbZoneToJson(*zone);
    return j;
}

template <class T, class F>
void ApplyOptional(Registry& r, Entity e, const json& j, const char* key, F fromJson)
{
    if (const auto it = j.find(key); it != j.end())
        r.EmplaceOrReplace<T>(e, fromJson(*it));
    else
        r.Remove<T>(e);
}

// Overwrites the components with the JSON's; components missing there are removed.
void ApplyComponents(Scene& scene, Entity e, const json& j, ModelRefs& models)
{
    Registry& r = scene.GetRegistry();
    Read(j, "name", r.Get<Name>(e).value);
    if (const auto it = j.find("transform"); it != j.end()) {
        Transform t;
        Read(*it, "position", t.position);
        Read(*it, "rotation", t.rotation);
        Read(*it, "scale", t.scale);
        scene.SetTransform(e, t);
    }
    if (const auto it = j.find("mesh"); it != j.end()) {
        MeshRenderer mesh{.model = models.Read(it->at("model")), .meshIndex = 0};
        Read(*it, "index", mesh.meshIndex);
        r.EmplaceOrReplace<MeshRenderer>(e, mesh);
    } else {
        r.Remove<MeshRenderer>(e);
    }
    if (const auto it = j.find("light"); it != j.end())
        r.EmplaceOrReplace<Light>(e, LightFromJson(*it));
    else
        r.Remove<Light>(e);
    if (const auto it = j.find("modelInstance"); it != j.end())
        r.EmplaceOrReplace<ModelInstance>(e, ModelInstance{.model = models.Read(it->at("model"))});
    else
        r.Remove<ModelInstance>(e);
    if (const auto it = j.find("modelNode"); it != j.end())
        r.EmplaceOrReplace<ModelNodeRef>(e, ModelNodeRef{.node = it->get<std::uint32_t>()});
    else
        r.Remove<ModelNodeRef>(e);
    ApplyOptional<RigidBody>(r, e, j, "rigidBody", RigidBodyFromJson);
    ApplyOptional<Collider>(r, e, j, "collider", ColliderFromJson);
    ApplyOptional<CharacterController>(r, e, j, "character", CharacterFromJson);
    ApplyOptional<CameraComponent>(r, e, j, "cameraComponent", [](const json& c) {
        CameraComponent cam;
        Read(c, "fovY", cam.fovY);
        Read(c, "nearPlane", cam.nearPlane);
        Read(c, "primary", cam.primary);
        cam.fovY      = std::clamp(cam.fovY, 0.05f, 3.0f);
        cam.nearPlane = std::max(cam.nearPlane, 1e-4f);
        return cam;
    });
    ApplyOptional<ScriptComponent>(r, e, j, "script", [&](const json& s) {
        ScriptComponent script;
        Read(s, "graph", script.graph);
        script.graph = models.ReadPath(script.graph);
        return script;
    });
    ApplyOptional<AudioSource>(r, e, j, "audioSource", [&](const json& a) {
        AudioSource source = AudioSourceFromJson(a);
        source.sound       = models.ReadPath(source.sound);
        return source;
    });
    ApplyOptional<AudioListener>(r, e, j, "audioListener", [](const json&) { return AudioListener{}; });
    ApplyOptional<ReverbZone>(r, e, j, "reverbZone", ReverbZoneFromJson);
    scene.MarkChanged(e); // bounds / shadow caches
}

// Pre-order (parents first) over the subtree of `root`, children in their order.
void CollectSubtree(const Registry& r, Entity root, std::vector<Entity>& out)
{
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        out.push_back(e);
        const auto& children = r.Get<Hierarchy>(e).children;
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
}

std::uint64_t UuidOf(const Registry& r, Entity e)
{
    return e == NullEntity ? 0 : r.Get<Uuid>(e).value;
}

// --- Settings ---------------------------------------------------------------------------------

json RendererToJson(const SceneRenderer& sr)
{
    const SkySettings& sky = sr.lighting.sky;
    const PostSettings& p  = sr.post;
    const ShadowSettings& s = sr.shadows;
    const AoSettings& ao    = sr.ao;
    const LocalShadowSettings& ls = sr.localShadows;
    return {
        {"lighting",
         {{"sunDirection", ToJson(sky.sunDirection)}, {"sunColor", ToJson(sky.sunColor)},
          {"sunIntensity", sky.sunIntensity}, {"skyIntensity", sky.skyIntensity},
          {"iblIntensity", sr.lighting.iblIntensity}}},
        {"post",
         {{"exposure", p.exposure}, {"tonemapper", ToString(p.tonemapper)}, {"bloom", p.bloom},
          {"bloomStrength", p.bloomStrength}, {"bloomRadius", p.bloomRadius}, {"autoExposure", p.autoExposure},
          {"exposureKey", p.exposureKey}, {"adaptationSpeed", p.adaptationSpeed},
          {"minLogLuminance", p.minLogLuminance}, {"maxLogLuminance", p.maxLogLuminance}}},
        {"shadows",
         {{"enabled", s.enabled}, {"cascadeCount", s.cascadeCount}, {"resolution", s.resolution},
          {"maxDistance", s.maxDistance}, {"splitLambda", s.splitLambda}, {"depthBias", s.depthBias},
          {"slopeBias", s.slopeBias}, {"normalBias", s.normalBias}, {"filterRadius", s.filterRadius},
          {"cascadeBlend", s.cascadeBlend}}},
        {"ao",
         {{"enabled", ao.enabled}, {"radius", ao.radius}, {"falloff", ao.falloff}, {"power", ao.power},
          {"sliceCount", ao.sliceCount}, {"stepsPerSide", ao.stepsPerSide}, {"sharpness", ao.sharpness}}},
        {"lights", {{"enabled", sr.lights.enabled}, {"clusterFar", sr.lights.clusterFar}}},
        {"localShadows",
         {{"enabled", ls.enabled}, {"atlasSize", ls.atlasSize}, {"maxLights", ls.maxLights},
          {"maxTileSize", ls.maxTileSize}, {"minTileSize", ls.minTileSize}, {"depthBias", ls.depthBias},
          {"slopeBias", ls.slopeBias}, {"normalBias", ls.normalBias}, {"filterRadius", ls.filterRadius}}},
        {"culling", {{"gpuDriven", sr.culling.gpuDriven}, {"occlusion", sr.culling.occlusion}, {"lod", sr.culling.lod},
                     {"lodPixelError", sr.culling.lodPixelError}}}, // no debug state
    };
}

void RendererFromJson(const json& j, SceneRenderer& sr)
{
    if (const auto it = j.find("lighting"); it != j.end()) {
        Read(*it, "sunDirection", sr.lighting.sky.sunDirection);
        Read(*it, "sunColor", sr.lighting.sky.sunColor);
        Read(*it, "sunIntensity", sr.lighting.sky.sunIntensity);
        Read(*it, "skyIntensity", sr.lighting.sky.skyIntensity);
        Read(*it, "iblIntensity", sr.lighting.iblIntensity);
    }
    if (const auto it = j.find("post"); it != j.end()) {
        PostSettings& p = sr.post;
        Read(*it, "exposure", p.exposure);
        ReadEnum(*it, "tonemapper", p.tonemapper);
        Read(*it, "bloom", p.bloom);
        Read(*it, "bloomStrength", p.bloomStrength);
        Read(*it, "bloomRadius", p.bloomRadius);
        Read(*it, "autoExposure", p.autoExposure);
        Read(*it, "exposureKey", p.exposureKey);
        Read(*it, "adaptationSpeed", p.adaptationSpeed);
        Read(*it, "minLogLuminance", p.minLogLuminance);
        Read(*it, "maxLogLuminance", p.maxLogLuminance);
    }
    if (const auto it = j.find("shadows"); it != j.end()) {
        ShadowSettings& s = sr.shadows;
        Read(*it, "enabled", s.enabled);
        Read(*it, "cascadeCount", s.cascadeCount);
        Read(*it, "resolution", s.resolution);
        Read(*it, "maxDistance", s.maxDistance);
        Read(*it, "splitLambda", s.splitLambda);
        Read(*it, "depthBias", s.depthBias);
        Read(*it, "slopeBias", s.slopeBias);
        Read(*it, "normalBias", s.normalBias);
        Read(*it, "filterRadius", s.filterRadius);
        Read(*it, "cascadeBlend", s.cascadeBlend);
    }
    if (const auto it = j.find("ao"); it != j.end()) {
        AoSettings& ao = sr.ao;
        Read(*it, "enabled", ao.enabled);
        Read(*it, "radius", ao.radius);
        Read(*it, "falloff", ao.falloff);
        Read(*it, "power", ao.power);
        Read(*it, "sliceCount", ao.sliceCount);
        Read(*it, "stepsPerSide", ao.stepsPerSide);
        Read(*it, "sharpness", ao.sharpness);
    }
    if (const auto it = j.find("lights"); it != j.end()) {
        Read(*it, "enabled", sr.lights.enabled);
        Read(*it, "clusterFar", sr.lights.clusterFar);
    }
    if (const auto it = j.find("localShadows"); it != j.end()) {
        LocalShadowSettings& ls = sr.localShadows;
        Read(*it, "enabled", ls.enabled);
        Read(*it, "atlasSize", ls.atlasSize);
        Read(*it, "maxLights", ls.maxLights);
        Read(*it, "maxTileSize", ls.maxTileSize);
        Read(*it, "minTileSize", ls.minTileSize);
        Read(*it, "depthBias", ls.depthBias);
        Read(*it, "slopeBias", ls.slopeBias);
        Read(*it, "normalBias", ls.normalBias);
        Read(*it, "filterRadius", ls.filterRadius);
    }
    if (const auto it = j.find("culling"); it != j.end()) {
        Read(*it, "gpuDriven", sr.culling.gpuDriven);
        Read(*it, "occlusion", sr.culling.occlusion);
        Read(*it, "lod", sr.culling.lod);
        Read(*it, "lodPixelError", sr.culling.lodPixelError);
    }
}

json CameraToJson(const FlyCamera& c)
{
    return {{"position", ToJson(c.position)}, {"yaw", c.yaw},           {"pitch", c.pitch},
            {"fovY", c.fovY},                 {"nearPlane", c.nearPlane}, {"moveSpeed", c.moveSpeed}};
}

void CameraFromJson(const json& j, FlyCamera& c)
{
    Read(j, "position", c.position);
    Read(j, "yaw", c.yaw);
    Read(j, "pitch", c.pitch);
    Read(j, "fovY", c.fovY);
    Read(j, "nearPlane", c.nearPlane);
    Read(j, "moveSpeed", c.moveSpeed);
}

} // namespace

// --- Scene files ------------------------------------------------------------------------------

void SaveSceneFile(const std::filesystem::path& file, const Scene& scene, const AssetManager& assets,
                   const SceneFileOptions& options)
{
    const Registry& r = scene.GetRegistry();
    ModelRefs       models;
    models.constAssets = &assets;
    models.baseDir     = std::filesystem::absolute(file).parent_path();

    // Roots in creation-slot order (as the hierarchy panel shows them), then pre-order subtrees.
    std::vector<Entity> roots;
    for (std::uint32_t i = 0;; ++i) {
        const Entity e = r.EntityAtIndex(i);
        if (e == NullEntity)
            break;
        if (const auto* h = r.TryGet<Hierarchy>(e); h && h->parent == NullEntity)
            roots.push_back(e);
    }
    json entities = json::array();
    for (Entity root : roots) {
        std::vector<Entity> subtree;
        CollectSubtree(r, root, subtree);
        for (Entity e : subtree) {
            json j      = EntityToJson(r, e, models);
            j["parent"] = UuidOf(r, r.Get<Hierarchy>(e).parent);
            entities.push_back(std::move(j));
        }
    }

    json root{{"version", kSceneVersion}, {"entities", std::move(entities)}};
    if (options.renderer)
        root["renderer"] = RendererToJson(*options.renderer);
    if (options.camera)
        root["camera"] = CameraToJson(*options.camera);
    if (options.physics)
        root["physics"] = PhysicsSettingsToJson(*options.physics);

    // Write next to the target, then replace it: a failed save never truncates the old file.
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write '" + ToUtf8(temp) + "'");
        out << root.dump(2) << '\n';
        if (!out)
            throw std::runtime_error("write failed: '" + ToUtf8(temp) + "'");
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec)
        throw std::runtime_error("cannot replace '" + ToUtf8(file) + "': " + ec.message());
}

std::vector<ModelHandle> LoadSceneFile(const std::filesystem::path& file, Scene& scene, AssetManager& assets,
                                       const SceneFileOptions& options)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read '" + ToUtf8(file) + "'");
    json root;
    try {
        root = json::parse(in);
    } catch (const json::exception& e) {
        throw std::runtime_error("'" + ToUtf8(file) + "': " + e.what());
    }
    const int version = root.value("version", 0);
    if (version < 1 || version > kSceneVersion)
        throw std::runtime_error("'" + ToUtf8(file) + "': unsupported scene version " + std::to_string(version));

    ModelRefs models;
    models.assets      = &assets;
    models.constAssets = &assets;
    models.baseDir     = std::filesystem::absolute(file).parent_path();
    std::vector<Entity>                        created;
    std::unordered_map<std::uint64_t, Entity>  byFileUuid;
    try {
        for (const json& j : root.at("entities")) {
            const std::uint64_t uuid       = j.value<std::uint64_t>("uuid", 0);
            const std::uint64_t parentUuid = j.value<std::uint64_t>("parent", 0);
            const auto          parentIt   = byFileUuid.find(parentUuid);
            const Entity        parent     = parentIt != byFileUuid.end() ? parentIt->second : NullEntity;
            const Entity        e = scene.CreateEntity(j.value("name", std::string("Entity")), parent, uuid);
            created.push_back(e);
            byFileUuid[uuid] = e;
            ApplyComponents(scene, e, j, models);
        }
        if (options.renderer)
            if (const auto it = root.find("renderer"); it != root.end())
                RendererFromJson(*it, *options.renderer);
        if (options.camera)
            if (const auto it = root.find("camera"); it != root.end())
                CameraFromJson(*it, *options.camera);
        if (options.physics)
            if (const auto it = root.find("physics"); it != root.end())
                PhysicsSettingsFromJson(*it, *options.physics);
    } catch (const std::exception& e) {
        for (auto it = created.rbegin(); it != created.rend(); ++it)
            scene.DestroyEntity(*it);
        for (const auto& [key, handle] : models.acquired)
            assets.Release(handle);
        throw std::runtime_error("'" + ToUtf8(file) + "': " + e.what());
    }

    std::vector<ModelHandle> handles;
    for (const auto& [key, handle] : models.acquired)
        handles.push_back(handle);
    return handles;
}

// --- Snapshots --------------------------------------------------------------------------------

std::string SnapshotEntities(const Scene& scene, std::span<const Entity> roots)
{
    const Registry& r = scene.GetRegistry();
    ModelRefs       models;
    json            out = json::array();
    for (Entity root : roots) {
        std::vector<Entity> subtree;
        CollectSubtree(r, root, subtree);
        json entities = json::array();
        for (Entity e : subtree) {
            json j      = EntityToJson(r, e, models);
            j["parent"] = UuidOf(r, r.Get<Hierarchy>(e).parent);
            entities.push_back(std::move(j));
        }
        out.push_back({{"parent", UuidOf(r, r.Get<Hierarchy>(root).parent)},
                       {"index", scene.SiblingIndex(root)},
                       {"entities", std::move(entities)}});
    }
    return out.dump();
}

std::vector<Entity> RestoreEntities(Scene& scene, const std::string& snapshot, RestoreMode mode)
{
    const json          in = json::parse(snapshot);
    ModelRefs           models;
    std::vector<Entity> roots;
    for (const json& record : in) {
        const Entity parent = scene.FindByUuid(record.at("parent").get<std::uint64_t>());
        std::unordered_map<std::uint64_t, Entity> byOldUuid;
        bool first = true;
        for (const json& j : record.at("entities")) {
            const std::uint64_t oldUuid = j.at("uuid").get<std::uint64_t>();
            const std::uint64_t uuid    = mode == RestoreMode::Original ? oldUuid : 0;
            Entity              e;
            if (first) {
                e = scene.CreateEntity(j.value("name", std::string()), NullEntity, uuid);
                if (parent != NullEntity)
                    scene.SetParent(e, parent,
                                    mode == RestoreMode::Original ? record.at("index").get<std::size_t>() : Scene::kAppend);
                roots.push_back(e);
                first = false;
            } else {
                e = scene.CreateEntity(j.value("name", std::string()), byOldUuid.at(j.at("parent").get<std::uint64_t>()),
                                       uuid);
            }
            byOldUuid[oldUuid] = e;
            ApplyComponents(scene, e, j, models);
        }
    }
    return roots;
}

std::string SnapshotEntityState(const Scene& scene, Entity entity)
{
    ModelRefs models;
    return EntityToJson(scene.GetRegistry(), entity, models).dump();
}

void ApplyEntityState(Scene& scene, Entity entity, const std::string& state)
{
    ModelRefs models;
    ApplyComponents(scene, entity, json::parse(state), models);
}

namespace {
// Writes the differences between before and after into target (same structure).
void MergeDiff(const json& before, const json& after, json& target)
{
    if (after.is_object() && before.is_object() && target.is_object()) {
        for (auto it = after.begin(); it != after.end(); ++it) {
            const auto old = before.find(it.key());
            if (old == before.end())
                target[it.key()] = *it; // added
            else if (const auto t = target.find(it.key()); t != target.end())
                MergeDiff(*old, *it, *t);
            // else: a component the target does not have: skipped
        }
        for (auto it = before.begin(); it != before.end(); ++it)
            if (!after.contains(it.key()))
                target.erase(it.key()); // removed
    } else if (after.is_array() && before.is_array() && target.is_array() && after.size() == before.size() &&
               after.size() == target.size()) {
        for (std::size_t i = 0; i < after.size(); ++i)
            MergeDiff(before[i], after[i], target[i]);
    } else if (after != before) {
        target = after;
    }
}
} // namespace

bool ApplyEntityStateDiff(Scene& scene, Entity target, const std::string& before, const std::string& after)
{
    json from = json::parse(before);
    json to   = json::parse(after);
    for (const char* key : {"uuid", "name"}) { // identity is never shared
        from.erase(key);
        to.erase(key);
    }
    ModelRefs  models;
    json       state    = EntityToJson(scene.GetRegistry(), target, models);
    const json original = state;
    MergeDiff(from, to, state);
    if (state == original)
        return false;
    ApplyComponents(scene, target, state, models);
    return true;
}

} // namespace Engine
