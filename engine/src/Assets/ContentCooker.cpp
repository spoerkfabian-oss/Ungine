#include "Engine/Assets/ContentCooker.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"

#include "../Scene/SceneJson.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

constexpr char          kModelMagic[4] = {'U', 'M', 'D', 'L'};
constexpr std::uint32_t kModelVersion  = 1; // bump when the layout below changes

std::string GenericUtf8(const fs::path& path)
{
    const std::u8string s = path.generic_u8string();
    return {s.begin(), s.end()};
}

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Extension(const fs::path& path) { return Lower(PathToUtf8(path.extension())); }

class Writer {
public:
    std::vector<std::byte> out;

    template <class T>
    void Pod(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const std::byte*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void U8(std::uint8_t v) { Pod(v); }
    void U32(std::uint32_t v) { Pod(v); }
    void I32(std::int32_t v) { Pod(v); }
    void F32(float v) { Pod(v); }
    void U64(std::uint64_t v) { Pod(v); }
    void Bool(bool v) { U8(v ? 1 : 0); }
    void String(const std::string& s)
    {
        U64(s.size());
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        out.insert(out.end(), p, p + s.size());
    }
    template <class T>
    void Array(const std::vector<T>& values) // element types without padding (checked by size)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        U64(values.size());
        const auto* p = reinterpret_cast<const std::byte*>(values.data());
        out.insert(out.end(), p, p + values.size() * sizeof(T));
    }
    void Transform(const Engine::Transform& t)
    {
        Pod(t.position);
        Pod(t.rotation);
        Pod(t.scale);
    }
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : m_In(bytes) {}

    template <class T>
    T Pod()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        Need(sizeof(T));
        T value;
        std::memcpy(&value, m_In.data() + m_At, sizeof(T));
        m_At += sizeof(T);
        return value;
    }
    std::uint8_t  U8() { return Pod<std::uint8_t>(); }
    std::uint32_t U32() { return Pod<std::uint32_t>(); }
    std::int32_t  I32() { return Pod<std::int32_t>(); }
    float         F32() { return Pod<float>(); }
    std::uint64_t U64() { return Pod<std::uint64_t>(); }
    bool          Bool() { return U8() != 0; }
    std::size_t   Count(std::size_t elementSize)
    {
        const std::uint64_t n = U64();
        if (elementSize > 0 && n > (m_In.size() - m_At) / elementSize)
            throw std::runtime_error("cooked model: bad element count");
        return static_cast<std::size_t>(n);
    }
    std::string String()
    {
        const std::size_t n = Count(1);
        std::string       s(reinterpret_cast<const char*>(m_In.data() + m_At), n);
        m_At += n;
        return s;
    }
    template <class T>
    std::vector<T> Array()
    {
        const std::size_t n = Count(sizeof(T));
        std::vector<T>    values(n);
        if (n > 0) // memcpy from / to null is undefined even for 0 bytes
            std::memcpy(values.data(), m_In.data() + m_At, n * sizeof(T));
        m_At += n * sizeof(T);
        return values;
    }
    Engine::Transform Transform()
    {
        Engine::Transform t;
        t.position = Pod<glm::vec3>();
        t.rotation = Pod<glm::quat>();
        t.scale    = Pod<glm::vec3>();
        return t;
    }
    [[nodiscard]] bool AtEnd() const { return m_At == m_In.size(); }

private:
    void Need(std::size_t n) const
    {
        if (n > m_In.size() - m_At)
            throw std::runtime_error("cooked model: truncated");
    }
    std::span<const std::byte> m_In;
    std::size_t                m_At = 0;
};

// Layouts written as raw arrays must not contain padding (uninitialized bytes would make the
// output depend on the stack).
static_assert(sizeof(Vertex) == 80);
static_assert(sizeof(Submesh) == 3 * 4 + 4 + 2 * 12 + 4 + kMaxLods * 12);
static_assert(sizeof(VertexSkinInfluence) == 24);
static_assert(sizeof(SkinJoint) == 4 + 64);

std::uint64_t SettingsHash(const MeshOptimizeSettings& s)
{
    Writer w;
    w.Bool(s.optimize);
    w.U32(s.lodCount);
    w.F32(s.lodRatio);
    w.U32(s.minLodTriangles);
    w.F32(s.maxLodError);
    return Fnv1a64(w.out);
}

