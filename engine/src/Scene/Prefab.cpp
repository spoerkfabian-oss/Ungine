#include "Engine/Scene/Prefab.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "PrefabInternal.h"
#include "SceneJson.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

using SceneJson::json;

// A parsed prefab file: entities in pre-order (root first) in canonical form (as EntityToJson
// writes them in Absolute mode, plus "uuid" / "parent"), so instances compare against it exactly.
struct PrefabAsset {
    std::string                                    file; // normalized
    std::filesystem::file_time_type                mtime{};
    std::uintmax_t                                 size = 0;
    std::uint64_t                                  root = 0;
    std::vector<json>                              entities;
    std::unordered_map<std::uint64_t, std::size_t> index; // prefab UUID -> entities[i]
};

namespace {

using SceneJson::ApplyComponents;
using SceneJson::CollectSubtree;
using SceneJson::EntityToJson;
using SceneJson::FromUtf8;
using SceneJson::ModelRefs;
using SceneJson::NormalizedFile;
using SceneJson::PathMode;
using SceneJson::ToUtf8;
using SceneJson::UuidOf;

constexpr int kPrefabVersion = 1;

using Acquired  = std::unordered_map<std::string, ModelHandle>;
using SourceMap = std::unordered_map<std::uint64_t, std::uint64_t>; // prefab UUID -> instance UUID

// --- Prefab files -------------------------------------------------------------------------------

struct PrefabCache {
    std::mutex                                                          mutex;
    std::unordered_map<std::string, std::shared_ptr<const PrefabAsset>> entries; // by normalized file
};
PrefabCache& Cache()
{
    static PrefabCache cache;
    return cache;
}

// Canonical form of one prefab entity (paths already absolute): read into a scratch entity and
// written back, so defaults, clamping and float precision match what instances produce.
json Canonical(const json& source, Scene& scratch)
{
    json in = source;
    for (const char* key : {"uuid", "parent", "prefabInstance", "prefabLink", "prefab"})
        in.erase(key);
    json mesh, instance; // model references are not resolved here
    if (const auto it = in.find("mesh"); it != in.end()) {
        mesh = *it;
        in.erase(it);
    }
    if (const auto it = in.find("modelInstance"); it != in.end()) {
        instance = *it;
        in.erase(it);
    }
    ModelRefs    memory; // nothing model-related left; paths stay as they are
    const Entity e   = scratch.CreateEntity(in.value("name", std::string()));
    ApplyComponents(scratch, e, in, memory);
    json out = EntityToJson(scratch.GetRegistry(), e, memory);
    scratch.DestroyEntity(e);
    out.erase("uuid");
    if (mesh.is_object() && mesh.contains("model"))
        out["mesh"] = {{"model", SceneJson::CanonicalModelRef(mesh["model"])},
                       {"index", mesh.value("index", std::uint32_t{0})}};
    if (instance.is_object() && instance.contains("model"))
        out["modelInstance"] = {{"model", SceneJson::CanonicalModelRef(instance["model"])}};
    return out;
}

std::shared_ptr<const PrefabAsset> ParsePrefab(const std::string& file, std::filesystem::file_time_type mtime,
                                               std::uintmax_t size)
{
    const std::filesystem::path path = FromUtf8(file);
    std::ifstream               in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read '" + file + "'");
    json root;
    try {
        root = json::parse(in);
    } catch (const json::exception& e) {
        throw std::runtime_error("'" + file + "': " + e.what());
    }
    const int version = root.value("version", 0);
    if (version < 1 || version > kPrefabVersion || !root.value("prefab", false))
        throw std::runtime_error("'" + file + "': not a prefab (version " + std::to_string(version) + ")");
    const auto list = root.find("entities");
    if (list == root.end() || !list->is_array() || list->empty())
        throw std::runtime_error("'" + file + "': no entities");

    auto asset   = std::make_shared<PrefabAsset>();
    asset->file  = file;
    asset->mtime = mtime;
    asset->size  = size;
    const std::filesystem::path dir = path.parent_path();
    Scene                       scratch;
    for (const json& j : *list) {
        const std::uint64_t uuid   = j.value("uuid", std::uint64_t{0});
        const std::uint64_t parent = j.value("parent", std::uint64_t{0});
        if (uuid == 0 || asset->index.contains(uuid))
            throw std::runtime_error("'" + file + "': missing or duplicate entity uuid");
        if (asset->entities.empty() ? parent != 0 : !asset->index.contains(parent))
            throw std::runtime_error("'" + file + "': entities must be listed parents first, below one root");
        json absolute = j;
        SceneJson::TransformPaths(absolute, [&](const std::string& p) {
            const std::filesystem::path f = FromUtf8(p);
            return NormalizedFile(f.is_relative() ? dir / f : f);
        });
        json c                 = Canonical(absolute, scratch);
        c["uuid"]              = uuid;
        c["parent"]            = parent;
        asset->index[uuid]     = asset->entities.size();
        asset->entities.push_back(std::move(c));
    }
    asset->root = asset->entities.front().at("uuid").get<std::uint64_t>();
    return asset;
}

// The current content of a prefab file (cached until the file changes). Null if it cannot be read
// (error filled in).
std::shared_ptr<const PrefabAsset> LoadPrefab(const std::string& file, std::string* error = nullptr)
{
    const std::string           key  = NormalizedFile(FromUtf8(file));
    const std::filesystem::path path = FromUtf8(key);
    std::error_code             ec;
    const auto                  mtime = std::filesystem::last_write_time(path, ec);
    const std::uintmax_t        size  = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec) {
        if (error)
            *error = "cannot read '" + key + "': " + ec.message();
        return nullptr;
    }
    PrefabCache&     cache = Cache();
    std::scoped_lock lock(cache.mutex);
    auto&            slot = cache.entries[key];
    if (slot && slot->mtime == mtime && slot->size == size)
        return slot;
    try {
        slot = ParsePrefab(key, mtime, size);
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return nullptr;
    }
    return slot;
}

