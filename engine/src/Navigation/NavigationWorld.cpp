#include "Engine/Navigation/NavigationWorld.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Model.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Terrain/TerrainData.h"

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>
#include <Recast.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Engine {

// --- NavTypes ---

std::optional<std::size_t> NavSettings::Profile(std::string_view name) const
{
    if (name.empty())
        return profiles.empty() ? std::nullopt : std::optional<std::size_t>(0);
    for (std::size_t i = 0; i < profiles.size(); ++i)
        if (profiles[i].name == name)
            return i;
    return std::nullopt;
}

std::optional<std::size_t> NavSettings::Area(std::string_view name) const
{
    for (std::size_t i = 0; i < areas.size(); ++i)
        if (areas[i].name == name)
            return i;
    return std::nullopt;
}

namespace {

// FNV-1a over plain values (signatures of navmesh sources, build hash).
class Hasher {
public:
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    Hasher& Add(const T& value)
    {
        m_Hash = Fnv1a64(std::as_bytes(std::span(&value, 1)), m_Hash);
        return *this;
    }
    Hasher& Add(std::string_view text)
    {
        Add(text.size());
        m_Hash = Fnv1a64(std::as_bytes(std::span(text.data(), text.size())), m_Hash);
        return *this;
    }
    [[nodiscard]] std::uint64_t Value() const { return m_Hash; }

private:
    std::uint64_t m_Hash = 0xcbf29ce484222325ull;
};

} // namespace

std::uint64_t NavSettings::BuildHash() const
{
    Hasher h;
    h.Add(std::uint32_t{1}).Add(cellSize).Add(cellHeight).Add(tileSize).Add(edgeMaxLength).Add(edgeMaxError);
    h.Add(regionMinSize).Add(regionMergeSize).Add(maxTiles);
    for (const NavAgentProfile& p : profiles)
        h.Add(std::string_view(p.name)).Add(p.radius).Add(p.height).Add(p.maxClimb).Add(p.maxSlope);
    for (const NavAreaType& a : areas)
        h.Add(std::string_view(a.name)); // ids / flags follow the order
    return h.Value();
}

void SanitizeNavSettings(NavSettings& s)
{
    s.cellSize        = std::clamp(s.cellSize, 0.02f, 4.0f);
    s.cellHeight      = std::clamp(s.cellHeight, 0.01f, 4.0f);
    s.tileSize        = std::clamp(s.tileSize, 16u, 255u);
    s.edgeMaxLength   = std::clamp(s.edgeMaxLength, 0.0f, 1000.0f);
    s.edgeMaxError    = std::clamp(s.edgeMaxError, 0.1f, 10.0f);
    s.regionMinSize   = std::clamp(s.regionMinSize, 0.0f, 150.0f);
    s.regionMergeSize = std::clamp(s.regionMergeSize, 0.0f, 150.0f);
    s.maxTiles        = std::clamp(s.maxTiles, 16u, 16384u);
    s.maxObstacles    = std::clamp(s.maxObstacles, 1u, 4096u);
    s.maxAgents       = std::clamp(s.maxAgents, 1u, 4096u);

    const auto uniqueNames = [](auto& list, const char* fallback) {
        std::set<std::string> seen;
        for (auto& item : list) {
            if (item.name.empty())
                item.name = fallback;
            std::string base = item.name, name = base;
            for (int n = 2; seen.contains(name); ++n)
                name = base + " " + std::to_string(n);
            item.name = name;
            seen.insert(name);
        }
    };
    if (s.profiles.empty())
        s.profiles.emplace_back();
    if (s.profiles.size() > kNavMaxProfiles)
        s.profiles.resize(kNavMaxProfiles);
    for (NavAgentProfile& p : s.profiles) {
        p.radius   = std::clamp(p.radius, 0.05f, 20.0f);
        p.height   = std::clamp(p.height, 0.1f, 50.0f);
        p.maxClimb = std::clamp(p.maxClimb, 0.0f, 20.0f);
        p.maxSlope = std::clamp(p.maxSlope, 0.0f, 89.0f);
    }
    uniqueNames(s.profiles, "Profile");

    if (s.areas.empty() || s.areas[0].name != "Default")
        s.areas.insert(s.areas.begin(), NavAreaType{});
    std::erase_if(s.areas, [](const NavAreaType& a) { return a.name == kNavAreaBlocked; });
    if (s.areas.size() > kNavMaxAreas)
        s.areas.resize(kNavMaxAreas);
    for (NavAreaType& a : s.areas) {
        a.cost  = std::clamp(a.cost, 1.0f, 10000.0f);
        a.color = glm::clamp(a.color, glm::vec3(0.0f), glm::vec3(1.0f));
    }
    uniqueNames(s.areas, "Area");
    for (std::size_t i = 1; i < s.areas.size(); ++i)
        if (s.areas[i].name == "Default")
            s.areas[i].name = "Area " + std::to_string(i);
}

float NavPath::Length() const
{
    float length = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i)
        length += glm::distance(points[i - 1], points[i]);
    return length;
}

fs::path NavMeshFileFor(const fs::path& scene)
{
    std::string name = PathToUtf8(scene.filename());
    for (const std::string_view suffix : {".json", ".scene", ".uscene"})
        if (name.size() > suffix.size() && name.ends_with(suffix))
            name.resize(name.size() - suffix.size());
    return scene.parent_path() / PathFromUtf8(name + ".unav");
}

namespace {

using TileKey = std::uint64_t;

constexpr int          kMaxLayers       = 32;
constexpr int          kMaxPathPolys    = 512;
constexpr int          kMaxStraightPath = 256;
constexpr std::uint32_t kBakedMagic     = 0x56414E55u; // "UNAV"
constexpr std::uint32_t kBakedVersion   = 1;

TileKey MakeKey(int x, int y)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
}
int KeyX(TileKey k) { return static_cast<int>(static_cast<std::uint32_t>(k >> 32)); }
int KeyY(TileKey k) { return static_cast<int>(static_cast<std::uint32_t>(k & 0xFFFFFFFFu)); }

struct Box {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    void Add(const glm::vec3& p) { min = glm::min(min, p), max = glm::max(max, p); }
    void Add(const Box& b) { min = glm::min(min, b.min), max = glm::max(max, b.max); }
    [[nodiscard]] bool Valid() const { return min.x <= max.x; }
    [[nodiscard]] bool OverlapsXZ(const glm::vec3& lo, const glm::vec3& hi) const
    {
        return Valid() && min.x <= hi.x && max.x >= lo.x && min.z <= hi.z && max.z >= lo.z;
    }
    [[nodiscard]] bool Overlaps(const Box& o) const
    {
        return Valid() && o.Valid() && min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }
};

std::array<glm::vec3, 8> BoxCorners(const glm::mat4& world, const glm::vec3& center, const glm::vec3& half)
{
    std::array<glm::vec3, 8> corners{};
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local = center + half * glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                                                          (i & 4) ? 1.0f : -1.0f);
        corners[static_cast<std::size_t>(i)] = glm::vec3(world * glm::vec4(local, 1.0f));
    }
    return corners;
}

// Convex hull of points in XZ (Andrew's monotone chain), counter-clockwise.
std::vector<glm::vec2> HullXZ(std::span<const glm::vec3> points)
{
    std::vector<glm::vec2> p;
    for (const glm::vec3& v : points)
        p.emplace_back(v.x, v.z);
    std::ranges::sort(p, [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() < 3)
        return p;
    const auto cross = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<glm::vec2> hull(2 * p.size());
    std::size_t            k = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], p[i]) <= 0.0f)
            --k;
        hull[k++] = p[i];
    }
    for (std::size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], p[i - 1]) <= 0.0f)
            --k;
        hull[k++] = p[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

// --- Navmesh inputs (world space) ---

// Walkable geometry of one collider.
struct SourceRecord {
    Entity                 entity    = NullEntity;
    std::uint64_t          signature = 0;
    Box                    bounds;
    std::vector<glm::vec3> triangles; // 3 per triangle
    bool                   hasTriangles = false; // false: loaded from a baked file, filled on the next collect
    bool                   missed       = false; // a tile was built without its triangles (mesh still loading)
    bool                   seen         = false;
    // Terrain sources: triangles are made per tile from the height field (no copy of all of them).
    std::shared_ptr<const TerrainData> terrain;
    glm::vec3                          terrainOrigin{0.0f};
    std::uint64_t                      terrainRevision = 0;
    double                             terrainChecked  = 0.0;
};

// NavArea: convex XZ footprint + height range.
struct AreaRecord {
    std::uint64_t          signature = 0;
    Box                    bounds;
    std::vector<glm::vec2> footprint;
    unsigned char          areaId = 1; // Recast area: 0 = blocked, area index + 1
    std::uint64_t          uuid   = 0;
    bool                   seen   = false;
};

struct LinkRecord {
    std::uint64_t signature = 0;
    Box           bounds;
    glm::vec3     start{0.0f}, end{0.0f};
    float         radius        = 0.5f;
    bool          bidirectional = true;
    unsigned char areaId        = 1;
    std::uint32_t profiles      = 0xFFFFFFFFu;
    std::uint64_t uuid          = 0;
    bool          seen          = false;
};

struct BoundsRecord {
    std::uint64_t signature = 0;
    Box           bounds;
    bool          seen = false;
};

struct ObstacleRecord {
    std::uint64_t              signature = 0;
    Box                        bounds;
    NavObstacleShape           shape = NavObstacleShape::Box;
    glm::vec3                  center{0.0f}, halfExtents{0.0f};
    float                      yaw = 0.0f, radius = 0.0f, height = 0.0f;
    std::vector<dtObstacleRef> refs;       // per profile (0: not added)
    bool                       added   = false;
    bool                       refresh = false; // re-add (its tiles were rebuilt from geometry)
    bool                       seen    = false;
};

// --- Recast / tile cache glue ---

class NavContext final : public rcContext {
public:
    NavContext() : rcContext(true) { enableTimer(false); }

protected:
    void doLog(const rcLogCategory category, const char* msg, const int len) override
    {
        if (category == RC_LOG_ERROR)
            ENGINE_WARN("Navigation: {}", std::string_view(msg, static_cast<std::size_t>(std::max(len, 0))));
    }
};

// Tiles are stored uncompressed (the cache keeps them per layer; baked files hold them as is).
struct PlainCompressor final : dtTileCacheCompressor {
    int maxCompressedSize(const int bufferSize) override { return bufferSize; }
    dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed,
                      const int maxCompressedSize, int* compressedSize) override
    {
        if (bufferSize > maxCompressedSize)
            return DT_FAILURE | DT_BUFFER_TOO_SMALL;
        std::memcpy(compressed, buffer, static_cast<std::size_t>(bufferSize));
        *compressedSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer,
                        const int maxBufferSize, int* bufferSize) override
    {
        if (compressedSize > maxBufferSize)
            return DT_FAILURE | DT_BUFFER_TOO_SMALL;
        std::memcpy(buffer, compressed, static_cast<std::size_t>(compressedSize));
        *bufferSize = compressedSize;
        return DT_SUCCESS;
    }
};

struct LinearAllocator final : dtTileCacheAlloc {
    explicit LinearAllocator(std::size_t capacity) : buffer(capacity) {}
    void reset() override { top = 0; }
    void* alloc(const size_t size) override
    {
        const std::size_t aligned = (size + 15) & ~std::size_t{15};
        if (top + aligned > buffer.size())
            return nullptr;
        void* memory = buffer.data() + top;
        top += aligned;
        return memory;
    }
    void free(void*) override {}

    std::vector<unsigned char> buffer;
    std::size_t                top = 0;
};

