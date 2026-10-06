#include "Engine/Scene/LevelStreaming.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Scene/Scene.h"
#include "SceneJson.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

namespace {

using PreparedPtr = std::shared_ptr<const PreparedScene>;

bool ParseDone(const std::future<PreparedPtr>& parse)
{
    return parse.valid() && parse.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// One scene file on its way into memory: parsed on the pool, then its models loaded.
struct Preparation {
    std::filesystem::path    file;
    std::future<PreparedPtr> parse;
    PreparedPtr              scene;
    SceneModels              models;
    LevelState               state = LevelState::Unloaded;
    std::string              error;

    void Start(ThreadPool& jobs, const std::filesystem::path& level)
    {
        file   = level;
        scene  = nullptr;
        error.clear();
        state  = LevelState::Preparing;
        parse  = jobs.Submit([level] { return PrepareSceneFile(level); });
    }

    // Main thread: parse result -> model acquisition -> Ready once no model is loading.
    void Advance(AssetManager* assets)
    {
        if (state == LevelState::Preparing && ParseDone(parse)) {
            try {
                scene = parse.get();
                state = LevelState::Loading;
                if (assets)
                    AcquireSceneModels(*scene, *assets, models);
            } catch (const std::exception& e) {
                error = e.what();
                state = LevelState::Failed;
            }
        }
        if (state == LevelState::Loading && SettledModels(assets) == models.size())
            state = LevelState::Ready;
    }

    [[nodiscard]] std::size_t SettledModels(AssetManager* assets) const
    {
        if (!assets)
            return models.size();
        std::size_t settled = 0;
        for (const auto& [key, handle] : models) {
            const AssetState s = assets->State(handle);
            settled += s != AssetState::Loading && s != AssetState::Uploading ? 1 : 0;
        }
        return settled;
    }

    [[nodiscard]] float Progress(AssetManager* assets) const
    {
        switch (state) {
        case LevelState::Preparing: return 0.1f;
        case LevelState::Loading:
            return models.empty() ? 0.9f
                                  : 0.2f + 0.7f * static_cast<float>(SettledModels(assets)) / static_cast<float>(models.size());
        case LevelState::Ready:
        case LevelState::Loaded: return 1.0f;
        default: return 0.0f;
        }
    }

    // Models released; a running parse is handed to `orphans` (it cannot be interrupted).
    void Reset(AssetManager* assets, std::vector<std::future<PreparedPtr>>& orphans)
    {
        if (parse.valid())
            orphans.push_back(std::move(parse));
        if (assets)
            for (const auto& [key, handle] : models)
                assets->Release(handle);
        models.clear();
        scene = nullptr;
        state = LevelState::Unloaded;
    }
};

void DropFinished(std::vector<std::future<PreparedPtr>>& orphans)
{
    std::erase_if(orphans, [](const std::future<PreparedPtr>& f) { return ParseDone(f); });
}

void WaitAll(std::vector<std::future<PreparedPtr>>& orphans)
{
    for (auto& f : orphans)
        if (f.valid())
            f.wait();
    orphans.clear();
}

} // namespace

const char* LevelStateName(LevelState state)
{
    switch (state) {
    case LevelState::Unloaded: return "Unloaded";
    case LevelState::Preparing: return "Preparing";
    case LevelState::Loading: return "Loading";
    case LevelState::Ready: return "Ready";
    case LevelState::Loaded: return "Loaded";
    case LevelState::Failed: return "Failed";
    }
    return "?";
}

// --- LevelLoader ----------------------------------------------------------------------------------

struct LevelLoader::Impl {
    ThreadPool&                           jobs;
    AssetManager*                         assets;
    Preparation                           prep;
    std::vector<std::future<PreparedPtr>> orphans;
};

LevelLoader::LevelLoader(ThreadPool& jobs, AssetManager* assets) : m_Impl(std::make_unique<Impl>(Impl{jobs, assets, {}, {}})) {}

LevelLoader::~LevelLoader()
{
    m_Impl->prep.Reset(m_Impl->assets, m_Impl->orphans);
    WaitAll(m_Impl->orphans);
}

bool LevelLoader::Begin(const std::filesystem::path& file)
{
    Impl& w = *m_Impl;
    if (w.prep.state == LevelState::Preparing || w.prep.state == LevelState::Loading || w.prep.state == LevelState::Ready)
        return false;
    w.prep.Reset(w.assets, w.orphans);
    w.prep.Start(w.jobs, file);
    return true;
}

void LevelLoader::Update()
{
    DropFinished(m_Impl->orphans);
    m_Impl->prep.Advance(m_Impl->assets);
}

void LevelLoader::Cancel() { m_Impl->prep.Reset(m_Impl->assets, m_Impl->orphans); }

std::optional<LevelLoader::Result> LevelLoader::Take()
{
    Preparation& p = m_Impl->prep;
    if (p.state != LevelState::Ready)
        return std::nullopt;
    Result result{std::move(p.scene), std::move(p.models)};
    p.models.clear();
    p.scene = nullptr;
    p.state = LevelState::Unloaded;
    return result;
}

LevelState                   LevelLoader::State() const { return m_Impl->prep.state; }
float                        LevelLoader::Progress() const { return m_Impl->prep.Progress(m_Impl->assets); }
const std::string&           LevelLoader::Error() const { return m_Impl->prep.error; }
const std::filesystem::path& LevelLoader::File() const { return m_Impl->prep.file; }

// --- LevelStreamer --------------------------------------------------------------------------------

struct LevelStreamer::Impl {
    struct Level {
        std::filesystem::path file; // as first requested (absolute)
        std::string           display;
        Preparation           prep;
        bool                  requested = false;
        bool                  byVolume  = false; // a volume wanted it at the last update
        bool                  retry     = true;  // a failed level is tried again only after a new request
        LevelState            state     = LevelState::Unloaded;
        std::vector<Entity>   roots;
    };

