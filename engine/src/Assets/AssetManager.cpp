#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/Renderer.h"

#include <cassert>
#include <exception>
#include <system_error>
#include <utility>

namespace Engine {

namespace {
// Absolute and normalized, so "a/../b.glb" and "b.glb" share a cache entry.
std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    // absolute() first: weakly_canonical keeps a relative path relative if no prefix exists.
    std::error_code             ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path base     = ec ? path : absolute;
    std::filesystem::path       result   = std::filesystem::weakly_canonical(base, ec);
    return ec ? base.lexically_normal() : result;
}

// path::string() may throw on Windows for characters outside the ANSI code page.
std::string ToUtf8(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return {s.begin(), s.end()};
}
} // namespace

AssetManager::AssetManager(Renderer& renderer, ThreadPool& jobs, EventBus& events)
    : m_Renderer(renderer), m_Jobs(jobs), m_Events(events)
{
}

AssetManager::~AssetManager()
{
    m_ShuttingDown.store(true, std::memory_order_relaxed); // queued jobs skip their work
    {
        std::unique_lock lock{m_ResultMutex};
        m_JobsDone.wait(lock, [this] { return m_JobsInFlight == 0; });
    }

    // Finish every recorded upload so the deferred releases below never race the transfer
    // queue, even if the renderer keeps running (stalls once; shutdown only).
    m_Renderer.GetUploader().Flush();
    for (JobResult& r : m_Results)
        if (r.model)
            ReleaseModel(m_Renderer, std::move(*r.model));
    for (Entry& e : m_Entries)
        if (e.model)
            ReleaseModel(m_Renderer, std::move(*e.model));
    for (Graveyard& g : m_Graveyard)
        ReleaseModel(m_Renderer, std::move(*g.model));
}

ModelHandle AssetManager::LoadModel(const std::filesystem::path& path)
{
    std::filesystem::path normalized = NormalizePath(path);
    std::u8string         key        = normalized.generic_u8string();

    if (const auto it = m_Cache.find(key); it != m_Cache.end()) {
        Entry& e = m_Entries[it->second];
        ++e.refCount;
        return {it->second, e.generation};
    }

    const ModelHandle handle = StartJob(normalized, key, [normalized] { return LoadGltf(normalized); });
    m_Cache.emplace(std::move(key), handle.index);
    return handle;
}

ModelHandle AssetManager::CreateModel(ModelData data)
{
    std::filesystem::path name = data.name;
    auto shared = std::make_shared<ModelData>(std::move(data)); // std::function must be copyable
    return StartJob(std::move(name), {}, [shared] { return std::move(*shared); });
}

ModelHandle AssetManager::StartJob(std::filesystem::path path, std::u8string key, std::function<ModelData()> produce)
{
    const ModelHandle handle = Allocate();
    Entry&            e      = m_Entries[handle.index];
    e.state    = AssetState::Loading;
    e.refCount = 1;
    e.key      = std::move(key);
    e.path     = std::move(path);

    {
        std::scoped_lock lock{m_ResultMutex};
        ++m_JobsInFlight;
    }
    m_Jobs.Enqueue([this, handle, produce = std::move(produce)] { RunLoadJob(handle, produce); });
    return handle;
}

void AssetManager::Release(ModelHandle handle)
{
    Entry* e = Find(handle);
    if (!e) {
        ENGINE_WARN("AssetManager::Release: stale or null model handle");
        return;
    }
    assert(e->refCount > 0);
    if (--e->refCount > 0)
        return;

    if (!e->key.empty())
        m_Cache.erase(e->key); // a new Load() of this path starts from scratch
    if (e->state == AssetState::Loading) {
        e->orphaned = true; // the worker still reports to this slot
        return;
    }
    Destroy(std::move(e->model), e->ticket);
    Free(handle.index);
}

