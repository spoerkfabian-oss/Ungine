#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/Renderer.h"

#include <cassert>
#include <exception>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace Engine {

namespace {
constexpr std::uint32_t kNoSlot = ~0u;

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

std::u8string ToU8(std::string_view ascii)
{
    return {ascii.begin(), ascii.end()};
}

std::vector<std::byte> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error(std::format("cannot open '{}'", ToUtf8(path)));
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file)
        throw std::runtime_error(std::format("cannot read '{}'", ToUtf8(path)));
    return bytes;
}

std::filesystem::file_time_type ModifiedTime(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto      time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type::min() : time; // missing: appears as a change later
}

// While a texture loads, materials sample the slot's neutral default; after a failure color
// textures show the error checker (data textures stay neutral: a checker there is unreadable).
DefaultTexture LoadingTexture(TextureKind kind)
{
    return kind == TextureKind::Normal ? DefaultTexture::FlatNormal : DefaultTexture::White;
}

DefaultTexture FailedTexture(TextureKind kind)
{
    return kind == TextureKind::Color ? DefaultTexture::Error : LoadingTexture(kind);
}

void ReleaseTexture(Renderer& renderer, Texture&& texture)
{
    if (texture.bindlessSlot != kNoSlot) {
        Renderer* r = &renderer;
        renderer.DeferCall([r, slot = texture.bindlessSlot] { r->GetBindless().RemoveSampledImage(slot); });
    }
    renderer.DeferRelease(std::move(texture.image));
}
} // namespace

// --- Slot tables ---------------------------------------------------------------------------------

template <class E>
std::uint32_t AssetManager::Table<E>::Allocate()
{
    std::uint32_t index = 0;
    if (!freeSlots.empty()) {
        index = freeSlots.back();
        freeSlots.pop_back();
    } else {
        index = static_cast<std::uint32_t>(entries.size());
        entries.emplace_back();
    }
    entries[index].alive = true;
    return index;
}

template <class E>
void AssetManager::Table<E>::Free(std::uint32_t index)
{
    E&                  e          = entries[index];
    const std::uint32_t generation = e.generation + 1 == 0 ? 1 : e.generation + 1; // 0 is the null handle
    e            = E{};
    e.generation = generation;
    freeSlots.push_back(index);
}

template <class E>
template <class T>
E* AssetManager::Table<E>::Find(AssetHandle<T> handle)
{
    return const_cast<E*>(std::as_const(*this).Find(handle));
}

template <class E>
template <class T>
const E* AssetManager::Table<E>::Find(AssetHandle<T> handle) const
{
    if (!handle || handle.index >= entries.size())
        return nullptr;
    const E& e = entries[handle.index];
    return e.alive && !e.orphaned && e.generation == handle.generation ? &e : nullptr;
}

// --- Lifetime ------------------------------------------------------------------------------------

AssetManager::AssetManager(Renderer& renderer, ThreadPool& jobs, EventBus& events, const AssetManagerDesc& desc)
    : m_Renderer(renderer), m_Jobs(jobs), m_Events(events), m_Desc(desc), m_Cook(desc.textures),
      m_HotReload(desc.hotReload)
{
    m_Cook.compress = desc.textures.compress && renderer.GetContext().SupportsBC();
    CreatePlaceholder();
}

AssetManager::~AssetManager()
{
    m_ShuttingDown.store(true, std::memory_order_relaxed); // queued jobs skip their work
    {
        std::unique_lock lock{m_ResultMutex};
        m_JobsDone.wait(lock, [this] { return m_JobsInFlight == 0; });
    }

    // Finish every recorded upload so the deferred releases below never race the transfer
    // queue, even if the renderer keeps running (stalls once; shutdown only). Texture
    // references between assets are dropped wholesale: everything goes.
    m_Renderer.GetUploader().Flush();
    const auto releaseModel = [this](std::unique_ptr<Model>& model) {
        if (model)
            ReleaseModel(m_Renderer, std::move(*model));
    };
    const auto releaseTexture = [this](std::unique_ptr<Texture>& texture) {
        if (texture)
            ReleaseTexture(m_Renderer, std::move(*texture));
    };
    for (ModelResult& r : m_ModelResults)
        releaseModel(r.model);
    for (TextureResult& r : m_TextureResults)
        releaseTexture(r.texture);
    for (ModelEntry& e : m_Models.entries) {
        releaseModel(e.model);
        releaseModel(e.pending);
    }
    for (TextureEntry& e : m_Textures.entries) {
        releaseTexture(e.texture);
        releaseTexture(e.pending);
        if (e.alive && !e.orphaned) // orphans freed theirs in Release
            m_Renderer.FreeTextureEntry(e.tableEntry);
    }
    for (auto& [model, ticket] : m_ModelGraveyard)
        releaseModel(model);
    for (auto& [texture, ticket] : m_TextureGraveyard)
        releaseTexture(texture);
    releaseModel(m_Placeholder);
}