std::shared_ptr<const PrefabAsset> LoadPrefabOrThrow(const std::string& file)
{
    std::string error;
    auto        asset = LoadPrefab(file, &error);
    if (!asset)
        throw std::runtime_error("Prefab: " + error);
    return asset;
}

// --- Instances ----------------------------------------------------------------------------------

std::uint64_t NewUuid(const Scene& scene, const std::unordered_set<std::uint64_t>& taken)
{
    static thread_local std::mt19937_64 random{std::random_device{}()};
    std::uint64_t                       uuid = 0;
    while (uuid == 0 || scene.FindByUuid(uuid) != NullEntity || taken.contains(uuid))
        uuid = random();
    return uuid;
}

// Members of the instance rooted at `root` (not the root), by prefab UUID.
std::unordered_map<std::uint64_t, Entity> Members(const Scene& scene, Entity root)
{
    const Registry&     r    = scene.GetRegistry();
    const std::uint64_t uuid = UuidOf(r, root);
    std::vector<Entity> subtree;
    CollectSubtree(r, root, subtree);
    std::unordered_map<std::uint64_t, Entity> members;
    for (std::size_t i = 1; i < subtree.size(); ++i)
        if (const auto* link = r.TryGet<PrefabLink>(subtree[i]); link && link->instance == uuid)
            members.try_emplace(link->source, subtree[i]);
    return members;
}

std::shared_ptr<const PrefabAsset> AssetOf(const Registry& r, Entity root)
{
    const auto& instance = r.Get<PrefabInstance>(root);
    return instance.built ? instance.built : LoadPrefab(instance.prefab);
}

