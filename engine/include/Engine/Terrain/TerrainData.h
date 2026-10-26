#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

// Heightmap terrain asset (.uterrain): a square height grid with splat-mapped material layers,
// a hole mask and painted foliage densities. Placed by a Terrain component: the grid is centered
// on the entity's world X/Z, height 0 at its world Y (rotation and scale are ignored).

inline constexpr std::uint32_t kMaxTerrainLayers  = 8;
inline constexpr std::uint32_t kMaxFoliageLayers  = 8;
inline constexpr std::uint32_t kMinTerrainResolution = 17;
inline constexpr std::uint32_t kMaxTerrainResolution = 4097;
inline constexpr const char*   kTerrainExtension     = ".uterrain";

// One splat-mapped material. Texture paths: absolute in memory, relative to the terrain file on disk.
struct TerrainLayer {
    std::string name = "Layer";
    std::string albedo{};          // sRGB
    std::string normal{};          // tangent space (terrain X/Z)
    std::string orm{};             // R occlusion, G roughness, B metallic (glTF convention)
    glm::vec3   tint{1.0f};
    float       tiling         = 4.0f;  // meters per texture repeat
    float       roughness      = 0.9f;  // without an ORM texture (else multiplied)
    float       metallic       = 0.0f;
    float       normalStrength = 1.0f;

    bool operator==(const TerrainLayer&) const = default;
};

enum class FoliageKind : std::uint8_t { Grass, Mesh };

// Instanced vegetation scattered where its density map is painted.
struct FoliageLayer {
    std::string  name = "Grass";
    FoliageKind  kind = FoliageKind::Grass;
    std::string  texture{};          // grass: blade card (sRGB + alpha); empty = procedural blades
    std::string  mesh{};             // mesh foliage: glTF file + mesh index
    std::uint32_t meshIndex   = 0;
    float        density      = 6.0f;   // instances per square meter where the map is full
    float        width        = 0.35f;  // grass card size (meters); meshes: scale 1
    float        height       = 0.5f;
    float        minScale     = 0.8f;   // random per instance
    float        maxScale     = 1.25f;
    glm::vec3    baseColor{0.18f, 0.32f, 0.10f};
    glm::vec3    tipColor{0.45f, 0.62f, 0.25f};
    float        wind         = 0.25f;  // sway amplitude (meters at the tip)
    float        viewDistance = 60.0f;  // instances fade out towards it
    float        maxSlope     = 40.0f;  // degrees; steeper ground stays empty
    bool         alignToNormal = true;  // meshes: up along the ground normal
    bool         castShadows  = false;
    std::uint32_t seed        = 1;
    std::vector<std::uint8_t> densityMap; // splatResolution^2, 0..255

    bool operator==(const FoliageLayer&) const = default;
};

struct TerrainData {
    std::uint32_t resolution  = 257;    // height samples per side (2^n + 1 recommended)
    float         size        = 256.0f; // meters per side
    float         heightScale = 64.0f;  // meters at height 65535
    std::vector<std::uint16_t> heights;  // resolution^2, row-major (z rows, x columns)
    std::uint32_t splatResolution = 256;
    std::vector<std::uint8_t> splat;     // splatResolution^2 x kMaxTerrainLayers weights (0..255)
    std::vector<std::uint8_t> holes;     // resolution^2, non-zero = no surface (cell to the +x/+z of the sample)
    std::vector<TerrainLayer> layers;    // <= kMaxTerrainLayers
    std::vector<FoliageLayer> foliage;   // <= kMaxFoliageLayers
    float triplanarSlope = 40.0f;        // degrees where triplanar mapping starts (90 = off)

    [[nodiscard]] float CellSize() const { return size / static_cast<float>(resolution - 1); }
    [[nodiscard]] float HeightAtSample(std::uint32_t x, std::uint32_t z) const
    {
        return static_cast<float>(heights[static_cast<std::size_t>(z) * resolution + x]) * (heightScale / 65535.0f);
    }
    [[nodiscard]] bool HoleAtSample(std::uint32_t x, std::uint32_t z) const
    {
        return !holes.empty() && holes[static_cast<std::size_t>(z) * resolution + x] != 0;
    }
};

// A new flat terrain with one default layer (all splat weight on it) and no foliage.
[[nodiscard]] TerrainData MakeFlatTerrain(std::uint32_t resolution = 257, float size = 256.0f, float heightScale = 64.0f,
                                          float height = 0.0f /* meters */);
// Resizes the arrays to the resolutions (fills new data), clamps values. Call after edits from outside.
void SanitizeTerrain(TerrainData& terrain);

