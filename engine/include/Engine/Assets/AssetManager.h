#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/MeshOptimizer.h"
#include "Engine/Assets/Model.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/TextureCooker.h"
#include "Engine/Renderer/Vulkan/Upload.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

class EventBus;
class Renderer;
class ThreadPool;

// Published on the EventBus from AssetManager::Update (main thread).
template <class T>
struct AssetLoadedEvent { // first successful load (also after a failed first load was retried)
    AssetHandle<T> handle;
};

template <class T>
struct AssetReloadedEvent { // a Ready asset's content was replaced (reload, hot reload)
    AssetHandle<T> handle;
};

template <class T>
struct AssetFailedEvent { // load, retry or reload failed (a Ready asset keeps its old content)
    AssetHandle<T> handle;
    std::string    path;
    std::string    error;
};

// Where a model came from: a file, a primitive recipe, or neither (CreateModel: not serializable).
struct ModelSource {
    std::filesystem::path        file;
    std::optional<PrimitiveDesc> primitive;
};

struct ModelInfo {
    ModelHandle   handle;
    AssetState    state    = AssetState::Invalid;
    std::uint32_t refCount = 0;
    std::string   path;  // UTF-8 file path, or the name of a generated model
    std::string   error; // Failed, or the last failed reload
    std::uint32_t revision  = 0;
    bool          reloading = false; // a reload / retry is in flight (the old content stays in use)
    bool          reloadable = false; // has a file or recipe
    std::uint32_t textures  = 0;
    std::uint64_t gpuBytes  = 0; // geometry pool (textures: see TextureInfo)
    std::uint64_t cpuBytes  = 0;
    std::uint32_t triangles = 0; // LOD 0
    std::uint32_t lodLevels = 0; // generated levels beyond LOD 0, all submeshes
};

struct TextureInfo {
    TextureHandle handle;
    AssetState    state    = AssetState::Invalid;
    std::uint32_t refCount = 0;
    std::string   path;  // UTF-8 file path, or "<model>#<image>" for embedded images
    std::string   error;
    std::uint32_t revision   = 0;
    bool          reloading  = false;
    bool          embedded   = false; // part of a model file: reloads with the model
    TextureKind   kind       = TextureKind::Color;
    VkFormat      format     = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0, height = 0, mipLevels = 0;
    std::uint64_t gpuBytes   = 0;
    bool          cacheHit   = false;
    std::uint32_t bindlessSlot = 0; // current image (previews); valid while Ready
    std::uint32_t tableEntry   = 0;
};

struct AssetManagerDesc {
    // Texture import. Compression is also off when the GPU cannot sample BC formats.
    TextureCookSettings  textures{.compress = true, .cacheDirectory = "asset_cache/textures", .quality = 0};
    MeshOptimizeSettings meshes{};
    bool                 hotReload   = false; // poll source files and reload changed assets
    double               pollSeconds = 0.5;
};

// What to draw for a MeshRenderer: the model if Ready; the error placeholder (a magenta box,
// mesh 0) if it failed; nothing while it loads or when the mesh index is out of range.
struct ResolvedMesh {
    const Model*  model       = nullptr;
    std::uint32_t meshIndex   = 0;
    bool          placeholder = false;
};

// Asynchronous, cached asset loading. Main thread only, except the worker jobs it spawns.
//
//   Load*(...)  cache hit: same handle, refcount + 1. Miss: new handle in state Loading; a
//               worker reads + processes the source and records the GPU upload.
//   Update()    once per frame: collects finished jobs, swaps in content the renderer has
//               acquired, polls files for hot reload, publishes events.
//   Release(h)  refcount - 1. At zero the asset is destroyed (deferred until the GPU is done)
//               and every copy of the handle becomes stale.
//
// Models own references to their textures (texture assets shared by file path or, for
// embedded images, by content). A model is Ready once its geometry is resident and none of its
// textures is still loading; failed textures show a placeholder. Materials reference textures
// through the renderer's texture table, so textures reload without touching any model.
//
// Handles are plain values: holding one does not keep the asset alive.
class AssetManager {
public:
    AssetManager(Renderer& renderer, ThreadPool& jobs, EventBus& events, const AssetManagerDesc& desc = {});
    ~AssetManager(); // waits for running jobs, then releases every asset

    AssetManager(const AssetManager&)            = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // --- Models
    [[nodiscard]] ModelHandle LoadModel(const std::filesystem::path& path);
    // Generated geometry (see Primitives.h): same async upload and lifecycle, never cached.
    [[nodiscard]] ModelHandle CreateModel(ModelData data);
    // Generated from a recipe; cached like files (same recipe: same handle, refcount + 1).
    [[nodiscard]] ModelHandle CreatePrimitive(const PrimitiveDesc& desc);
    void Release(ModelHandle handle);

