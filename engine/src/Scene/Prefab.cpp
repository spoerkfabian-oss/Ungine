#include "Engine/Scene/Prefab.h"
#include "Engine/Core/FileSystem.h"
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

// Where an entity of a prefab comes from: the file itself (nest < 0) or one of its nested prefab
// instances (`source` = the entity's UUID in that nested prefab).
struct PrefabOrigin {
    int           nest   = -1;
    std::uint64_t source = 0;
};
// A prefab instance inside a prefab file (a variant's root is one: its base).
struct PrefabNested {
    std::string                                       file; // normalized
    std::uint64_t                                     root = 0;
    std::shared_ptr<const struct PrefabAsset>         asset;
    std::unordered_map<std::uint64_t, std::uint64_t> map; // nested prefab UUID -> UUID in this prefab
};
struct PrefabDependency {
    std::string                     file;
    std::filesystem::file_time_type mtime{};
    std::uintmax_t                  size = 0;
};

// A parsed prefab file, nested prefab instances expanded: entities in pre-order (root first) in
// canonical form (as EntityToJson writes them in Absolute mode, plus "uuid" / "parent"), so
// instances compare against it exactly.
struct PrefabAsset {
    std::string                                    file; // normalized
    std::filesystem::file_time_type                mtime{};
    std::uintmax_t                                 size = 0;
    std::uint64_t                                  root = 0;
    std::vector<json>                              entities;
    std::vector<PrefabOrigin>                      origins; // per entity
    std::unordered_map<std::uint64_t, std::size_t> index;   // prefab UUID -> entities[i]
    std::vector<PrefabNested>                      nested;
    std::vector<PrefabDependency>                  dependencies; // nested prefab files, transitively

    [[nodiscard]] bool DependsOn(const std::string& f) const
    {
        return std::ranges::any_of(dependencies, [&](const PrefabDependency& d) { return d.file == f; });
    }
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

constexpr int         kPrefabVersion   = 2; // 2: nested prefab instances / variants
constexpr std::size_t kMaxPrefabNesting = 16;

using Acquired  = std::unordered_map<std::string, ModelHandle>;
using SourceMap = std::unordered_map<std::uint64_t, std::uint64_t>; // prefab UUID -> instance UUID

std::shared_ptr<const PrefabAsset> LoadPrefab(const std::string& file, std::string* error = nullptr);
json Expected(const json& prefabEntity, const SourceMap& uuids, bool root);

// Stable UUID of a nested prefab's entity inside the containing prefab when the file does not
// record one (splitmix64 of the nested instance root and the entity's UUID in the nested prefab).
std::uint64_t DeriveUuid(std::uint64_t a, std::uint64_t b)
{
    const auto mix = [](std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    };
    const std::uint64_t h = mix(a ^ mix(b));
    return h != 0 ? h : 1;
}

thread_local std::vector<std::string> t_Parsing; // prefab files being parsed (nesting cycles)

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
    const std::filesystem::path      path = FromUtf8(file);
    const std::optional<std::string> text = Vfs::ReadText(path); // pak entry or disk
    if (!text)
        throw std::runtime_error("cannot read '" + file + "'");
    json root;
    try {
        root = json::parse(*text);
    } catch (const json::exception& e) {
        throw std::runtime_error("'" + file + "': " + e.what());
    }
    const int version = root.value("version", 0);
    if (version < 1 || version > kPrefabVersion || !root.value("prefab", false))
        throw std::runtime_error("'" + file + "': not a prefab (version " + std::to_string(version) + ")");
    const auto list = root.find("entities");
    if (list == root.end() || !list->is_array() || list->empty())
        throw std::runtime_error("'" + file + "': no entities");
    if (std::ranges::find(t_Parsing, file) != t_Parsing.end())
        throw std::runtime_error("'" + file + "': contains an instance of itself");
    if (t_Parsing.size() >= kMaxPrefabNesting)
        throw std::runtime_error("'" + file + "': prefabs nested too deeply");
    t_Parsing.push_back(file);
    struct PopParsing {
        ~PopParsing() { t_Parsing.pop_back(); }
    } popParsing;