void AssetManager::CreatePlaceholder()
{
    ModelData data = MakePrimitive({.shape = PrimitiveShape::Box, .size = 1.0f});
    data.name                          = "Missing model";
    MaterialData& material             = data.materials.at(0);
    material.baseColorTexture          = kErrorTexture; // magenta checker
    material.emissiveFactor            = glm::vec3(0.15f, 0.0f, 0.15f); // visible in the dark
    m_Placeholder                      = std::make_unique<Model>();
    BuildModel(m_Renderer, data, *m_Placeholder, m_PlaceholderTicket);
    m_Renderer.GetUploader().Flush(); // resident before the first failure can ask for it
}

void AssetManager::EnqueueJob(std::function<void()> job)
{
    {
        std::scoped_lock lock{m_ResultMutex};
        ++m_JobsInFlight;
    }
    m_Jobs.Enqueue(std::move(job));
}

void AssetManager::FinishJob()
{
    if (--m_JobsInFlight == 0)
        m_JobsDone.notify_all(); // the caller holds m_ResultMutex: last touch of `this`
}

std::uint32_t AssetManager::JobsInFlight() const
{
    std::scoped_lock lock{m_ResultMutex};
    return m_JobsInFlight;
}

std::vector<AssetManager::WatchedFile> AssetManager::Watch(std::vector<std::filesystem::path> files)
{
    std::vector<WatchedFile> watched;
    watched.reserve(files.size());
    for (std::filesystem::path& file : files) {
        const auto time = ModifiedTime(file);
        watched.push_back({.path = std::move(file), .loaded = time, .seen = time});
    }
    return watched;
}

// --- Models --------------------------------------------------------------------------------------

ModelHandle AssetManager::LoadModel(const std::filesystem::path& path)
{
    std::filesystem::path normalized = NormalizePath(path);
    std::u8string         key        = normalized.generic_u8string();
    if (const auto it = m_ModelCache.find(key); it != m_ModelCache.end()) {
        ModelEntry& e = m_Models.entries[it->second];
        ++e.refCount;
        return {it->second, e.generation};
    }
    const ModelHandle handle =
        StartModel(normalized, key, [normalized] { return LoadGltf(normalized); }, /*reloadable*/ true, /*watch*/ true);
    m_ModelCache.emplace(std::move(key), handle.index);
    return handle;
}

ModelHandle AssetManager::CreateModel(ModelData data)
{
    std::filesystem::path name = data.name;
    auto shared = std::make_shared<ModelData>(std::move(data)); // std::function must be copyable
    return StartModel(std::move(name), {}, [shared] { return std::move(*shared); }, false, false);
}

ModelHandle AssetManager::CreatePrimitive(const PrimitiveDesc& desc)
{
    std::u8string key = ToU8(PrimitiveKey(desc)); // ASCII
    if (const auto it = m_ModelCache.find(key); it != m_ModelCache.end()) {
        ModelEntry& e = m_Models.entries[it->second];
        ++e.refCount;
        return {it->second, e.generation};
    }
    const ModelHandle handle =
        StartModel(PrimitiveKey(desc), key, [desc] { return MakePrimitive(desc); }, true, false);
    m_Models.entries[handle.index].primitive = desc;
    m_ModelCache.emplace(std::move(key), handle.index);
    return handle;
}