void WriteLight(Writer& w, const Light& l)
{
    w.U8(static_cast<std::uint8_t>(l.type));
    w.Pod(l.color);
    w.F32(l.intensity);
    w.F32(l.range);
    w.F32(l.innerConeAngle);
    w.F32(l.outerConeAngle);
    w.Bool(l.castShadows);
}

Light ReadLight(Reader& r)
{
    Light l;
    l.type           = static_cast<LightType>(r.U8());
    l.color          = r.Pod<glm::vec3>();
    l.intensity      = r.F32();
    l.range          = r.F32();
    l.innerConeAngle = r.F32();
    l.outerConeAngle = r.F32();
    l.castShadows    = r.Bool();
    return l;
}

// Paths relative to the model's directory ('/'-separated); absolute when that is not possible.
std::string StorePath(const fs::path& path, const fs::path& modelDir)
{
    const fs::path relative = path.lexically_normal().lexically_relative(modelDir);
    return GenericUtf8(relative.empty() ? path : relative);
}

fs::path LoadPath(const std::string& stored, const fs::path& modelDir)
{
    const fs::path p = PathFromUtf8(stored);
    return (p.is_relative() ? modelDir / p : p).lexically_normal();
}

} // namespace

// --- Cooked models -----------------------------------------------------------------------------------

std::vector<std::byte> SerializeModelData(const ModelData& d, const fs::path& modelFile, const MeshOptimizeSettings& settings)
{
    const fs::path modelDir = fs::absolute(modelFile).lexically_normal().parent_path();
    Writer         w;
    w.out.insert(w.out.end(), reinterpret_cast<const std::byte*>(kModelMagic),
                 reinterpret_cast<const std::byte*>(kModelMagic) + sizeof(kModelMagic));
    w.U32(kModelVersion);
    w.U64(SettingsHash(settings));
    w.String(d.name);
    w.Array(d.vertices);
    w.Array(d.indices);
    w.U64(d.textures.size());
    for (const TextureData& t : d.textures) {
        w.String(t.name);
        w.U8(static_cast<std::uint8_t>(t.kind));
        w.String(t.file.empty() ? std::string() : StorePath(t.file, modelDir));
        w.Array(t.encoded);
        w.U64(t.hash);
    }
    w.U64(d.materials.size());
    for (const MaterialData& m : d.materials) {
        w.String(m.name);
        w.Pod(m.baseColorFactor);
        w.Pod(m.emissiveFactor);
        w.F32(m.metallic);
        w.F32(m.roughness);
        w.F32(m.alphaCutoff);
        w.F32(m.normalScale);
        w.F32(m.occlusionStrength);
        w.Bool(m.alphaMask);
        w.Bool(m.alphaBlend);
        w.Bool(m.doubleSided);
        for (const std::int32_t t : {m.baseColorTexture, m.normalTexture, m.metallicRoughnessTexture, m.emissiveTexture,
                                     m.occlusionTexture})
            w.I32(t);
    }
    w.U64(d.meshes.size());
    for (const Mesh& m : d.meshes) {
        w.String(m.name);
        w.Array(m.submeshes);
    }
    w.U64(d.nodes.size());
    for (const ModelNode& n : d.nodes) {
        w.String(n.name);
        w.Transform(n.local);
        w.I32(n.mesh);
        w.I32(n.parent);
        w.I32(n.skin);
        w.Bool(n.light.has_value());
        if (n.light)
            WriteLight(w, *n.light);
    }
    w.Array(d.skinInfluences);
    w.U64(d.skins.size());
    for (const Skin& s : d.skins) {
        w.String(s.name);
        w.I32(s.skeletonRoot);
        w.Array(s.joints);
    }
    w.U64(d.animations.size());
    for (const AnimationClip& a : d.animations) {
        w.String(a.name);
        w.F32(a.duration);
        w.U64(a.tracks.size());
        for (const AnimationTrack& t : a.tracks) {
            w.U32(t.node);
            w.U8(static_cast<std::uint8_t>(t.path));
            w.U8(static_cast<std::uint8_t>(t.interpolation));
            w.Array(t.times);
            w.Array(t.values);
            w.Array(t.inTangents);
            w.Array(t.outTangents);
        }
    }
    w.Pod(d.boundsMin);
    w.Pod(d.boundsMax);
    w.U64(d.dependencies.size());
    for (const fs::path& p : d.dependencies)
        w.String(StorePath(p, modelDir));
    return std::move(w.out);
}