// What the prefab entity looks like in this instance (UUID references mapped into the instance).
// Instance roots keep their own name and transform.
json Expected(const json& prefabEntity, const SourceMap& uuids, bool root)
{
    json j = prefabEntity;
    j.erase("uuid");
    j.erase("parent");
    SceneJson::RemapEntityRefs(j, [&](std::uint64_t u) {
        const auto it = uuids.find(u);
        return it != uuids.end() ? it->second : u;
    });
    if (root) {
        j.erase("name");
        j.erase("transform");
    }
    return j;
}

json Actual(const Registry& r, Entity e, const AssetManager* assets, bool root)
{
    ModelRefs refs;
    refs.mode        = PathMode::Absolute;
    refs.constAssets = assets;
    json j           = EntityToJson(r, e, refs);
    j.erase("uuid");
    if (root) {
        j.erase("name");
        j.erase("transform");
    }
    return j;
}

// RFC 7396 merge patch turning `from` into `to` (objects recursively, everything else replaced,
// removed keys as null).
json CreateMergePatch(const json& from, const json& to)
{
    if (!from.is_object() || !to.is_object())
        return to;
    json patch = json::object();
    for (auto it = to.begin(); it != to.end(); ++it) {
        const auto old = from.find(it.key());
        if (old == from.end())
            patch[it.key()] = *it;
        else if (*old != *it)
            patch[it.key()] = old->is_object() && it->is_object() ? CreateMergePatch(*old, *it) : *it;
    }
    for (auto it = from.begin(); it != from.end(); ++it)
        if (!to.contains(it.key()))
            patch[it.key()] = nullptr;
    return patch;
}

struct InstanceData {
    SourceMap members;                   // prefab UUID -> instance UUID
    json      patches = json::object(); // prefab UUID (string) -> merge patch, null = removed
};

SourceMap InstanceUuids(const Registry& r, Entity root, const PrefabAsset& asset,
                        const std::unordered_map<std::uint64_t, Entity>& members)
{
    SourceMap uuids;
    for (const auto& [source, e] : members)
        uuids[source] = UuidOf(r, e);
    uuids[asset.root] = UuidOf(r, root);
    return uuids;
}

InstanceData Capture(const Scene& scene, const AssetManager* assets, Entity root, const PrefabAsset& asset)
{
    const Registry&   r       = scene.GetRegistry();
    const auto        members = Members(scene, root);
    InstanceData      data;
    data.members = InstanceUuids(r, root, asset, members);
    for (const json& pj : asset.entities) {
        const std::uint64_t source = pj.at("uuid").get<std::uint64_t>();
        const bool          isRoot = source == asset.root;
        Entity              e      = root;
        if (!isRoot) {
            const auto it = members.find(source);
            e             = it != members.end() ? it->second : NullEntity;
        }
        if (e == NullEntity) {
            data.patches[std::to_string(source)] = nullptr;
            continue;
        }
        json patch = CreateMergePatch(Expected(pj, data.members, isRoot), Actual(r, e, assets, isRoot));
        if (!patch.empty())
            data.patches[std::to_string(source)] = std::move(patch);
    }
    return data;
}