// Queries in terrain space: local = (x, z) in [0, size] from the -X/-Z corner.
[[nodiscard]] float              TerrainHeight(const TerrainData& terrain, glm::vec2 local); // bilinear, meters
[[nodiscard]] glm::vec3          TerrainNormal(const TerrainData& terrain, glm::vec2 local);
[[nodiscard]] bool               TerrainHole(const TerrainData& terrain, glm::vec2 local);
// Ray against the height field (terrain space, origin y in meters above height 0): distance or nullopt.
[[nodiscard]] std::optional<float> RaycastTerrain(const TerrainData& terrain, glm::vec3 origin, glm::vec3 direction,
                                                  float maxDistance);

// File I/O (.uterrain: "UTRN" header, JSON settings, raw arrays). Load reads through the VFS.
[[nodiscard]] bool SaveTerrain(const std::filesystem::path& file, const TerrainData& terrain, std::string* error = nullptr);
[[nodiscard]] std::optional<TerrainData> LoadTerrain(const std::filesystem::path& file, std::string* error = nullptr);

// Heightmap exchange: 8/16-bit grayscale PNG or square little-endian 16-bit RAW (.raw/.r16), resampled
// to the terrain's resolution on import. Export writes 16-bit PNG or RAW at the terrain resolution.
[[nodiscard]] bool ImportHeightmap(TerrainData& terrain, const std::filesystem::path& file, std::string* error = nullptr);
[[nodiscard]] bool ExportHeightmap(const TerrainData& terrain, const std::filesystem::path& file, std::string* error = nullptr);

// --- Shared runtime copies --------------------------------------------------------------------------

// What an edit touched (TerrainStore::MarkDirty, ChangesSince).
enum TerrainChange : std::uint32_t {
    TerrainChangeHeights  = 1u << 0,
    TerrainChangeSplat    = 1u << 1,
    TerrainChangeHoles    = 1u << 2,
    TerrainChangeFoliage  = 1u << 3,
    TerrainChangeSettings = 1u << 4, // layers, foliage settings, sizes: rebuild everything derived
    TerrainChangeAll      = 0xFFu,
};

struct TerrainChanges {
    std::uint32_t kinds = 0;            // TerrainChange bits since the asked revision
    glm::vec4     rect{0.0f, 0.0f, 1.0f, 1.0f}; // normalized (u0, v0, u1, v1) union of the edits
    bool          full = false;         // everything (reload, new data, too many edits)
};

// Process-wide cache of terrain files (main thread): the renderer, physics, navigation and the
// editor share one copy per path. Editors change the data in place and report it per MarkDirty.
class TerrainStore {
public:
    static TerrainStore& Instance();

    // Loads on first use; null when missing / broken (warned once per path until it changes).
    // Files are re-checked once a second; a changed file replaces the data (full change).
    [[nodiscard]] std::shared_ptr<TerrainData> Get(const std::string& path);
    // Unsaved data under a path (new terrain in the editor); replaces a loaded copy.
    void Provide(const std::string& path, std::shared_ptr<TerrainData> data);
    void MarkDirty(const std::string& path, std::uint32_t kinds, glm::vec4 rect = {0.0f, 0.0f, 1.0f, 1.0f});
    [[nodiscard]] std::uint64_t  Revision(const std::string& path) const; // 0: unknown
    [[nodiscard]] TerrainChanges ChangesSince(const std::string& path, std::uint64_t revision) const;
    [[nodiscard]] bool           Save(const std::string& path, std::string* error = nullptr);
    [[nodiscard]] bool           Dirty(const std::string& path) const; // edited since load / save
    void                         Reload(const std::string& path);     // drops edits
    void                         Clear();                              // tests, project switch

private:
    struct Edit {
        std::uint64_t revision;
        std::uint32_t kinds;
        glm::vec4     rect;
    };
    struct Entry {
        std::shared_ptr<TerrainData>    data;
        std::uint64_t                   revision     = 0;
        std::uint64_t                   fullRevision = 0; // last full change
        std::vector<Edit>               edits;            // since fullRevision, capped
        std::filesystem::file_time_type mtime{};
        double                          lastCheck = 0.0;
        bool                            dirty     = false;
        bool                            warned    = false;
    };
    void Bump(Entry& entry, bool full, std::uint32_t kinds, glm::vec4 rect);

    std::vector<std::pair<std::string, Entry>> m_Entries;
    std::uint64_t                              m_NextRevision = 1;
};

} // namespace Engine