ModelData DeserializeModelData(std::span<const std::byte> bytes, const fs::path& modelFile,
                               const MeshOptimizeSettings& settings, bool* settingsMatch)
{
    if (bytes.size() < sizeof(kModelMagic) || std::memcmp(bytes.data(), kModelMagic, sizeof(kModelMagic)) != 0)
        throw std::runtime_error("not a cooked model");
    Reader r(bytes.subspan(sizeof(kModelMagic)));
    if (const std::uint32_t version = r.U32(); version != kModelVersion)
        throw std::runtime_error(std::format("cooked model version {} (expected {})", version, kModelVersion));
    const bool match = r.U64() == SettingsHash(settings);
    if (settingsMatch)
        *settingsMatch = match;

    const fs::path modelDir = fs::absolute(modelFile).lexically_normal().parent_path();
    ModelData      d;
    d.name     = r.String();
    d.vertices = r.Array<Vertex>();
    d.indices  = r.Array<std::uint32_t>();
    d.textures.resize(r.Count(1));
    for (TextureData& t : d.textures) {
        t.name                   = r.String();
        t.kind                   = static_cast<TextureKind>(r.U8());
        const std::string stored = r.String();
        if (!stored.empty())
            t.file = LoadPath(stored, modelDir);
        t.encoded = r.Array<std::byte>();
        t.hash    = r.U64();
    }
    d.materials.resize(r.Count(1));
    for (MaterialData& m : d.materials) {
        m.name              = r.String();
        m.baseColorFactor   = r.Pod<glm::vec4>();
        m.emissiveFactor    = r.Pod<glm::vec3>();
        m.metallic          = r.F32();
        m.roughness         = r.F32();
        m.alphaCutoff       = r.F32();
        m.normalScale       = r.F32();
        m.occlusionStrength = r.F32();
        m.alphaMask         = r.Bool();
        m.alphaBlend        = r.Bool();
        m.doubleSided       = r.Bool();
        for (std::int32_t* t : {&m.baseColorTexture, &m.normalTexture, &m.metallicRoughnessTexture, &m.emissiveTexture,
                                &m.occlusionTexture})
            *t = r.I32();
    }
    d.meshes.resize(r.Count(1));
    for (Mesh& m : d.meshes) {
        m.name      = r.String();
        m.submeshes = r.Array<Submesh>();
    }
    d.nodes.resize(r.Count(1));
    for (ModelNode& n : d.nodes) {
        n.name   = r.String();
        n.local  = r.Transform();
        n.mesh   = r.I32();
        n.parent = r.I32();
        n.skin   = r.I32();
        if (r.Bool())
            n.light = ReadLight(r);
    }
    d.skinInfluences = r.Array<VertexSkinInfluence>();
    d.skins.resize(r.Count(1));
    for (Skin& s : d.skins) {
        s.name         = r.String();
        s.skeletonRoot = r.I32();
        s.joints       = r.Array<SkinJoint>();
    }
    d.animations.resize(r.Count(1));
    for (AnimationClip& a : d.animations) {
        a.name     = r.String();
        a.duration = r.F32();
        a.tracks.resize(r.Count(1));
        for (AnimationTrack& t : a.tracks) {
            t.node          = r.U32();
            t.path          = static_cast<AnimationPath>(r.U8());
            t.interpolation = static_cast<AnimationInterpolation>(r.U8());
            t.times         = r.Array<float>();
            t.values        = r.Array<glm::vec4>();
            t.inTangents    = r.Array<glm::vec4>();
            t.outTangents   = r.Array<glm::vec4>();
        }
    }
    d.boundsMin = r.Pod<glm::vec3>();
    d.boundsMax = r.Pod<glm::vec3>();
    d.dependencies.resize(r.Count(1));
    for (fs::path& p : d.dependencies)
        p = LoadPath(r.String(), modelDir);
    if (!r.AtEnd())
        throw std::runtime_error("cooked model: trailing data");
    d.optimized = true;
    return d;
}