// Makes the instance rooted at `root` match `asset` + `data`: keeps existing members (stable
// handles), creates missing ones, destroys members the prefab or the data no longer has (their
// non-member children move to the root) and rewrites components only where they differ.
void Rebuild(Scene& scene, AssetManager* assets, Entity root, std::shared_ptr<const PrefabAsset> asset,
             const InstanceData& data, Acquired& acquired)
{
    Registry&           r        = scene.GetRegistry();
    const std::uint64_t rootUuid = UuidOf(r, root);
    const auto          existing = Members(scene, root);

    // Instance UUIDs: recorded, existing, else new.
    SourceMap                         uuids;
    std::unordered_set<std::uint64_t> taken;
    for (const json& pj : asset->entities) {
        const std::uint64_t source = pj.at("uuid").get<std::uint64_t>();
        std::uint64_t       uuid   = 0;
        if (source == asset->root) {
            uuid = rootUuid;
        } else if (const auto it = data.members.find(source); it != data.members.end() && it->second != 0) {
            uuid = it->second;
        } else if (const auto e = existing.find(source); e != existing.end()) {
            uuid = UuidOf(r, e->second);
        }
        if (uuid == 0 || taken.contains(uuid))
            uuid = NewUuid(scene, taken);
        taken.insert(uuid);
        uuids[source] = uuid;
    }

    // Flatten the existing members below the root first: placing them in prefab order can then
    // never create a cycle, whatever the old hierarchy was.
    for (const auto& [source, e] : existing)
        if (r.Get<Hierarchy>(e).parent != root)
            scene.SetParent(e, root);

    ModelRefs refs;
    refs.mode        = PathMode::Absolute;
    refs.assets      = assets;
    refs.constAssets = assets;
    refs.acquired    = &acquired;

    std::unordered_map<std::uint64_t, Entity>      placed{{asset->root, root}};
    std::unordered_map<std::uint64_t, std::size_t> nextIndex; // per prefab parent
    struct State {
        Entity        entity;
        std::uint64_t source;
        json          components;
    };
    std::vector<State> states;
    for (const json& pj : asset->entities) {
        const std::uint64_t source = pj.at("uuid").get<std::uint64_t>();
        const bool          isRoot = source == asset->root;
        const auto          patch  = data.patches.find(std::to_string(source));
        Entity              e      = root;
        if (!isRoot) {
            if (patch != data.patches.end() && patch->is_null())
                continue; // removed from this instance
            const std::uint64_t parentSource = pj.at("parent").get<std::uint64_t>();
            const auto          parent       = placed.find(parentSource);
            if (parent == placed.end())
                continue; // below a removed member
            const std::size_t index = nextIndex[parentSource]++;
            if (const auto it = existing.find(source); it != existing.end()) {
                e = it->second;
                if (r.Get<Hierarchy>(e).parent != parent->second || scene.SiblingIndex(e) != index)
                    scene.SetParent(e, parent->second, index);
            } else {
                e = scene.CreateEntity(pj.value("name", std::string("Entity")), NullEntity, uuids.at(source));
                scene.SetParent(e, parent->second, index);
            }
            placed[source] = e;
        }
        json state = Expected(pj, uuids, isRoot);
        if (patch != data.patches.end() && patch->is_object())
            state.merge_patch(*patch);
        states.push_back({e, source, std::move(state)});
    }

    // Members the instance no longer has: rescue their other children, then destroy them.
    std::unordered_set<std::uint64_t> keep;
    for (const auto& [source, e] : placed)
        keep.insert(static_cast<std::uint64_t>(e));
    std::vector<Entity> doomed;
    for (const auto& [source, e] : existing)
        if (!keep.contains(static_cast<std::uint64_t>(e)))
            doomed.push_back(e);
    const auto isDoomed = [&](Entity e) { return std::ranges::find(doomed, e) != doomed.end(); };
    for (Entity e : doomed) {
        const std::vector<Entity> children = r.Get<Hierarchy>(e).children;
        for (Entity child : children)
            if (!isDoomed(child))
                scene.SetParent(child, root);
    }
    for (Entity e : doomed)
        scene.DestroyEntity(e); // no-op for ones already destroyed with a doomed parent

    for (const State& state : states) {
        const bool isRoot = state.entity == root;
        if (Actual(r, state.entity, assets, isRoot) != state.components) // unchanged: no physics / script restart
            ApplyComponents(scene, state.entity, state.components, refs);
        if (!isRoot)
            r.EmplaceOrReplace<PrefabLink>(state.entity, PrefabLink{.instance = rootUuid, .source = state.source});
    }
    r.Get<PrefabInstance>(root).built = std::move(asset);
}

void AppendHandles(const Acquired& acquired, std::vector<ModelHandle>& models)
{
    for (const auto& [key, handle] : acquired)
        models.push_back(handle);
}