    auto asset   = std::make_shared<PrefabAsset>();
    asset->file  = file;
    asset->mtime = mtime;
    asset->size  = size;
    const std::filesystem::path dir        = path.parent_path();
    const auto                  toAbsolute = [&](const std::string& p) {
        const std::filesystem::path f = FromUtf8(p);
        return NormalizedFile(f.is_relative() ? dir / f : f);
    };
    const auto add = [&](json c, std::uint64_t uuid, std::uint64_t parent, PrefabOrigin origin) {
        c["uuid"]          = uuid;
        c["parent"]        = parent;
        asset->index[uuid] = asset->entities.size();
        asset->entities.push_back(std::move(c));
        asset->origins.push_back(origin);
    };
    Scene scratch;
    for (const json& j : *list) {
        const std::uint64_t uuid   = j.value("uuid", std::uint64_t{0});
        const std::uint64_t parent = j.value("parent", std::uint64_t{0});
        if (uuid == 0 || asset->index.contains(uuid))
            throw std::runtime_error("'" + file + "': missing or duplicate entity uuid");
        if (asset->entities.empty() ? parent != 0 : !asset->index.contains(parent))
            throw std::runtime_error("'" + file + "': entities must be listed parents first, below one root");

        const auto record = j.find("prefab");
        if (record == j.end() || !record->is_object()) {
            json absolute = j;
            SceneJson::TransformPaths(absolute, toAbsolute);
            add(Canonical(absolute, scratch), uuid, parent, {-1, uuid});
            continue;
        }

        // A nested prefab instance (stored like an instance in a scene file): expanded here with
        // this file's overrides, so instances of this prefab see the nested prefab's changes.
        std::string error;
        PrefabNested nested;
        nested.file  = toAbsolute(record->value("file", std::string()));
        nested.root  = uuid;
        nested.asset = LoadPrefab(nested.file, &error);
        if (!nested.asset)
            throw std::runtime_error("'" + file + "': nested prefab: " + error);
        const PrefabAsset& inner = *nested.asset;
        SourceMap          recorded;
        if (const auto members = record->find("members"); members != record->end() && members->is_array())
            for (const json& m : *members)
                if (m.is_array() && m.size() == 2 && m[0].is_number_unsigned() && m[1].is_number_unsigned())
                    recorded[m[0].get<std::uint64_t>()] = m[1].get<std::uint64_t>();
        std::unordered_set<std::uint64_t> used{uuid};
        for (const json& pj : inner.entities) {
            const std::uint64_t source = pj.at("uuid").get<std::uint64_t>();
            std::uint64_t       outer  = uuid;
            if (source != inner.root) {
                const auto it = recorded.find(source);
                outer         = it != recorded.end() ? it->second : 0;
                if (outer == 0 || asset->index.contains(outer) || used.contains(outer))
                    outer = DeriveUuid(uuid, source);
                while (asset->index.contains(outer) || used.contains(outer))
                    outer = DeriveUuid(outer, source);
            }
            used.insert(outer);
            nested.map[source] = outer;
        }
        json patches = json::object();
        if (const auto overrides = record->find("overrides"); overrides != record->end() && overrides->is_object())
            patches = *overrides;
        for (json& patch : patches)
            if (patch.is_object())
                SceneJson::TransformPaths(patch, toAbsolute);

        const int                         nest = static_cast<int>(asset->nested.size());
        std::unordered_set<std::uint64_t> placed;
        for (const json& pj : inner.entities) {
            const std::uint64_t source = pj.at("uuid").get<std::uint64_t>();
            const bool          isRoot = source == inner.root;
            const auto          patch  = patches.find(std::to_string(source));
            std::uint64_t       outerParent = parent;
            if (!isRoot) {
                const std::uint64_t innerParent = pj.at("parent").get<std::uint64_t>();
                if ((patch != patches.end() && patch->is_null()) || !placed.contains(innerParent))
                    continue; // removed here (or below a removed entity)
                outerParent = nested.map.at(innerParent);
            }
            placed.insert(source);
            json state = Expected(pj, nested.map, isRoot);
            if (isRoot) { // the nested instance root's own name and transform
                state["name"] = j.value("name", pj.value("name", std::string("Prefab")));
                if (const auto t = j.find("transform"); t != j.end())
                    state["transform"] = *t;
            }
            if (patch != patches.end() && patch->is_object())
                state.merge_patch(*patch);
            add(Canonical(state, scratch), nested.map.at(source), outerParent, {nest, source});
        }
        asset->dependencies.push_back({inner.file, inner.mtime, inner.size});
        asset->dependencies.insert(asset->dependencies.end(), inner.dependencies.begin(), inner.dependencies.end());
        asset->nested.push_back(std::move(nested));
    }
    asset->root = asset->entities.front().at("uuid").get<std::uint64_t>();
    return asset;
}