std::optional<ModelData> LoadCookedModel(const fs::path& file, const MeshOptimizeSettings& settings)
{
    const std::optional<std::vector<std::byte>> bytes = Vfs::ReadCookedFor(file, "models");
    if (!bytes)
        return std::nullopt;
    bool      match = true;
    ModelData data  = DeserializeModelData(*bytes, file, settings, &match);
    if (!match)
        ENGINE_WARN("Cooked model '{}' was optimized with other mesh settings (used anyway)", PathToUtf8(file));
    return data;
}

// --- Report ------------------------------------------------------------------------------------------

std::string CookReport::Text() const
{
    std::string out = "Ungine build report\n===================\n\n";
    out += std::format("Result: {}\n", Ok() ? "OK" : "FAILED");
    out += std::format("Raw files: {} ({:.2f} MB); replaced by cooked models: {}\n", rawFiles, rawBytes / 1048576.0,
                       replacedFiles);
    out += std::format("Cooked: {} models, {} textures ({:.2f} MB)\n", cookedModels, cookedTextures, cookedBytes / 1048576.0);
    out += std::format("Pak: {:.2f} MB, {} entries, {:.1f} s\n", pakBytes / 1048576.0, entries.size(), seconds);
    if (!errors.empty()) {
        out += "\nErrors:\n";
        for (const std::string& e : errors)
            out += "  - " + e + "\n";
    }
    if (!warnings.empty()) {
        out += "\nWarnings:\n";
        for (const std::string& w : warnings)
            out += "  - " + w + "\n";
    }
    out += "\nEntries:\n";
    for (const Entry& e : entries)
        out += std::format("  {:<8} {:>12}  {}\n", e.kind, e.bytes, e.path);
    return out;
}

// --- Content cooking ---------------------------------------------------------------------------------

namespace {

struct TextureJob {
    std::shared_ptr<const std::vector<std::byte>> source;
    TextureKind                                   kind = TextureKind::Color;
    std::string                                   what; // for messages
};

bool IsModelFile(const fs::path& p)
{
    const std::string e = Extension(p);
    return e == ".gltf" || e == ".glb";
}

bool IsImageFile(const fs::path& p)
{
    const std::string e = Extension(p);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".ktx2";
}

bool IsSceneLike(const fs::path& p)
{
    const std::string name = Lower(PathToUtf8(p.filename()));
    return name.ends_with(".json") || name.ends_with(".uprefab");
}

} // namespace

