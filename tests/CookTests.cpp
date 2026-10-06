// Cooked builds (CPU): the pak format and the virtual file system, cooked models, cooking a
// project into a pak that a "packaged game" reads, save game versioning and the user's options.
#include "BlueprintTestUtil.h"
#include "Test.h"

#include "Engine/Assets/ContentCooker.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Assets/TextureCooker.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/GameOptions.h"
#include "Engine/Core/Input.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptRegistry.h"

#include "../apps/player/OptionsMenu.h"

#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / std::format("ungine_{}_{}", name, std::random_device{}());
    fs::create_directories(dir);
    return dir;
}

std::vector<std::byte> Bytes(std::string_view text)
{
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return {p, p + text.size()};
}

void WriteBytes(const fs::path& file, std::span<const std::byte> bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> ReadAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    const std::string s{std::istreambuf_iterator<char>(in), {}};
    return Bytes(s);
}

std::vector<std::byte> SolidPng(std::uint8_t r, std::uint8_t g, std::uint8_t b, int size = 16)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size * size * 4));
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i]     = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    std::vector<std::byte> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int n) {
            auto* out = static_cast<std::vector<std::byte>*>(context);
            out->insert(out->end(), static_cast<std::byte*>(data), static_cast<std::byte*>(data) + n);
        },
        &png, size, size, 4, rgba.data(), size * 4);
    return png;
}

// A triangle .gltf with an external buffer and an external base color texture "tex.png".
void WriteTexturedTriangle(const fs::path& dir)
{
    const float         data[]    = {0, 0, 0, 1, 0, 0, 0, 1, 0, /*uv*/ 0, 0, 1, 0, 0, 1};
    const std::uint16_t indices[] = {0, 1, 2};
    std::vector<std::byte> bin(sizeof(data) + sizeof(indices));
    std::memcpy(bin.data(), data, sizeof(data));
    std::memcpy(bin.data() + sizeof(data), indices, sizeof(indices));
    WriteBytes(dir / "tri.bin", bin);
    WriteBytes(dir / "tex.png", SolidPng(200, 40, 40));
    std::ofstream(dir / "tri.gltf") << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "Tri"}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
  "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
  "textures": [{"source": 0}], "images": [{"uri": "tex.png"}],
  "buffers": [{"byteLength": 66, "uri": "tri.bin"}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 24}, {"buffer": 0, "byteOffset": 60, "byteLength": 6}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
                {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]})";
}