std::string RelativeTo(const std::string& absolute, const std::filesystem::path& dir)
{
    std::error_code             ec;
    const std::filesystem::path relative = std::filesystem::relative(FromUtf8(absolute), dir, ec);
    return ec || relative.empty() ? absolute : ToUtf8(relative);
}

// Writes the subtree of `root` as the prefab `file` and links the subtree to it (nested instances
// are flattened). Returns the new content.
std::shared_ptr<const PrefabAsset> WriteAndLink(const std::filesystem::path& file, Scene& scene,
                                                const AssetManager* assets, Entity root)
{
    Registry&           r        = scene.GetRegistry();
    const std::uint64_t rootUuid = UuidOf(r, root);
    std::vector<Entity> subtree;
    CollectSubtree(r, root, subtree);

    // Prefab UUIDs: members keep their source, the root its prefab root, new entities their UUID.
    std::uint64_t rootSource = rootUuid;
    if (r.Has<PrefabInstance>(root))
        if (const auto asset = AssetOf(r, root))
            rootSource = asset->root;
    SourceMap sources; // instance UUID -> prefab UUID
    sources[rootUuid] = rootSource;
    for (std::size_t i = 1; i < subtree.size(); ++i) {
        const auto* link   = r.TryGet<PrefabLink>(subtree[i]);
        const auto  uuid   = UuidOf(r, subtree[i]);
        sources[uuid]      = link && link->instance == rootUuid && link->source != 0 ? link->source : uuid;
    }
    {   // members of different instances could share a source: keep the prefab UUIDs unique
        std::unordered_set<std::uint64_t> used;
        for (Entity e : subtree) {
            auto& s = sources[UuidOf(r, e)];
            if (!used.insert(s).second) {
                s = UuidOf(r, e);
                used.insert(s);
            }
        }
    }

    const std::string           target = NormalizedFile(file);
    const std::filesystem::path dir    = FromUtf8(target).parent_path();
    json                        entities = json::array();
    for (Entity e : subtree) {
        ModelRefs refs;
        refs.mode        = PathMode::Absolute;
        refs.constAssets = assets;
        json j           = EntityToJson(r, e, refs);
        j["uuid"]        = sources.at(UuidOf(r, e));
        j["parent"]      = e == root ? 0 : sources.at(UuidOf(r, r.Get<Hierarchy>(e).parent));
        SceneJson::RemapEntityRefs(j, [&](std::uint64_t u) {
            const auto it = sources.find(u);
            return it != sources.end() ? it->second : u;
        });
        if (e == root)
            j["transform"] = {{"position", {0.0f, 0.0f, 0.0f}}, {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}}, {"scale", {1.0f, 1.0f, 1.0f}}};
        SceneJson::TransformPaths(j, [&](const std::string& p) { return RelativeTo(p, dir); });
        entities.push_back(std::move(j));
    }
    const json out{{"version", kPrefabVersion}, {"prefab", true}, {"root", rootSource}, {"entities", std::move(entities)}};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path temp = FromUtf8(target);
    temp += ".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Prefab: cannot write '" + ToUtf8(temp) + "'");
        stream << out.dump(2) << '\n';
        if (!stream)
            throw std::runtime_error("Prefab: write failed: '" + ToUtf8(temp) + "'");
    }
    std::filesystem::rename(temp, FromUtf8(target), ec);
    if (ec)
        throw std::runtime_error("Prefab: cannot replace '" + target + "': " + ec.message());

    auto asset = LoadPrefabOrThrow(target);
    for (std::size_t i = 1; i < subtree.size(); ++i) {
        r.Remove<PrefabInstance>(subtree[i]); // flattened
        r.EmplaceOrReplace<PrefabLink>(subtree[i],
                                       PrefabLink{.instance = rootUuid, .source = sources.at(UuidOf(r, subtree[i]))});
    }
    r.Remove<PrefabLink>(root);
    PrefabInstance instance(target);
    instance.built = asset;
    r.EmplaceOrReplace<PrefabInstance>(root, std::move(instance));
    return asset;
}

