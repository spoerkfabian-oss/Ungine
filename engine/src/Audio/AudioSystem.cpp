#include "Engine/Audio/AudioSystem.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Engine {

namespace {

constexpr float  kMaxDopplerSpeed   = 150.0f; // m/s: teleports must not scream
constexpr float  kRestartFade       = 0.03f;  // seconds, when a source's sound changes
constexpr double kOneShotMaxWait    = 1.0;    // seconds a one-shot may wait for its sound
constexpr float  kOcclusionMargin   = 0.5f;   // hits this close to the listener do not count

struct Pose {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

Pose PoseOf(const glm::mat4& world)
{
    Pose            p;
    p.position      = glm::vec3(world[3]);
    const glm::vec3 f = -glm::vec3(world[2]);
    const glm::vec3 u = glm::vec3(world[1]);
    if (glm::dot(f, f) > 1e-12f)
        p.forward = glm::normalize(f);
    if (glm::dot(u, u) > 1e-12f)
        p.up = glm::normalize(u);
    return p;
}

// Restart needed (vs. applied live) when these differ.
bool NeedsRestart(const AudioSource& a, const AudioSource& b)
{
    return a.sound != b.sound || a.stream != b.stream || a.bus != b.bus || a.spatial != b.spatial;
}

VoiceDesc DescFor(const AudioSource& a, const glm::vec3& position)
{
    VoiceDesc d;
    d.bus           = a.bus;
    d.volume        = a.volume;
    d.pitch         = a.pitch;
    d.loop          = a.loop;
    d.fadeInSeconds = a.fadeIn;
    d.spatial       = a.spatial;
    d.position      = position;
    d.attenuation   = a.attenuation;
    d.minDistance   = a.minDistance;
    d.maxDistance   = a.maxDistance;
    d.rolloff       = a.rolloff;
    d.doppler       = a.doppler;
    return d;
}

} // namespace

struct AudioSystem::Impl {
    struct SoundRef {
        SoundHandle                      handle; // with an AssetManager
        std::shared_ptr<const SoundData> data;   // without
        bool                             failed = false;
    };

    struct Source {
        AudioSource config;
        VoiceId     voice    = 0;
        bool        wantPlay = false;
        bool        hasPose  = false;
        glm::vec3   position{0.0f};
        float       occlusion = 0.0f;
    };

    struct OneShot {
        std::string key;
        VoiceDesc   desc;
        double      requested = 0.0;
    };

    AudioEngine&   engine;
    AssetManager*  assets;
    PhysicsWorld*  physics;
    AudioSettings  settings;
    AudioSystemStats stats;
    bool           running = false;
    bool           paused  = false;
    Scene*         activeScene = nullptr; // while running
    double         time    = 0.0;

    std::unordered_map<std::string, SoundRef> sounds;
    std::unordered_map<Entity, Source>        sources;
    std::vector<OneShot>                      pendingOneShots;
    std::vector<VoiceId>                      oneShots;
    VoiceId                                   preview = 0;
    std::string                               pendingPreview;
    std::size_t                               rayCursor = 0;

    bool      hasListener = false;
    glm::vec3 listenerPosition{0.0f};

    Impl(AudioEngine& e, AssetManager* a, PhysicsWorld* p) : engine(e), assets(a), physics(p) {}

    ~Impl()
    {
        StopAllVoices();
        if (assets)
            for (auto& [key, ref] : sounds)
                if (ref.handle)
                    assets->Release(ref.handle);
    }

    void StopAllVoices()
    {
        for (auto& [e, s] : sources)
            engine.Stop(s.voice);
        for (VoiceId v : oneShots)
            engine.Stop(v);
        engine.Stop(preview);
        sources.clear();
        oneShots.clear();
        pendingOneShots.clear();
        preview = 0;
        pendingPreview.clear();
    }

    static std::string Key(const std::string& path, bool stream) { return path + (stream ? "|stream" : "|auto"); }