ModelHandle AssetManager::StartModel(std::filesystem::path path, std::u8string key,
                                     std::function<ModelData()> produce, bool reloadable, bool watch)
{
    const std::uint32_t index = m_Models.Allocate();
    ModelEntry&         e     = m_Models.entries[index];
    e.state    = AssetState::Loading;
    e.refCount = 1;
    e.key      = std::move(key);
    e.path     = std::move(path);
    if (watch)
        e.watched = Watch({e.path});
    if (reloadable)
        e.produce = produce;
    const ModelHandle handle{index, e.generation};
    StartModelJob(handle, std::move(produce));
    return handle;
}

void AssetManager::StartModelJob(ModelHandle handle, std::function<ModelData()> produce)
{
    ModelEntry& e = m_Models.entries[handle.index];
    e.jobRunning  = true;
    for (WatchedFile& f : e.watched) // changes from here on trigger the next reload
        f.loaded = f.seen = ModifiedTime(f.path);
    EnqueueJob([this, handle, produce = std::move(produce)] { RunModelJob(handle, produce); });
}

void AssetManager::RunModelJob(ModelHandle handle, const std::function<ModelData()>& produce)
{
    // Worker thread: touches only the Renderer's thread-safe parts and the result queue.
    ModelResult result;
    result.handle = handle;
    if (m_ShuttingDown.load(std::memory_order_relaxed)) {
        result.error = "cancelled";
    } else {
        try {
            ModelData  data  = produce();
            const auto start = std::chrono::steady_clock::now();
            const MeshOptimizeStats optimized = OptimizeMeshes(data, m_Desc.meshes);
            if (optimized.lodLevels > 0)
                ENGINE_INFO("Optimized '{}': {} submeshes, {} LOD levels (+{} indices) in {:.0f} ms", data.name,
                            optimized.submeshes, optimized.lodLevels, optimized.lodIndices,
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            result.model = std::make_unique<Model>();
            BuildModelGeometry(m_Renderer, data, *result.model, result.ticket);
            result.materials    = std::move(data.materials);
            result.textures     = std::move(data.textures);
            result.dependencies = std::move(data.dependencies);
        } catch (const std::exception& ex) {
            result.error = ex.what();
        } catch (...) {
            result.error = "unknown error";
        }
    }
    std::scoped_lock lock{m_ResultMutex};
    m_ModelResults.push_back(std::move(result));
    FinishJob();
}

void AssetManager::OnModelResult(ModelResult& result)
{
    const ModelHandle handle = result.handle;
    ModelEntry&       e      = m_Models.entries[handle.index]; // slot stays pinned while its job runs
    assert(e.alive && e.generation == handle.generation);
    e.jobRunning = false;

    if (e.orphaned) {
        DestroyModel(std::move(result.model), result.ticket);
        m_Models.Free(handle.index);
        return;
    }
    if (!result.error.empty()) {
        DestroyModel(std::move(result.model), result.ticket); // partially built GPU resources
        FailModel(handle, e, std::move(result.error));
        return;
    }

    // Textures are shared assets, acquired now so the materials can use their table entries.
    // (Neither call touches m_Models: `e` stays valid.)
    Model&                     model = *result.model;
    std::vector<std::uint32_t> entries;
    for (TextureData& texture : result.textures) {
        const TextureHandle t = texture.file.empty() ? AcquireEmbedded(texture, model.name)
                                                     : LoadTexture(texture.file, texture.kind);
        model.textures.push_back(t);
        entries.push_back(TableEntry(t));
    }
    try {
        BuildModelMaterials(m_Renderer, result.materials, entries, model, result.ticket);
    } catch (const std::exception& ex) {
        DestroyModel(std::move(result.model), result.ticket);
        FailModel(handle, e, ex.what());
        return;
    }

    if (!e.watched.empty()) { // source file first, then what this version depends on
        e.watched.resize(1);
        for (WatchedFile& f : Watch(std::move(result.dependencies)))
            e.watched.push_back(std::move(f));
    }
    DestroyModel(std::move(e.pending), e.pendingTicket); // never set here: one job at a time
    e.pending       = std::move(result.model);
    e.pendingTicket = result.ticket;
    if (e.state == AssetState::Loading)
        e.state = AssetState::Uploading;
    m_PendingModels.push_back(handle);
}

void AssetManager::FailModel(ModelHandle handle, ModelEntry& e, std::string error)
{
    e.error = std::move(error);
    ENGINE_ERROR("Failed to load '{}': {}", ToUtf8(e.path), e.error);
    if (e.state != AssetState::Ready && e.state != AssetState::Failed) {
        e.state = AssetState::Failed; // renders as the placeholder from now on
        ++e.revision;
        ++m_ContentVersion;
    }
    m_Publish.push_back([this, ev = AssetFailedEvent<Model>{handle, ToUtf8(e.path), e.error}] { m_Events.Publish(ev); });
    if (e.reloadQueued) {
        e.reloadQueued = false;
        StartModelJob(handle, e.produce);
    }
}

void AssetManager::DestroyModel(std::unique_ptr<Model> model, UploadTicket ticket)
{
    if (!model)
        return;
    for (TextureHandle t : model->textures)
        Release(t);
    model->textures.clear();
    // Never free what the transfer queue may still write, nor skip the acquire it is queued for.
    if (m_Renderer.GetUploader().IsReady(ticket))
        ReleaseModel(m_Renderer, std::move(*model));
    else
        m_ModelGraveyard.emplace_back(std::move(model), ticket);
}

bool AssetManager::TexturesSettled(const Model& model) const
{
    for (TextureHandle t : model.textures) {
        const AssetState s = State(t);
        if (s == AssetState::Loading || s == AssetState::Uploading)
            return false;
    }
    return true;
}

void AssetManager::Release(ModelHandle handle)
{
    ModelEntry* e = m_Models.Find(handle);
    if (!e) {
        ENGINE_WARN("AssetManager::Release: stale or null model handle");
        return;
    }
    assert(e->refCount > 0);
    if (--e->refCount > 0)
        return;

    if (!e->key.empty())
        m_ModelCache.erase(e->key); // a new Load() of this path starts from scratch
    DestroyModel(std::move(e->pending), e->pendingTicket);
    DestroyModel(std::move(e->model), e->ticket);
    ++m_ContentVersion; // users drop what they built from it (BVH proxies, cached shadows)
    if (e->jobRunning) {
        e->orphaned = true; // the worker still reports to this slot
        return;
    }
    m_Models.Free(handle.index);
}

bool AssetManager::Reload(ModelHandle handle)
{
    ModelEntry* e = m_Models.Find(handle);
    if (!e || !e->produce)
        return false;
    if (e->jobRunning || e->pending)
        e->reloadQueued = true;
    else
        StartModelJob(handle, e->produce);
    return true;
}

const Model* AssetManager::Get(ModelHandle handle) const
{
    const ModelEntry* e = m_Models.Find(handle);
    return e && e->state == AssetState::Ready ? e->model.get() : nullptr;
}

AssetState AssetManager::State(ModelHandle handle) const
{
    const ModelEntry* e = m_Models.Find(handle);
    return e ? e->state : AssetState::Invalid;
}

std::string AssetManager::Error(ModelHandle handle) const
{
    const ModelEntry* e = m_Models.Find(handle);
    return e ? e->error : std::string{};
}

std::uint32_t AssetManager::RefCount(ModelHandle handle) const
{
    const ModelEntry* e = m_Models.Find(handle);
    return e ? e->refCount : 0;
}

std::uint32_t AssetManager::Revision(ModelHandle handle) const
{
    const ModelEntry* e = m_Models.Find(handle);
    return e ? e->revision : 0;
}

ModelSource AssetManager::Source(ModelHandle handle) const
{
    const ModelEntry* e = m_Models.Find(handle);
    if (!e)
        return {};
    if (e->primitive)
        return {.file = {}, .primitive = e->primitive};
    if (e->key.empty())
        return {}; // CreateModel
    return {.file = e->path, .primitive = std::nullopt};
}

ResolvedMesh AssetManager::ResolveMesh(ModelHandle handle, std::uint32_t meshIndex) const
{
    const ModelEntry* e = m_Models.Find(handle);
    if (!e)
        return {};
    if (e->state == AssetState::Ready && e->model) {
        if (meshIndex < e->model->meshes.size())
            return {.model = e->model.get(), .meshIndex = meshIndex, .placeholder = false};
        return {};
    }
    if (e->state == AssetState::Failed && m_Placeholder)
        return {.model = m_Placeholder.get(), .meshIndex = 0, .placeholder = true};
    return {};
}

std::vector<ModelInfo> AssetManager::Models() const
{
    std::vector<ModelInfo> models;
    for (std::uint32_t i = 0; i < m_Models.entries.size(); ++i) {
        const ModelEntry& e = m_Models.entries[i];
        if (!e.alive || e.orphaned)
            continue;
        ModelInfo info{.handle     = {i, e.generation},
                       .state      = e.state,
                       .refCount   = e.refCount,
                       .path       = ToUtf8(e.path),
                       .error      = e.error,
                       .revision   = e.revision,
                       .reloading  = e.jobRunning || e.pending != nullptr,
                       .reloadable = static_cast<bool>(e.produce)};
        if (const Model* model = e.model.get()) {
            info.textures = static_cast<std::uint32_t>(model->textures.size());
            info.gpuBytes = ModelGpuBytes(*model);
            info.cpuBytes = ModelCpuBytes(*model);
            for (const Mesh& mesh : model->meshes)
                for (const Submesh& sm : mesh.submeshes) {
                    info.triangles += sm.indexCount / 3;
                    info.lodLevels += sm.lodCount - 1;
                }
        }
        models.push_back(std::move(info));
    }
    return models;
}

// --- Textures ------------------------------------------------------------------------------------

TextureHandle AssetManager::LoadTexture(const std::filesystem::path& path, TextureKind kind)
{
    std::filesystem::path normalized = NormalizePath(path);
    std::u8string         key        = normalized.generic_u8string() + u8"|" + ToU8(ToString(kind));
    return AcquireTexture(std::move(key), normalized, kind, false, [normalized] { return ReadBytes(normalized); });
}

TextureHandle AssetManager::AcquireEmbedded(TextureData& data, const std::string& owner)
{
    // Shared by content: identical images (other models, a reloaded model) reuse the asset.
    std::u8string key = ToU8(std::format("embedded:{:016x}:{}:{}", data.hash, data.encoded.size(), ToString(data.kind)));
    auto bytes = std::make_shared<const std::vector<std::byte>>(std::move(data.encoded));
    return AcquireTexture(std::move(key), owner + "#" + data.name, data.kind, true, [bytes] { return *bytes; });
}

TextureHandle AssetManager::AcquireTexture(std::u8string key, std::filesystem::path path, TextureKind kind,
                                           bool embedded, std::function<std::vector<std::byte>()> source)
{
    if (const auto it = m_TextureCache.find(key); it != m_TextureCache.end()) {
        TextureEntry& e = m_Textures.entries[it->second];
        ++e.refCount;
        return {it->second, e.generation};
    }
    const std::uint32_t index = m_Textures.Allocate();
    TextureEntry&       e     = m_Textures.entries[index];
    e.state      = AssetState::Loading;
    e.refCount   = 1;
    e.key        = key;
    e.path       = std::move(path);
    e.kind       = kind;
    e.embedded   = embedded;
    e.tableEntry = m_Renderer.AllocateTextureEntry(m_Renderer.DefaultTextureIndex(LoadingTexture(kind)));
    if (!embedded) { // embedded bytes are not kept: they reload with their model
        e.watched = Watch({e.path});
        e.source  = source;
    }
    m_TextureCache.emplace(std::move(key), index);
    const TextureHandle handle{index, e.generation};
    StartTextureJob(handle, std::move(source));
    return handle;
}

void AssetManager::StartTextureJob(TextureHandle handle, std::function<std::vector<std::byte>()> source)
{
    TextureEntry& e = m_Textures.entries[handle.index];
    e.jobRunning    = true;
    for (WatchedFile& f : e.watched)
        f.loaded = f.seen = ModifiedTime(f.path);
    EnqueueJob([this, handle, source = std::move(source), kind = e.kind, name = ToUtf8(e.path.filename())] {
        RunTextureJob(handle, source, kind, name);
    });
}

void AssetManager::RunTextureJob(TextureHandle handle, const std::function<std::vector<std::byte>()>& source,
                                 TextureKind kind, const std::string& name)
{
    TextureResult result;
    result.handle = handle;
    if (m_ShuttingDown.load(std::memory_order_relaxed)) {
        result.error = "cancelled";
    } else {
        try {
            const CookResult    cooked = CookTexture(source(), kind, m_Cook);
            const TextureImage& image  = *cooked.image;
            result.texture             = std::make_unique<Texture>();
            Texture& texture           = *result.texture;
            texture.name      = name;
            texture.kind      = kind;
            texture.format    = image.format;
            texture.width     = image.width;
            texture.height    = image.height;
            texture.mipLevels = static_cast<std::uint32_t>(image.levels.size());
            texture.cacheHit  = cooked.cacheHit;
            std::vector<UploadMip> mips;
            for (const TextureImage::Level& level : image.levels) {
                mips.push_back({.offset = level.offset, .size = level.size, .width = level.width, .height = level.height});
                texture.gpuBytes += level.size;
            }
            texture.image = m_Renderer.GetUploader().CreateTexture(image.format, image.width, image.height, image.data,
                                                                   mips, result.ticket, name.c_str());
            texture.bindlessSlot = m_Renderer.GetBindless().AddSampledImage(texture.image.View());
        } catch (const std::exception& ex) {
            result.error = ex.what();
        } catch (...) {
            result.error = "unknown error";
        }
    }
    std::scoped_lock lock{m_ResultMutex};
    m_TextureResults.push_back(std::move(result));
    FinishJob();
}

void AssetManager::OnTextureResult(TextureResult& result)
{
    const TextureHandle handle = result.handle;
    TextureEntry&       e      = m_Textures.entries[handle.index];
    assert(e.alive && e.generation == handle.generation);
    e.jobRunning = false;

    if (e.orphaned) {
        DestroyTexture(std::move(result.texture), result.ticket);
        m_Textures.Free(handle.index);
        return;
    }
    if (!result.error.empty()) {
        DestroyTexture(std::move(result.texture), result.ticket);
        FailTexture(handle, e, std::move(result.error));
        return;
    }
    DestroyTexture(std::move(e.pending), e.pendingTicket); // never set here: one job at a time
    e.pending       = std::move(result.texture);
    e.pendingTicket = result.ticket;
    if (e.state == AssetState::Loading)
        e.state = AssetState::Uploading;
    m_PendingTextures.push_back(handle);
}

void AssetManager::FailTexture(TextureHandle handle, TextureEntry& e, std::string error)
{
    e.error = std::move(error);
    ENGINE_ERROR("Failed to load texture '{}': {}", ToUtf8(e.path), e.error);
    if (e.state != AssetState::Ready && e.state != AssetState::Failed) {
        e.state = AssetState::Failed;
        ++e.revision;
        m_Renderer.SetTextureEntry(e.tableEntry, m_Renderer.DefaultTextureIndex(FailedTexture(e.kind)));
    }
    m_Publish.push_back([this, ev = AssetFailedEvent<Texture>{handle, ToUtf8(e.path), e.error}] { m_Events.Publish(ev); });
    if (e.reloadQueued) {
        e.reloadQueued = false;
        StartTextureJob(handle, e.source);
    }
}

void AssetManager::DestroyTexture(std::unique_ptr<Texture> texture, UploadTicket ticket)
{
    if (!texture)
        return;
    if (m_Renderer.GetUploader().IsReady(ticket))
        ReleaseTexture(m_Renderer, std::move(*texture));
    else
        m_TextureGraveyard.emplace_back(std::move(texture), ticket);
}

void AssetManager::Release(TextureHandle handle)
{
    TextureEntry* e = m_Textures.Find(handle);
    if (!e) {
        ENGINE_WARN("AssetManager::Release: stale or null texture handle");
        return;
    }
    assert(e->refCount > 0);
    if (--e->refCount > 0)
        return;

    m_TextureCache.erase(e->key);
    DestroyTexture(std::move(e->pending), e->pendingTicket);
    DestroyTexture(std::move(e->texture), e->ticket);
    m_Renderer.FreeTextureEntry(e->tableEntry); // deferred: in-flight frames may still read it
    if (e->jobRunning) {
        e->orphaned = true;
        return;
    }
    m_Textures.Free(handle.index);
}

bool AssetManager::Reload(TextureHandle handle)
{
    TextureEntry* e = m_Textures.Find(handle);
    if (!e || e->embedded || !e->source)
        return false;
    if (e->jobRunning || e->pending)
        e->reloadQueued = true;
    else
        StartTextureJob(handle, e->source);
    return true;
}

const Texture* AssetManager::Get(TextureHandle handle) const
{
    const TextureEntry* e = m_Textures.Find(handle);
    return e && e->state == AssetState::Ready ? e->texture.get() : nullptr;
}

AssetState AssetManager::State(TextureHandle handle) const
{
    const TextureEntry* e = m_Textures.Find(handle);
    return e ? e->state : AssetState::Invalid;
}

std::string AssetManager::Error(TextureHandle handle) const
{
    const TextureEntry* e = m_Textures.Find(handle);
    return e ? e->error : std::string{};
}

std::uint32_t AssetManager::RefCount(TextureHandle handle) const
{
    const TextureEntry* e = m_Textures.Find(handle);
    return e ? e->refCount : 0;
}

std::uint32_t AssetManager::Revision(TextureHandle handle) const
{
    const TextureEntry* e = m_Textures.Find(handle);
    return e ? e->revision : 0;
}

std::uint32_t AssetManager::TableEntry(TextureHandle handle) const
{
    const TextureEntry* e = m_Textures.Find(handle);
    return e ? e->tableEntry : static_cast<std::uint32_t>(DefaultTexture::White);
}

std::vector<TextureInfo> AssetManager::Textures() const
{
    std::vector<TextureInfo> textures;
    for (std::uint32_t i = 0; i < m_Textures.entries.size(); ++i) {
        const TextureEntry& e = m_Textures.entries[i];
        if (!e.alive || e.orphaned)
            continue;
        TextureInfo info{.handle     = {i, e.generation},
                         .state      = e.state,
                         .refCount   = e.refCount,
                         .path       = ToUtf8(e.path),
                         .error      = e.error,
                         .revision   = e.revision,
                         .reloading  = e.jobRunning || e.pending != nullptr,
                         .embedded   = e.embedded,
                         .kind       = e.kind,
                         .tableEntry = e.tableEntry};
        if (const Texture* t = e.texture.get()) {
            info.format       = t->format;
            info.width        = t->width;
            info.height       = t->height;
            info.mipLevels    = t->mipLevels;
            info.gpuBytes     = t->gpuBytes;
            info.cacheHit     = t->cacheHit;
            info.bindlessSlot = t->bindlessSlot;
        }
        textures.push_back(std::move(info));
    }
    return textures;
}

// --- Per frame -----------------------------------------------------------------------------------

void AssetManager::SetHotReload(bool enabled)
{
    m_HotReload = enabled;
}

void AssetManager::PollFiles()
{
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - m_LastPoll).count() < m_Desc.pollSeconds)
        return;
    m_LastPoll = now;

    // Changed since the load and unchanged since the last poll: the writer is done (debounce).
    const auto changed = [](std::vector<WatchedFile>& files) {
        bool any = false, stable = true;
        for (WatchedFile& f : files) {
            const auto time = ModifiedTime(f.path);
            if (time != f.loaded) {
                any    = true;
                stable = stable && time == f.seen;
            }
            f.seen = time;
        }
        return any && stable;
    };
    for (std::uint32_t i = 0; i < m_Models.entries.size(); ++i) {
        ModelEntry& e = m_Models.entries[i];
        if (e.alive && !e.orphaned && !e.jobRunning && !e.pending && e.produce && changed(e.watched)) {
            ENGINE_INFO("Hot reload: '{}'", ToUtf8(e.path));
            StartModelJob({i, e.generation}, e.produce);
        }
    }
    for (std::uint32_t i = 0; i < m_Textures.entries.size(); ++i) {
        TextureEntry& e = m_Textures.entries[i];
        if (e.alive && !e.orphaned && !e.jobRunning && !e.pending && e.source && changed(e.watched)) {
            ENGINE_INFO("Hot reload: '{}'", ToUtf8(e.path));
            StartTextureJob({i, e.generation}, e.source);
        }
    }
}