template <class T>
bool SameBytes(const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool SameModel(const ModelData& a, const ModelData& b)
{
    bool same = a.name == b.name && SameBytes(a.vertices, b.vertices) && SameBytes(a.indices, b.indices) &&
                SameBytes(a.skinInfluences, b.skinInfluences) && a.textures.size() == b.textures.size() &&
                a.materials.size() == b.materials.size() && a.meshes.size() == b.meshes.size() &&
                a.nodes.size() == b.nodes.size() && a.skins.size() == b.skins.size() &&
                a.animations.size() == b.animations.size() && a.boundsMin == b.boundsMin && a.boundsMax == b.boundsMax;
    for (std::size_t i = 0; same && i < a.textures.size(); ++i)
        same = a.textures[i].name == b.textures[i].name && a.textures[i].kind == b.textures[i].kind &&
               a.textures[i].file == b.textures[i].file && a.textures[i].encoded == b.textures[i].encoded &&
               a.textures[i].hash == b.textures[i].hash;
    for (std::size_t i = 0; same && i < a.materials.size(); ++i) {
        const MaterialData &x = a.materials[i], &y = b.materials[i];
        same = x.name == y.name && x.baseColorFactor == y.baseColorFactor && x.emissiveFactor == y.emissiveFactor &&
               x.metallic == y.metallic && x.roughness == y.roughness && x.alphaMask == y.alphaMask &&
               x.alphaBlend == y.alphaBlend && x.doubleSided == y.doubleSided && x.baseColorTexture == y.baseColorTexture &&
               x.normalTexture == y.normalTexture && x.metallicRoughnessTexture == y.metallicRoughnessTexture;
    }
    for (std::size_t i = 0; same && i < a.meshes.size(); ++i)
        same = a.meshes[i].name == b.meshes[i].name && SameBytes(a.meshes[i].submeshes, b.meshes[i].submeshes);
    for (std::size_t i = 0; same && i < a.nodes.size(); ++i) {
        const ModelNode &x = a.nodes[i], &y = b.nodes[i];
        same = x.name == y.name && x.local == y.local && x.mesh == y.mesh && x.parent == y.parent && x.skin == y.skin &&
               x.light.has_value() == y.light.has_value();
    }
    for (std::size_t i = 0; same && i < a.skins.size(); ++i)
        same = a.skins[i].name == b.skins[i].name && a.skins[i].skeletonRoot == b.skins[i].skeletonRoot &&
               SameBytes(a.skins[i].joints, b.skins[i].joints);
    for (std::size_t i = 0; same && i < a.animations.size(); ++i) {
        const AnimationClip &x = a.animations[i], &y = b.animations[i];
        same = x.name == y.name && x.duration == y.duration && x.tracks.size() == y.tracks.size();
        for (std::size_t t = 0; same && t < x.tracks.size(); ++t)
            same = x.tracks[t].node == y.tracks[t].node && x.tracks[t].path == y.tracks[t].path &&
                   x.tracks[t].interpolation == y.tracks[t].interpolation && x.tracks[t].times == y.tracks[t].times &&
                   SameBytes(x.tracks[t].values, y.tracks[t].values);
    }
    return same;
}

} // namespace