    SoundRef& Acquire(const std::string& path, bool stream)
    {
        const std::string key = Key(path, stream);
        if (const auto it = sounds.find(key); it != sounds.end())
            return it->second;
        SoundRef& ref = sounds[key];
        const std::filesystem::path file = PathFromUtf8(path);
        const SoundLoadMode         mode = stream ? SoundLoadMode::Stream : SoundLoadMode::Auto;
        if (path.empty()) {
            ref.failed = true;
        } else if (assets) {
            ref.handle = assets->LoadSound(file, mode);
        } else {
            try {
                ref.data = std::make_shared<const SoundData>(LoadSoundFile(file, mode));
            } catch (const std::exception& e) {
                ENGINE_ERROR("Audio: {}", e.what());
                ref.failed = true;
            }
        }
        return ref;
    }

    // nullptr while loading (or failed: `failed` set).
    std::shared_ptr<const SoundData> Resolve(const std::string& key, bool& failed)
    {
        const auto it = sounds.find(key);
        if (it == sounds.end()) {
            failed = true;
            return nullptr;
        }
        SoundRef& ref = it->second;
        if (assets && ref.handle) {
            const AssetState state = assets->State(ref.handle);
            failed = state == AssetState::Failed || state == AssetState::Invalid;
            return assets->Get(ref.handle); // a failed reload keeps the old sound
        }
        failed = ref.failed;
        return ref.data;
    }

    std::shared_ptr<const SoundData> ResolveSource(const AudioSource& a, bool& failed)
    {
        Acquire(a.sound, a.stream);
        return Resolve(Key(a.sound, a.stream), failed);
    }

    bool QueueOneShot(const std::string& path, VoiceDesc desc)
    {
        desc.paused = paused;
        Acquire(path, false);
        const std::string key    = Key(path, false);
        bool              failed = false;
        if (auto data = Resolve(key, failed)) {
            const VoiceId v = engine.Play(std::move(data), desc);
            if (v)
                oneShots.push_back(v);
            return v != 0;
        }
        if (failed)
            return false;
        pendingOneShots.push_back({key, desc, time});
        return true;
    }

    void StartSource(Source& s, const glm::vec3& position)
    {
        bool failed = false;
        auto data   = ResolveSource(s.config, failed);
        if (!data) {
            if (failed)
                s.wantPlay = false; // the AssetManager logged why
            return;
        }
        s.wantPlay       = false;
        VoiceDesc desc   = DescFor(s.config, position);
        desc.paused      = paused;
        s.voice          = engine.Play(std::move(data), desc);
        engine.SetOcclusion(s.voice, s.occlusion);
    }

    void UpdateListener(Scene& scene, float dt, const CameraData* view)
    {
        Registry& r = scene.GetRegistry();
        Entity    listener = NullEntity;
        r.ViewOf<AudioListener>().Each([&](Entity e, AudioListener&) {
            if (listener == NullEntity)
                listener = e;
        });
        if (listener == NullEntity)
            listener = scene.FindPrimaryCamera();
        Pose pose;
        bool found = true;
        if (listener != NullEntity)
            pose = PoseOf(r.Get<WorldTransform>(listener).matrix);
        else if (view)
            pose = PoseOf(glm::inverse(view->view));
        else
            found = false;
        if (!found)
            return; // keep the last one
        glm::vec3 velocity(0.0f);
        if (hasListener && dt > 0.0f)
            velocity = (pose.position - listenerPosition) / dt;
        if (glm::length(velocity) > kMaxDopplerSpeed)
            velocity = glm::vec3(0.0f);
        hasListener      = true;
        listenerPosition = pose.position;
        engine.SetListener(pose.position, pose.forward, pose.up, velocity);
    }