    ThreadPool&                                                 jobs;
    AssetManager*                                               assets;
    EventBus*                                                   events;
    MoveOnlyFunction<void(Scene&, std::span<const Entity>)>     unloadHook;
    std::unordered_map<std::string, Level>                      levels; // by key
    std::vector<std::future<PreparedPtr>>                       orphans;

    Level& Get(const std::filesystem::path& file)
    {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(file, ec).lexically_normal();
        Level& level = levels[Key(absolute)];
        if (level.file.empty()) {
            level.file    = absolute;
            level.display = DisplayPath(absolute);
        }
        return level;
    }

    void Publish(const Level& level, bool loaded, bool failed)
    {
        if (events)
            events->Publish(LevelStreamedEvent{level.display, loaded, failed, level.prep.error});
    }

    void Activate(Scene& scene, const std::string& key, Level& level)
    {
        try {
            level.roots = InstantiatePreparedScene(*level.prep.scene, scene, assets, level.prep.models);
        } catch (const std::exception& e) {
            level.prep.Reset(assets, orphans); // its models
            level.prep.error = e.what();
            level.state      = LevelState::Failed;
            level.retry      = false;
            ENGINE_ERROR("Level streaming: {}", e.what());
            Publish(level, false, true);
            return;
        }
        for (const Entity root : level.roots)
            scene.GetRegistry().EmplaceOrReplace<StreamedLevel>(root, StreamedLevel{key});
        level.prep.scene = nullptr; // the parsed JSON is not needed any more
        level.state      = LevelState::Loaded;
        scene.UpdateTransforms();
        ENGINE_INFO("Level streaming: loaded '{}' ({} root entities)", level.display, level.roots.size());
        Publish(level, true, false);
    }