// Area id -> flags (bit per area) and the profile's off-mesh links for every built tile.
struct MeshProcess final : dtTileCacheMeshProcess {
    void process(dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override
    {
        for (int i = 0; i < params->polyCount; ++i) {
            if (polyAreas[i] == DT_TILECACHE_WALKABLE_AREA)
                polyAreas[i] = 1;
            polyFlags[i] = static_cast<unsigned short>(polyAreas[i] > 0 && polyAreas[i] <= 16 ? 1u << (polyAreas[i] - 1) : 0u);
        }
        params->buildBvTree = true;
        if (!linkAreas.empty()) {
            params->offMeshConVerts  = linkVerts.data();
            params->offMeshConRad    = linkRadii.data();
            params->offMeshConDir    = linkDirs.data();
            params->offMeshConAreas  = linkAreas.data();
            params->offMeshConFlags  = linkFlags.data();
            params->offMeshConUserID = linkIds.data();
            params->offMeshConCount  = static_cast<int>(linkAreas.size());
        }
    }

    std::vector<float>          linkVerts, linkRadii;
    std::vector<unsigned char>  linkDirs, linkAreas;
    std::vector<unsigned short> linkFlags;
    std::vector<unsigned int>   linkIds;
};

struct Layer {
    unsigned char* data = nullptr; // dtAlloc'd; the tile cache frees it once added
    int            size = 0;
};

struct TileInput {
    std::uint32_t profile = 0;
    TileKey       key     = 0;
    std::uint64_t generation = 0;
    rcConfig      config{};
    float         slope = 45.0f;
    std::vector<float> vertices;
    std::vector<int>   indices;
    std::vector<AreaRecord> areas;
    std::vector<Box>   bounds; // NavMeshBounds overlapping the tile
};

struct TileOutput {
    std::uint32_t      profile = 0;
    TileKey            key     = 0;
    std::uint64_t      generation = 0;
    std::vector<Layer> layers;
};

void FreeLayers(std::vector<Layer>& layers)
{
    for (Layer& l : layers)
        dtFree(l.data);
    layers.clear();
}

// Rasterizes one tile into tile-cache layers (any thread).
TileOutput BuildTileLayers(const TileInput& in)
{
    TileOutput out{.profile = in.profile, .key = in.key, .generation = in.generation, .layers = {}};
    if (in.indices.empty() || in.bounds.empty())
        return out;
    NavContext     ctx;
    const rcConfig& cfg = in.config;

    struct Scratch {
        rcHeightfield*         solid = nullptr;
        rcCompactHeightfield*  chf   = nullptr;
        rcHeightfieldLayerSet* lset  = nullptr;
        ~Scratch()
        {
            rcFreeHeightField(solid);
            rcFreeCompactHeightfield(chf);
            rcFreeHeightfieldLayerSet(lset);
        }
    } s;
    s.solid = rcAllocHeightfield();
    if (!s.solid || !rcCreateHeightfield(&ctx, *s.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        return out;
    const int triangleCount = static_cast<int>(in.indices.size() / 3);
    std::vector<unsigned char> triAreas(static_cast<std::size_t>(triangleCount), 0);
    rcMarkWalkableTriangles(&ctx, in.slope, in.vertices.data(), static_cast<int>(in.vertices.size() / 3),
                            in.indices.data(), triangleCount, triAreas.data());
    if (!rcRasterizeTriangles(&ctx, in.vertices.data(), static_cast<int>(in.vertices.size() / 3), in.indices.data(),
                              triAreas.data(), triangleCount, *s.solid, cfg.walkableClimb))
        return out;
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *s.solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *s.solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *s.solid);

    s.chf = rcAllocCompactHeightfield();
    if (!s.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *s.solid, *s.chf))
        return out;
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *s.chf))
        return out;

    // Walkable -> area "Default"; spans outside every NavMeshBounds box -> not walkable.
    rcCompactHeightfield& chf = *s.chf;
    for (int z = 0; z < chf.height; ++z)
        for (int x = 0; x < chf.width; ++x) {
            const rcCompactCell& cell = chf.cells[x + z * chf.width];
            const float wx = chf.bmin[0] + (static_cast<float>(x) + 0.5f) * chf.cs;
            const float wz = chf.bmin[2] + (static_cast<float>(z) + 0.5f) * chf.cs;
            for (unsigned int i = cell.index, end = cell.index + cell.count; i < end; ++i) {
                if (chf.areas[i] == RC_NULL_AREA)
                    continue;
                const float wy     = chf.bmin[1] + static_cast<float>(chf.spans[i].y) * chf.ch;
                bool        inside = false;
                for (const Box& b : in.bounds)
                    if (wx >= b.min.x && wx <= b.max.x && wz >= b.min.z && wz <= b.max.z && wy >= b.min.y &&
                        wy <= b.max.y) {
                        inside = true;
                        break;
                    }
                chf.areas[i] = static_cast<unsigned char>(
                    inside ? (chf.areas[i] == RC_WALKABLE_AREA ? 1 : chf.areas[i]) : RC_NULL_AREA);
            }
        }
    for (const AreaRecord& area : in.areas) {
        std::vector<float> verts;
        for (const glm::vec2& p : area.footprint)
            verts.insert(verts.end(), {p.x, 0.0f, p.y});
        if (area.footprint.size() >= 3)
            rcMarkConvexPolyArea(&ctx, verts.data(), static_cast<int>(area.footprint.size()), area.bounds.min.y,
                                 area.bounds.max.y, area.areaId, chf);
    }

    s.lset = rcAllocHeightfieldLayerSet();
    if (!s.lset || !rcBuildHeightfieldLayers(&ctx, chf, cfg.borderSize, cfg.walkableHeight, *s.lset))
        return out;
    PlainCompressor compressor;
    for (int i = 0; i < std::min(s.lset->nlayers, kMaxLayers); ++i) {
        const rcHeightfieldLayer& layer = s.lset->layers[i];
        dtTileCacheLayerHeader    header{};
        header.magic   = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx      = KeyX(in.key);
        header.ty      = KeyY(in.key);
        header.tlayer  = i;
        dtVcopy(header.bmin, layer.bmin);
        dtVcopy(header.bmax, layer.bmax);
        header.width  = static_cast<unsigned char>(layer.width);
        header.height = static_cast<unsigned char>(layer.height);
        header.minx   = static_cast<unsigned char>(layer.minx);
        header.maxx   = static_cast<unsigned char>(layer.maxx);
        header.miny   = static_cast<unsigned char>(layer.miny);
        header.maxy   = static_cast<unsigned char>(layer.maxy);
        header.hmin   = static_cast<unsigned short>(layer.hmin);
        header.hmax   = static_cast<unsigned short>(layer.hmax);
        Layer built;
        if (dtStatusFailed(dtBuildTileCacheLayer(&compressor, &header, layer.heights, layer.areas, layer.cons,
                                                 &built.data, &built.size))) {
            FreeLayers(out.layers);
            return out;
        }
        out.layers.push_back(built);
    }
    return out;
}

// Results of tile jobs; shared with the jobs so the world can go away while they run.
struct JobResults {
    std::mutex              mutex;
    std::condition_variable done;
    std::vector<TileOutput> outputs;
    int                     running = 0;
};

// Random numbers for Detour's random point queries (a plain function pointer).
thread_local std::mt19937_64* t_Random = nullptr;
float RandomFloat()
{
    return t_Random ? std::uniform_real_distribution<float>(0.0f, 1.0f)(*t_Random) : 0.5f;
}

glm::vec3 V3(const float* p) { return {p[0], p[1], p[2]}; }

} // namespace

// --- World ---

struct NavigationWorld::Impl {
    struct Profile {
        NavAgentProfile config;
        dtNavMesh*      mesh  = nullptr;
        dtTileCache*    cache = nullptr;
        dtNavMeshQuery* query = nullptr;
        dtCrowd*        crowd = nullptr;
        std::unique_ptr<LinearAllocator> allocator = std::make_unique<LinearAllocator>(1u << 20);
        PlainCompressor                  compressor;
        MeshProcess                      process;
        rcConfig                         rc{};
        std::unordered_set<TileKey>                 dirty;
        std::unordered_set<TileKey>                 linkDirty; // navmesh tiles to rebuild from the cache (links)
        std::unordered_map<TileKey, std::uint64_t>  generation; // latest requested build per tile
        std::unordered_set<TileKey>                 building;
        std::vector<std::uint32_t>                  filterMasks; // crowd filter index -> area mask

        Profile() = default;
        Profile(const Profile&)            = delete;
        Profile& operator=(const Profile&) = delete;
        ~Profile()
        {
            dtFreeCrowd(crowd);
            dtFreeNavMeshQuery(query);
            dtFreeTileCache(cache);
            dtFreeNavMesh(mesh);
        }
    };

    struct Agent {
        std::uint32_t profile = 0;
        int           index   = -1;
        NavAgent      params;
        NavMoveStatus status  = NavMoveStatus::Idle;
        glm::vec3     target{0.0f};
        glm::vec3     written{0.0f}; // last position written to the entity (direct movement)
        float         acceptance = -1.0f; // of the current move (< 0: params.stoppingDistance)
        bool          character = false;
        bool          seen      = false;
    };

    ThreadPool*         jobs;
    const AssetManager* assets;
    PhysicsWorld*       physics;
    NavSettings         settings;
    std::vector<std::unique_ptr<Profile>> profiles;
    std::shared_ptr<JobResults>           results = std::make_shared<JobResults>();