    void UpdateSources(Scene& scene, float dt)
    {
        Registry&           r = scene.GetRegistry();
        std::vector<Entity> seen;
        r.ViewOf<AudioSource>().Each([&](Entity e, AudioSource& config) {
            seen.push_back(e);
            const glm::vec3 position = glm::vec3(r.Get<WorldTransform>(e).matrix[3]);
            auto [it, inserted]      = sources.try_emplace(e);
            Source& s                = it->second;
            if (inserted) {
                s.config   = config;
                s.wantPlay = config.playOnStart;
            } else if (!(s.config == config)) {
                const bool restart = NeedsRestart(s.config, config);
                const bool active  = s.voice && engine.IsPlaying(s.voice);
                s.config           = config;
                if (restart) {
                    engine.Stop(s.voice, kRestartFade);
                    s.voice    = 0;
                    s.wantPlay = s.wantPlay || active;
                } else if (s.voice) {
                    engine.SetVolume(s.voice, config.volume, 0.05f);
                    engine.SetPitch(s.voice, config.pitch);
                    engine.SetLooping(s.voice, config.loop);
                    engine.SetSpatialRange(s.voice, config.minDistance, config.maxDistance, config.rolloff);
                }
            }
            if (s.voice && !engine.IsPlaying(s.voice))
                s.voice = 0;
            if (s.wantPlay && !s.voice)
                StartSource(s, position);
            if (s.voice && s.config.spatial) {
                glm::vec3 velocity(0.0f);
                if (s.hasPose && dt > 0.0f)
                    velocity = (position - s.position) / dt;
                if (glm::length(velocity) > kMaxDopplerSpeed)
                    velocity = glm::vec3(0.0f);
                engine.SetTransform(s.voice, position, velocity);
            }
            s.position = position;
            s.hasPose  = true;
        });
        // Removed components / destroyed entities.
        std::ranges::sort(seen);
        std::erase_if(sources, [&](const auto& entry) {
            if (std::ranges::binary_search(seen, entry.first))
                return false;
            engine.Stop(entry.second.voice, kRestartFade);
            return true;
        });
    }

    void UpdateOcclusion(Scene& scene)
    {
        stats.rays = 0;
        if (!physics || !settings.occlusion || !hasListener || sources.empty()) {
            for (auto& [e, s] : sources)
                if (s.occlusion > 0.0f) {
                    s.occlusion = 0.0f;
                    engine.SetOcclusion(s.voice, 0.0f);
                }
            return;
        }
        std::vector<Entity> candidates;
        for (auto& [e, s] : sources)
            if (s.voice && s.config.spatial && s.config.occlusion)
                candidates.push_back(e);
        if (candidates.empty())
            return;
        std::ranges::sort(candidates); // stable round-robin order
        const std::size_t count = std::min<std::size_t>(candidates.size(), settings.occlusionRays);
        for (std::size_t i = 0; i < count; ++i) {
            const Entity e = candidates[(rayCursor + i) % candidates.size()];
            Source&      s = sources[e];
            // From the source towards the listener: a character capsule around the listener is
            // reached last and ignored by the margin.
            const glm::vec3 toListener = listenerPosition - s.position;
            const float     distance   = glm::length(toListener);
            bool            blocked    = false;
            if (distance > kOcclusionMargin) {
                const glm::vec3 dir    = toListener / distance;
                glm::vec3       origin = s.position;
                float           range  = distance;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    ++stats.rays;
                    const auto hit = physics->Raycast(origin, dir, range, e);
                    if (!hit || hit->distance > range - kOcclusionMargin)
                        break;
                    if (hit->entity != NullEntity && scene.IsAncestor(hit->entity, e) && attempt == 0) {
                        // Started inside the body the source is attached to: step out of it.
                        origin += dir * kOcclusionMargin;
                        range -= kOcclusionMargin;
                        continue;
                    }
                    blocked = true;
                    break;
                }
            }
            const float target = blocked ? std::clamp(settings.occlusionStrength, 0.0f, 1.0f) : 0.0f;
            if (target != s.occlusion) {
                s.occlusion = target;
                engine.SetOcclusion(s.voice, target);
            }
        }
        rayCursor = (rayCursor + count) % candidates.size();
    }