std::vector<Entity> InstanceRoots(Scene& scene)
{
    std::vector<Entity> roots;
    scene.GetRegistry().ViewOf<PrefabInstance>().Each([&](Entity e, PrefabInstance&) { roots.push_back(e); });
    return roots;
}

} // namespace

// --- Public API -----------------------------------------------------------------------------------

void CreatePrefab(const std::filesystem::path& file, Scene& scene, const AssetManager* assets, Entity root)
{
    if (!scene.GetRegistry().Has<Hierarchy>(root))
        throw std::invalid_argument("CreatePrefab: invalid entity");
    WriteAndLink(file, scene, assets, root);
}

Entity InstantiatePrefab(Scene& scene, AssetManager* assets, const std::filesystem::path& file, Entity parent,
                         const Transform& transform, std::vector<ModelHandle>& models)
{
    auto asset = LoadPrefabOrThrow(ToUtf8(file));
    const Entity root = scene.CreateEntity(asset->entities.front().value("name", std::string("Prefab")),
                                           scene.GetRegistry().Has<Hierarchy>(parent) ? parent : NullEntity);
    scene.SetTransform(root, transform);
    scene.GetRegistry().Emplace<PrefabInstance>(root, PrefabInstance(asset->file));
    Acquired acquired;
    Rebuild(scene, assets, root, asset, {}, acquired);
    AppendHandles(acquired, models);
    return root;
}

Entity PrefabInstanceRoot(const Scene& scene, Entity entity)
{
    const Registry& r = scene.GetRegistry();
    if (r.Has<PrefabInstance>(entity))
        return entity;
    const auto* link = r.TryGet<PrefabLink>(entity);
    if (!link || !r.Has<Hierarchy>(entity))
        return NullEntity;
    for (Entity p = r.Get<Hierarchy>(entity).parent; p != NullEntity; p = r.Get<Hierarchy>(p).parent)
        if (UuidOf(r, p) == link->instance)
            return r.Has<PrefabInstance>(p) ? p : NullEntity;
    return NullEntity;
}