TEST_CASE(Pak_WriteReadMountAndCorruption)
{
    const fs::path dir = TempDir("pak");
    const fs::path pakFile = dir / "Content.upak";
    {
        PakWriter writer(pakFile);
        writer.Add("Content/a.txt", Bytes("hello"));
        writer.Add("Content/Sub/b.bin", Bytes("binary data"));
        writer.Add(std::string(kCookedPrefix) + "things/x", Bytes("cooked"));
        bool duplicate = false;
        try {
            writer.Add("Content/a.txt", Bytes("again"));
        } catch (const std::invalid_argument&) {
            duplicate = true;
        }
        CHECK(duplicate);
        writer.Finish();
    }
    {
        PakWriter unfinished(dir / "Broken.upak"); // dropped without Finish: no file left behind
        unfinished.Add("x", Bytes("y"));
    }
    CHECK(!fs::exists(dir / "Broken.upak"));

    const auto pak = PakFile::Open(pakFile);
    CHECK(pak->Entries().size() == 3 && pak->Find("Content/a.txt") && !pak->Find("Content/missing"));
    CHECK(pak->Read(*pak->Find("Content/Sub/b.bin")) == Bytes("binary data"));

    // Mounted at a root: files below it come from the pak, the disk still works around it.
    const fs::path root = dir / "Game";
    WriteBytes(root / "Content" / "disk.txt", Bytes("on disk"));
    CHECK(!Vfs::Mounted());
    Vfs::Mount(pak, root);
    CHECK(Vfs::Mounted());
    CHECK(Vfs::Read(root / "Content" / "a.txt") == Bytes("hello"));
    CHECK(Vfs::Read(root / "Content" / ".." / "Content" / "Sub" / "b.bin") == Bytes("binary data"));
    CHECK(Vfs::Read(root / "Content" / "disk.txt") == Bytes("on disk"));
    CHECK(!Vfs::Read(root / "Content" / "missing.txt"));
    CHECK(Vfs::Exists(root / "Content" / "a.txt") && Vfs::IsPacked(root / "Content" / "a.txt") &&
          !Vfs::IsPacked(root / "Content" / "disk.txt"));
    const auto location = Vfs::Locate(root / "Content" / "a.txt");
    CHECK(location && location->packed && location->size == 5 && location->file == pak->File());
    CHECK(Vfs::ModifiedTime(root / "Content" / "a.txt") != fs::file_time_type::min());
    const std::vector<fs::path> direct = Vfs::ListFiles(root / "Content", false);
    const std::vector<fs::path> all    = Vfs::ListFiles(root / "Content", true);
    CHECK(direct.size() == 2 && all.size() == 3); // a.txt + disk.txt; + Sub/b.bin
    CHECK(Vfs::ReadCooked("things/x") == Bytes("cooked") && !Vfs::ReadCooked("things/y"));
    CHECK(Vfs::ReadCookedFor(root / "Content" / "a.txt", "none") == std::nullopt);
    CHECK(!Vfs::Exists(root / ":cooked" / "things" / "x")); // cooked data is not a file
    Vfs::UnmountAll();
    CHECK(!Vfs::Read(root / "Content" / "a.txt"));

    // Corruption: a changed entry fails its hash check, a changed index the whole pak.
    std::vector<std::byte> bytes = ReadAll(pakFile);
    const PakEntryInfo     entry = *pak->Find("Content/a.txt");
    bytes[entry.offset] ^= std::byte{0x20};
    WriteBytes(dir / "Corrupt.upak", bytes);
    const auto corrupt = PakFile::Open(dir / "Corrupt.upak");
    bool       mismatch = false;
    try {
        (void)corrupt->Read(*corrupt->Find("Content/a.txt"));
    } catch (const std::runtime_error& e) {
        mismatch = std::string(e.what()).find("corrupt") != std::string::npos;
    }
    CHECK(mismatch);
    CHECK(corrupt->Read(*corrupt->Find("Content/Sub/b.bin")) == Bytes("binary data"));
    bytes = ReadAll(pakFile);
    bytes[bytes.size() - 3] ^= std::byte{0x01}; // inside the index
    WriteBytes(dir / "BadIndex.upak", bytes);
    bool rejected = false;
    try {
        (void)PakFile::Open(dir / "BadIndex.upak");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Cook_ModelDataRoundTrip)
{
    // Embedded textures (GLB), a skin with animations (the Basic template's banner) and an
    // external texture whose path moves with the model.
    const fs::path             dir = TempDir("cook_model");
    const MeshOptimizeSettings settings{};
    for (const fs::path& file : {fs::path(ENGINE_ASSET_DIR) / "models" / "BoxTextured.glb",
                                 fs::path(ENGINE_TEST_SOURCE_DIR) / "templates" / "Basic" / "Content" / "Models" / "AnimatedBanner.gltf"}) {
        ModelData data = LoadGltf(file);
        OptimizeMeshes(data, settings);
        CHECK(data.optimized);
        const std::vector<std::byte> bytes = SerializeModelData(data, file, settings);
        CHECK(SerializeModelData(data, file, settings) == bytes); // deterministic
        bool            match = false;
        const ModelData back  = DeserializeModelData(bytes, file, settings, &match);
        CHECK(match && back.optimized && SameModel(data, back));
        MeshOptimizeSettings other = settings;
        other.lodCount             = 2;
        (void)DeserializeModelData(bytes, file, other, &match);
        CHECK(!match);
        bool truncated = false;
        try {
            (void)DeserializeModelData(std::span(bytes).first(bytes.size() - 5), file, settings);
        } catch (const std::runtime_error&) {
            truncated = true;
        }
        CHECK(truncated);
    }
    WriteTexturedTriangle(dir / "A");
    ModelData tri = LoadGltf(dir / "A" / "tri.gltf");
    CHECK(tri.textures.size() == 1 && tri.textures[0].file == (dir / "A" / "tex.png").lexically_normal());
    const std::vector<std::byte> bytes = SerializeModelData(tri, dir / "A" / "tri.gltf", settings);
    const ModelData moved = DeserializeModelData(bytes, dir / "B" / "tri.gltf", settings);
    CHECK(moved.textures.size() == 1 && moved.textures[0].file == (dir / "B" / "tex.png").lexically_normal());
    CHECK(moved.dependencies.size() == 1 && moved.dependencies[0] == (dir / "B" / "tri.bin").lexically_normal());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Cook_ProjectContentIntoPakAndPackagedView)
{
    const fs::path dir = TempDir("cook_project");
    std::string    error;
    std::optional<Project> project;
    for (const ProjectTemplate& t : ProjectTemplates())
        if (t.id == "Blank")
            project = Project::Create(dir, "CookGame", t, &error);
    CHECK(project.has_value());
    if (!project)
        return;
    const fs::path content = project->ContentDirectory();
    fs::create_directories(content / "Models");
    fs::copy_file(fs::path(ENGINE_ASSET_DIR) / "models" / "BoxTextured.glb", content / "Models" / "Box.glb");
    WriteTexturedTriangle(content / "Models");
    WriteBytes(content / "Textures" / "logo.png", SolidPng(20, 200, 40));
    WriteBytes(content / "Textures" / "unused.png", SolidPng(20, 40, 200));
    fs::create_directories(content / "Sounds");
    WriteWav(content / "Sounds" / "beep.wav", MakeTone(440.0f, 0.25f, 48000, 1));
    std::ofstream(content / "Types.uenum") << R"({"version": 1, "enum": "Fruit", "values": ["Apple", "Pear"]})";
    fs::create_directories(content / "Scenes");
    std::ofstream(content / "Scenes" / "Main.scene.json") << R"({"version": 1, "entities": [
      {"uuid": 1, "parent": 0, "name": "Box", "transform": {}, "mesh": {"model": {"file": "../Models/Box.glb"}, "index": 0}},
      {"uuid": 2, "parent": 0, "name": "Logo", "transform": {}, "uiWidget": {"type": "image", "image": "../Textures/logo.png"}},
      {"uuid": 3, "parent": 0, "name": "Speaker", "transform": {}, "audioSource": {"sound": "../Sounds/missing.wav"}}]})";
    project->settings.startScene = "Content/Scenes/Main.scene.json";
    CHECK(project->Save());

    const fs::path   out    = dir / "Out";
    CookOptions      options;
    options.threads         = 2;
    const CookReport report = CookProjectContent(*project, out / "Content.upak", options);
    for (const std::string& e : report.errors)
        std::printf("    error: %s\n", e.c_str());
    CHECK(report.Ok() && report.cookedModels == 2 && report.replacedFiles == 3); // Box.glb, tri.gltf + tri.bin
    CHECK(report.cookedTextures == 4); // box texture, tex.png, logo.png, unused.png
    const auto warned = [&](std::string_view text) {
        return std::ranges::any_of(report.warnings, [&](const std::string& w) { return w.find(text) != std::string::npos; });
    };
    CHECK(warned("missing.wav") && warned("unused.png") && !warned("logo.png"));
    const auto inPak = [&](std::string_view path) {
        return std::ranges::any_of(report.entries, [&](const CookReport::Entry& e) { return e.path == path; });
    };
    CHECK(inPak("Content/Scenes/Main.scene.json") && inPak("Content/Textures/logo.png") && !inPak("Content/Models/Box.glb") &&
          !inPak("Content/Models/tri.bin") && inPak(":cooked/models/Content/Models/Box.glb"));
    CHECK(report.Text().find("Result: OK") != std::string::npos);
    // Reproducible: the same content gives the same bytes.
    const CookReport again = CookProjectContent(*project, out / "Again.upak", options);
    CHECK(again.Ok() && ReadAll(out / "Content.upak") == ReadAll(out / "Again.upak"));

    // The packaged game: the pak mounted at another root, no Content/ on disk.
    const auto pak = PakFile::Open(out / "Content.upak");
    Vfs::Mount(pak, out);
    const MeshOptimizeSettings meshes{};
    const std::optional<ModelData> box = LoadCookedModel(out / "Content" / "Models" / "Box.glb", meshes);
    CHECK(box.has_value() && box->optimized && !box->textures.empty());
    TextureCookSettings runtime{.compress = true, .cacheDirectory = {}, .quality = 0};
    if (box && !box->textures.empty()) {
        const CookResult r = CookTexture(box->textures[0].encoded, box->textures[0].kind, runtime);
        CHECK(r.packed && !r.encoded);
    }
    const std::optional<ModelData> tri = LoadCookedModel(out / "Content" / "Models" / "tri.gltf", meshes);
    CHECK(tri && tri->textures.size() == 1 && tri->textures[0].file == (out / "Content" / "Models" / "tex.png").lexically_normal());
    if (tri && tri->textures.size() == 1) {
        const std::optional<std::vector<std::byte>> image = Vfs::Read(tri->textures[0].file);
        CHECK(image.has_value());
        if (image)
            CHECK(CookTexture(*image, TextureKind::Color, runtime).packed);
    }
    if (const auto logo = Vfs::Read(out / "Content" / "Textures" / "logo.png"))
        CHECK(CookTexture(*logo, TextureKind::Color, runtime).packed);
    else
        CHECK(false);
    // Not cooked in that kind: encoded now (the player warns about it).
    if (const auto logo = Vfs::Read(out / "Content" / "Textures" / "logo.png"))
        CHECK(CookTexture(*logo, TextureKind::Linear, runtime).encoded);

    const SoundData packed = LoadSoundFile(out / "Content" / "Sounds" / "beep.wav", SoundLoadMode::Decode);
    const SoundData disk   = LoadSoundFile(content / "Sounds" / "beep.wav", SoundLoadMode::Decode);
    CHECK(packed.frames == disk.frames && packed.samples == disk.samples && packed.frames > 0);
    const SoundData streamed = LoadSoundFile(out / "Content" / "Sounds" / "beep.wav", SoundLoadMode::Stream);
    CHECK(streamed.Streamed() && streamed.frames == disk.frames);

    Scene scene;
    (void)LoadSceneFile(out / "Content" / "Scenes" / "Main.scene.json", scene, static_cast<AssetManager*>(nullptr));
    CHECK(scene.GetRegistry().AliveCount() == 3);
    ScriptRegistry::Clear();
    const std::vector<std::string> problems = ScriptRegistry::LoadDirectory(out / "Content");
    CHECK(problems.empty() && ScriptRegistry::FindEnum("Fruit") != nullptr);
    ScriptRegistry::Clear();
    Vfs::UnmountAll();

    // Packaging: player, shaders, project file, pak and report; no loose Content/.
    const fs::path fakePlayer = dir / "UnginePlayer.bin";
    std::ofstream(fakePlayer) << "player";
    CookReport packaged;
    CHECK(PackageProject(*project, fakePlayer, dir / "Package", &error, &packaged) && packaged.Ok());
    CHECK(fs::exists(dir / "Package" / "Content.upak") && fs::exists(dir / "Package" / "BuildReport.txt") &&
          !fs::exists(dir / "Package" / "Content"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(SaveGame_VersionMigrationAndNewerSlots)
{
    using namespace BlueprintTest;
    const fs::path dir = TempDir("save_versions");
    using json         = nlohmann::json;
    std::ofstream(dir / "Old.sav") << json{{"version", 1}, {"values", {{"Points", {{"type", "int"}, {"value", 5}}}}}}.dump();
    std::ofstream(dir / "New.sav") << json{{"version", 1}, {"gameVersion", 5}, {"values", json::object()}}.dump();
    const std::string newBefore = std::string(reinterpret_cast<const char*>(ReadAll(dir / "New.sav").data()), ReadAll(dir / "New.sav").size());

    Graph g;
    // Migration: records the versions and adds a value.
    const auto migrate = g.Node("Event.MigrateSaveGame");
    const auto mark    = g.Node("SaveGame.SetValue", "bool");
    g.Link(migrate, "Out", mark, "In");
    g.Link(migrate, "Slot", mark, "Slot");
    g.Set(mark, "Key", std::string("Migrated"));
    g.Set(mark, "Value", true);
    const auto printFrom = g.Node("Debug.Print");
    g.Link(mark, "Then", printFrom, "In");
    g.Link(migrate, "From Version", printFrom, "Text");
    const auto printTo = g.Node("Debug.Print");
    g.Link(printFrom, "Then", printTo, "In");
    g.Link(migrate, "To Version", printTo, "Text");
    // BeginPlay: load both slots, save both, report.
    std::uint32_t last = g.Node("Event.BeginPlay");
    const char*   lastPin = "Out";
    const auto step = [&](const char* type, const char* slot) {
        const auto n = g.Node(type);
        g.Set(n, "Slot", std::string(slot));
        g.Link(last, lastPin, n, "In");
        const auto p = g.Node("Debug.Print");
        g.Link(n, "Then", p, "In");
        g.Link(n, "Success", p, "Text");
        last    = p;
        lastPin = "Then";
    };
    step("SaveGame.Load", "Old");
    step("SaveGame.Load", "New");
    step("SaveGame.Save", "Old");
    step("SaveGame.Save", "New");
    const auto version = g.Node("SaveGame.SlotVersion");
    g.Set(version, "Slot", std::string("New"));
    const auto printVersion = g.Node("Debug.Print");
    g.Link(last, lastPin, printVersion, "In");
    g.Link(version, "Version", printVersion, "Text");
    const auto printCompatible = g.Node("Debug.Print");
    g.Link(printVersion, "Then", printCompatible, "In");
    g.Link(version, "Compatible", printCompatible, "Text");
    CHECK(g.Valid());

    Runner r;
    r.scripts.SetSaveDirectory(dir);
    r.scripts.SetSaveVersion(2);
    r.Add("Saver", "saver.ugraph", g.g);
    r.scripts.Begin(r.scene);
    r.Run(0.2f);
    std::vector<std::string> texts;
    for (const ScriptMessage& m : r.scripts.Messages())
        texts.push_back(m.text);
    const std::vector<std::string> expected{"1", "2", "true", "false", "true", "false", "5", "false"};
    std::vector<std::string> printed;
    for (const std::string& t : texts)
        if (t == "1" || t == "2" || t == "5" || t == "true" || t == "false")
            printed.push_back(t);
    CHECK(printed == expected);
    if (printed != expected)
        for (const std::string& t : texts)
            std::printf("    message: %s\n", t.c_str());
    CHECK(std::ranges::any_of(texts, [](const std::string& t) { return t.find("newer game version") != std::string::npos; }));

    std::ifstream oldIn(dir / "Old.sav");
    const json    old = json::parse(oldIn);
    CHECK(old.value("gameVersion", 0) == 2 && old["values"].contains("Migrated") && old["values"].contains("Points"));
    const std::vector<std::byte> newAfter = ReadAll(dir / "New.sav");
    CHECK(std::string(reinterpret_cast<const char*>(newAfter.data()), newAfter.size()) == newBefore); // not overwritten
    r.scripts.End(r.scene);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(Options_StoreRemapBlueprintsAndMenu)
{
    using namespace BlueprintTest;
    const fs::path  dir = TempDir("options");
    ProjectSettings project;
    project.fullscreen = false;
    project.audio.volume[static_cast<std::size_t>(AudioBus::Music)] = 0.5f;
    project.input.actions = {{"Jump", {"Space"}}, {"Fire", {"MouseLeft"}}};
    project.input.axes    = {{"MoveForward", {{"W", 1.0f}, {"S", -1.0f}}}};

    GameOptions options(project, dir / "Settings.json");
    CHECK(options.Values() == options.Defaults() && options.Values().audio.volume[static_cast<std::size_t>(AudioBus::Music)] == 0.5f);
    int applied = 0;
    options.SetApply([&](const GameOptions&) { ++applied; });

    CHECK(options.RemapAction("Jump", 0, "J") && options.Input().FindAction("Jump")->keys == std::vector<std::string>{"J"});
    CHECK(!options.RemapAction("Missing", 0, "J"));
    CHECK(options.Conflicts("W", "Jump") == std::vector<std::string>{"MoveForward"});
    CHECK(options.RemapAxis("MoveForward", 1, "Down") && options.Input().FindAxis("MoveForward")->keys[1].key == "Down" &&
          options.Input().FindAxis("MoveForward")->keys[1].scale == -1.0f);
    CHECK(options.RemapAxis("MoveForward", 1, "S") && options.Values().axes.empty()); // back to the project's: not stored
    options.Edit().shadowQuality = 1;
    options.Edit().vsync         = false;
    options.Changed();
    CHECK(applied == 1);
    CHECK(options.Save());
    GameOptions reloaded(project, dir / "Settings.json");
    CHECK(reloaded.Values() == options.Values() && reloaded.Input().FindAction("Jump")->keys[0] == "J");
    // A binding for an action the project no longer has is dropped; a broken file gives the defaults.
    {
        ProjectSettings changed = project;
        changed.input.actions.erase(changed.input.actions.begin());
        GameOptions without(changed, dir / "Settings.json");
        CHECK(without.Values().actions.empty() && without.Values().shadowQuality == 1);
    }
    std::ofstream(dir / "Broken.json") << "{ not json";
    CHECK(GameOptions(project, dir / "Broken.json").Values() == options.Defaults());
    CHECK(GameOptions::ShadowResolution(0) == 0 && GameOptions::ShadowResolution(3) == 4096);

    // Blueprint nodes.
    {
        Graph g;
        const auto begin = g.Node("Event.BeginPlay");
        const auto remap = g.Node("Input.RemapAction", "Jump");
        g.Set(remap, "Key", std::string("W"));
        g.Link(begin, "Out", remap, "In");
        const auto count = g.Node("Array.Length", "string");
        g.Link(remap, "Conflicts", count, "Array");
        const auto print = g.Node("Debug.Print");
        g.Link(remap, "Then", print, "In");
        g.Link(count, "Length", print, "Text");
        const auto quality = g.Node("Options.SetShadowQuality");
        g.Set(quality, "Quality", 2);
        g.Link(print, "Then", quality, "In");
        const auto save = g.Node("Options.Save");
        g.Link(quality, "Then", save, "In");
        const auto saved = g.Node("Debug.Print");
        g.Link(save, "Then", saved, "In");
        g.Link(save, "Success", saved, "Text");
        CHECK(g.Valid());
        Runner r;
        r.scripts.SetGameOptions(&options);
        r.Add("Menu", "menu.ugraph", g.g);
        r.scripts.Begin(r.scene);
        r.Run(0.2f);
        CHECK(r.Printed("1") && r.Printed("true")); // W is also MoveForward's
        CHECK(options.Values().shadowQuality == 2 && options.Input().FindAction("Jump")->keys[0] == "W");
        r.scripts.End(r.scene);
        r.scripts.SetGameOptions(nullptr);
    }

    // The player's Options page.
    {
        EventBus  bus;
        Input     input(bus);
        Scene     scene;
        const Entity canvas = scene.CreateEntity("Pause Menu");
        scene.GetRegistry().Emplace<UiCanvas>(canvas, UiCanvas{.designSize = {1280.0f, 720.0f}});
        OptionsMenu menu;
        menu.Create(scene, canvas);
        menu.Show(scene, options, true);
        const auto named = [&](const std::string& name) {
            std::vector<Entity> found;
            scene.GetRegistry().ViewOf<Name>().Each([&](Entity e, const Name& n) {
                if (n.value == name)
                    found.push_back(e);
            });
            std::ranges::sort(found, {}, [](Entity e) { return EntityIndex(e); });
            return found;
        };
        bool closed = false;
        CHECK(menu.Handle(scene, options, {named("Options Fullscreen").at(0), UiEventType::CheckedChanged, 0.0f, true}, closed));
        CHECK(options.Values().fullscreen);
        const std::uint32_t before = options.Values().shadowQuality;
        CHECK(menu.Handle(scene, options, {named("Options Shadows").at(0), UiEventType::Clicked}, closed));
        CHECK(options.Values().shadowQuality == (before + 1) % 4);
        CHECK(scene.GetRegistry().Get<UiWidget>(named("Options Shadows").at(0)).text.starts_with("Shadows: "));
        CHECK(menu.Handle(scene, options, {named("Volume Music").at(0), UiEventType::ValueChanged, 0.25f}, closed));
        CHECK(options.Values().audio.volume[static_cast<std::size_t>(AudioBus::Music)] == 0.25f);
        // Controls: rows Jump, Fire, MoveForward (+1), MoveForward (-1); remap Fire to K.
        CHECK(menu.Handle(scene, options, {named("Options Controls").at(0), UiEventType::Clicked}, closed));
        CHECK(scene.GetRegistry().Get<UiWidget>(named("Controls Page").at(0)).visible);
        const std::vector<Entity> keys = named("Options Key");
        CHECK(keys.size() == OptionsMenu::kRowsPerPage);
        CHECK(menu.Handle(scene, options, {keys.at(1), UiEventType::Clicked}, closed) && menu.Capturing());
        input.NewFrame();
        bus.Publish(KeyEvent{Key::K, 0, InputAction::Press, 0});
        menu.Update(scene, options, input);
        CHECK(!menu.Capturing() && options.Input().FindAction("Fire")->keys[0] == "K");
        CHECK(scene.GetRegistry().Get<UiWidget>(keys.at(1)).text == "K");
        // Remap to a key in use: allowed, with a hint; Escape cancels a capture.
        CHECK(menu.Handle(scene, options, {keys.at(1), UiEventType::Clicked}, closed));
        input.NewFrame();
        bus.Publish(KeyEvent{Key::K, 0, InputAction::Release, 0});
        bus.Publish(KeyEvent{Key::S, 0, InputAction::Press, 0});
        menu.Update(scene, options, input);
        CHECK(options.Input().FindAction("Fire")->keys[0] == "S");
        CHECK(scene.GetRegistry().Get<UiWidget>(named("Controls Hint").at(0)).text.find("MoveForward") != std::string::npos);
        CHECK(menu.Handle(scene, options, {keys.at(0), UiEventType::Clicked}, closed) && menu.Capturing());
        input.NewFrame();
        bus.Publish(KeyEvent{Key::S, 0, InputAction::Release, 0});
        bus.Publish(KeyEvent{Key::Escape, 0, InputAction::Press, 0});
        menu.Update(scene, options, input);
        CHECK(!menu.Capturing() && options.Input().FindAction("Jump")->keys[0] == "W");
        // Reset keys, then Back saves.
        CHECK(menu.Handle(scene, options, {named("Options Reset keys").at(0), UiEventType::Clicked}, closed));
        CHECK(options.Values().actions.empty() && options.Values().axes.empty());
        CHECK(menu.Handle(scene, options, {named("Options Back").at(0), UiEventType::Clicked}, closed) && closed);
        CHECK(GameOptions(project, dir / "Settings.json").Values() == options.Values());
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