    // nullptr unless Ready.
    [[nodiscard]] const Model*  Get(ModelHandle handle) const;
    [[nodiscard]] AssetState    State(ModelHandle handle) const;
    [[nodiscard]] std::string   Error(ModelHandle handle) const; // Failed, or the last failed reload
    [[nodiscard]] std::uint32_t RefCount(ModelHandle handle) const;
    [[nodiscard]] ModelSource   Source(ModelHandle handle) const; // empty for invalid handles
    // Changes whenever ResolveMesh's answer for this handle changes (loaded, failed, reloaded).
    [[nodiscard]] std::uint32_t Revision(ModelHandle handle) const;
    [[nodiscard]] ResolvedMesh  ResolveMesh(ModelHandle handle, std::uint32_t meshIndex) const;
    // Every model that has not been released (tools, diagnostics).
    [[nodiscard]] std::vector<ModelInfo> Models() const;
    // Distinct model paths / recipes currently held (any state except released).
    [[nodiscard]] std::size_t CachedCount() const { return m_ModelCache.size(); }

    // --- Textures
    [[nodiscard]] TextureHandle LoadTexture(const std::filesystem::path& path, TextureKind kind);
    void Release(TextureHandle handle);
    [[nodiscard]] const Texture*  Get(TextureHandle handle) const; // nullptr unless Ready
    [[nodiscard]] AssetState      State(TextureHandle handle) const;
    [[nodiscard]] std::string     Error(TextureHandle handle) const;
    [[nodiscard]] std::uint32_t   RefCount(TextureHandle handle) const;
    [[nodiscard]] std::uint32_t   Revision(TextureHandle handle) const;
    // Texture table entry for materials: the image once Ready, a default before, a placeholder
    // after a failure.
    [[nodiscard]] std::uint32_t   TableEntry(TextureHandle handle) const;
    [[nodiscard]] std::vector<TextureInfo> Textures() const;

    // --- Reload / retry. Re-reads the source: a Ready asset keeps its content until the new one
    // is resident (a failed reload keeps it for good), a Failed one is retried. While a load is
    // in flight the request is queued. False without a file / recipe (CreateModel, embedded
    // textures: reload their model).
    bool Reload(ModelHandle handle);
    bool Reload(TextureHandle handle);
    void SetHotReload(bool enabled);
    [[nodiscard]] bool HotReload() const { return m_HotReload; }

    void Update();

    // Bumps whenever any model's Revision changes or a model is released (consumers rescan their
    // model references; a released handle's Revision is 0).
    [[nodiscard]] std::uint64_t ContentVersion() const { return m_ContentVersion; }
    [[nodiscard]] const AssetManagerDesc&    Desc() const { return m_Desc; }
    [[nodiscard]] const TextureCookSettings& CookSettings() const { return m_Cook; } // effective
    [[nodiscard]] std::uint32_t JobsInFlight() const;

private:
    struct WatchedFile {
        std::filesystem::path           path;
        std::filesystem::file_time_type loaded{}; // at the start of the last load
        std::filesystem::file_time_type seen{};   // at the last poll (debounce: must be stable)
    };

    // Shared by model and texture entries.
    struct EntryBase {
        std::uint32_t            generation = 1; // bumped on free: stale handles stop matching
        bool                     alive      = false;
        bool                     orphaned   = false; // released while its job runs: freed when it lands
        bool                     jobRunning = false;
        bool                     reloadQueued = false;
        AssetState               state      = AssetState::Invalid;
        std::uint32_t            refCount   = 0;
        std::uint32_t            revision   = 0;
        std::u8string            key;  // cache key; empty: not cached
        std::filesystem::path    path; // file, or display name
        std::string              error;
        std::vector<WatchedFile> watched; // hot reload: source file (+ dependencies)
    };

    struct ModelEntry : EntryBase {
        std::optional<PrimitiveDesc> primitive;
        std::function<ModelData()>   produce; // empty: cannot be reloaded (CreateModel)
        std::unique_ptr<Model>       model;   // current content (Ready)
        UploadTicket                 ticket = 0;
        std::unique_ptr<Model>       pending; // next content: waits for its upload + textures
        UploadTicket                 pendingTicket = 0;
    };

    struct TextureEntry : EntryBase {
        TextureKind                             kind       = TextureKind::Color;
        bool                                    embedded   = false;
        std::uint32_t                           tableEntry = 0;
        std::function<std::vector<std::byte>()> source; // file contents; empty for embedded images
        std::unique_ptr<Texture>                texture;
        UploadTicket                            ticket = 0;
        std::unique_ptr<Texture>                pending;
        UploadTicket                            pendingTicket = 0;
    };