void AssetManager::Update()
{
    std::vector<JobResult> results;
    {
        std::scoped_lock lock{m_ResultMutex};
        results.swap(m_Results);
    }

    // Collected first, published last: handlers may call Load/Release (re-entrancy).
    std::vector<AssetLoadedEvent<Model>> loaded;
    std::vector<AssetFailedEvent<Model>> failed;

    for (JobResult& r : results) {
        Entry& e = m_Entries[r.handle.index]; // slot stays pinned while its job runs
        assert(e.alive && e.generation == r.handle.generation);

        if (e.orphaned) {
            Destroy(std::move(r.model), r.ticket);
            Free(r.handle.index);
        } else if (!r.error.empty()) {
            Destroy(std::move(r.model), r.ticket); // partially built GPU resources
            e.state = AssetState::Failed;
            e.error = std::move(r.error);
            ENGINE_ERROR("Failed to load '{}': {}", ToUtf8(e.path), e.error);
            failed.push_back({r.handle, ToUtf8(e.path), e.error});
        } else {
            e.model  = std::move(r.model);
            e.ticket = r.ticket;
            e.state  = AssetState::Uploading;
            m_Uploading.push_back(r.handle);
        }
    }

    const UploadQueue& uploader = m_Renderer.GetUploader();
    std::erase_if(m_Uploading, [&](ModelHandle h) {
        Entry* e = Find(h);
        if (!e || e->state != AssetState::Uploading)
            return true; // released in the meantime
        if (!uploader.IsReady(e->ticket))
            return false;
        e->state = AssetState::Ready;
        loaded.push_back({h});
        return true;
    });

    std::erase_if(m_Graveyard, [&](Graveyard& g) {
        if (!uploader.IsReady(g.ticket))
            return false;
        ReleaseModel(m_Renderer, std::move(*g.model));
        return true;
    });

    for (const auto& ev : failed)
        m_Events.Publish(ev);
    for (const auto& ev : loaded)
        m_Events.Publish(ev);
}

const Model* AssetManager::Get(ModelHandle handle) const
{
    const Entry* e = Find(handle);
    return e && e->state == AssetState::Ready ? e->model.get() : nullptr;
}

AssetState AssetManager::State(ModelHandle handle) const
{
    const Entry* e = Find(handle);
    return e ? e->state : AssetState::Invalid;
}

std::string AssetManager::Error(ModelHandle handle) const
{
    const Entry* e = Find(handle);
    return e ? e->error : std::string{};
}

std::uint32_t AssetManager::RefCount(ModelHandle handle) const
{
    const Entry* e = Find(handle);
    return e ? e->refCount : 0;
}

AssetManager::Entry* AssetManager::Find(ModelHandle handle)
{
    return const_cast<Entry*>(std::as_const(*this).Find(handle));
}

const AssetManager::Entry* AssetManager::Find(ModelHandle handle) const
{
    if (!handle || handle.index >= m_Entries.size())
        return nullptr;
    const Entry& e = m_Entries[handle.index];
    return e.alive && !e.orphaned && e.generation == handle.generation ? &e : nullptr;
}

ModelHandle AssetManager::Allocate()
{
    std::uint32_t index = 0;
    if (!m_FreeSlots.empty()) {
        index = m_FreeSlots.back();
        m_FreeSlots.pop_back();
    } else {
        index = static_cast<std::uint32_t>(m_Entries.size());
        m_Entries.emplace_back();
    }
    Entry& e = m_Entries[index];
    e.alive  = true;
    return {index, e.generation};
}

void AssetManager::Free(std::uint32_t index)
{
    Entry&              e          = m_Entries[index];
    const std::uint32_t generation = e.generation + 1 == 0 ? 1 : e.generation + 1; // 0 is the null handle
    e            = Entry{};
    e.generation = generation;
    m_FreeSlots.push_back(index);
}

void AssetManager::Destroy(std::unique_ptr<Model> model, UploadTicket ticket)
{
    if (!model)
        return;
    // Never free what the transfer queue may still write, nor skip the acquire it is queued for.
    if (m_Renderer.GetUploader().IsReady(ticket))
        ReleaseModel(m_Renderer, std::move(*model));
    else
        m_Graveyard.push_back({std::move(model), ticket});
}

void AssetManager::RunLoadJob(ModelHandle handle, const std::function<ModelData()>& produce)
{
    // Worker thread: touches only the Renderer's thread-safe parts and m_Results.
    JobResult result;
    result.handle = handle;
    if (m_ShuttingDown.load(std::memory_order_relaxed)) {
        result.error = "cancelled";
    } else {
        try {
            const ModelData data = produce();
            result.model         = std::make_unique<Model>();
            BuildModel(m_Renderer, data, *result.model, result.ticket);
        } catch (const std::exception& ex) {
            result.error = ex.what();
        } catch (...) {
            result.error = "unknown error";
        }
    }

    std::scoped_lock lock{m_ResultMutex};
    m_Results.push_back(std::move(result));
    if (--m_JobsInFlight == 0)
        m_JobsDone.notify_all(); // last touch of `this`, still under the lock
}

} // namespace Engine