    void Deactivate(Scene& scene, Level& level)
    {
        // The level's entities: its roots that still live (scripts may have destroyed some, and
        // spawned entities do not belong to it).
        std::vector<Entity> roots;
        for (const Entity e : level.roots)
            if (scene.GetRegistry().Valid(e) && scene.GetRegistry().Has<StreamedLevel>(e))
                roots.push_back(e);
        if (unloadHook && !roots.empty())
            unloadHook(scene, roots);
        for (const Entity e : roots)
            if (scene.GetRegistry().Valid(e))
                scene.DestroyEntity(e);
        level.roots.clear();
        level.prep.Reset(assets, orphans);
        level.state = LevelState::Unloaded;
        ENGINE_INFO("Level streaming: unloaded '{}'", level.display);
        Publish(level, false, false);
    }

    // Box test in the volume's space: `margin` grows the box.
    static bool Inside(const glm::mat4& inverseWorld, const glm::vec3& halfExtents, float margin, const glm::vec3& point)
    {
        const glm::vec3 local = glm::vec3(inverseWorld * glm::vec4(point, 1.0f));
        const glm::vec3 limit = halfExtents + glm::vec3(margin);
        return std::abs(local.x) <= limit.x && std::abs(local.y) <= limit.y && std::abs(local.z) <= limit.z;
    }
};

LevelStreamer::LevelStreamer(ThreadPool& jobs, AssetManager* assets, EventBus* events)
    : m_Impl(std::make_unique<Impl>(Impl{jobs, assets, events, {}, {}, {}}))
{
}

LevelStreamer::~LevelStreamer()
{
    Reset();
    WaitAll(m_Impl->orphans);
}

void LevelStreamer::SetUnloadHook(MoveOnlyFunction<void(Scene&, std::span<const Entity>)> hook)
{
    m_Impl->unloadHook = std::move(hook);
}

void LevelStreamer::Load(const std::filesystem::path& level)
{
    Impl::Level& l = m_Impl->Get(level);
    l.requested    = true;
    l.retry        = true;
}

void LevelStreamer::Unload(const std::filesystem::path& level)
{
    Impl& w = *m_Impl;
    if (const auto it = w.levels.find(Key(level)); it != w.levels.end())
        it->second.requested = false;
}

void LevelStreamer::Update(Scene& scene, std::optional<glm::vec3> fallbackSource)
{
    Impl&     w = *m_Impl;
    Registry& r = scene.GetRegistry();
    DropFinished(w.orphans);

    // Volumes: which levels a source is inside of (load) or near (keep).
    std::unordered_map<std::string, bool> volumeLoad, volumeKeep;
    if (volumesEnabled) {
        std::vector<glm::vec3> sources;
        r.ViewOf<StreamingSource, WorldTransform>().Each(
            [&](Entity, const StreamingSource&, const WorldTransform& world) { sources.push_back(glm::vec3(world.matrix[3])); });
        if (sources.empty() && fallbackSource)
            sources.push_back(*fallbackSource);
        r.ViewOf<LevelStreamingVolume, WorldTransform>().Each(
            [&](Entity, const LevelStreamingVolume& volume, const WorldTransform& world) {
                if (volume.level.empty())
                    return;
                const float det = glm::determinant(world.matrix);
                if (!std::isfinite(det) || std::abs(det) < 1.0e-12f)
                    return;
                const glm::mat4   inverse = glm::inverse(world.matrix);
                const std::string key     = Key(PathFromUtf8(volume.level));
                bool              load = false, keep = false;
                for (const glm::vec3& p : sources) {
                    load = load || Impl::Inside(inverse, volume.halfExtents, volume.loadMargin, p);
                    keep = keep || Impl::Inside(inverse, volume.halfExtents, volume.loadMargin + volume.unloadMargin, p);
                }
                if (load || keep)
                    w.Get(PathFromUtf8(volume.level)); // known from now on
                volumeLoad[key] = volumeLoad[key] || load;
                volumeKeep[key] = volumeKeep[key] || keep;
            });
    }

    std::vector<std::string> keys;
    for (const auto& [key, level] : w.levels)
        keys.push_back(key);
    std::ranges::sort(keys); // deterministic activation order
    for (const std::string& key : keys) {
        Impl::Level& level = w.levels.at(key);
        const bool   load  = volumeLoad.contains(key) && volumeLoad[key];
        const bool   keep  = volumeKeep.contains(key) && volumeKeep[key];
        if (load && !level.byVolume)
            level.retry = true; // entering a volume again retries a failed level
        level.byVolume    = load || (level.byVolume && keep);
        const bool wanted = level.requested || level.byVolume;

        if (!wanted) {
            if (level.state == LevelState::Loaded)
                w.Deactivate(scene, level);
            else if (level.state != LevelState::Unloaded) {
                level.prep.Reset(w.assets, w.orphans);
                level.state = LevelState::Unloaded;
            }
            continue;
        }
        if (level.state == LevelState::Unloaded || (level.state == LevelState::Failed && level.retry)) {
            level.prep.Reset(w.assets, w.orphans);
            level.prep.Start(w.jobs, level.file);
            level.state = LevelState::Preparing;
            level.retry = false;
        }
        if (level.state == LevelState::Preparing || level.state == LevelState::Loading) {
            level.prep.Advance(w.assets);
            level.state = level.prep.state;
            if (level.state == LevelState::Failed) {
                ENGINE_ERROR("Level streaming: {}", level.prep.error);
                level.prep.Reset(w.assets, w.orphans);
                level.state = LevelState::Failed;
                w.Publish(level, false, true);
            }
        }
        if (level.state == LevelState::Ready)
            w.Activate(scene, key, level);
    }
}

void LevelStreamer::UnloadAll(Scene& scene)
{
    Impl& w = *m_Impl;
    for (auto& [key, level] : w.levels) {
        level.requested = level.byVolume = false;
        if (level.state == LevelState::Loaded)
            w.Deactivate(scene, level);
        else
            level.prep.Reset(w.assets, w.orphans);
        level.state = LevelState::Unloaded;
    }
    w.levels.clear();
}

void LevelStreamer::Reset()
{
    Impl& w = *m_Impl;
    for (auto& [key, level] : w.levels)
        level.prep.Reset(w.assets, w.orphans);
    w.levels.clear();
}

LevelState LevelStreamer::State(const std::filesystem::path& level) const
{
    const auto it = m_Impl->levels.find(Key(level));
    return it != m_Impl->levels.end() ? it->second.state : LevelState::Unloaded;
}

bool LevelStreamer::IsLoaded(const std::filesystem::path& level) const { return State(level) == LevelState::Loaded; }

bool LevelStreamer::Busy() const
{
    return std::ranges::any_of(m_Impl->levels, [](const auto& entry) {
        return entry.second.state == LevelState::Preparing || entry.second.state == LevelState::Loading;
    });
}

std::vector<StreamedLevelInfo> LevelStreamer::Levels() const
{
    std::vector<StreamedLevelInfo> out;
    for (const auto& [key, level] : m_Impl->levels)
        out.push_back({.level     = level.display,
                       .key       = key,
                       .state     = level.state,
                       .progress  = level.state == LevelState::Loaded ? 1.0f : level.prep.Progress(m_Impl->assets),
                       .requested = level.requested,
                       .byVolume  = level.byVolume,
                       .roots     = level.roots.size(),
                       .error     = level.prep.error});
    std::ranges::sort(out, {}, &StreamedLevelInfo::key);
    return out;
}

std::string LevelStreamer::Key(const std::filesystem::path& level) { return SceneJson::NormalizedFile(level); }

std::string LevelStreamer::DisplayPath(const std::filesystem::path& level)
{
    std::error_code             ec;
    const std::filesystem::path absolute = std::filesystem::absolute(level, ec).lexically_normal();
    const std::filesystem::path relative = absolute.lexically_relative(std::filesystem::current_path(ec));
    if (!ec && !relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
        return SceneJson::ToUtf8(relative);
    return SceneJson::ToUtf8(absolute);
}

} // namespace Engine