    std::unordered_map<std::uint64_t, SourceRecord>   sources;
    std::unordered_map<std::uint64_t, AreaRecord>     areas;
    std::unordered_map<std::uint64_t, LinkRecord>     links;
    std::unordered_map<std::uint64_t, BoundsRecord>   bounds;
    std::unordered_map<std::uint64_t, ObstacleRecord> obstacles;
    std::vector<Box>                                  dirtyRegions;
    bool                                              everythingDirty = false;
    bool                                              linksChanged    = false;
    std::unordered_map<std::uint64_t, Agent>          agents; // by entity id
    struct MoveRequest {
        glm::vec3 target{0.0f};
        float     acceptance = -1.0f;
    };
    std::unordered_map<std::uint64_t, MoveRequest>    pendingMoves;
    std::unordered_map<std::uint64_t, NavMoveStatus>  orphanMoves; // requests of entities that never became agents
    std::mt19937_64                                   random{static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    NavStats                                          stats;
    std::uint64_t                                     nextGeneration = 1;
    std::set<std::string>                             warnedProfiles;

    Impl(ThreadPool* j, const AssetManager* a, PhysicsWorld* p) : jobs(j), assets(a), physics(p)
    {
        SanitizeNavSettings(settings);
        CreateProfiles();
    }

    ~Impl()
    {
        DropAgents();
        std::unique_lock lock(results->mutex);
        results->done.wait(lock, [&] { return results->running == 0; });
        for (TileOutput& o : results->outputs)
            FreeLayers(o.layers);
        results->outputs.clear();
    }

    // --- Profiles ---

    void CreateProfiles()
    {
        DropAgents();
        profiles.clear();
        const float tileWorld = static_cast<float>(settings.tileSize) * settings.cellSize;
        int         tileBits  = 0;
        while ((1u << tileBits) < settings.maxTiles)
            ++tileBits;
        const int polyBits = std::max(22 - tileBits, 6);
        for (const NavAgentProfile& cfg : settings.profiles) {
            auto p    = std::make_unique<Profile>();
            p->config = cfg;
            rcConfig& c           = p->rc;
            c.cs                  = settings.cellSize;
            c.ch                  = settings.cellHeight;
            c.walkableSlopeAngle  = cfg.maxSlope;
            c.walkableHeight      = static_cast<int>(std::ceil(cfg.height / c.ch));
            c.walkableClimb       = static_cast<int>(std::floor(cfg.maxClimb / c.ch));
            c.walkableRadius      = static_cast<int>(std::ceil(cfg.radius / c.cs));
            c.maxEdgeLen          = static_cast<int>(settings.edgeMaxLength / c.cs);
            c.maxSimplificationError = settings.edgeMaxError;
            c.minRegionArea       = static_cast<int>(settings.regionMinSize * settings.regionMinSize);
            c.mergeRegionArea     = static_cast<int>(settings.regionMergeSize * settings.regionMergeSize);
            c.maxVertsPerPoly     = DT_VERTS_PER_POLYGON;
            c.tileSize            = static_cast<int>(settings.tileSize);
            c.borderSize          = c.walkableRadius + 3;
            c.width = c.height    = c.tileSize + c.borderSize * 2;

            dtTileCacheParams tc{};
            tc.cs = settings.cellSize, tc.ch = settings.cellHeight;
            tc.width = tc.height  = static_cast<int>(settings.tileSize);
            tc.walkableHeight     = cfg.height;
            tc.walkableRadius     = cfg.radius;
            tc.walkableClimb      = cfg.maxClimb;
            tc.maxSimplificationError = settings.edgeMaxError;
            tc.maxTiles           = static_cast<int>(settings.maxTiles);
            tc.maxObstacles       = static_cast<int>(settings.maxObstacles);
            p->cache              = dtAllocTileCache();
            dtNavMeshParams nm{};
            nm.tileWidth = nm.tileHeight = tileWorld;
            nm.maxTiles                  = static_cast<int>(settings.maxTiles);
            nm.maxPolys                  = 1 << polyBits;
            p->mesh                      = dtAllocNavMesh();
            p->query                     = dtAllocNavMeshQuery();
            p->crowd                     = dtAllocCrowd();
            if (!p->cache || !p->mesh || !p->query || !p->crowd ||
                dtStatusFailed(p->cache->init(&tc, p->allocator.get(), &p->compressor, &p->process)) ||
                dtStatusFailed(p->mesh->init(&nm)) || dtStatusFailed(p->query->init(p->mesh, 4096)) ||
                !p->crowd->init(static_cast<int>(settings.maxAgents), cfg.radius, p->mesh)) {
                ENGINE_ERROR("Navigation: cannot create the navmesh of profile '{}'", cfg.name);
                continue;
            }
            // Avoidance qualities (DetourCrowd demo values): Low, Medium, High (index 0 unused = Off).
            dtObstacleAvoidanceParams avoid = *p->crowd->getObstacleAvoidanceParams(0);
            const std::array<std::array<unsigned char, 3>, 4> levels{{{5, 2, 1}, {5, 2, 1}, {7, 2, 2}, {7, 3, 3}}};
            for (int i = 0; i < 4; ++i) {
                avoid.velBias       = 0.5f;
                avoid.adaptiveDivs  = levels[static_cast<std::size_t>(i)][0];
                avoid.adaptiveRings = levels[static_cast<std::size_t>(i)][1];
                avoid.adaptiveDepth = levels[static_cast<std::size_t>(i)][2];
                p->crowd->setObstacleAvoidanceParams(i, &avoid);
            }
            p->filterMasks.push_back(0xFFFFFFFFu);
            ApplyFilter(*p->crowd->getEditableFilter(0), 0xFFFFFFFFu);
            profiles.push_back(std::move(p));
        }
        everythingDirty = true;
        linksChanged    = true;
        for (auto& [uuid, o] : obstacles)
            o.refs.clear(), o.added = false;
        RebuildLinkArrays();
    }

    void ApplyFilter(dtQueryFilter& filter, std::uint32_t areaMask) const
    {
        filter.setIncludeFlags(static_cast<unsigned short>(areaMask & 0xFFFFu));
        filter.setExcludeFlags(0);
        for (std::size_t i = 0; i < settings.areas.size(); ++i)
            filter.setAreaCost(static_cast<int>(i + 1), settings.areas[i].cost);
    }

    [[nodiscard]] dtQueryFilter Filter(std::uint32_t areaMask) const
    {
        dtQueryFilter filter;
        ApplyFilter(filter, areaMask);
        return filter;
    }

    [[nodiscard]] Profile* GetProfile(std::uint32_t index) const
    {
        return index < profiles.size() ? profiles[index].get() : nullptr;
    }

    [[nodiscard]] float TileWorld() const { return static_cast<float>(settings.tileSize) * settings.cellSize; }

    // --- Collecting the scene ---

    [[nodiscard]] unsigned char AreaId(const std::string& name) const
    {
        if (name == kNavAreaBlocked)
            return RC_NULL_AREA;
        const std::optional<std::size_t> index = settings.Area(name);
        return static_cast<unsigned char>(index.value_or(0) + 1);
    }

    static void AddTriangle(std::vector<glm::vec3>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
    {
        out.insert(out.end(), {a, b, c});
    }

    // World triangles of a collider; nullopt while its mesh is not loaded.
    std::optional<std::vector<glm::vec3>> ColliderTriangles(const Scene& scene, Entity e, const Collider& c,
                                                            const glm::mat4& world) const
    {
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), c.center) * glm::mat4_cast(c.rotation);
        const glm::mat4 m     = world * local;
        std::vector<glm::vec3> tris;
        const auto pt = [&](const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); };
        switch (c.shape) {
        case ColliderShape::Box: {
            const glm::vec3 h = c.halfExtents;
            const std::array<glm::vec3, 8> v{pt({-h.x, -h.y, -h.z}), pt({h.x, -h.y, -h.z}), pt({h.x, h.y, -h.z}),
                                             pt({-h.x, h.y, -h.z}),  pt({-h.x, -h.y, h.z}), pt({h.x, -h.y, h.z}),
                                             pt({h.x, h.y, h.z}),    pt({-h.x, h.y, h.z})};
            constexpr std::array<std::array<int, 4>, 6> faces{
                {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}}};
            for (const auto& f : faces) {
                AddTriangle(tris, v[static_cast<std::size_t>(f[0])], v[static_cast<std::size_t>(f[1])],
                            v[static_cast<std::size_t>(f[2])]);
                AddTriangle(tris, v[static_cast<std::size_t>(f[0])], v[static_cast<std::size_t>(f[2])],
                            v[static_cast<std::size_t>(f[3])]);
            }
            break;
        }
        case ColliderShape::Sphere:
        case ColliderShape::Capsule: {
            // UV sphere (capsule: hemispheres apart by 2 halfHeight along Y); scaled like physics (max axis).
            const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                  glm::length(glm::vec3(world[2])));
            const float     s    = std::max({scale.x, scale.y, scale.z});
            const glm::mat4 unscaled =
                glm::scale(world, glm::vec3(s) / glm::max(scale, glm::vec3(1e-6f))) * local;
            const float r     = c.radius;
            const float half  = c.shape == ColliderShape::Capsule ? c.halfHeight : 0.0f;
            constexpr int rings = 8, segments = 12;
            const auto ring = [&](int i, int j) {
                const float phi   = glm::pi<float>() * static_cast<float>(i) / rings;
                const float theta = glm::two_pi<float>() * static_cast<float>(j) / segments;
                glm::vec3   p(r * std::sin(phi) * std::cos(theta), r * std::cos(phi), r * std::sin(phi) * std::sin(theta));
                p.y += (i <= rings / 2 ? half : -half);
                return glm::vec3(unscaled * glm::vec4(p, 1.0f));
            };
            for (int i = 0; i < rings; ++i)
                for (int j = 0; j < segments; ++j) {
                    const glm::vec3 a = ring(i, j), b = ring(i + 1, j), cc = ring(i + 1, j + 1), d = ring(i, j + 1);
                    AddTriangle(tris, a, d, cc);
                    AddTriangle(tris, a, cc, b);
                    if (i == rings / 2 - 1 && half > 0.0f) { // cylinder between the hemispheres
                        const glm::vec3 lowB = b - glm::vec3(unscaled[1]) * (2.0f * half);
                        const glm::vec3 lowC = cc - glm::vec3(unscaled[1]) * (2.0f * half);
                        AddTriangle(tris, b, cc, lowC);
                        AddTriangle(tris, b, lowC, lowB);
                    }
                }
            break;
        }
        case ColliderShape::Mesh: {
            const MeshRenderer* mr = scene.GetRegistry().TryGet<MeshRenderer>(e);
            if (!mr || !assets || !mr->model)
                return tris;
            const AssetState state = assets->State(mr->model);
            if (state == AssetState::Failed || state == AssetState::Invalid)
                return tris;
            const Model* model = assets->Get(mr->model);
            if (!model)
                return std::nullopt;
            if (mr->meshIndex >= model->meshes.size())
                return tris;
            for (const Submesh& sm : model->meshes[mr->meshIndex].submeshes)
                for (std::uint32_t i = 0; i + 2 < sm.indexCount; i += 3) {
                    const auto vertex = [&](std::uint32_t k) {
                        const std::int64_t index =
                            static_cast<std::int64_t>(model->collisionIndices[sm.firstIndex + i + k]) + sm.vertexOffset;
                        return glm::vec3(world * glm::vec4(model->collisionPositions[static_cast<std::size_t>(index)], 1.0f));
                    };
                    AddTriangle(tris, vertex(0), vertex(1), vertex(2));
                }
            break;
        }
        }
        return tris;
    }

    std::uint64_t MeshKey(const Scene& scene, Entity e) const
    {
        Hasher h;
        const MeshRenderer* mr = scene.GetRegistry().TryGet<MeshRenderer>(e);
        if (!mr || !assets || !mr->model)
            return h.Add(std::uint32_t{0}).Value();
        const ModelSource source = assets->Source(mr->model);
        h.Add(std::string_view(source.primitive ? PrimitiveKey(*source.primitive) : PathToUtf8(source.file)));
        h.Add(mr->meshIndex).Add(assets->State(mr->model));
        if (const Model* model = assets->Get(mr->model); model && mr->meshIndex < model->meshes.size())
            for (const Submesh& sm : model->meshes[mr->meshIndex].submeshes)
                h.Add(sm.indexCount).Add(sm.boundsMin).Add(sm.boundsMax);
        return h.Value();
    }

    void MarkDirty(const Box& region)
    {
        if (region.Valid())
            dirtyRegions.push_back(region);
    }

    template <typename Record>
    void Sweep(std::unordered_map<std::uint64_t, Record>& records)
    {
        for (auto it = records.begin(); it != records.end();) {
            if (!it->second.seen) {
                MarkDirty(it->second.bounds);
                it = records.erase(it);
            } else {
                it->second.seen = false;
                ++it;
            }
        }
    }

    // Terrain components (navigation on): heights + holes hashed into the signature (stable across
    // bake files), edits mark only their rectangle dirty, checked at most twice a second.
    void CollectTerrains(Scene& scene)
    {
        TerrainStore& store = TerrainStore::Instance();
        const double  now   = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        scene.GetRegistry().ViewOf<Terrain, WorldTransform, Uuid>().Each([&](Entity e, Terrain& t, WorldTransform& wt, Uuid& id) {
            if (!t.navigation)
                return;
            std::shared_ptr<TerrainData> data = store.Get(t.data);
            if (!data)
                return;
            const std::uint64_t key    = id.value ^ 0x7465727261696e00ull; // apart from a collider on the same entity
            const glm::vec3     origin = glm::vec3(wt.matrix[3]) - glm::vec3(0.5f * data->size, 0.0f, 0.5f * data->size);
            SourceRecord&       record = sources[key];
            record.entity              = e;
            record.seen                = true;
            const std::uint64_t revision = store.Revision(t.data);
            const auto signatureOf = [&](const TerrainData& d) {
                Hasher h;
                h.Add(origin).Add(d.resolution).Add(d.size).Add(d.heightScale);
                h.Add(Fnv1a64(std::as_bytes(std::span(d.heights))));
                h.Add(Fnv1a64(std::as_bytes(std::span(d.holes))));
                return h.Value();
            };
            Box box;
            box.Add(origin);
            box.Add(origin + glm::vec3(data->size, data->heightScale, data->size));
            if (record.terrain != data || record.terrainOrigin != origin) {
                const std::uint64_t signature = signatureOf(*data);
                if (signature != record.signature || record.missed) {
                    MarkDirty(record.bounds);
                    MarkDirty(box);
                }
                record.signature       = signature;
                record.terrain         = data;
                record.terrainOrigin   = origin;
                record.terrainRevision = revision;
                record.terrainChecked  = now;
            } else if (revision != record.terrainRevision && now - record.terrainChecked >= 0.5) {
                const TerrainChanges changes = store.ChangesSince(t.data, record.terrainRevision);
                if (changes.kinds & (TerrainChangeHeights | TerrainChangeHoles | TerrainChangeSettings)) {
                    if (changes.full) {
                        MarkDirty(box);
                    } else {
                        Box region;
                        region.Add(origin + glm::vec3(changes.rect.x * data->size, 0.0f, changes.rect.y * data->size));
                        region.Add(origin + glm::vec3(changes.rect.z * data->size, data->heightScale, changes.rect.w * data->size));
                        region.min -= glm::vec3(data->CellSize());
                        region.max += glm::vec3(data->CellSize());
                        MarkDirty(region);
                    }
                    record.signature = signatureOf(*data);
                }
                record.terrainRevision = revision;
                record.terrainChecked  = now;
            }
            record.missed       = false;
            record.bounds       = box;
            record.hasTriangles = true;
            record.triangles.clear();
        });
    }

    // Two triangles per terrain cell inside `clip` (holes left out), counter-clockwise from above.
    static void AppendTerrainTriangles(const SourceRecord& s, const Box& clip, std::vector<float>& vertices,
                                       std::vector<int>& indices)
    {
        const TerrainData& t    = *s.terrain;
        const float        cell = t.CellSize();
        const auto         lo   = [&](float v) {
            return static_cast<std::uint32_t>(std::clamp(std::floor(v / cell), 0.0f, static_cast<float>(t.resolution - 2)));
        };
        const auto hi = [&](float v) {
            return static_cast<std::uint32_t>(std::clamp(std::ceil(v / cell), 0.0f, static_cast<float>(t.resolution - 1)));
        };
        const std::uint32_t x0 = lo(clip.min.x - s.terrainOrigin.x), x1 = hi(clip.max.x - s.terrainOrigin.x);
        const std::uint32_t z0 = lo(clip.min.z - s.terrainOrigin.z), z1 = hi(clip.max.z - s.terrainOrigin.z);
        const auto vertex = [&](std::uint32_t x, std::uint32_t z) {
            const glm::vec3 p = s.terrainOrigin + glm::vec3(static_cast<float>(x) * cell, t.HeightAtSample(x, z), static_cast<float>(z) * cell);
            vertices.insert(vertices.end(), {p.x, p.y, p.z});
        };
        for (std::uint32_t z = z0; z < z1; ++z)
            for (std::uint32_t x = x0; x < x1; ++x) {
                if (t.HoleAtSample(x, z))
                    continue;
                const int base = static_cast<int>(vertices.size() / 3);
                vertex(x, z), vertex(x, z + 1), vertex(x + 1, z), vertex(x + 1, z + 1);
                indices.insert(indices.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
            }
    }

    void Collect(Scene& scene)
    {
        Registry& reg = scene.GetRegistry();
        // Geometry: colliders of static bodies and colliders without a body.
        reg.ViewOf<Collider, WorldTransform, Uuid>().Each([&](Entity e, Collider& c, WorldTransform& wt, Uuid& id) {
            if (c.trigger)
                return;
            if (const RigidBody* rb = reg.TryGet<RigidBody>(e); rb && rb->type != BodyType::Static)
                return;
            if (reg.TryGet<CharacterController>(e) || reg.TryGet<NavAgent>(e))
                return;
            Hasher h;
            h.Add(wt.matrix).Add(c.shape).Add(c.halfExtents).Add(c.radius).Add(c.halfHeight).Add(c.center).Add(c.rotation);
            if (c.shape == ColliderShape::Mesh)
                h.Add(MeshKey(scene, e));
            const std::uint64_t signature = h.Value();
            SourceRecord&       record    = sources[id.value];
            record.entity                 = e;
            if (record.signature == signature && record.hasTriangles) {
                record.seen = true;
                return;
            }
            std::optional<std::vector<glm::vec3>> tris = ColliderTriangles(scene, e, c, wt.matrix);
            if (!tris) { // mesh still loading: keep what was there
                record.seen = record.signature != 0;
                if (!record.seen)
                    sources.erase(id.value);
                return;
            }
            Box box;
            for (const glm::vec3& p : *tris)
                box.Add(p);
            if (record.signature != signature || record.missed) {
                MarkDirty(record.bounds);
                MarkDirty(box);
            }
            record.missed       = false;
            record.signature    = signature;
            record.bounds       = box;
            record.triangles    = std::move(*tris);
            record.hasTriangles = true;
            record.seen         = true;
        });
        CollectTerrains(scene);
        Sweep(sources);

        reg.ViewOf<NavArea, WorldTransform, Uuid>().Each([&](Entity, NavArea& a, WorldTransform& wt, Uuid& id) {
            const unsigned char areaId = AreaId(a.area);
            const std::uint64_t signature =
                Hasher().Add(wt.matrix).Add(a.halfExtents).Add(areaId).Value();
            AreaRecord& record = areas[id.value];
            record.seen        = true;
            record.uuid        = id.value;
            if (record.signature == signature && !record.footprint.empty())
                return;
            const std::array<glm::vec3, 8> corners = BoxCorners(wt.matrix, glm::vec3(0.0f), a.halfExtents);
            Box box;
            for (const glm::vec3& p : corners)
                box.Add(p);
            if (record.signature != signature) {
                MarkDirty(record.bounds);
                MarkDirty(box);
            }
            record.signature = signature;
            record.bounds    = box;
            record.footprint = HullXZ(corners);
            record.areaId    = areaId;
        });
        Sweep(areas);

        reg.ViewOf<NavMeshBounds, WorldTransform, Uuid>().Each([&](Entity, NavMeshBounds& b, WorldTransform& wt, Uuid& id) {
            const std::uint64_t signature = Hasher().Add(wt.matrix).Add(b.halfExtents).Value();
            BoundsRecord&       record    = bounds[id.value];
            record.seen                   = true;
            if (record.signature == signature && record.bounds.Valid())
                return;
            Box box;
            for (const glm::vec3& p : BoxCorners(wt.matrix, glm::vec3(0.0f), b.halfExtents))
                box.Add(p);
            if (record.signature != signature) {
                MarkDirty(record.bounds);
                MarkDirty(box);
            }
            record.signature = signature;
            record.bounds    = box;
        });
        Sweep(bounds);

        reg.ViewOf<NavLink, WorldTransform, Uuid>().Each([&](Entity, NavLink& l, WorldTransform& wt, Uuid& id) {
            const unsigned char areaId    = std::max<unsigned char>(AreaId(l.area), 1);
            const std::uint64_t signature = Hasher()
                                                .Add(wt.matrix)
                                                .Add(l.start)
                                                .Add(l.end)
                                                .Add(l.radius)
                                                .Add(l.bidirectional)
                                                .Add(areaId)
                                                .Add(l.profiles)
                                                .Value();
            LinkRecord& record = links[id.value];
            record.seen        = true;
            record.uuid        = id.value;
            if (record.signature == signature && record.bounds.Valid())
                return;
            linksChanged = true;
            if (record.bounds.Valid())
                MarkLinkTiles(record);
            record.signature     = signature;
            record.start         = glm::vec3(wt.matrix * glm::vec4(l.start, 1.0f));
            record.end           = glm::vec3(wt.matrix * glm::vec4(l.end, 1.0f));
            record.radius        = std::max(l.radius, 0.05f);
            record.bidirectional = l.bidirectional;
            record.areaId        = areaId;
            record.profiles      = l.profiles;
            record.bounds        = Box();
            record.bounds.Add(record.start);
            record.bounds.Add(record.end);
            MarkLinkTiles(record);
        });
        for (auto it = links.begin(); it != links.end();) {
            if (!it->second.seen) {
                linksChanged = true;
                MarkLinkTiles(it->second);
                it = links.erase(it);
            } else {
                it->second.seen = false;
                ++it;
            }
        }
        if (linksChanged)
            RebuildLinkArrays();

        reg.ViewOf<NavObstacle, WorldTransform, Uuid>().Each([&](Entity, NavObstacle& o, WorldTransform& wt, Uuid& id) {
            const std::uint64_t signature =
                Hasher().Add(wt.matrix).Add(o.shape).Add(o.halfExtents).Add(o.radius).Add(o.height).Add(o.enabled).Value();
            ObstacleRecord& record = obstacles[id.value];
            record.seen            = true;
            if (record.signature == signature)
                return;
            RemoveObstacle(record);
            record.signature = signature;
            record.shape     = o.shape;
            const glm::vec3 scale(glm::length(glm::vec3(wt.matrix[0])), glm::length(glm::vec3(wt.matrix[1])),
                                  glm::length(glm::vec3(wt.matrix[2])));
            const glm::vec3 position(wt.matrix[3]);
            const glm::vec3 forward = -glm::vec3(wt.matrix[2]);
            record.yaw              = std::atan2(-forward.x, -forward.z);
            record.center           = position;
            record.halfExtents      = o.halfExtents * scale;
            record.radius           = o.radius * std::max(scale.x, scale.z);
            record.height           = o.height * scale.y;
            record.bounds           = Box();
            if (o.shape == NavObstacleShape::Box) {
                const float r = glm::length(glm::vec2(record.halfExtents.x, record.halfExtents.z));
                record.bounds.Add(position - glm::vec3(r, record.halfExtents.y, r));
                record.bounds.Add(position + glm::vec3(r, record.halfExtents.y, r));
            } else {
                record.bounds.Add(position - glm::vec3(record.radius, 0.0f, record.radius));
                record.bounds.Add(position + glm::vec3(record.radius, record.height, record.radius));
            }
            record.added = !o.enabled; // disabled: nothing to add
            if (!o.enabled)
                record.refs.clear();
        });
        for (auto it = obstacles.begin(); it != obstacles.end();) {
            if (!it->second.seen) {
                RemoveObstacle(it->second);
                it = obstacles.erase(it);
            } else {
                it->second.seen = false;
                ++it;
            }
        }
    }

    void MarkLinkTiles(const LinkRecord& link)
    {
        // Links are stored in the tile of their start (and connect to neighbors): rebuild those
        // navmesh tiles from the cache (no rasterization).
        for (std::size_t p = 0; p < profiles.size(); ++p)
            for (const glm::vec3& point : {link.start, link.end}) {
                const float tw = TileWorld();
                profiles[p]->linkDirty.insert(
                    MakeKey(static_cast<int>(std::floor(point.x / tw)), static_cast<int>(std::floor(point.z / tw))));
            }
    }

    void RebuildLinkArrays()
    {
        linksChanged = false;
        std::vector<const LinkRecord*> sorted;
        for (const auto& [uuid, l] : links)
            sorted.push_back(&l);
        std::ranges::sort(sorted, [](const LinkRecord* a, const LinkRecord* b) { return a->uuid < b->uuid; });
        for (std::size_t p = 0; p < profiles.size(); ++p) {
            MeshProcess& mp = profiles[p]->process;
            mp.linkVerts.clear(), mp.linkRadii.clear(), mp.linkDirs.clear();
            mp.linkAreas.clear(), mp.linkFlags.clear(), mp.linkIds.clear();
            for (const LinkRecord* l : sorted) {
                if (!(l->profiles & (1u << p)))
                    continue;
                mp.linkVerts.insert(mp.linkVerts.end(), {l->start.x, l->start.y, l->start.z, l->end.x, l->end.y, l->end.z});
                mp.linkRadii.push_back(l->radius);
                mp.linkDirs.push_back(static_cast<unsigned char>(l->bidirectional ? DT_OFFMESH_CON_BIDIR : 0u));
                mp.linkAreas.push_back(l->areaId);
                mp.linkFlags.push_back(static_cast<unsigned short>(1u << (l->areaId - 1)));
                mp.linkIds.push_back(static_cast<unsigned int>(l->uuid & 0xFFFFFFFFu));
            }
        }
    }

    // --- Obstacles ---

    void RemoveObstacle(ObstacleRecord& o)
    {
        for (std::size_t p = 0; p < o.refs.size() && p < profiles.size(); ++p)
            if (o.refs[p])
                profiles[p]->cache->removeObstacle(o.refs[p]);
        o.refs.clear();
        o.added = false;
    }

    void SyncObstacles()
    {
        for (auto& [uuid, o] : obstacles) {
            if (o.refresh && o.added && !o.refs.empty()) {
                RemoveObstacle(o);
                o.refresh = false;
            }
            if (o.added)
                continue;
            o.refs.assign(profiles.size(), 0);
            bool ok = true;
            for (std::size_t p = 0; p < profiles.size(); ++p) {
                dtTileCache& cache = *profiles[p]->cache;
                dtStatus     status;
                if (o.shape == NavObstacleShape::Box)
                    status = cache.addBoxObstacle(glm::value_ptr(o.center), glm::value_ptr(o.halfExtents), o.yaw, &o.refs[p]);
                else
                    status = cache.addObstacle(glm::value_ptr(o.center), o.radius, o.height, &o.refs[p]);
                ok = ok && dtStatusSucceed(status);
            }
            if (!ok) { // request queue full / out of obstacles: retry next update
                RemoveObstacle(o);
                continue;
            }
            o.added = true;
        }
        for (auto& p : profiles) {
            bool upToDate = false;
            for (int i = 0; i < 64 && !upToDate; ++i)
                if (dtStatusFailed(p->cache->update(0.0f, p->mesh, &upToDate)))
                    break;
        }
    }

    // --- Tiles ---

    [[nodiscard]] Box TileRect(const Profile& p, TileKey key, bool withBorder) const
    {
        const float tw     = TileWorld();
        const float border = withBorder ? static_cast<float>(p.rc.borderSize) * settings.cellSize : 0.0f;
        Box b;
        b.min = glm::vec3(static_cast<float>(KeyX(key)) * tw - border, std::numeric_limits<float>::lowest(),
                          static_cast<float>(KeyY(key)) * tw - border);
        b.max = glm::vec3(static_cast<float>(KeyX(key) + 1) * tw + border, std::numeric_limits<float>::max(),
                          static_cast<float>(KeyY(key) + 1) * tw + border);
        return b;
    }

    void ForTilesIn(const Profile& p, const Box& region, const std::function<void(TileKey)>& fn) const
    {
        const float tw     = TileWorld();
        const float border = static_cast<float>(p.rc.borderSize) * settings.cellSize;
        const int   x0 = static_cast<int>(std::floor((region.min.x - border) / tw));
        const int   x1 = static_cast<int>(std::floor((region.max.x + border) / tw));
        const int   z0 = static_cast<int>(std::floor((region.min.z - border) / tw));
        const int   z1 = static_cast<int>(std::floor((region.max.z + border) / tw));
        if (static_cast<std::int64_t>(x1 - x0 + 1) * (z1 - z0 + 1) > 1'000'000) {
            ENGINE_WARN("Navigation: region too large ({}x{} tiles), ignored", x1 - x0 + 1, z1 - z0 + 1);
            return;
        }
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                fn(MakeKey(x, z));
    }

    // Tiles that exist now (to remove them when their area goes away).
    void ForBuiltTiles(const Profile& p, const std::function<void(TileKey)>& fn) const
    {
        std::unordered_set<TileKey> keys;
        for (int i = 0; i < p.cache->getTileCount(); ++i)
            if (const dtCompressedTile* t = p.cache->getTile(i); t && t->header)
                keys.insert(MakeKey(t->header->tx, t->header->ty));
        for (const TileKey k : keys)
            fn(k);
    }

    void PropagateDirty()
    {
        for (auto& p : profiles) {
            if (everythingDirty) {
                for (const auto& [uuid, b] : bounds)
                    ForTilesIn(*p, b.bounds, [&](TileKey k) { p->dirty.insert(k); });
                ForBuiltTiles(*p, [&](TileKey k) { p->dirty.insert(k); });
            } else {
                for (const Box& region : dirtyRegions)
                    ForTilesIn(*p, region, [&](TileKey k) { p->dirty.insert(k); });
            }
        }
        everythingDirty = false;
        dirtyRegions.clear();
    }

    TileInput MakeInput(std::uint32_t profileIndex, TileKey key)
    {
        Profile&  p = *profiles[profileIndex];
        TileInput in;
        in.profile    = profileIndex;
        in.key        = key;
        in.generation = p.generation[key] = nextGeneration++;
        in.slope      = p.config.maxSlope;
        in.config     = p.rc;
        const Box rect = TileRect(p, key, true);
        for (const auto& [uuid, b] : bounds)
            if (b.bounds.OverlapsXZ(TileRect(p, key, false).min, TileRect(p, key, false).max))
                in.bounds.push_back(b.bounds);
        if (in.bounds.empty())
            return in; // outside every NavMeshBounds: the tile goes away
        float ymin = std::numeric_limits<float>::max(), ymax = std::numeric_limits<float>::lowest();
        for (const Box& b : in.bounds)
            ymin = std::min(ymin, b.min.y), ymax = std::max(ymax, b.max.y);
        rcConfig& c = in.config;
        c.bmin[0] = rect.min.x, c.bmin[1] = ymin, c.bmin[2] = rect.min.z;
        c.bmax[0] = rect.max.x, c.bmax[1] = ymax + p.config.height, c.bmax[2] = rect.max.z;
        Box clip  = rect;
        clip.min.y = ymin - 1.0f;
        clip.max.y = c.bmax[1] + 1.0f;
        for (auto& [uuid, s] : sources) {
            if (!s.bounds.Overlaps(clip))
                continue;
            s.missed = s.missed || !s.hasTriangles; // rebuilt once its triangles are there
            if (s.terrain) {
                AppendTerrainTriangles(s, clip, in.vertices, in.indices);
                continue;
            }
            for (std::size_t t = 0; t + 2 < s.triangles.size(); t += 3) {
                Box tri;
                tri.Add(s.triangles[t]), tri.Add(s.triangles[t + 1]), tri.Add(s.triangles[t + 2]);
                if (!tri.Overlaps(clip))
                    continue;
                const int base = static_cast<int>(in.vertices.size() / 3);
                for (int k = 0; k < 3; ++k) {
                    const glm::vec3& v = s.triangles[t + static_cast<std::size_t>(k)];
                    in.vertices.insert(in.vertices.end(), {v.x, v.y, v.z});
                }
                in.indices.insert(in.indices.end(), {base, base + 1, base + 2});
            }
        }
        std::vector<const AreaRecord*> sorted;
        for (const auto& [uuid, a] : areas)
            if (a.bounds.Overlaps(clip))
                sorted.push_back(&a);
        std::ranges::sort(sorted, [](const AreaRecord* a, const AreaRecord* b) { return a->uuid < b->uuid; });
        for (const AreaRecord* a : sorted)
            in.areas.push_back(*a);
        return in;
    }

    void LaunchBuilds(bool all)
    {
        const std::size_t budget = jobs ? std::max<std::size_t>(jobs->ThreadCount(), 1) * 2 : 4;
        std::size_t       running;
        {
            std::lock_guard lock(results->mutex);
            running = static_cast<std::size_t>(results->running);
        }
        for (std::uint32_t pi = 0; pi < profiles.size(); ++pi) {
            Profile& p = *profiles[pi];
            std::vector<TileKey> keys(p.dirty.begin(), p.dirty.end());
            std::ranges::sort(keys);
            for (const TileKey key : keys) {
                if (!all && running >= budget)
                    return;
                p.dirty.erase(key);
                p.building.insert(key);
                auto input = std::make_shared<TileInput>(MakeInput(pi, key));
                ++running;
                if (!jobs) {
                    TileOutput out = BuildTileLayers(*input);
                    std::lock_guard lock(results->mutex);
                    results->outputs.push_back(std::move(out));
                    continue;
                }
                {
                    std::lock_guard lock(results->mutex);
                    ++results->running;
                }
                jobs->Enqueue([shared = results, input] {
                    TileOutput out;
                    try {
                        out = BuildTileLayers(*input);
                    } catch (...) {
                        out = TileOutput{.profile = input->profile, .key = input->key, .generation = input->generation,
                                         .layers = {}};
                    }
                    std::lock_guard lock(shared->mutex);
                    shared->outputs.push_back(std::move(out));
                    --shared->running;
                    shared->done.notify_all();
                });
            }
        }
    }

    void ApplyOutputs(bool wait)
    {
        std::vector<TileOutput> outputs;
        {
            std::unique_lock lock(results->mutex);
            if (wait)
                results->done.wait(lock, [&] { return results->running == 0; });
            outputs.swap(results->outputs);
        }
        for (TileOutput& out : outputs) {
            Profile* p = GetProfile(out.profile);
            if (!p || p->generation[out.key] != out.generation) { // superseded (rebuilt again, profiles recreated)
                FreeLayers(out.layers);
                continue;
            }
            p->building.erase(out.key);
            ReplaceTile(*p, out.key, out.layers);
            ++stats.tilesBuilt;
        }
    }

    void ReplaceTile(Profile& p, TileKey key, std::vector<Layer>& layers)
    {
        const int tx = KeyX(key), ty = KeyY(key);
        std::array<dtCompressedTileRef, kMaxLayers> refs{};
        const int n = p.cache->getTilesAt(tx, ty, refs.data(), kMaxLayers);
        for (int i = 0; i < n; ++i)
            p.cache->removeTile(refs[static_cast<std::size_t>(i)], nullptr, nullptr);
        std::array<const dtMeshTile*, kMaxLayers> tiles{};
        const int nt = p.mesh->getTilesAt(tx, ty, tiles.data(), kMaxLayers);
        std::vector<dtTileRef> meshRefs;
        for (int i = 0; i < nt; ++i)
            meshRefs.push_back(p.mesh->getTileRef(tiles[static_cast<std::size_t>(i)]));
        for (const dtTileRef ref : meshRefs)
            p.mesh->removeTile(ref, nullptr, nullptr);
        for (Layer& l : layers) {
            if (dtStatusFailed(p.cache->addTile(l.data, l.size, DT_COMPRESSEDTILE_FREE_DATA, nullptr)))
                dtFree(l.data);
            l.data = nullptr;
        }
        layers.clear();
        p.cache->buildNavMeshTilesAt(tx, ty, p.mesh);
        // Obstacles on this tile are tied to the old cache tiles: add them again.
        const Box rect = TileRect(p, key, false);
        for (auto& [uuid, o] : obstacles)
            if (o.added && o.bounds.OverlapsXZ(rect.min, rect.max))
                o.refresh = true;
    }

    void RebuildLinkTiles()
    {
        for (auto& p : profiles) {
            for (const TileKey key : p->linkDirty)
                if (!p->dirty.contains(key) && !p->building.contains(key))
                    p->cache->buildNavMeshTilesAt(KeyX(key), KeyY(key), p->mesh);
            p->linkDirty.clear();
        }
    }

    // --- Agents ---

    void RemoveAgent(std::uint64_t id, Agent& a)
    {
        if (Profile* p = GetProfile(a.profile); p && a.index >= 0)
            p->crowd->removeAgent(a.index);
        if (a.character && physics) // the character would keep walking with the last input
            physics->SetCharacterInput(static_cast<Entity>(id), glm::vec3(0.0f), false);
        a.index = -1;
    }

    void DropAgents()
    {
        for (auto& [id, a] : agents)
            RemoveAgent(id, a);
        agents.clear();
    }

    int FilterIndex(Profile& p, std::uint32_t mask)
    {
        for (std::size_t i = 0; i < p.filterMasks.size(); ++i)
            if (p.filterMasks[i] == mask)
                return static_cast<int>(i);
        if (p.filterMasks.size() >= DT_CROWD_MAX_QUERY_FILTER_TYPE)
            return 0;
        p.filterMasks.push_back(mask);
        ApplyFilter(*p.crowd->getEditableFilter(static_cast<int>(p.filterMasks.size() - 1)), mask);
        return static_cast<int>(p.filterMasks.size() - 1);
    }

    dtCrowdAgentParams AgentParams(Profile& p, const NavAgent& a)
    {
        dtCrowdAgentParams ap{};
        ap.radius                = p.config.radius;
        ap.height                = p.config.height;
        ap.maxAcceleration       = std::max(a.acceleration, 0.01f);
        ap.maxSpeed              = std::max(a.maxSpeed, 0.0f);
        ap.collisionQueryRange   = ap.radius * 12.0f;
        ap.pathOptimizationRange = ap.radius * 30.0f;
        ap.separationWeight      = std::max(a.separationWeight, 0.0f);
        ap.updateFlags           = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
        if (a.avoidance != NavAvoidance::Off)
            ap.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
        if (a.separationWeight > 0.0f)
            ap.updateFlags |= DT_CROWD_SEPARATION;
        ap.obstacleAvoidanceType = static_cast<unsigned char>(std::max<int>(static_cast<int>(a.avoidance) - 1, 0));
        ap.queryFilterType       = static_cast<unsigned char>(FilterIndex(p, a.areaMask));
        return ap;
    }

    [[nodiscard]] std::optional<std::pair<dtPolyRef, glm::vec3>> Nearest(const Profile& p, const glm::vec3& point,
                                                                         const glm::vec3& extents,
                                                                         const dtQueryFilter& filter) const
    {
        dtPolyRef ref = 0;
        glm::vec3 nearest{0.0f};
        if (dtStatusFailed(p.query->findNearestPoly(glm::value_ptr(point), glm::value_ptr(extents), &filter, &ref,
                                                    glm::value_ptr(nearest))) ||
            !ref)
            return std::nullopt;
        return std::pair{ref, nearest};
    }

    void UpdateAgents(Scene& scene, float dt)
    {
        Registry& reg = scene.GetRegistry();
        reg.ViewOf<NavAgent, WorldTransform>().Each([&](Entity e, NavAgent& config, WorldTransform& wt) {
            const std::uint64_t id    = static_cast<std::uint64_t>(e);
            std::size_t         index = 0;
            if (const auto found = settings.Profile(config.profile))
                index = *found;
            else if (warnedProfiles.insert(config.profile).second)
                ENGINE_WARN("Navigation: unknown agent profile '{}', using '{}'", config.profile,
                            settings.profiles[0].name);
            Profile* p = GetProfile(static_cast<std::uint32_t>(index));
            if (!p)
                return;
            const glm::vec3 position(wt.matrix[3]);
            Agent&          a = agents[id];
            a.seen            = true;
            a.character       = config.driveCharacter && physics && reg.TryGet<CharacterController>(e);
            const bool teleported =
                a.index >= 0 && !a.character && glm::distance(position, a.written) > 0.05f;
            if (a.index >= 0 && (a.profile != index || teleported)) {
                GetProfile(a.profile)->crowd->removeAgent(a.index);
                a.index = -1;
                if (a.status == NavMoveStatus::Moving)
                    pendingMoves.emplace(id, MoveRequest{a.target, a.acceptance});
            }
            if (a.index < 0) {
                const glm::vec3 extents(p->config.radius * 2.0f, p->config.height, p->config.radius * 2.0f);
                const auto      placed = Nearest(*p, position, extents, *p->crowd->getFilter(0));
                if (!placed)
                    return; // not on the navmesh (yet)
                a.profile = static_cast<std::uint32_t>(index);
                a.params  = config;
                const dtCrowdAgentParams ap = AgentParams(*p, config);
                a.index   = p->crowd->addAgent(glm::value_ptr(placed->second), &ap);
                a.written = position;
                if (a.index < 0)
                    return;
            } else if (!(a.params == config)) {
                a.params                    = config;
                const dtCrowdAgentParams ap = AgentParams(*p, config);
                p->crowd->updateAgentParameters(a.index, &ap);
            }
            // The character controller moved it: follow (not while the crowd carries it over a link).
            if (a.character && p->crowd->getAgent(a.index)->state != DT_CROWDAGENT_STATE_OFFMESH)
                dtVcopy(p->crowd->getEditableAgent(a.index)->npos, glm::value_ptr(position));
            if (const auto move = pendingMoves.find(id); move != pendingMoves.end()) {
                a.target                     = move->second.target;
                a.acceptance                 = move->second.acceptance;
                const dtQueryFilter& filter  = *p->crowd->getFilter(p->crowd->getAgent(a.index)->params.queryFilterType);
                const auto           target  = Nearest(*p, move->second.target, glm::vec3(2.0f, 4.0f, 2.0f), filter);
                if (target && p->crowd->requestMoveTarget(a.index, target->first, glm::value_ptr(target->second))) {
                    a.target = target->second;
                    a.status = NavMoveStatus::Moving;
                } else {
                    a.status = NavMoveStatus::Failed;
                }
                pendingMoves.erase(move);
            }
        });
        for (auto it = agents.begin(); it != agents.end();) {
            if (!it->second.seen) {
                RemoveAgent(it->first, it->second);
                pendingMoves.erase(it->first);
                it = agents.erase(it);
            } else {
                it->second.seen = false;
                ++it;
            }
        }
        // Requests for entities that are no agents (or not placed): fail unless tiles are coming.
        for (auto it = pendingMoves.begin(); it != pendingMoves.end();) {
            const auto agent = agents.find(it->first);
            if (agent == agents.end() && !IsBuilding()) {
                orphanMoves[it->first] = NavMoveStatus::Failed;
                it = pendingMoves.erase(it);
                continue;
            }
            ++it;
        }

        for (auto& p : profiles)
            p->crowd->update(std::max(dt, 0.0f), nullptr);

        for (auto& [id, a] : agents) {
            if (a.index < 0)
                continue;
            Profile&            p  = *profiles[a.profile];
            const dtCrowdAgent* ag = p.crowd->getAgent(a.index);
            if (!ag || !ag->active)
                continue;
            const Entity    e = static_cast<Entity>(id);
            const glm::vec3 velocity = V3(ag->vel);
            const glm::vec3 position = V3(ag->npos);
            if (a.status == NavMoveStatus::Moving) {
                const glm::vec3 delta = a.target - position;
                const float     reach = std::max(a.acceptance >= 0.0f ? a.acceptance : a.params.stoppingDistance, 0.05f);
                if (glm::length(glm::vec2(delta.x, delta.z)) <= reach && std::abs(delta.y) < p.config.height) {
                    p.crowd->resetMoveTarget(a.index);
                    a.status = NavMoveStatus::Arrived;
                } else if (ag->targetState == DT_CROWDAGENT_TARGET_FAILED) {
                    p.crowd->resetMoveTarget(a.index);
                    a.status = NavMoveStatus::Failed;
                } else if (ag->targetState == DT_CROWDAGENT_TARGET_VALID && ag->partial && ag->ncorners > 0 &&
                           (ag->cornerFlags[ag->ncorners - 1] & DT_STRAIGHTPATH_END) &&
                           glm::distance(V3(&ag->cornerVerts[(ag->ncorners - 1) * 3]), position) <= reach) {
                    p.crowd->resetMoveTarget(a.index); // reached the end of a partial path: unreachable
                    a.status = NavMoveStatus::Failed;
                }
            }
            const bool offMesh = ag->state == DT_CROWDAGENT_STATE_OFFMESH;
            if (a.character && !offMesh) {
                physics->SetCharacterInput(e, glm::vec3(velocity.x, 0.0f, velocity.z), false);
                continue;
            }
            if (a.character) // over a link (jump, ladder): placed directly, the physics takes it as a teleport
                physics->SetCharacterInput(e, glm::vec3(0.0f), false);
            // Direct movement: world position (and yaw) -> local transform.
            glm::mat4 parentWorld(1.0f);
            if (const Hierarchy* h = reg.TryGet<Hierarchy>(e); h && h->parent != NullEntity)
                if (const WorldTransform* pw = reg.TryGet<WorldTransform>(h->parent))
                    parentWorld = pw->matrix;
            const glm::mat4 toLocal = glm::inverse(parentWorld);
            Transform&      t       = scene.EditTransform(e);
            t.position              = glm::vec3(toLocal * glm::vec4(position, 1.0f));
            if (a.params.faceMovement && glm::length(glm::vec2(velocity.x, velocity.z)) > 0.1f) {
                const float     yaw   = std::atan2(-velocity.x, -velocity.z);
                const glm::quat world = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                const glm::quat parent =
                    glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(parentWorld[0])), glm::normalize(glm::vec3(parentWorld[1])),
                                             glm::normalize(glm::vec3(parentWorld[2]))));
                t.rotation = glm::normalize(glm::inverse(parent) * world);
            }
            a.written = position;
        }
        scene.UpdateTransforms();
    }

    [[nodiscard]] bool IsBuilding() const
    {
        for (const auto& p : profiles)
            if (!p->dirty.empty() || !p->building.empty())
                return true;
        return false;
    }

    void UpdateStats()
    {
        stats.profiles = static_cast<std::uint32_t>(profiles.size());
        stats.tiles = stats.polygons = stats.pendingTiles = 0;
        for (const auto& p : profiles) {
            const dtNavMesh& mesh = *p->mesh;
            for (int i = 0; i < mesh.getMaxTiles(); ++i)
                if (const dtMeshTile* t = mesh.getTile(i); t && t->header) {
                    ++stats.tiles;
                    stats.polygons += static_cast<std::uint32_t>(t->header->polyCount);
                }
            stats.pendingTiles += static_cast<std::uint32_t>(p->dirty.size() + p->building.size());
        }
        stats.sources   = static_cast<std::uint32_t>(sources.size());
        stats.areas     = static_cast<std::uint32_t>(areas.size());
        stats.links     = static_cast<std::uint32_t>(links.size());
        stats.obstacles = static_cast<std::uint32_t>(obstacles.size());
        stats.agents    = 0;
        for (const auto& [id, a] : agents)
            stats.agents += a.index >= 0 ? 1u : 0u;
    }
};


