#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/Model.h"
#include "Engine/Renderer/Vulkan/Upload.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

class EventBus;
class Renderer;
class ThreadPool;

// Published on the EventBus from AssetManager::Update (main thread).
template <class T>
struct AssetLoadedEvent {
    AssetHandle<T> handle;
};

template <class T>
struct AssetFailedEvent {
    AssetHandle<T> handle;
    std::string    path;
    std::string    error;
};

// Asynchronous, path-cached asset loading. Main thread only, except the worker jobs it spawns.
//
//   Load(path)  cache hit: same handle, refcount + 1. Miss: new handle in state Loading; a
//               worker parses the file and records the GPU upload.
//   Update()    once per frame: collects finished jobs, promotes uploads that the renderer
//               has acquired to Ready, publishes AssetLoadedEvent / AssetFailedEvent.
//   Release(h)  refcount - 1. At zero the asset is destroyed (deferred until the GPU is done)
//               and every copy of the handle becomes stale.
//
// Handles are plain values: holding one does not keep the asset alive.
class AssetManager {
public:
    AssetManager(Renderer& renderer, ThreadPool& jobs, EventBus& events);
    ~AssetManager(); // waits for running jobs, then releases every asset

    AssetManager(const AssetManager&)            = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    [[nodiscard]] ModelHandle LoadModel(const std::filesystem::path& path);
    void Release(ModelHandle handle);

    void Update();

    // nullptr unless Ready.
    [[nodiscard]] const Model*  Get(ModelHandle handle) const;
    [[nodiscard]] AssetState    State(ModelHandle handle) const;
    [[nodiscard]] std::string   Error(ModelHandle handle) const; // empty unless Failed
    [[nodiscard]] std::uint32_t RefCount(ModelHandle handle) const;

    // Distinct paths currently held (any state except released).
    [[nodiscard]] std::size_t CachedCount() const { return m_Cache.size(); }

private:
    struct Entry {
        std::uint32_t          generation = 1; // bumped on free: stale handles stop matching
        bool                   alive      = false;
        bool                   orphaned   = false; // released while its job runs: freed when it lands
        AssetState             state      = AssetState::Invalid;
        std::uint32_t          refCount   = 0;
        std::u8string          key;            // cache key (normalized path)
        std::filesystem::path  path;
        std::unique_ptr<Model> model;
        UploadTicket           ticket = 0;
        std::string            error;
    };

    // Worker -> main thread.
    struct JobResult {
        ModelHandle            handle;
        std::unique_ptr<Model> model; // may be partially built if `error` is set
        UploadTicket           ticket = 0;
        std::string            error;
    };

    // GPU resources that must outlive an upload still in flight.
    struct Graveyard {
        std::unique_ptr<Model> model;
        UploadTicket           ticket = 0;
    };

    [[nodiscard]] Entry*       Find(ModelHandle handle);
    [[nodiscard]] const Entry* Find(ModelHandle handle) const;
    [[nodiscard]] ModelHandle  Allocate();
    void Free(std::uint32_t index);
    void Destroy(std::unique_ptr<Model> model, UploadTicket ticket);
    void RunLoadJob(ModelHandle handle, std::filesystem::path path);

    Renderer&   m_Renderer;
    ThreadPool& m_Jobs;
    EventBus&   m_Events;

    std::vector<Entry>                              m_Entries;
    std::vector<std::uint32_t>                      m_FreeSlots;
    std::unordered_map<std::u8string, std::uint32_t> m_Cache;
    std::vector<ModelHandle>                        m_Uploading;
    std::vector<Graveyard>                          m_Graveyard;

    // Jobs notify under the lock, so the destructor cannot free `this` mid-notify.
    std::mutex              m_ResultMutex;
    std::condition_variable m_JobsDone;
    std::vector<JobResult>  m_Results;      // guarded by m_ResultMutex
    std::uint32_t           m_JobsInFlight = 0; // guarded by m_ResultMutex
    std::atomic<bool>       m_ShuttingDown{false};
};

} // namespace Engine