    void UpdateReverb(Scene& scene)
    {
        Registry&    r = scene.GetRegistry();
        ReverbParams sum{.roomSize = 0.0f, .damping = 0.0f, .wet = 0.0f, .width = 0.0f};
        float        total = 0.0f;
        stats.zones        = 0;
        if (hasListener)
            r.ViewOf<ReverbZone>().Each([&](Entity e, ReverbZone& zone) {
                const glm::mat4& world   = r.Get<WorldTransform>(e).matrix;
                const glm::vec3  local   = glm::vec3(glm::inverse(world) * glm::vec4(listenerPosition, 1.0f));
                const glm::vec3  clamped = glm::clamp(local, -zone.halfExtents, zone.halfExtents);
                const float      outside = glm::length(glm::vec3(world * glm::vec4(clamped, 1.0f)) - listenerPosition);
                float            weight  = 0.0f;
                if (outside <= 1e-4f)
                    weight = 1.0f;
                else if (zone.blendDistance > 0.0f)
                    weight = std::max(0.0f, 1.0f - outside / zone.blendDistance);
                if (weight <= 0.0f)
                    return;
                ++stats.zones;
                total += weight;
                sum.roomSize += weight * zone.reverb.roomSize;
                sum.damping += weight * zone.reverb.damping;
                sum.wet += weight * zone.reverb.wet;
                sum.width += weight * zone.reverb.width;
            });
        ReverbParams result; // defaults: dry
        if (total > 0.0f) {
            result.roomSize = sum.roomSize / total;
            result.damping  = sum.damping / total;
            result.width    = sum.width / total;
            result.wet      = sum.wet / std::max(total, 1.0f); // fades out towards the zone border
        }
        stats.reverb = result;
        engine.SetReverb(result);
    }

    void UpdateOneShots()
    {
        std::erase_if(pendingOneShots, [&](const OneShot& shot) {
            bool failed = false;
            if (auto data = Resolve(shot.key, failed)) {
                if (const VoiceId v = engine.Play(std::move(data), shot.desc))
                    oneShots.push_back(v);
                return true;
            }
            return failed || time - shot.requested > kOneShotMaxWait;
        });
        std::erase_if(oneShots, [&](VoiceId v) { return !engine.IsPlaying(v); });
        if (!pendingPreview.empty()) {
            bool failed = false;
            if (auto data = Resolve(Key(pendingPreview, false), failed)) {
                VoiceDesc desc;
                desc.bus = AudioBus::Ui;
                preview  = engine.Play(std::move(data), desc);
                pendingPreview.clear();
            } else if (failed) {
                pendingPreview.clear();
            }
        }
        if (preview && !engine.IsPlaying(preview))
            preview = 0;
    }
};

AudioSystem::AudioSystem(AudioEngine& engine, AssetManager* assets, PhysicsWorld* physics)
    : m_Impl(std::make_unique<Impl>(engine, assets, physics))
{
    Apply({});
}

AudioSystem::~AudioSystem() = default;

void AudioSystem::Begin(Scene& scene)
{
    Impl& s = *m_Impl;
    if (s.running)
        End(scene);
    s.running = true;
    s.activeScene = &scene;
    s.sources.clear();
    s.hasListener = false;
}

void AudioSystem::Update(Scene& scene, float dt, const CameraData* view)
{
    Impl& s = *m_Impl;
    s.time += dt;
    s.UpdateListener(scene, dt, view);
    if (s.running) {
        s.UpdateSources(scene, dt);
        s.UpdateOcclusion(scene);
        s.UpdateReverb(scene);
    } else {
        s.engine.SetReverb({});
        s.stats.reverb = {};
        s.stats.zones  = 0;
        s.stats.rays   = 0;
    }
    s.UpdateOneShots();
    s.engine.Update();

    AudioSystemStats& st = s.stats;
    st.sources = static_cast<std::uint32_t>(s.sources.size());
    st.playing = st.waiting = st.occluded = 0;
    for (const auto& [e, source] : s.sources) {
        st.playing += source.voice ? 1u : 0u;
        st.waiting += source.wantPlay && !source.voice ? 1u : 0u;
        st.occluded += source.voice && source.occlusion > 0.0f ? 1u : 0u;
    }
    st.oneShots = static_cast<std::uint32_t>(s.oneShots.size());
    st.listener = s.listenerPosition;
}

void AudioSystem::End(Scene& /*scene*/)
{
    Impl& s = *m_Impl;
    for (auto& [e, source] : s.sources)
        s.engine.Stop(source.voice);
    for (VoiceId v : s.oneShots)
        s.engine.Stop(v);
    s.sources.clear();
    s.oneShots.clear();
    s.pendingOneShots.clear();
    s.engine.SetReverb({});
    s.engine.Update();
    s.running = false;
    s.activeScene = nullptr;
}