CookReport CookProjectContent(const Project& project, const fs::path& pakFile, const CookOptions& options)
{
    const auto  start = std::chrono::steady_clock::now();
    CookReport  report;
    const auto  progress = [&](float value, const std::string& step) {
        if (options.progress)
            options.progress(value, step);
    };
    const fs::path root    = fs::absolute(project.Root()).lexically_normal();
    const fs::path content = fs::absolute(project.ContentDirectory()).lexically_normal();
    const auto     entryPath = [&](const fs::path& file) { return GenericUtf8(file.lexically_relative(root)); };
    const auto     inContent = [&](const fs::path& file) {
        const fs::path relative = file.lexically_relative(content);
        return !relative.empty() && !GenericUtf8(relative).starts_with("..");
    };

    // Every regular file below Content/ (sorted: the pak does not depend on directory order).
    std::vector<fs::path> files;
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(content, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e;
            if (!it->is_regular_file(e))
                continue;
            const std::string name = PathToUtf8(it->path().filename());
            if (name.starts_with(".") || name.ends_with(".tmp"))
                continue;
            files.push_back(it->path().lexically_normal());
        }
        if (ec)
            report.errors.push_back("cannot list '" + PathToUtf8(content) + "': " + ec.message());
    }
    std::ranges::sort(files);

    std::map<std::string, std::vector<std::byte>> cooked;   // pak entry -> bytes (sorted)
    std::map<std::string, TextureJob>             textures; // cook key -> job (sorted)
    std::set<fs::path>                            replaced; // raw model files left out of the pak
    std::set<std::string>                         usedImages;
    const auto addTexture = [&](std::shared_ptr<const std::vector<std::byte>> bytes, TextureKind kind, std::string what) {
        if (!bytes || bytes->empty() || IsKtx2(*bytes)) // KTX2 is loaded as it is
            return;
        const std::string key = TextureCookKey(*bytes, kind, options.textures);
        textures.try_emplace(key, TextureJob{std::move(bytes), kind, std::move(what)});
    };
    std::map<fs::path, std::shared_ptr<const std::vector<std::byte>>> fileBytes;
    const auto readShared = [&](const fs::path& file) -> std::shared_ptr<const std::vector<std::byte>> {
        if (const auto it = fileBytes.find(file); it != fileBytes.end())
            return it->second;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        if (std::optional<std::vector<std::byte>> b = Vfs::Read(file))
            bytes = std::make_shared<const std::vector<std::byte>>(std::move(*b));
        fileBytes.emplace(file, bytes);
        return bytes;
    };

    // Models: parsed + optimized once here, their textures cooked in the kinds they are used as.
    std::size_t done = 0;
    for (const fs::path& file : files) {
        ++done;
        if (!IsModelFile(file))
            continue;
        progress(0.4f * static_cast<float>(done) / static_cast<float>(files.size()), "Cooking " + PathToUtf8(file.filename()));
        try {
            ModelData data = LoadGltf(file);
            OptimizeMeshes(data, options.meshes);
            for (const TextureData& t : data.textures) {
                if (!t.file.empty()) {
                    const fs::path image = t.file.lexically_normal();
                    if (const auto bytes = readShared(image)) {
                        usedImages.insert(PathToUtf8(image));
                        addTexture(bytes, t.kind, PathToUtf8(image.filename()));
                        // The game finds the cooked texture by its source bytes: the image must ship too.
                        if (!inContent(image))
                            report.warnings.push_back(std::format("'{}': texture '{}' is outside Content/ and not packaged",
                                                                  entryPath(file), PathToUtf8(image)));
                    } else {
                        report.warnings.push_back(std::format("'{}': texture '{}' is missing", entryPath(file), PathToUtf8(image)));
                    }
                } else {
                    addTexture(std::make_shared<const std::vector<std::byte>>(t.encoded), t.kind,
                               PathToUtf8(file.filename()) + ": " + t.name);
                }
            }
            cooked[std::string(kCookedPrefix) + "models/" + entryPath(file)] = SerializeModelData(data, file, options.meshes);
            ++report.cookedModels;
            replaced.insert(file);
            for (const fs::path& dependency : data.dependencies)
                replaced.insert(dependency.lexically_normal());
        } catch (const std::exception& e) {
            report.errors.push_back(std::format("'{}': {}", entryPath(file), e.what()));
        }
    }

    // Scenes and prefabs: UI images are color textures; references to missing files are reported.
    for (const fs::path& file : files) {
        if (!IsSceneLike(file))
            continue;
        const std::optional<std::string> text = Vfs::ReadText(file);
        json                             doc;
        try {
            doc = json::parse(text.value_or(std::string()));
        } catch (const std::exception&) {
            continue; // not a scene (other JSON content)
        }
        const auto list = doc.find("entities");
        if (!doc.is_object() || list == doc.end() || !list->is_array())
            continue;
        const fs::path dir   = file.parent_path();
        const auto     check = [&](const std::string& reference, bool image) {
            const fs::path target = (PathFromUtf8(reference).is_relative() ? dir / PathFromUtf8(reference) : PathFromUtf8(reference))
                                        .lexically_normal();
            std::error_code ec;
            const bool      inside = inContent(target);
            if (!fs::is_regular_file(target, ec))
                report.warnings.push_back(std::format("'{}': missing file '{}'", entryPath(file), reference));
            else if (!inside)
                report.warnings.push_back(std::format("'{}': '{}' is outside Content/ and not packaged", entryPath(file), reference));
            else if (image) {
                usedImages.insert(PathToUtf8(target));
                addTexture(readShared(target), TextureKind::Color, PathToUtf8(target.filename()));
            }
        };
        for (json entity : *list) {
            if (!entity.is_object())
                continue;
            if (const auto ui = entity.find("uiWidget"); ui != entity.end() && ui->is_object())
                if (const auto image = ui->find("image"); image != ui->end() && image->is_string() && !image->get<std::string>().empty()) {
                    check(image->get<std::string>(), true);
                    ui->erase("image"); // checked: TransformPaths below skips it
                }
            SceneJson::TransformPaths(entity, [&](const std::string& p) {
                check(p, false);
                return p;
            });
            if (const auto prefab = entity.find("prefab"); prefab != entity.end() && prefab->is_object())
                if (const auto f = prefab->find("file"); f != prefab->end() && f->is_string() && !f->get<std::string>().empty())
                    check(f->get<std::string>(), false);
        }
    }

    // Other images: cooked as color (UI images used from blueprints).
    for (const fs::path& file : files)
        if (IsImageFile(file) && !usedImages.contains(PathToUtf8(file))) {
            addTexture(readShared(file), TextureKind::Color, PathToUtf8(file.filename()));
            report.warnings.push_back(std::format("'{}': not used by a model or scene, cooked as a color texture", entryPath(file)));
        }

    // Texture encoding in parallel (BC7 is slow).
    {
        std::vector<const std::pair<const std::string, TextureJob>*> jobs;
        for (const auto& job : textures)
            jobs.push_back(&job);
        std::vector<std::vector<std::byte>> results(jobs.size());
        std::vector<std::string>            failures(jobs.size());
        std::atomic<std::size_t>            next{0}, finished{0};
        const std::uint32_t threads = std::max(1u, options.threads ? options.threads : std::thread::hardware_concurrency());
        std::mutex progressMutex;
        const auto work = [&] {
            for (std::size_t i = next++; i < jobs.size(); i = next++) {
                const TextureJob& job = jobs[i]->second;
                try {
                    const CookResult r = CookTexture(*job.source, job.kind, options.textures);
                    results[i]         = WriteKtx2(*r.image);
                } catch (const std::exception& e) {
                    failures[i] = std::format("texture '{}': {}", job.what, e.what());
                }
                const std::size_t n = ++finished;
                std::scoped_lock  lock(progressMutex);
                progress(0.4f + 0.5f * static_cast<float>(n) / static_cast<float>(jobs.size()), "Encoding " + job.what);
            }
        };
        std::vector<std::jthread> pool;
        for (std::uint32_t t = 1; t < std::min<std::uint32_t>(threads, static_cast<std::uint32_t>(jobs.size())); ++t)
            pool.emplace_back(work);
        work();
        pool.clear();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (!failures[i].empty()) {
                report.errors.push_back(failures[i]);
                continue;
            }
            cooked[std::string(kCookedPrefix) + "textures/" + jobs[i]->first + ".ktx2"] = std::move(results[i]);
            ++report.cookedTextures;
        }
    }

    // The pak: raw files (except replaced model sources), then the cooked data.
    progress(0.9f, "Writing " + PathToUtf8(pakFile.filename()));
    try {
        PakWriter writer(pakFile);
        for (const fs::path& file : files) {
            if (replaced.contains(file)) {
                ++report.replacedFiles;
                continue;
            }
            std::shared_ptr<const std::vector<std::byte>> bytes; // read now: raw files are not kept in memory
            if (const auto it = fileBytes.find(file); it != fileBytes.end()) {
                bytes = std::move(it->second);
                fileBytes.erase(it);
            } else if (std::optional<std::vector<std::byte>> b = Vfs::Read(file)) {
                bytes = std::make_shared<const std::vector<std::byte>>(std::move(*b));
            }
            if (!bytes) {
                report.errors.push_back("cannot read '" + entryPath(file) + "'");
                continue;
            }
            const std::string path = entryPath(file);
            writer.Add(path, *bytes);
            report.entries.push_back({path, bytes->size(), "raw"});
            ++report.rawFiles;
            report.rawBytes += bytes->size();
        }
        for (const auto& [path, bytes] : cooked) {
            writer.Add(path, bytes);
            report.entries.push_back({path, bytes.size(), path.starts_with(std::string(kCookedPrefix) + "models/") ? "model" : "texture"});
            report.cookedBytes += bytes.size();
        }
        writer.Finish();
        report.pakBytes = writer.BytesWritten();
    } catch (const std::exception& e) {
        report.errors.push_back(e.what());
    }
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    progress(1.0f, "Done");
    return report;
}

} // namespace Engine