void AssetManager::Update()
{
    std::vector<ModelResult>   modelResults;
    std::vector<TextureResult> textureResults;
    {
        std::scoped_lock lock{m_ResultMutex};
        modelResults.swap(m_ModelResults);
        textureResults.swap(m_TextureResults);
    }
    for (TextureResult& r : textureResults)
        OnTextureResult(r);
    for (ModelResult& r : modelResults) // may acquire textures (new jobs)
        OnModelResult(r);

    const UploadQueue& uploader = m_Renderer.GetUploader();

    // Textures first: a model waits for its textures and can then swap in this frame.
    std::vector<TextureHandle> restartTextures;
    std::erase_if(m_PendingTextures, [&](TextureHandle h) {
        TextureEntry* e = m_Textures.Find(h);
        if (!e || !e->pending)
            return true; // released in the meantime
        if (!uploader.IsReady(e->pendingTicket))
            return false;
        const bool reload = e->state == AssetState::Ready;
        DestroyTexture(std::move(e->texture), e->ticket); // in-flight frames keep it (deferred)
        e->texture       = std::move(e->pending);
        e->ticket        = e->pendingTicket;
        e->pendingTicket = 0;
        e->state         = AssetState::Ready;
        e->error.clear();
        ++e->revision;
        m_Renderer.SetTextureEntry(e->tableEntry, e->texture->bindlessSlot);
        if (reload)
            m_Publish.push_back([this, h] { m_Events.Publish(AssetReloadedEvent<Texture>{h}); });
        else
            m_Publish.push_back([this, h] { m_Events.Publish(AssetLoadedEvent<Texture>{h}); });
        if (e->reloadQueued)
            restartTextures.push_back(h);
        return true;
    });

    std::vector<ModelHandle> restartModels;
    std::erase_if(m_PendingModels, [&](ModelHandle h) {
        ModelEntry* e = m_Models.Find(h);
        if (!e || !e->pending)
            return true;
        if (!uploader.IsReady(e->pendingTicket) || !TexturesSettled(*e->pending))
            return false;
        const bool reload = e->state == AssetState::Ready;
        // The old version's textures are released after the new one acquired its own: shared
        // (unchanged) textures survive the reload.
        DestroyModel(std::move(e->model), e->ticket);
        e->model         = std::move(e->pending);
        e->ticket        = e->pendingTicket;
        e->pendingTicket = 0;
        e->state         = AssetState::Ready;
        e->error.clear();
        ++e->revision;
        ++m_ContentVersion;
        if (reload)
            m_Publish.push_back([this, h] { m_Events.Publish(AssetReloadedEvent<Model>{h}); });
        else
            m_Publish.push_back([this, h] { m_Events.Publish(AssetLoadedEvent<Model>{h}); });
        if (e->reloadQueued)
            restartModels.push_back(h);
        return true;
    });
    for (TextureHandle h : restartTextures)
        if (TextureEntry* e = m_Textures.Find(h)) {
            e->reloadQueued = false;
            StartTextureJob(h, e->source);
        }
    for (ModelHandle h : restartModels)
        if (ModelEntry* e = m_Models.Find(h)) {
            e->reloadQueued = false;
            StartModelJob(h, e->produce);
        }

    std::erase_if(m_ModelGraveyard, [&](auto& g) {
        if (!uploader.IsReady(g.second))
            return false;
        ReleaseModel(m_Renderer, std::move(*g.first));
        return true;
    });
    std::erase_if(m_TextureGraveyard, [&](auto& g) {
        if (!uploader.IsReady(g.second))
            return false;
        ReleaseTexture(m_Renderer, std::move(*g.first));
        return true;
    });

    if (m_HotReload)
        PollFiles();

    // Last: handlers may call Load / Release / Reload.
    std::vector<std::function<void()>> publish;
    publish.swap(m_Publish);
    for (const auto& fn : publish)
        fn();
}

} // namespace Engine