bool AudioSystem::Running() const { return m_Impl->running; }

void AudioSystem::SetPaused(bool paused)
{
    Impl& s = *m_Impl;
    if (s.paused == paused)
        return;
    s.paused = paused;
    for (auto& [e, source] : s.sources)
        s.engine.SetPaused(source.voice, paused);
    for (VoiceId v : s.oneShots)
        s.engine.SetPaused(v, paused);
    for (Impl::OneShot& shot : s.pendingOneShots)
        shot.desc.paused = paused;
}

bool AudioSystem::Paused() const { return m_Impl->paused; }

bool AudioSystem::Play(Entity entity)
{
    Impl& s = *m_Impl;
    if (!s.running)
        return false;
    auto it = s.sources.find(entity);
    if (it == s.sources.end()) { // added since the last Update (e.g. BeginPlay): sync it now
        Registry& r = s.activeScene->GetRegistry();
        const AudioSource* config = r.Valid(entity) ? r.TryGet<AudioSource>(entity) : nullptr;
        if (!config)
            return false;
        it = s.sources.try_emplace(entity).first;
        it->second.config   = *config;
        it->second.position = glm::vec3(r.Get<WorldTransform>(entity).matrix[3]);
        it->second.hasPose  = true;
    }
    Impl::Source& source = it->second;
    s.engine.Stop(source.voice, kRestartFade);
    source.voice = 0;
    source.wantPlay = true;
    s.StartSource(source, source.position);
    return true;
}

void AudioSystem::Stop(Entity entity, float fadeOutSeconds)
{
    Impl& s  = *m_Impl;
    auto  it = s.sources.find(entity);
    if (it == s.sources.end())
        return;
    s.engine.Stop(it->second.voice, fadeOutSeconds);
    it->second.voice    = 0;
    it->second.wantPlay = false;
}

bool AudioSystem::IsPlaying(Entity entity) const
{
    const Impl& s  = *m_Impl;
    const auto  it = s.sources.find(entity);
    return it != s.sources.end() && (it->second.wantPlay || (it->second.voice && s.engine.IsPlaying(it->second.voice)));
}

bool AudioSystem::PlayAt(const std::string& sound, const glm::vec3& position, float volume, float pitch, AudioBus bus)
{
    VoiceDesc desc;
    desc.bus      = bus;
    desc.volume   = volume;
    desc.pitch    = pitch;
    desc.spatial  = true;
    desc.position = position;
    return m_Impl->QueueOneShot(sound, desc);
}

bool AudioSystem::Play2D(const std::string& sound, float volume, AudioBus bus)
{
    VoiceDesc desc;
    desc.bus    = bus;
    desc.volume = volume;
    return m_Impl->QueueOneShot(sound, desc);
}

void AudioSystem::Preview(const std::filesystem::path& sound)
{
    Impl& s = *m_Impl;
    StopPreview();
    const std::string path = PathToUtf8(sound);
    s.Acquire(path, false);
    s.pendingPreview = path;
    s.UpdateOneShots(); // starts at once if already loaded
}

void AudioSystem::StopPreview()
{
    Impl& s = *m_Impl;
    s.engine.Stop(s.preview, 0.02f);
    s.preview = 0;
    s.pendingPreview.clear();
}

bool AudioSystem::Previewing() const
{
    const Impl& s = *m_Impl;
    return !s.pendingPreview.empty() || (s.preview && s.engine.IsPlaying(s.preview));
}

void AudioSystem::Apply(const AudioSettings& settings)
{
    Impl& s    = *m_Impl;
    s.settings = settings;
    for (std::size_t i = 0; i < kAudioBusCount; ++i) {
        s.engine.SetBusVolume(static_cast<AudioBus>(i), settings.volume[i]);
        s.engine.SetBusMuted(static_cast<AudioBus>(i), settings.muted[i]);
    }
}

const AudioSettings&    AudioSystem::Settings() const { return m_Impl->settings; }
const AudioSystemStats& AudioSystem::Stats() const { return m_Impl->stats; }
AudioEngine&            AudioSystem::Engine() { return m_Impl->engine; }

} // namespace Engine