    // Worker -> main thread.
    struct ModelResult {
        ModelHandle                        handle;
        std::unique_ptr<Model>             model; // geometry only; may be partial if `error` is set
        UploadTicket                       ticket = 0;
        std::string                        error;
        std::vector<MaterialData>          materials;
        std::vector<TextureData>           textures;
        std::vector<std::filesystem::path> dependencies;
    };
    struct TextureResult {
        TextureHandle            handle;
        std::unique_ptr<Texture> texture; // may be partial if `error` is set
        UploadTicket             ticket = 0;
        std::string              error;
    };

    template <class E>
    struct Table {
        std::vector<E>             entries;
        std::vector<std::uint32_t> freeSlots;
        [[nodiscard]] std::uint32_t Allocate();
        void Free(std::uint32_t index);
        template <class T>
        [[nodiscard]] E* Find(AssetHandle<T> handle);
        template <class T>
        [[nodiscard]] const E* Find(AssetHandle<T> handle) const;
    };

    // Models
    [[nodiscard]] ModelHandle StartModel(std::filesystem::path path, std::u8string key,
                                         std::function<ModelData()> produce, bool reloadable, bool watch);
    void StartModelJob(ModelHandle handle, std::function<ModelData()> produce);
    void RunModelJob(ModelHandle handle, const std::function<ModelData()>& produce);
    void OnModelResult(ModelResult& result);
    void FailModel(ModelHandle handle, ModelEntry& e, std::string error);
    void DestroyModel(std::unique_ptr<Model> model, UploadTicket ticket);
    [[nodiscard]] bool TexturesSettled(const Model& model) const;
    void CreatePlaceholder();

    // Textures
    [[nodiscard]] TextureHandle AcquireTexture(std::u8string key, std::filesystem::path path, TextureKind kind,
                                               bool embedded, std::function<std::vector<std::byte>()> source);
    [[nodiscard]] TextureHandle AcquireEmbedded(TextureData& data, const std::string& owner);
    void StartTextureJob(TextureHandle handle, std::function<std::vector<std::byte>()> source);
    void RunTextureJob(TextureHandle handle, const std::function<std::vector<std::byte>()>& source,
                       TextureKind kind, const std::string& name);
    void OnTextureResult(TextureResult& result);
    void FailTexture(TextureHandle handle, TextureEntry& e, std::string error);
    void DestroyTexture(std::unique_ptr<Texture> texture, UploadTicket ticket);

    void PollFiles();
    [[nodiscard]] static std::vector<WatchedFile> Watch(std::vector<std::filesystem::path> files);
    void EnqueueJob(std::function<void()> job);
    void FinishJob(); // worker: last touch of `this`, under m_ResultMutex

    Renderer&           m_Renderer;
    ThreadPool&         m_Jobs;
    EventBus&           m_Events;
    AssetManagerDesc    m_Desc;
    TextureCookSettings m_Cook;
    bool                m_HotReload = false;
    std::chrono::steady_clock::time_point m_LastPoll{};
    std::uint64_t       m_ContentVersion = 0;

    Table<ModelEntry>                                m_Models;
    Table<TextureEntry>                              m_Textures;
    std::unordered_map<std::u8string, std::uint32_t> m_ModelCache;
    std::unordered_map<std::u8string, std::uint32_t> m_TextureCache;
    std::vector<ModelHandle>                         m_PendingModels;   // content waiting to swap in
    std::vector<TextureHandle>                       m_PendingTextures;

    // GPU resources that must outlive an upload still in flight.
    std::vector<std::pair<std::unique_ptr<Model>, UploadTicket>>   m_ModelGraveyard;
    std::vector<std::pair<std::unique_ptr<Texture>, UploadTicket>> m_TextureGraveyard;

    std::unique_ptr<Model> m_Placeholder; // error model (mesh 0: box)
    UploadTicket           m_PlaceholderTicket = 0;

    // Events collected during Update, published at its end (handlers may call back).
    std::vector<std::function<void()>> m_Publish;

    // Jobs notify under the lock, so the destructor cannot free `this` mid-notify.
    mutable std::mutex         m_ResultMutex;
    std::condition_variable    m_JobsDone;
    std::vector<ModelResult>   m_ModelResults;   // guarded by m_ResultMutex
    std::vector<TextureResult> m_TextureResults; // guarded by m_ResultMutex
    std::uint32_t              m_JobsInFlight = 0; // guarded by m_ResultMutex
    std::atomic<bool>          m_ShuttingDown{false};
};

} // namespace Engine