std::vector<PrefabOverride> PrefabOverrides(const Scene& scene, const AssetManager* assets, Entity root)
{
    std::vector<PrefabOverride> out;
    const Registry&             r = scene.GetRegistry();
    if (!r.Has<PrefabInstance>(root))
        return out;
    const auto asset = AssetOf(r, root);
    if (!asset)
        return out;
    const InstanceData data    = Capture(scene, assets, root, *asset);
    const auto         members = Members(scene, root);
    for (auto it = data.patches.begin(); it != data.patches.end(); ++it) {
        PrefabOverride o;
        o.source = std::stoull(it.key());
        if (o.source == asset->root)
            o.entity = root;
        else if (const auto m = members.find(o.source); m != members.end())
            o.entity = m->second;
        o.removed = it->is_null();
        if (it->is_object())
            for (auto k = it->begin(); k != it->end(); ++k)
                o.keys.push_back(k.key());
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<std::string> PrefabOverriddenKeys(const Scene& scene, const AssetManager* assets, Entity entity)
{
    std::vector<std::string> keys;
    const Entity             root = PrefabInstanceRoot(scene, entity);
    if (root == NullEntity)
        return keys;
    const Registry& r     = scene.GetRegistry();
    const auto      asset = AssetOf(r, root);
    if (!asset)
        return keys;
    const bool          isRoot = entity == root;
    const std::uint64_t source = isRoot ? asset->root : r.Get<PrefabLink>(entity).source;
    const auto          pj     = asset->index.find(source);
    if (pj == asset->index.end())
        return keys;
    const json patch = CreateMergePatch(
        Expected(asset->entities[pj->second], InstanceUuids(r, root, *asset, Members(scene, root)), isRoot),
        Actual(r, entity, assets, isRoot));
    for (auto it = patch.begin(); it != patch.end(); ++it)
        keys.push_back(it.key());
    return keys;
}

void ApplyPrefabInstance(Scene& scene, AssetManager* assets, Entity root, std::vector<ModelHandle>& models)
{
    Registry& r = scene.GetRegistry();
    if (!r.Has<PrefabInstance>(root))
        throw std::invalid_argument("ApplyPrefabInstance: not an instance root");
    const std::string file = NormalizedFile(FromUtf8(r.Get<PrefabInstance>(root).prefab));

    // The other instances' overrides against the old content first.
    std::vector<std::pair<Entity, InstanceData>> others;
    for (Entity e : InstanceRoots(scene)) {
        if (e == root || scene.IsAncestor(root, e) || NormalizedFile(FromUtf8(r.Get<PrefabInstance>(e).prefab)) != file)
            continue;
        if (const auto asset = AssetOf(r, e))
            others.emplace_back(e, Capture(scene, assets, e, *asset));
    }
    const auto asset = WriteAndLink(FromUtf8(file), scene, assets, root);
    Acquired   acquired;
    for (const auto& [e, data] : others)
        Rebuild(scene, assets, e, asset, data, acquired);
    AppendHandles(acquired, models);
}

void RevertPrefabOverrides(Scene& scene, AssetManager* assets, Entity entity, std::string_view key,
                           std::vector<ModelHandle>& models)
{
    const Entity root = PrefabInstanceRoot(scene, entity);
    if (root == NullEntity)
        return;
    Registry&  r     = scene.GetRegistry();
    const auto asset = AssetOf(r, root);
    if (!asset)
        return;
    InstanceData data = Capture(scene, assets, root, *asset);
    if (entity == root && key.empty()) {
        data.patches = json::object();
    } else {
        const std::uint64_t source = entity == root ? asset->root : r.Get<PrefabLink>(entity).source;
        const auto          it     = data.patches.find(std::to_string(source));
        if (it == data.patches.end())
            return;
        if (key.empty() || !it->is_object())
            data.patches.erase(it);
        else if (it->erase(std::string(key)) == 0)
            return;
    }
    Acquired acquired;
    Rebuild(scene, assets, root, asset, data, acquired);
    AppendHandles(acquired, models);
}

void UnlinkPrefabInstance(Scene& scene, Entity root)
{
    Registry& r = scene.GetRegistry();
    if (!r.Has<PrefabInstance>(root))
        return;
    for (const auto& [source, e] : Members(scene, root))
        r.Remove<PrefabLink>(e);
    r.Remove<PrefabInstance>(root);
}

std::size_t RefreshPrefabInstances(Scene& scene, AssetManager* assets, std::vector<ModelHandle>& models)
{
    Registry&                                                           r = scene.GetRegistry();
    std::unordered_map<std::string, std::shared_ptr<const PrefabAsset>> current; // per call: one stat per file
    std::size_t                                                         updated = 0;
    Acquired                                                            acquired;
    for (Entity root : InstanceRoots(scene)) {
        if (!r.Valid(root) || !r.Has<PrefabInstance>(root))
            continue;
        PrefabInstance& instance = r.Get<PrefabInstance>(root);
        auto [slot, added]       = current.try_emplace(instance.prefab);
        if (added)
            slot->second = LoadPrefab(instance.prefab);
        const auto latest = slot->second;
        if (!latest || latest == instance.built)
            continue;
        if (!instance.built) { // restored from a snapshot: assume it matches the current file
            instance.built = latest;
            continue;
        }
        const InstanceData data = Capture(scene, assets, root, *instance.built);
        Rebuild(scene, assets, root, latest, data, acquired);
        ++updated;
    }
    AppendHandles(acquired, models);
    return updated;
}

// --- Scene files ----------------------------------------------------------------------------------

namespace PrefabDetail {

bool IsMember(const Registry& r, Entity e)
{
    const auto* link = r.TryGet<PrefabLink>(e);
    if (!link || r.Has<PrefabInstance>(e))
        return false;
    for (Entity p = r.Get<Hierarchy>(e).parent; p != NullEntity; p = r.Get<Hierarchy>(p).parent)
        if (UuidOf(r, p) == link->instance)
            return r.Has<PrefabInstance>(p);
    return false;
}

json InstanceToFileJson(const Scene& scene, const AssetManager* assets, Entity root, const std::filesystem::path& sceneDir)
{
    const Registry&       r        = scene.GetRegistry();
    const PrefabInstance& instance = r.Get<PrefabInstance>(root);
    const auto            toScene  = [&](const std::string& p) { return RelativeTo(p, sceneDir); };

    json data;
    if (const auto asset = AssetOf(r, root)) {
        const InstanceData captured = Capture(scene, assets, root, *asset);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> members(captured.members.begin(), captured.members.end());
        std::ranges::sort(members);
        data["members"] = json::array();
        for (const auto& [source, uuid] : members)
            if (source != asset->root)
                data["members"].push_back({source, uuid});
        data["overrides"] = captured.patches;
    } else if (instance.unresolved) {
        data = json::parse(*instance.unresolved, nullptr, false);
        if (data.is_discarded() || !data.is_object())
            data = json::object();
    }
    if (auto it = data.find("overrides"); it != data.end() && it->is_object())
        for (auto& patch : *it)
            if (patch.is_object())
                SceneJson::TransformPaths(patch, toScene);
    data["file"] = toScene(NormalizedFile(FromUtf8(instance.prefab)));
    return data;
}

std::vector<std::pair<std::uint64_t, Entity>> InstanceFromFileJson(Scene& scene, AssetManager* assets, Entity root,
                                                                   const json& data, const std::filesystem::path& sceneDir,
                                                                   ModelRefs& refs)
{
    const auto toAbsolute = [&](const std::string& p) {
        const std::filesystem::path f = FromUtf8(p);
        return NormalizedFile(f.is_relative() ? sceneDir / f : f);
    };
    const std::string file = toAbsolute(data.value("file", std::string()));

    InstanceData instance;
    if (const auto it = data.find("members"); it != data.end() && it->is_array())
        for (const json& m : *it)
            if (m.is_array() && m.size() == 2)
                instance.members[m[0].get<std::uint64_t>()] = m[1].get<std::uint64_t>();
    if (const auto it = data.find("overrides"); it != data.end() && it->is_object())
        instance.patches = *it;
    for (auto& patch : instance.patches)
        if (patch.is_object())
            SceneJson::TransformPaths(patch, toAbsolute);

    Registry& r = scene.GetRegistry();
    std::string error;
    const auto  asset = LoadPrefab(file, &error);
    if (!asset) {
        ENGINE_WARN("Scene: prefab instance '{}' kept without its prefab: {}", r.Get<Name>(root).value, error);
        json unresolved = data;
        unresolved["file"]      = file;
        unresolved["overrides"] = instance.patches;
        PrefabInstance pi(file);
        pi.unresolved = std::make_shared<const std::string>(unresolved.dump());
        r.EmplaceOrReplace<PrefabInstance>(root, std::move(pi));
        // Entities saved below members stay inside the instance: they hang on the root.
        std::vector<std::pair<std::uint64_t, Entity>> mapping;
        for (const auto& [source, uuid] : instance.members)
            mapping.emplace_back(uuid, root);
        return mapping;
    }
    r.EmplaceOrReplace<PrefabInstance>(root, PrefabInstance(file));
    Rebuild(scene, assets, root, asset, instance, *refs.acquired);

    // Recorded UUID -> member (differs if the recorded one was taken, e.g. a scene loaded twice).
    std::vector<std::pair<std::uint64_t, Entity>> mapping;
    const auto                                    members = Members(scene, root);
    for (const auto& [source, uuid] : instance.members)
        if (const auto it = members.find(source); it != members.end())
            mapping.emplace_back(uuid, it->second);
    return mapping;
}

} // namespace PrefabDetail
} // namespace Engine