// Whether the files a parsed prefab was expanded from are unchanged.
bool DependenciesCurrent(const PrefabAsset& asset)
{
    for (const PrefabDependency& d : asset.dependencies) {
        const std::filesystem::path        path     = FromUtf8(d.file);
        const std::optional<Vfs::Location> location = Vfs::Locate(path);
        if (!location || location->size != d.size || Vfs::ModifiedTime(path) != d.mtime)
            return false;
    }
    return true;
}

// The current content of a prefab file (cached until the file changes). Null if it cannot be read
// (error filled in).
std::shared_ptr<const PrefabAsset> LoadPrefab(const std::string& file, std::string* error)
{
    const std::string           key  = NormalizedFile(FromUtf8(file));
    const std::filesystem::path path = FromUtf8(key);
    const std::optional<Vfs::Location> location = Vfs::Locate(path); // pak entry or disk file
    if (!location) {
        if (error)
            *error = "cannot read '" + key + "': not found";
        return nullptr;
    }
    const auto           mtime = Vfs::ModifiedTime(path);
    const std::uintmax_t size  = location->size;
    PrefabCache&         cache = Cache();
    {
        std::scoped_lock lock(cache.mutex);
        const auto       it = cache.entries.find(key);
        if (it != cache.entries.end() && it->second->mtime == mtime && it->second->size == size &&
            DependenciesCurrent(*it->second))
            return it->second;
    }
    std::shared_ptr<const PrefabAsset> parsed; // without the lock: nested prefabs load recursively
    try {
        parsed = ParsePrefab(key, mtime, size);
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return nullptr;
    }
    std::scoped_lock lock(cache.mutex);
    cache.entries[key] = parsed;
    return parsed;
}