NavigationWorld::NavigationWorld(ThreadPool* jobs, const AssetManager* assets, PhysicsWorld* physics)
    : m_Impl(std::make_unique<Impl>(jobs, assets, physics))
{
}

NavigationWorld::~NavigationWorld() = default;

void NavigationWorld::SetSettings(const NavSettings& settings)
{
    Impl&       I    = *m_Impl;
    NavSettings next = settings;
    SanitizeNavSettings(next);
    const bool rebuild = next.BuildHash() != I.settings.BuildHash() || next.maxObstacles != I.settings.maxObstacles ||
                         next.maxAgents != I.settings.maxAgents;
    I.settings = next;
    if (rebuild) {
        I.ApplyOutputs(true); // let running tiles finish; their outputs are dropped with the profiles
        I.CreateProfiles();
        I.pendingMoves.clear();
        return;
    }
    for (auto& p : I.profiles) // costs only
        for (std::size_t i = 0; i < p->filterMasks.size(); ++i)
            I.ApplyFilter(*p->crowd->getEditableFilter(static_cast<int>(i)), p->filterMasks[i]);
}

const NavSettings& NavigationWorld::Settings() const { return m_Impl->settings; }

void NavigationWorld::Update(Scene& scene, float dt, bool simulate)
{
    Impl&      I     = *m_Impl;
    const auto start = std::chrono::steady_clock::now();
    scene.UpdateTransforms();
    I.ApplyOutputs(false);
    I.Collect(scene);
    if (autoRebuild)
        I.PropagateDirty();
    else
        I.dirtyRegions.clear();
    I.LaunchBuilds(false);
    I.ApplyOutputs(false);
    I.RebuildLinkTiles();
    I.SyncObstacles();
    if (simulate) {
        I.UpdateAgents(scene, dt);
    } else {
        I.DropAgents();
        I.pendingMoves.clear();
        I.orphanMoves.clear();
    }
    I.UpdateStats();
    I.stats.updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void NavigationWorld::Build(Scene& scene)
{
    Impl& I = *m_Impl;
    scene.UpdateTransforms();
    I.Collect(scene);
    I.everythingDirty = true;
    I.PropagateDirty();
    while (I.IsBuilding()) {
        I.LaunchBuilds(true);
        I.ApplyOutputs(true);
    }
    I.RebuildLinkTiles();
    I.SyncObstacles();
    I.UpdateStats();
}

void NavigationWorld::Flush(Scene& scene)
{
    Impl& I = *m_Impl;
    scene.UpdateTransforms();
    I.Collect(scene);
    if (autoRebuild)
        I.PropagateDirty();
    else
        I.dirtyRegions.clear();
    while (I.IsBuilding()) {
        I.LaunchBuilds(true);
        I.ApplyOutputs(true);
    }
    I.RebuildLinkTiles();
    I.SyncObstacles();
    I.UpdateStats();
}

bool NavigationWorld::IsBuilding() const { return m_Impl->IsBuilding(); }

void NavigationWorld::Reset()
{
    Impl& I = *m_Impl;
    I.ApplyOutputs(true);
    I.sources.clear(), I.areas.clear(), I.links.clear(), I.bounds.clear(), I.obstacles.clear();
    I.dirtyRegions.clear();
    I.pendingMoves.clear(), I.orphanMoves.clear();
    I.CreateProfiles();
    I.everythingDirty = false;
    I.stats           = {};
    I.UpdateStats();
}

namespace {

template <typename T>
void Put(std::vector<std::byte>& out, const T& value)
{
    const auto bytes = std::as_bytes(std::span(&value, 1));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : m_Data(data) {}
    template <typename T>
    bool Get(T& value)
    {
        if (m_Pos + sizeof(T) > m_Data.size())
            return false;
        std::memcpy(&value, m_Data.data() + m_Pos, sizeof(T));
        m_Pos += sizeof(T);
        return true;
    }
    bool Bytes(std::size_t size, const std::byte*& at)
    {
        if (m_Pos + size > m_Data.size())
            return false;
        at = m_Data.data() + m_Pos;
        m_Pos += size;
        return true;
    }

private:
    std::span<const std::byte> m_Data;
    std::size_t                m_Pos = 0;
};

void PutBox(std::vector<std::byte>& out, const Box& b)
{
    Put(out, b.min);
    Put(out, b.max);
}

} // namespace

bool NavigationWorld::SaveBaked(const fs::path& file, std::string* error) const
{
    const Impl& I = *m_Impl;
    if (I.IsBuilding()) {
        if (error)
            *error = "the navmesh is still building";
        return false;
    }
    std::vector<std::byte> out;
    Put(out, kBakedMagic);
    Put(out, kBakedVersion);
    Put(out, I.settings.BuildHash());
    Put(out, static_cast<std::uint32_t>(I.profiles.size()));
    for (const auto& p : I.profiles) {
        std::vector<const dtCompressedTile*> tiles;
        for (int i = 0; i < p->cache->getTileCount(); ++i)
            if (const dtCompressedTile* t = p->cache->getTile(i); t && t->header)
                tiles.push_back(t);
        std::ranges::sort(tiles, [](const dtCompressedTile* a, const dtCompressedTile* b) {
            return std::tuple(a->header->tx, a->header->ty, a->header->tlayer) <
                   std::tuple(b->header->tx, b->header->ty, b->header->tlayer);
        });
        Put(out, static_cast<std::uint32_t>(tiles.size()));
        for (const dtCompressedTile* t : tiles) {
            Put(out, static_cast<std::uint32_t>(t->dataSize));
            const auto bytes = std::as_bytes(std::span(t->data, static_cast<std::size_t>(t->dataSize)));
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
    }
    // Source state (sorted for reproducible files): what the tiles were built from.
    const auto putRecords = [&](const auto& records) {
        std::vector<std::uint64_t> keys;
        for (const auto& [uuid, r] : records)
            keys.push_back(uuid);
        std::ranges::sort(keys);
        Put(out, static_cast<std::uint32_t>(keys.size()));
        for (const std::uint64_t uuid : keys) {
            Put(out, uuid);
            Put(out, records.at(uuid).signature);
            PutBox(out, records.at(uuid).bounds);
        }
    };
    putRecords(I.sources);
    putRecords(I.areas);
    putRecords(I.bounds);

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!stream) {
            if (error)
                *error = "cannot write '" + PathToUtf8(tmp) + "'";
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        if (error)
            *error = "cannot write '" + PathToUtf8(file) + "': " + ec.message();
        return false;
    }
    return true;
}

bool NavigationWorld::LoadBaked(const fs::path& file, std::string* error)
{
    Impl&                                        I    = *m_Impl;
    const auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    std::optional<std::vector<std::byte>> data;
    try {
        data = Vfs::Read(file);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (!data)
        return fail("cannot read '" + PathToUtf8(file) + "'");
    Reader        in(*data);
    std::uint32_t magic = 0, version = 0, profileCount = 0;
    std::uint64_t hash = 0;
    if (!in.Get(magic) || magic != kBakedMagic || !in.Get(version) || version != kBakedVersion || !in.Get(hash))
        return fail("'" + PathToUtf8(file) + "' is no navmesh file");
    if (hash != I.settings.BuildHash() || !in.Get(profileCount) || profileCount != I.settings.profiles.size())
        return fail("'" + PathToUtf8(file) + "' was built with other navigation settings");

    struct TileData {
        std::uint32_t   profile;
        const std::byte* bytes;
        std::uint32_t   size;
    };
    std::vector<TileData> tiles;
    for (std::uint32_t p = 0; p < profileCount; ++p) {
        std::uint32_t count = 0;
        if (!in.Get(count))
            return fail("'" + PathToUtf8(file) + "' is truncated");
        for (std::uint32_t i = 0; i < count; ++i) {
            TileData t{p, nullptr, 0};
            if (!in.Get(t.size) || t.size < sizeof(dtTileCacheLayerHeader) || !in.Bytes(t.size, t.bytes))
                return fail("'" + PathToUtf8(file) + "' is truncated");
            tiles.push_back(t);
        }
    }
    struct RecordData {
        std::uint64_t uuid = 0, signature = 0;
        Box           bounds;
    };
    std::array<std::vector<RecordData>, 3> records;
    for (auto& list : records) {
        std::uint32_t count = 0;
        if (!in.Get(count))
            return fail("'" + PathToUtf8(file) + "' is truncated");
        for (std::uint32_t i = 0; i < count; ++i) {
            RecordData r;
            if (!in.Get(r.uuid) || !in.Get(r.signature) || !in.Get(r.bounds.min) || !in.Get(r.bounds.max))
                return fail("'" + PathToUtf8(file) + "' is truncated");
            list.push_back(r);
        }
    }

    // Replace everything: fresh navmeshes with the file's tiles, source state from the file.
    I.ApplyOutputs(true);
    I.sources.clear(), I.areas.clear(), I.bounds.clear(), I.dirtyRegions.clear();
    I.CreateProfiles();
    I.everythingDirty = false;
    std::vector<std::set<TileKey>> keys(I.profiles.size());
    for (const TileData& t : tiles) {
        Impl::Profile* p = I.GetProfile(t.profile);
        if (!p)
            continue;
        auto* copy = static_cast<unsigned char*>(dtAlloc(t.size, DT_ALLOC_PERM));
        std::memcpy(copy, t.bytes, t.size);
        const auto* header = reinterpret_cast<const dtTileCacheLayerHeader*>(copy);
        const TileKey key  = MakeKey(header->tx, header->ty);
        if (dtStatusFailed(p->cache->addTile(copy, static_cast<int>(t.size), DT_COMPRESSEDTILE_FREE_DATA, nullptr))) {
            dtFree(copy);
            continue;
        }
        keys[t.profile].insert(key);
    }
    for (std::size_t p = 0; p < keys.size(); ++p)
        for (const TileKey key : keys[p])
            I.profiles[p]->cache->buildNavMeshTilesAt(KeyX(key), KeyY(key), I.profiles[p]->mesh);
    for (const RecordData& r : records[0])
        I.sources[r.uuid] = SourceRecord{.entity = NullEntity, .signature = r.signature, .bounds = r.bounds,
                                         .triangles = {}, .hasTriangles = false, .missed = false, .seen = false,
                                         .terrain = {}, .terrainOrigin = glm::vec3(0.0f), .terrainRevision = 0,
                                         .terrainChecked = 0.0};
    for (const RecordData& r : records[1]) {
        AreaRecord& a = I.areas[r.uuid];
        a.signature   = r.signature;
        a.bounds      = r.bounds;
        a.uuid        = r.uuid;
    }
    for (const RecordData& r : records[2])
        I.bounds[r.uuid] = BoundsRecord{.signature = r.signature, .bounds = r.bounds, .seen = false};
    I.linksChanged = true; // links are re-collected from the scene and rebuilt into their tiles
    for (auto& [uuid, link] : I.links)
        I.MarkLinkTiles(link);
    I.UpdateStats();
    return true;
}

// --- Queries ---

std::optional<glm::vec3> NavigationWorld::ProjectPoint(const glm::vec3& p, std::uint32_t profile,
                                                       const glm::vec3& extents) const
{
    const Impl::Profile* prof = m_Impl->GetProfile(profile);
    if (!prof)
        return std::nullopt;
    const auto nearest = m_Impl->Nearest(*prof, p, extents, m_Impl->Filter(0xFFFFFFFFu));
    return nearest ? std::optional(nearest->second) : std::nullopt;
}

NavPath NavigationWorld::FindPath(const glm::vec3& start, const glm::vec3& end, std::uint32_t profile,
                                  std::uint32_t areaMask) const
{
    NavPath              path;
    const Impl::Profile* p = m_Impl->GetProfile(profile);
    if (!p)
        return path;
    const dtQueryFilter filter  = m_Impl->Filter(areaMask);
    const glm::vec3     extents(2.0f, 4.0f, 2.0f);
    const auto          from = m_Impl->Nearest(*p, start, extents, filter);
    const auto          to   = m_Impl->Nearest(*p, end, extents, filter);
    if (!from || !to)
        return path;
    std::array<dtPolyRef, kMaxPathPolys> polys{};
    int                                  polyCount = 0;
    if (dtStatusFailed(p->query->findPath(from->first, to->first, glm::value_ptr(from->second), glm::value_ptr(to->second),
                                          &filter, polys.data(), &polyCount, kMaxPathPolys)) ||
        polyCount == 0)
        return path;
    glm::vec3 target = to->second;
    const bool complete = polys[static_cast<std::size_t>(polyCount - 1)] == to->first;
    if (!complete)
        p->query->closestPointOnPoly(polys[static_cast<std::size_t>(polyCount - 1)], glm::value_ptr(to->second),
                                     glm::value_ptr(target), nullptr);
    std::array<float, kMaxStraightPath * 3> straight{};
    int                                     count = 0;
    if (dtStatusFailed(p->query->findStraightPath(glm::value_ptr(from->second), glm::value_ptr(target), polys.data(),
                                                  polyCount, straight.data(), nullptr, nullptr, &count,
                                                  kMaxStraightPath)))
        return path;
    for (int i = 0; i < count; ++i)
        path.points.push_back(V3(&straight[static_cast<std::size_t>(i) * 3]));
    path.status = complete ? NavPathStatus::Complete : NavPathStatus::Partial;
    return path;
}

std::optional<glm::vec3> NavigationWorld::RandomPoint(std::uint32_t profile, std::uint32_t areaMask) const
{
    const Impl::Profile* p = m_Impl->GetProfile(profile);
    if (!p)
        return std::nullopt;
    const dtQueryFilter filter = m_Impl->Filter(areaMask);
    t_Random                   = &m_Impl->random;
    dtPolyRef ref              = 0;
    glm::vec3 point{0.0f};
    const dtStatus status = p->query->findRandomPoint(&filter, RandomFloat, &ref, glm::value_ptr(point));
    t_Random              = nullptr;
    if (dtStatusFailed(status) || !ref)
        return std::nullopt;
    return point;
}

std::optional<glm::vec3> NavigationWorld::RandomPointInRadius(const glm::vec3& center, float radius,
                                                              std::uint32_t profile, std::uint32_t areaMask) const
{
    const Impl::Profile* p = m_Impl->GetProfile(profile);
    if (!p)
        return std::nullopt;
    const dtQueryFilter filter = m_Impl->Filter(areaMask);
    const auto          from   = m_Impl->Nearest(*p, center, glm::vec3(2.0f, 4.0f, 2.0f), filter);
    if (!from)
        return std::nullopt;
    // Detour picks a reachable polygon within the radius and a point anywhere on it: retry until
    // the point itself lies within the radius (else the start).
    t_Random = &m_Impl->random;
    std::optional<glm::vec3> result;
    for (int attempt = 0; attempt < 32 && !result; ++attempt) {
        dtPolyRef ref = 0;
        glm::vec3 point{0.0f};
        if (dtStatusFailed(p->query->findRandomPointAroundCircle(from->first, glm::value_ptr(from->second),
                                                                  std::max(radius, 0.0f), &filter, RandomFloat, &ref,
                                                                  glm::value_ptr(point))) ||
            !ref)
            break;
        if (glm::length(glm::vec2(point.x - center.x, point.z - center.z)) <= radius)
            result = point;
    }
    t_Random = nullptr;
    return result ? result : std::optional(from->second);
}

NavRaycastHit NavigationWorld::Raycast(const glm::vec3& start, const glm::vec3& end, std::uint32_t profile,
                                       std::uint32_t areaMask) const
{
    NavRaycastHit        hit;
    const Impl::Profile* p = m_Impl->GetProfile(profile);
    if (!p)
        return hit;
    const dtQueryFilter filter = m_Impl->Filter(areaMask);
    const auto          from   = m_Impl->Nearest(*p, start, glm::vec3(2.0f, 4.0f, 2.0f), filter);
    if (!from) {
        hit.hit   = true;
        hit.point = start;
        return hit;
    }
    float                                t = 0.0f;
    glm::vec3                            normal{0.0f};
    std::array<dtPolyRef, kMaxPathPolys> polys{};
    int                                  count = 0;
    if (dtStatusFailed(p->query->raycast(from->first, glm::value_ptr(from->second), glm::value_ptr(end), &filter, &t,
                                         glm::value_ptr(normal), polys.data(), &count, kMaxPathPolys)))
        return hit;
    hit.hit    = t <= 1.0f;
    hit.point  = hit.hit ? from->second + (end - from->second) * t : end;
    hit.normal = normal;
    if (count > 0) { // height on the last polygon
        float height = hit.point.y;
        if (dtStatusSucceed(p->query->getPolyHeight(polys[static_cast<std::size_t>(count - 1)],
                                                    glm::value_ptr(hit.point), &height)))
            hit.point.y = height;
    }
    return hit;
}

void NavigationWorld::SetRandomSeed(std::uint64_t seed) { m_Impl->random.seed(seed); }

// --- Agents ---

void NavigationWorld::MoveTo(Entity entity, const glm::vec3& target, float acceptanceRadius)
{
    const std::uint64_t id = static_cast<std::uint64_t>(entity);
    m_Impl->pendingMoves[id] = Impl::MoveRequest{target, acceptanceRadius};
    m_Impl->orphanMoves.erase(id);
    if (const auto it = m_Impl->agents.find(id); it != m_Impl->agents.end())
        it->second.status = NavMoveStatus::Moving;
}

void NavigationWorld::StopMovement(Entity entity)
{
    const std::uint64_t id = static_cast<std::uint64_t>(entity);
    m_Impl->pendingMoves.erase(id);
    m_Impl->orphanMoves.erase(id);
    if (const auto it = m_Impl->agents.find(id); it != m_Impl->agents.end()) {
        if (Impl::Profile* p = m_Impl->GetProfile(it->second.profile); p && it->second.index >= 0)
            p->crowd->resetMoveTarget(it->second.index);
        it->second.status = NavMoveStatus::Idle;
    }
}

NavMoveStatus NavigationWorld::MoveStatus(Entity entity) const
{
    const std::uint64_t id = static_cast<std::uint64_t>(entity);
    if (m_Impl->pendingMoves.contains(id))
        return NavMoveStatus::Moving;
    if (const auto it = m_Impl->orphanMoves.find(id); it != m_Impl->orphanMoves.end())
        return it->second;
    const auto it = m_Impl->agents.find(id);
    return it != m_Impl->agents.end() ? it->second.status : NavMoveStatus::Idle;
}

glm::vec3 NavigationWorld::AgentVelocity(Entity entity) const
{
    const auto it = m_Impl->agents.find(static_cast<std::uint64_t>(entity));
    if (it == m_Impl->agents.end() || it->second.index < 0)
        return glm::vec3(0.0f);
    const dtCrowdAgent* ag = m_Impl->profiles[it->second.profile]->crowd->getAgent(it->second.index);
    return ag && ag->active ? V3(ag->vel) : glm::vec3(0.0f);
}

bool NavigationWorld::HasAgent(Entity entity) const
{
    const auto it = m_Impl->agents.find(static_cast<std::uint64_t>(entity));
    return it != m_Impl->agents.end() && it->second.index >= 0;
}

// --- Debug ---

void NavigationWorld::ForEachTriangle(
    std::uint32_t profile,
    const std::function<void(const glm::vec3&, const glm::vec3&, const glm::vec3&, std::uint32_t)>& fn) const
{
    const Impl::Profile* p = m_Impl->GetProfile(profile);
    if (!p)
        return;
    const dtNavMesh& mesh = *p->mesh;
    for (int i = 0; i < mesh.getMaxTiles(); ++i) {
        const dtMeshTile* tile = mesh.getTile(i);
        if (!tile || !tile->header)
            continue;
        for (int j = 0; j < tile->header->polyCount; ++j) {
            const dtPoly& poly = tile->polys[j];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.vertCount < 3)
                continue;
            const auto vertex = [&](int k) { return V3(&tile->verts[poly.verts[k] * 3]); };
            const std::uint32_t area = poly.getArea() > 0 ? poly.getArea() - 1u : 0u;
            for (int k = 2; k < poly.vertCount; ++k)
                fn(vertex(0), vertex(k - 1), vertex(k), area);
        }
    }
}

void NavigationWorld::ForEachBoundaryEdge(std::uint32_t profile,
                                          const std::function<void(const glm::vec3&, const glm::vec3&)>& fn) const
{
    const Impl::Profile* p = m_Impl->GetProfile(profile);
    if (!p)
        return;
    const dtNavMesh& mesh = *p->mesh;
    for (int i = 0; i < mesh.getMaxTiles(); ++i) {
        const dtMeshTile* tile = mesh.getTile(i);
        if (!tile || !tile->header)
            continue;
        for (int j = 0; j < tile->header->polyCount; ++j) {
            const dtPoly& poly = tile->polys[j];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;
            for (int k = 0; k < poly.vertCount; ++k) {
                if (poly.neis[k] & DT_EXT_LINK) { // tile border: a wall unless linked to the neighbor
                    bool connected = false;
                    for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                        if (tile->links[l].edge == k) {
                            connected = true;
                            break;
                        }
                    if (connected)
                        continue;
                } else if (poly.neis[k] != 0) {
                    continue;
                }
                fn(V3(&tile->verts[poly.verts[k] * 3]), V3(&tile->verts[poly.verts[(k + 1) % poly.vertCount] * 3]));
            }
        }
    }
}

void NavigationWorld::ForEachTile(std::uint32_t profile,
                                  const std::function<void(const glm::vec3&, const glm::vec3&)>& fn) const
{
    const Impl::Profile* p = m_Impl->GetProfile(profile);
    if (!p)
        return;
    const dtNavMesh& mesh = *p->mesh;
    for (int i = 0; i < mesh.getMaxTiles(); ++i)
        if (const dtMeshTile* tile = mesh.getTile(i); tile && tile->header)
            fn(V3(tile->header->bmin), V3(tile->header->bmax));
}

void NavigationWorld::ForEachAgent(const std::function<void(const NavAgentDebug&)>& fn) const
{
    for (const auto& [id, a] : m_Impl->agents) {
        if (a.index < 0)
            continue;
        const Impl::Profile& p  = *m_Impl->profiles[a.profile];
        const dtCrowdAgent*  ag = p.crowd->getAgent(a.index);
        if (!ag || !ag->active)
            continue;
        NavAgentDebug d;
        d.entity   = static_cast<Entity>(id);
        d.profile  = a.profile;
        d.position = V3(ag->npos);
        d.velocity = V3(ag->vel);
        d.target   = a.target;
        d.status   = a.status;
        d.radius   = p.config.radius;
        d.height   = p.config.height;
        for (int i = 0; i < ag->ncorners; ++i)
            d.corners.push_back(V3(&ag->cornerVerts[i * 3]));
        fn(d);
    }
}

const NavStats& NavigationWorld::Stats() const { return m_Impl->stats; }

} // namespace Engine