// Forgets the cached content of `file` and of the prefabs nesting it (rewritten on disk).
void InvalidatePrefab(const std::string& file)
{
    PrefabCache&     cache = Cache();
    std::scoped_lock lock(cache.mutex);
    std::erase_if(cache.entries, [&](const auto& entry) { return entry.first == file || entry.second->DependsOn(file); });
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

    // Links to this instance from entities that are not its members (moved out of the instance,
    // or a second entity for the same prefab entity) are stale: those are ordinary entities now.
    // Left in place they would be skipped as members when the scene is saved.
    {
        std::unordered_set<std::uint64_t> members;
        for (const auto& [source, e] : existing)
            members.insert(static_cast<std::uint64_t>(e));
        std::vector<Entity> stale;
        r.ViewOf<PrefabLink>().Each([&](Entity e, const PrefabLink& link) {
            if (link.instance == rootUuid && !members.contains(static_cast<std::uint64_t>(e)))
                stale.push_back(e);
        });
        for (const Entity e : stale)
            r.Remove<PrefabLink>(e);
    }

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

// A prefab instance inside the subtree a prefab is written from: stored as a nested instance.
struct NestedRecord {
    std::string                               file;
    std::shared_ptr<const PrefabAsset>        asset;
    Entity                                    root = NullEntity;
    std::unordered_map<std::uint64_t, Entity> members; // nested prefab UUID -> scene entity (root included)
};

// Writes the subtree of `root` as the prefab `file` and links the subtree to it. Prefab instances
// in the subtree stay nested: scene-level instances below the root, the nested instances of the
// root's own prefab when it is written back (apply), and the root's own instance when it is
// written to another file (a variant of its prefab). Returns the new content.
std::shared_ptr<const PrefabAsset> WriteAndLink(const std::filesystem::path& file, Scene& scene,
                                                const AssetManager* assets, Entity root)
{
    Registry&           r        = scene.GetRegistry();
    const std::uint64_t rootUuid = UuidOf(r, root);
    std::vector<Entity> subtree;
    CollectSubtree(r, root, subtree);
    const std::string target = NormalizedFile(file);

    // The root's own instance: written back into its prefab, or the base of a variant.
    std::shared_ptr<const PrefabAsset> own;
    bool                               applying = false;
    if (const auto* instance = r.TryGet<PrefabInstance>(root)) {
        own      = AssetOf(r, root);
        applying = own && NormalizedFile(FromUtf8(instance->prefab)) == target;
    }

    // Prefab UUIDs: members keep their source, the root its prefab root, new entities their UUID.
    std::uint64_t rootSource = own ? own->root : rootUuid;
    SourceMap     sources; // instance UUID -> prefab UUID
    sources[rootUuid] = rootSource;
    for (std::size_t i = 1; i < subtree.size(); ++i) {
        const auto* link   = r.TryGet<PrefabLink>(subtree[i]);
        const auto  uuid   = UuidOf(r, subtree[i]);
        sources[uuid]      = link && link->instance == rootUuid && link->source != 0 ? link->source : uuid;
    }
    {   // Members of different instances could share a source, and an entity's own UUID (used for
        // non-members) can be another member's source: keep the prefab UUIDs unique.
        std::unordered_set<std::uint64_t> used;
        for (Entity e : subtree) {
            auto& s = sources[UuidOf(r, e)];
            if (used.insert(s).second)
                continue;
            s = UuidOf(r, e);
            if (!used.insert(s).second) {
                std::unordered_set<std::uint64_t> taken = used;
                for (const auto& [uuid, source] : sources)
                    taken.insert(source);
                s = NewUuid(scene, taken);
                used.insert(s);
            }
        }
        rootSource = sources.at(rootUuid);
    }

    // Nested prefab instances.
    std::vector<NestedRecord>                                                records;
    std::unordered_map<std::uint64_t, std::size_t>                           byInstance; // scene instance root UUID
    std::unordered_map<int, std::size_t>                                     byNest;     // nest of the root's prefab
    std::unordered_map<std::uint64_t, std::pair<std::size_t, std::uint64_t>> membership; // entity -> record, source
    const auto join = [&](std::size_t rec, Entity e, std::uint64_t source) {
        NestedRecord& record = records[rec];
        if (!record.asset->index.contains(source) || record.members.contains(source))
            return; // not (or no longer) part of that prefab: an ordinary entity
        record.members[source] = e;
        if (source == record.asset->root)
            record.root = e;
        membership[static_cast<std::uint64_t>(e)] = {rec, source};
    };
    for (Entity e : subtree) {
        const auto* link      = r.TryGet<PrefabLink>(e);
        const bool  ownMember = own && (e == root || (link && link->instance == rootUuid));
        if (ownMember) {
            const std::uint64_t source = e == root ? own->root : link->source;
            if (!applying) { // variant: the whole instance is the base
                auto [it, added] = byInstance.try_emplace(rootUuid, records.size());
                if (added)
                    records.push_back({own->file, own, NullEntity, {}});
                join(it->second, e, source);
                continue;
            }
            const auto idx = own->index.find(source);
            if (idx == own->index.end() || own->origins[idx->second].nest < 0)
                continue;
            const PrefabOrigin& origin = own->origins[idx->second];
            const PrefabNested& nested = own->nested[static_cast<std::size_t>(origin.nest)];
            auto [it, added]           = byNest.try_emplace(origin.nest, records.size());
            if (added)
                records.push_back({nested.file, nested.asset, NullEntity, {}});
            join(it->second, e, origin.source);
        } else if (e != root && r.Has<PrefabInstance>(e)) {
            if (const auto asset = AssetOf(r, e)) { // unreadable prefab: flattened
                byInstance[UuidOf(r, e)] = records.size();
                records.push_back({asset->file, asset, NullEntity, {}});
                join(records.size() - 1, e, asset->root);
            }
        } else if (link) {
            if (const auto it = byInstance.find(link->instance); it != byInstance.end() && link->instance != rootUuid)
                join(it->second, e, link->source);
        }
    }
    // Members outside their nested instance root (or without it) are ordinary entities.
    for (NestedRecord& record : records)
        std::erase_if(record.members, [&](const auto& m) {
            const bool inside = record.root != NullEntity && (m.second == record.root || scene.IsAncestor(record.root, m.second));
            if (!inside)
                membership.erase(static_cast<std::uint64_t>(m.second));
            return !inside;
        });
    // A prefab cannot contain itself: such instances are flattened (ordinary entities of it).
    for (NestedRecord& record : records)
        if (record.root != NullEntity && (record.file == target || record.asset->DependsOn(target))) {
            ENGINE_WARN("Prefab: '{}' cannot contain an instance of itself: '{}' flattened", target,
                        r.Get<Name>(record.root).value);
            for (const auto& [source, member] : record.members)
                membership.erase(static_cast<std::uint64_t>(member));
            record.members.clear();
            record.root = NullEntity;
        }

    const std::filesystem::path dir      = FromUtf8(target).parent_path();
    const auto                  toPrefab = [&](std::uint64_t u) {
        const auto it = sources.find(u);
        return it != sources.end() ? it->second : u;
    };
    const auto relative = [&](const std::string& p) { return RelativeTo(p, dir); };
    const json identity = {{"position", {0.0f, 0.0f, 0.0f}}, {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}}, {"scale", {1.0f, 1.0f, 1.0f}}};
    json       entities = json::array();
    for (Entity e : subtree) {
        ModelRefs refs;
        refs.mode        = PathMode::Absolute;
        refs.constAssets = assets;
        const auto membershipIt = membership.find(static_cast<std::uint64_t>(e));
        const NestedRecord* record = membershipIt != membership.end() ? &records[membershipIt->second.first] : nullptr;
        if (record && record->root != e)
            continue; // a nested member: regenerated from its prefab + the overrides below

        json j    = EntityToJson(r, e, refs);
        json out  = json::object();
        out["uuid"]   = sources.at(UuidOf(r, e));
        out["parent"] = e == root ? 0 : sources.at(UuidOf(r, r.Get<Hierarchy>(e).parent));
        if (!record) {
            j.erase("uuid");
            SceneJson::RemapEntityRefs(j, toPrefab);
            if (e == root)
                j["transform"] = identity;
            SceneJson::TransformPaths(j, relative);
            out.update(j);
            entities.push_back(std::move(out));
            continue;
        }

        // Nested instance root: its own name / transform, members and overrides (as in scene files).
        out["name"]      = j.value("name", std::string("Prefab"));
        out["transform"] = e == root ? identity : j.value("transform", identity);
        SourceMap toOuter; // nested prefab UUID -> UUID in the new prefab
        for (const auto& [source, member] : record->members)
            toOuter[source] = sources.at(UuidOf(r, member));
        json members = json::array();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted(toOuter.begin(), toOuter.end());
        std::ranges::sort(sorted);
        for (const auto& [source, outer] : sorted)
            if (source != record->asset->root)
                members.push_back({source, outer});
        json overrides = json::object();
        for (const json& pj : record->asset->entities) {
            const std::uint64_t source = pj.at("uuid").get<std::uint64_t>();
            const bool          isRoot = source == record->asset->root;
            const auto          member = record->members.find(source);
            if (member == record->members.end()) {
                overrides[std::to_string(source)] = nullptr;
                continue;
            }
            json actual = Actual(r, member->second, assets, isRoot);
            SceneJson::RemapEntityRefs(actual, toPrefab);
            json patch = CreateMergePatch(Expected(pj, toOuter, isRoot), actual);
            if (patch.empty())
                continue;
            SceneJson::TransformPaths(patch, relative);
            overrides[std::to_string(source)] = std::move(patch);
        }
        out["prefab"] = {{"file", RelativeTo(record->file, dir)}, {"members", std::move(members)}, {"overrides", std::move(overrides)}};
        entities.push_back(std::move(out));
    }
    const json document{{"version", kPrefabVersion}, {"prefab", true}, {"root", rootSource}, {"entities", std::move(entities)}};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path temp = FromUtf8(target);
    temp += ".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Prefab: cannot write '" + ToUtf8(temp) + "'");
        stream << document.dump(2) << '\n';
        if (!stream)
            throw std::runtime_error("Prefab: write failed: '" + ToUtf8(temp) + "'");
    }
    std::filesystem::rename(temp, FromUtf8(target), ec);
    if (ec)
        throw std::runtime_error("Prefab: cannot replace '" + target + "': " + ec.message());
    InvalidatePrefab(target);

    auto asset = LoadPrefabOrThrow(target);
    for (std::size_t i = 1; i < subtree.size(); ++i) {
        const Entity        e      = subtree[i];
        const std::uint64_t source = sources.at(UuidOf(r, e));
        r.Remove<PrefabInstance>(e); // nested instances are part of this one now
        if (asset->index.contains(source))
            r.EmplaceOrReplace<PrefabLink>(e, PrefabLink{.instance = rootUuid, .source = source});
        else
            r.Remove<PrefabLink>(e);
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

namespace {

// The instances whose prefab is or nests `file`, with their overrides against the content they
// were built from (captured before the file changes).
std::vector<std::pair<Entity, InstanceData>> CaptureDependents(Scene& scene, const AssetManager* assets,
                                                               const std::string& file, Entity except)
{
    const Registry&                              r = scene.GetRegistry();
    std::vector<std::pair<Entity, InstanceData>> out;
    for (Entity e : InstanceRoots(scene)) {
        if (except != NullEntity && (e == except || scene.IsAncestor(except, e)))
            continue;
        if (const auto asset = AssetOf(r, e); asset && (asset->file == file || asset->DependsOn(file)))
            out.emplace_back(e, Capture(scene, assets, e, *asset));
    }
    return out;
}

void RebuildDependents(Scene& scene, AssetManager* assets, const std::vector<std::pair<Entity, InstanceData>>& captured,
                       std::vector<ModelHandle>& models)
{
    Registry& r = scene.GetRegistry();
    Acquired  acquired;
    for (const auto& [e, data] : captured) {
        if (!r.Valid(e) || !r.Has<PrefabInstance>(e))
            continue;
        if (const auto latest = LoadPrefab(r.Get<PrefabInstance>(e).prefab))
            Rebuild(scene, assets, e, latest, data, acquired);
    }
    AppendHandles(acquired, models);
}

struct LevelInfo {
    PrefabLevel                        level;
    std::shared_ptr<const PrefabAsset> asset;
    SourceMap                          toTop; // UUID in this level's prefab -> UUID in the outermost one
};

// The prefabs `entity` comes from, outermost (the instance in the scene) first.
std::vector<LevelInfo> LevelChain(const Scene& scene, Entity entity)
{
    std::vector<LevelInfo> chain;
    const Entity           root = PrefabInstanceRoot(scene, entity);
    if (root == NullEntity)
        return chain;
    const Registry& r   = scene.GetRegistry();
    const auto      top = AssetOf(r, root);
    if (!top)
        return chain;
    const auto          members = Members(scene, root);
    const auto          inScene = [&](std::uint64_t source) {
        if (source == top->root)
            return root;
        const auto it = members.find(source);
        return it != members.end() ? it->second : NullEntity;
    };
    std::uint64_t source = entity == root ? top->root : r.Get<PrefabLink>(entity).source;
    LevelInfo     first{{top->file, root, source}, top, {}};
    for (const json& pj : top->entities) {
        const std::uint64_t u = pj.at("uuid").get<std::uint64_t>();
        first.toTop[u]        = u;
    }
    chain.push_back(std::move(first));
    while (chain.size() <= kMaxPrefabNesting) {
        const LevelInfo& current = chain.back();
        const auto       idx     = current.asset->index.find(source);
        if (idx == current.asset->index.end())
            break;
        const PrefabOrigin& origin = current.asset->origins[idx->second];
        if (origin.nest < 0)
            break;
        const PrefabNested& nested = current.asset->nested[static_cast<std::size_t>(origin.nest)];
        LevelInfo           next;
        for (const auto& [inner, outer] : nested.map)
            if (const auto it = current.toTop.find(outer); it != current.toTop.end())
                next.toTop[inner] = it->second;
        const auto topRoot = next.toTop.find(nested.asset->root);
        next.level = {nested.file, topRoot != next.toTop.end() ? inScene(topRoot->second) : NullEntity, origin.source};
        next.asset = nested.asset;
        source     = origin.source;
        chain.push_back(std::move(next));
    }
    return chain;
}

} // namespace

void ApplyPrefabInstance(Scene& scene, AssetManager* assets, Entity root, std::vector<ModelHandle>& models)
{
    Registry& r = scene.GetRegistry();
    if (!r.Has<PrefabInstance>(root))
        throw std::invalid_argument("ApplyPrefabInstance: not an instance root");
    const std::string file = NormalizedFile(FromUtf8(r.Get<PrefabInstance>(root).prefab));

    // The other instances' overrides against the old content first (also those of prefabs
    // nesting this one).
    const auto others = CaptureDependents(scene, assets, file, root);
    WriteAndLink(FromUtf8(file), scene, assets, root);
    RebuildDependents(scene, assets, others, models);
}

std::vector<PrefabLevel> PrefabLevels(const Scene& scene, Entity entity)
{
    std::vector<PrefabLevel> levels;
    for (LevelInfo& info : LevelChain(scene, entity))
        levels.push_back(std::move(info.level));
    return levels;
}

void ApplyPrefabLevel(Scene& scene, AssetManager* assets, Entity entity, std::size_t level,
                      std::vector<ModelHandle>& models)
{
    const std::vector<LevelInfo> chain = LevelChain(scene, entity);
    if (level >= chain.size())
        throw std::invalid_argument("ApplyPrefabLevel: no such prefab level");
    if (level == 0) {
        ApplyPrefabInstance(scene, assets, chain[0].level.root, models);
        return;
    }
    const LevelInfo& target = chain[level];
    if (target.level.root == NullEntity)
        throw std::invalid_argument("ApplyPrefabLevel: the nested instance was removed");

    // The scene entities of that nested instance, by their UUID in its prefab.
    Registry&                                 r       = scene.GetRegistry();
    const Entity                              top     = chain[0].level.root;
    const auto                                members = Members(scene, top);
    std::unordered_map<std::uint64_t, Entity> inLevel;
    SourceMap                                 toLevel; // scene UUID -> UUID in that prefab
    for (const auto& [u, topUuid] : target.toTop) {
        Entity e = NullEntity;
        if (topUuid == chain[0].asset->root)
            e = top;
        else if (const auto it = members.find(topUuid); it != members.end())
            e = it->second;
        if (e != NullEntity) {
            inLevel[u]              = e;
            toLevel[UuidOf(r, e)]   = u;
        }
    }

    // A fresh instance of that prefab with its own UUIDs, set to the scene's values, is written
    // back (its nested instances stay nested). Entities removed or added in the scene are not
    // applied to inner levels.
    const PrefabAsset& asset = *target.asset;
    Acquired           scratchModels;
    {
        Scene        scratch;
        const Entity scratchRoot = scratch.CreateEntity(asset.entities.front().value("name", std::string("Prefab")),
                                                        NullEntity, asset.root);
        scratch.GetRegistry().Emplace<PrefabInstance>(scratchRoot, PrefabInstance(asset.file));
        InstanceData same;
        for (const json& pj : asset.entities) {
            const std::uint64_t u = pj.at("uuid").get<std::uint64_t>();
            same.members[u]       = u;
        }
        Rebuild(scratch, assets, scratchRoot, target.asset, same, scratchModels);
        const auto scratchMembers = Members(scratch, scratchRoot);
        ModelRefs  refs;
        refs.mode        = PathMode::Absolute;
        refs.assets      = assets;
        refs.constAssets = assets;
        refs.acquired    = &scratchModels;
        for (const auto& [u, e] : inLevel) {
            const bool isRoot = u == asset.root;
            Entity     copy   = scratchRoot;
            if (!isRoot) {
                const auto it = scratchMembers.find(u);
                if (it == scratchMembers.end())
                    continue;
                copy = it->second;
            }
            json state = Actual(r, e, assets, isRoot);
            SceneJson::RemapEntityRefs(state, [&](std::uint64_t uuid) {
                const auto it = toLevel.find(uuid);
                return it != toLevel.end() ? it->second : uuid;
            });
            ApplyComponents(scratch, copy, state, refs);
        }
        const auto dependents = CaptureDependents(scene, assets, asset.file, NullEntity);
        WriteAndLink(FromUtf8(asset.file), scratch, assets, scratchRoot);
        RebuildDependents(scene, assets, dependents, models);
    }
    if (assets)
        for (const auto& [key, handle] : scratchModels)
            assets->Release(handle);
}

void CreatePrefabVariant(const std::filesystem::path& file, const std::filesystem::path& base)
{
    const std::string target = NormalizedFile(file);
    const auto        parent = LoadPrefabOrThrow(ToUtf8(base));
    if (parent->file == target || parent->DependsOn(target))
        throw std::runtime_error("Prefab: '" + target + "' cannot be a variant of itself");
    const std::filesystem::path dir  = FromUtf8(target).parent_path();
    const std::uint64_t         root = DeriveUuid(parent->root, std::hash<std::string>{}(target));
    json                        entity{{"uuid", root},
                                       {"parent", 0},
                                       {"name", parent->entities.front().value("name", std::string("Prefab"))},
                                       {"prefab", {{"file", RelativeTo(parent->file, dir)}, {"members", json::array()}, {"overrides", json::object()}}}};
    const json document{{"version", kPrefabVersion}, {"prefab", true}, {"root", root}, {"entities", json::array({entity})}};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream stream(FromUtf8(target), std::ios::binary | std::ios::trunc);
    if (!stream || !(stream << document.dump(2) << '\n'))
        throw std::runtime_error("Prefab: cannot write '" + target + "'");
    stream.close();
    InvalidatePrefab(target);
}

std::string PrefabBase(const std::filesystem::path& file)
{
    const auto asset = LoadPrefab(ToUtf8(file));
    if (!asset || asset->nested.empty() || asset->origins.front().nest != 0)
        return {};
    return asset->nested.front().file;
}

std::vector<std::string> PrefabNestedFiles(const std::filesystem::path& file)
{
    std::vector<std::string> files;
    if (const auto asset = LoadPrefab(ToUtf8(file)))
        for (const PrefabDependency& d : asset->dependencies)
            if (std::ranges::find(files, d.file) == files.end())
                files.push_back(d.file);
    return files;
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
