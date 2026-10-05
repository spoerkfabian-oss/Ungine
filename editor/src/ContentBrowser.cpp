#include "Editor/Editor.h"
#include "Editor/ScriptGraphEditor.h"
#include "Editor/FileDialog.h"
#include "Editor/ImGuiLayer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptRegistry.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <unordered_map>
#include <variant>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;

namespace {

constexpr const char* kContentPayload = "UNGINE_CONTENT"; // drag & drop: UTF-8 path

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool EndsWith(const std::string& s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "Name.ext", "Name1.ext", ... not existing in `dir`.
fs::path UniquePath(const fs::path& dir, const std::string& stem, const std::string& extension)
{
    std::error_code ec;
    fs::path        path = dir / PathFromUtf8(stem + extension);
    for (int i = 1; fs::exists(path, ec); ++i)
        path = dir / PathFromUtf8(stem + std::to_string(i) + extension);
    return path;
}

ImVec4 KindColor(int kind)
{
    switch (kind) {
    case 0: return {0.95f, 0.80f, 0.35f, 1.0f}; // folder
    case 1: return {0.45f, 0.85f, 0.45f, 1.0f}; // scene
    case 2: return {0.40f, 0.65f, 1.00f, 1.0f}; // blueprint
    case 3: return {0.35f, 0.75f, 1.00f, 1.0f}; // prefab
    case 4: return {0.95f, 0.55f, 0.30f, 1.0f}; // model
    case 5: return {0.85f, 0.45f, 0.85f, 1.0f}; // texture
    case 6: return {0.35f, 0.85f, 0.85f, 1.0f}; // sound
    case 7: return {0.55f, 0.85f, 0.55f, 1.0f}; // enum / struct / interface
    default: return {0.65f, 0.65f, 0.65f, 1.0f};
    }
}

constexpr const char* kKindTags[] = {"DIR", "SCENE", "BP", "PFB", "MODEL", "TEX", "SND", "TYPE", "FILE"};

bool IsTypeFile(const std::string& lowerName)
{
    return EndsWith(lowerName, ".uenum") || EndsWith(lowerName, ".ustruct") || EndsWith(lowerName, ".uinterface");
}

int UriHex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::string> DecodeUriPath(std::string_view uri)
{
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] != '%') {
            decoded.push_back(uri[i]);
            continue;
        }
        if (i + 2 >= uri.size())
            return std::nullopt;
        const int hi = UriHex(uri[i + 1]), lo = UriHex(uri[i + 2]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return decoded;
}

std::string EncodeUriPath(const fs::path& path)
{
    const std::u8string generic = path.generic_u8string();
    const std::string utf8(generic.begin(), generic.end());
    constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char c : utf8) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '.' || c == '-' || c == '_' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[c >> 4]);
            encoded.push_back(digits[c & 0x0f]);
        }
    }
    return encoded;
}

std::optional<fs::path> RelocateReference(const fs::path& reference, const fs::path& from,
                                          const fs::path& to, bool directory);

struct StoredReferenceFile {
    fs::path path{};
    fs::path temporary{};
    fs::path backup{};
    bool backedUp = false;
    bool installed = false;
};

void RewriteStoredPaths(nlohmann::json& value, const fs::path& ownerDirectory,
                        const fs::path& from, const fs::path& to, bool directory, std::size_t& count,
                        const fs::path& oldOwnerDirectory = {})
{
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.value().is_string() && (it.key() == "graph" || it.key() == "sound" || it.key() == "image" ||
                                          it.key() == "file" || it.key() == "uri")) {
                const std::string source = it.value().get<std::string>();
                if (it.key() == "uri" && source.find(':') != std::string::npos)
                    continue;
                std::string decodedSource = source;
                if (it.key() == "uri") {
                    const auto decoded = DecodeUriPath(source);
                    if (!decoded)
                        continue;
                    decodedSource = *decoded;
                }
                fs::path reference = PathFromUtf8(decodedSource);
                const bool wasRelative = reference.is_relative();
                if (wasRelative)
                    reference = (oldOwnerDirectory.empty() ? ownerDirectory : oldOwnerDirectory) / reference;
                const auto relocated = RelocateReference(PathToUtf8(reference), from, to, directory);
                if (relocated || !oldOwnerDirectory.empty()) {
                    std::error_code ec;
                    const fs::path finalTarget = relocated ? *relocated : reference;
                    const fs::path replacement = wasRelative ?
                        finalTarget.lexically_relative(ownerDirectory) : finalTarget;
                    if (!replacement.empty() && !replacement.is_absolute())
                        it.value() = it.key() == "uri" ? EncodeUriPath(replacement) : PathToUtf8(replacement);
                    else
                        it.value() = PathToUtf8(fs::absolute(finalTarget, ec).lexically_normal());
                    ++count;
                }
            } else {
                RewriteStoredPaths(it.value(), ownerDirectory, from, to, directory, count, oldOwnerDirectory);
            }
        }
    } else if (value.is_array()) {
        for (nlohmann::json& child : value)
            RewriteStoredPaths(child, ownerDirectory, from, to, directory, count, oldOwnerDirectory);
    }
}

bool RepairStoredReferences(const fs::path& contentRoot, const fs::path& from, const fs::path& to,
                            bool directory, std::size_t& repaired, std::string& error)
{
    const std::size_t repairedBefore = repaired;
    std::vector<StoredReferenceFile> changed;
    const auto removePreparedFiles = [&] {
        for (const StoredReferenceFile& plan : changed) {
            std::error_code removeError;
            fs::remove(plan.temporary, removeError);
        }
    };
    std::error_code ec;
    for (fs::recursive_directory_iterator it(contentRoot, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const std::string name = Lower(PathToUtf8(it->path().filename()));
        const bool legacyScene = EndsWith(name, ".json") && Lower(PathToUtf8(it->path().parent_path().filename())) == "scenes";
        if (!(EndsWith(name, ".scene.json") || EndsWith(name, ".uprefab") || EndsWith(name, ".ugraph") ||
              EndsWith(name, ".gltf") || legacyScene))
            continue;

        std::ifstream input(it->path(), std::ios::binary);
        if (!input) {
            error = "Cannot inspect references in " + PathToUtf8(it->path());
            continue;
        }
        nlohmann::json document;
        try {
            input >> document;
        } catch (const std::exception&) {
            error = "Skipped invalid JSON file while repairing references: " + PathToUtf8(it->path());
            continue;
        }
        std::size_t fileChanges = 0;
        const fs::path oldOwner = !directory && it->path() == to ? from.parent_path() : fs::path{};
        RewriteStoredPaths(document, it->path().parent_path(), from, to, directory, fileChanges, oldOwner);
        if (fileChanges == 0)
            continue;

        StoredReferenceFile plan{.path = it->path()};
        plan.temporary = plan.path;
        plan.temporary += ".reference.tmp";
        plan.backup = plan.path;
        plan.backup += ".reference.bak";
        const bool tempExists = fs::exists(plan.temporary, ec);
        const bool backupExists = !ec && fs::exists(plan.backup, ec);
        if (ec || tempExists || backupExists) {
            error = "A reference-repair temporary file already exists beside " + PathToUtf8(plan.path);
            removePreparedFiles();
            return false;
        }
        std::ofstream output(plan.temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Cannot prepare reference update for " + PathToUtf8(plan.path);
            removePreparedFiles();
            return false;
        }
        output << document.dump(2) << '\n';
        output.close();
        if (!output) {
            error = "Cannot write reference update for " + PathToUtf8(plan.path);
            std::error_code removeError;
            fs::remove(plan.temporary, removeError);
            removePreparedFiles();
            return false;
        }
        repaired += fileChanges;
        changed.push_back(std::move(plan));
    }
    if (ec) {
        error = "Cannot scan Content for reference updates: " + ec.message();
        removePreparedFiles();
        return false;
    }

    bool failed = false;
    for (StoredReferenceFile& plan : changed) {
        fs::rename(plan.path, plan.backup, ec);
        if (!ec)
            plan.backedUp = true;
        if (!ec) {
            fs::rename(plan.temporary, plan.path, ec);
            if (!ec)
                plan.installed = true;
        }
        if (ec) {
            error = "Cannot commit reference update for " + PathToUtf8(plan.path) + ": " + ec.message();
            failed = true;
            break;
        }
    }

    if (failed) {
        for (auto it = changed.rbegin(); it != changed.rend(); ++it) {
            std::error_code rollbackError;
            if (it->installed)
                fs::remove(it->path, rollbackError);
            rollbackError.clear();
            if (it->backedUp)
                fs::rename(it->backup, it->path, rollbackError);
            rollbackError.clear();
            fs::remove(it->temporary, rollbackError);
        }
        repaired = repairedBefore;
        return false;
    }
    for (StoredReferenceFile& plan : changed)
        fs::remove(plan.backup, ec);
    return true;
}

std::optional<fs::path> RelocateReference(const fs::path& reference, const fs::path& from,
                                          const fs::path& to, bool directory)
{
    if (reference.empty())
        return std::nullopt;
    std::error_code ec;
    fs::path path = PathFromUtf8(reference);
    if (path.is_relative()) {
        path = fs::absolute(path, ec);
        if (ec)
            return std::nullopt;
    }
    const fs::path absolute = fs::absolute(from, ec).lexically_normal();
    if (ec)
        return std::nullopt;
    path = path.lexically_normal();
#ifdef _WIN32
    const bool samePath = Lower(PathToUtf8(path)) == Lower(PathToUtf8(absolute));
#else
    const bool samePath = path == absolute;
#endif
    if (samePath) {
        const fs::path relocated = fs::absolute(to, ec).lexically_normal();
        return ec ? std::nullopt : std::optional<fs::path>(relocated);
    }
    if (!directory)
        return std::nullopt;
    const fs::path relative = path.lexically_relative(absolute);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        return std::nullopt;
    return (to / relative).lexically_normal();
}

} // namespace

fs::path Editor::ContentRoot() const
{
    std::error_code ec;
    if (m_Ctx.project) {
        const fs::path content = m_Ctx.project->ContentDirectory();
        if (fs::is_directory(content, ec))
            return content;
    }
    return fs::current_path(ec);
}

bool Editor::SaveAll()
{
    bool ok = true;
    if (!m_ScenePath.empty() && m_PlayState == PlayState::Edit)
        ok = SaveScene(m_ScenePath) && ok;
    ok = m_Graphs->SaveAll() && ok;
    if (m_Ctx.project && m_ProjectDirty) {
        std::string error;
        if (m_Ctx.project->Save(&error))
            m_ProjectDirty = false;
        else
            ok = false;
    }
    if (ok)
        m_Status = "Saved all";
    return ok;
}

bool Editor::ConfirmQuit()
{
    if (m_QuitConfirmed || (!HasUnsavedChanges() && !m_Graphs->AnyDirty() && !m_ProjectDirty))
        return true;
    m_AskQuit = true;
    return false;
}

bool Editor::BuildAndRun()
{
    if (!m_Ctx.project)
        return false;
    if (m_PlayState != PlayState::Edit)
        Stop();
    SaveAll();
    Project& project = *m_Ctx.project;
    if (project.settings.startScene.empty() && !m_ScenePath.empty()) { // first run: the open scene starts
        project.settings.startScene = project.Relative(m_ScenePath);
        (void)project.Save();
    }
    if (project.settings.startScene.empty()) {
        m_Status = "Save the scene first (it becomes the start scene)";
        return false;
    }
    const fs::path player = SiblingExecutable("UnginePlayer");
    std::error_code ec;
    if (!fs::exists(player, ec)) {
        m_Status = "UnginePlayer not found next to the editor";
        ENGINE_ERROR("Build & Run: '{}' not found", PathToUtf8(player));
        return false;
    }
    const bool started = LaunchProcess(player, {PathToUtf8(project.File())}, project.Root());
    m_Status           = started ? "Running " + project.settings.name : "Could not start the player (see log)";
    return started;
}

bool Editor::PackageProject(const fs::path& outputDirectory)
{
    if (!m_Ctx.project)
        return false;
    SaveAll();
    std::string error;
    if (!Engine::PackageProject(*m_Ctx.project, SiblingExecutable("UnginePlayer"), outputDirectory, &error)) {
        ENGINE_ERROR("Package: {}", error);
        m_Status = "Packaging failed (see log)";
        return false;
    }
    m_Status = "Packaged into " + PathToUtf8(outputDirectory);
    return true;
}

void Editor::OpenAsset(const fs::path& file)
{
    const std::string name = Lower(PathToUtf8(file.filename()));
    if (EndsWith(name, ".scene.json") || (EndsWith(name, ".json") && file.parent_path().filename() == "Scenes")) {
        RequestSceneChange([this, file] { OpenScene(file); });
    } else if (EndsWith(name, ".ugraph")) {
        if (m_Graphs->Open(file)) {
            m_ShowBlueprint = true;
            m_Graphs->Focus();
        } else {
            m_Status = "Cannot open blueprint (see log)";
        }
    } else if (IsTypeFile(name)) {
        OpenTypeFile(file);
    } else if (EndsWith(name, ".uprefab")) { // double-click: an instance in front of the camera
        if (m_PlayState == PlayState::Edit)
            PlacePrefab(file, PlacementPoint(std::max(m_Ctx.camera.moveSpeed, 1.0f) * 2.0f));
        else
            m_Status = "Stop playing to place prefabs";
    } else if (EndsWith(name, ".glb") || EndsWith(name, ".gltf")) {
        const ModelHandle handle = m_Ctx.assets.LoadModel(file);
        m_Ctx.modelRefs.push_back(handle);
        m_PendingInstances.emplace_back(handle, PlacementPoint(std::max(m_Ctx.camera.moveSpeed, 1.0f) * 2.0f));
        m_Status = "Loading " + PathToUtf8(file.filename());
    } else if (IsSoundFile(file) && m_Ctx.audio) { // double-click: listen
        if (m_Ctx.audio->Previewing()) {
            m_Ctx.audio->StopPreview();
        } else {
            m_Ctx.audio->Preview(file);
            m_Status = "Preview " + PathToUtf8(file.filename());
        }
    }
}

void Editor::UpdatePendingInstances()
{
    std::erase_if(m_PendingInstances, [&](const std::pair<ModelHandle, glm::vec3>& pending) {
        const auto [handle, position] = pending;
        switch (m_Ctx.assets.State(handle)) {
        case AssetState::Ready: {
            const Model* model = m_Ctx.assets.Get(handle);
            if (!model)
                return true;
            const Entity root                = InstantiateModel(m_Ctx.scene, handle, *model);
            m_Ctx.scene.EditTransform(root).position = position;
            const Entity roots[] = {root};
            PushCreated("Instantiate " + model->name, roots);
            Select(root);
            m_Status = "Placed " + model->name;
            return true;
        }
        case AssetState::Failed:
        case AssetState::Invalid: m_Status = "Model failed to load (see log)"; return true;
        default: return false;
        }
    });
}

void Editor::RefreshContent()
{
    std::error_code ec;
    const fs::path  root = ContentRoot();
    if (m_ContentDir.empty() || !fs::is_directory(m_ContentDir, ec) ||
        fs::relative(m_ContentDir, root, ec).native().starts_with(fs::path("..").native()))
        m_ContentDir = root;
    m_ContentItems.clear();
    for (fs::directory_iterator it(m_ContentDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path&   path  = it->path();
        const std::string label = PathToUtf8(path.filename());
        const std::string lower = Lower(label);
        if (label.starts_with('.') || EndsWith(lower, ".tmp"))
            continue;
        ContentItem item{path, label, ContentItem::Kind::Other};
        std::error_code typeError;
        if (it->is_directory(typeError))
            item.kind = ContentItem::Kind::Folder;
        else if (EndsWith(lower, ".scene.json") || (EndsWith(lower, ".json") && m_ContentDir.filename() == "Scenes"))
            item.kind = ContentItem::Kind::Scene;
        else if (EndsWith(lower, ".ugraph"))
            item.kind = ContentItem::Kind::Blueprint;
        else if (EndsWith(lower, ".uprefab"))
            item.kind = ContentItem::Kind::Prefab;
        else if (EndsWith(lower, ".glb") || EndsWith(lower, ".gltf"))
            item.kind = ContentItem::Kind::Model;
        else if (EndsWith(lower, ".png") || EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") || EndsWith(lower, ".ktx2"))
            item.kind = ContentItem::Kind::Texture;
        else if (IsSoundFile(path))
            item.kind = ContentItem::Kind::Sound;
        else if (IsTypeFile(lower))
            item.kind = ContentItem::Kind::Type;
        m_ContentItems.push_back(std::move(item));
    }
    std::ranges::sort(m_ContentItems, [](const ContentItem& a, const ContentItem& b) {
        return a.kind != b.kind ? a.kind < b.kind : Lower(a.label) < Lower(b.label);
    });
    m_ContentScanTime = ImGui::GetTime();
}

void Editor::ReleaseContentPreview()
{
    if (m_ContentPreviewTexture && m_ImGui) {
        m_ImGui->RemoveTexture(m_ContentPreviewTexture);
        m_ContentPreviewTexture = 0;
    }
    if (m_ContentPreviewHandle) {
        m_Ctx.assets.Release(m_ContentPreviewHandle);
        m_ContentPreviewHandle = {};
    }
    if (m_ContentPreviewModel) {
        m_Ctx.assets.Release(m_ContentPreviewModel);
        m_ContentPreviewModel = {};
    }
    if (m_ContentPreviewSound) {
        m_Ctx.assets.Release(m_ContentPreviewSound);
        m_ContentPreviewSound = {};
    }
    m_ContentPreviewWaveform.clear();
    m_ContentPreviewWaveformReady = false;
    m_ContentPreviewRevision = 0;
}

void Editor::SelectContentFile(const fs::path& file)
{
    if (m_ContentSelected == file)
        return;
    ReleaseContentPreview();
    m_ContentSelected = file;
    const std::string lower = Lower(PathToUtf8(file.filename()));
    if (EndsWith(lower, ".png") || EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") ||
        EndsWith(lower, ".ktx2"))
        m_ContentPreviewHandle = m_Ctx.assets.LoadTexture(file, m_TextureImportKind);
    else if (EndsWith(lower, ".glb") || EndsWith(lower, ".gltf"))
        m_ContentPreviewModel = m_Ctx.assets.LoadModel(file);
    else if (IsSoundFile(file))
        m_ContentPreviewSound = m_Ctx.assets.LoadSound(file);
}

void Editor::ImportContentFile(const fs::path& source)
{
    if (m_ImportWorker.joinable()) {
        if (!m_ImportDone.load(std::memory_order_acquire)) {
            m_Status = "An import is already in progress";
            return;
        }
        m_ImportWorker.join();
    }
    std::error_code ec;
    if (m_ImportTargetDir.empty() || !fs::is_directory(m_ImportTargetDir, ec)) {
        m_Status = "Import target folder no longer exists";
        return;
    }
    const fs::path targetRoot = fs::weakly_canonical(ContentRoot(), ec);
    if (ec) {
        m_Status = "Cannot resolve the project's Content folder: " + ec.message();
        return;
    }
    const fs::path targetDir = fs::weakly_canonical(m_ImportTargetDir, ec);
    if (ec) {
        m_Status = "Cannot resolve the import target folder: " + ec.message();
        return;
    }
    const fs::path relative = targetDir.lexically_relative(targetRoot);
    if (ec || relative.empty() || relative.is_absolute() || (relative != "." && *relative.begin() == "..")) {
        m_Status = "Import target must be inside the project's Content folder";
        return;
    }
    if (!fs::is_regular_file(source, ec)) {
        m_Status = "Import source is not a regular file";
        return;
    }
    const std::string extension = Lower(PathToUtf8(source.extension()));
    const bool gltfBundle = extension == ".gltf";
    fs::path target = m_ImportTargetDir / source.filename();
    if (!gltfBundle && fs::exists(target, ec) && fs::equivalent(source, target, ec)) {
        m_Status = "File is already in this Content folder";
        return;
    }
    ec.clear();
    fs::path bundleRoot;
    if (gltfBundle) {
        bundleRoot = m_ImportTargetDir / source.stem();
        for (std::uint32_t suffix = 1; fs::exists(bundleRoot, ec); ++suffix) {
            bundleRoot = m_ImportTargetDir / PathFromUtf8(PathToUtf8(source.stem()) + std::to_string(suffix));
            if (ec || suffix == std::numeric_limits<std::uint32_t>::max()) {
                m_Status = "Cannot choose a unique import folder";
                return;
            }
        }
        target = bundleRoot / source.filename();
    } else {
        for (std::uint32_t suffix = 1; fs::exists(target, ec); ++suffix) {
            target = m_ImportTargetDir / PathFromUtf8(PathToUtf8(source.stem()) + std::to_string(suffix) +
                                                      PathToUtf8(source.extension()));
            if (ec || suffix == std::numeric_limits<std::uint32_t>::max()) {
                m_Status = "Cannot choose a unique import filename";
                return;
            }
        }
    }

    struct CopyItem { fs::path source; fs::path target; std::uintmax_t size = 0; };
    std::vector<CopyItem> copies{{source, target}};
    if (gltfBundle) {
        nlohmann::json gltf;
        std::ifstream input(source, std::ios::binary);
        if (!input) {
            m_Status = "Cannot open glTF import manifest";
            return;
        }
        try {
            input >> gltf;
        } catch (const std::exception& exception) {
            m_Status = "Cannot read glTF import manifest: " + std::string(exception.what());
            return;
        }
        std::unordered_map<std::string, fs::path> dependencies;
        for (const char* section : {"buffers", "images"}) {
            if (!gltf.contains(section) || !gltf[section].is_array())
                continue;
            for (const nlohmann::json& entry : gltf[section]) {
                if (!entry.is_object() || !entry.contains("uri") || !entry["uri"].is_string())
                    continue;
                const std::string uri = entry["uri"].get<std::string>();
                if (uri.starts_with("data:"))
                    continue;
                const auto decoded = DecodeUriPath(uri);
                if (!decoded || decoded->empty() || decoded->find(':') != std::string::npos ||
                    decoded->find('\\') != std::string::npos) {
                    m_Status = "Unsupported or invalid external glTF URI: " + uri;
                    return;
                }
                const fs::path relativePath = PathFromUtf8(*decoded).lexically_normal();
                if (relativePath.is_absolute() || *relativePath.begin() == "..") {
                    m_Status = "glTF dependency escapes its source folder: " + uri;
                    return;
                }
                const fs::path dependency = (source.parent_path() / relativePath).lexically_normal();
                if (!fs::is_regular_file(dependency, ec) || ec) {
                    m_Status = "glTF dependency is missing: " + PathToUtf8(dependency);
                    return;
                }
                dependencies.emplace(uri, relativePath);
            }
        }
        std::unordered_map<std::string, fs::path> uniqueSources;
        for (const auto& [uri, relativePath] : dependencies) {
            const fs::path dependency = (source.parent_path() / relativePath).lexically_normal();
            uniqueSources.emplace(PathToUtf8(dependency), relativePath);
        }
        for (const auto& [sourcePath, relativePath] : uniqueSources)
            copies.push_back({PathFromUtf8(sourcePath), bundleRoot / relativePath});
    }

    std::uint64_t total = 0;
    for (CopyItem& copy : copies) {
        copy.size = fs::file_size(copy.source, ec);
        if (ec || copy.size > std::numeric_limits<std::uint64_t>::max() - total) {
            m_Status = ec ? "Cannot read import source size: " + ec.message() : "Import bundle is too large";
            return;
        }
        total += copy.size;
        fs::path temporary = copy.target;
        temporary += ".importing.tmp";
        if (fs::exists(temporary, ec) || ec) {
            m_Status = "An import temporary file already exists: " + PathToUtf8(temporary.filename());
            return;
        }
    }
    if (!gltfBundle && copies.size() != 1) {
        m_Status = "Invalid import bundle";
        return;
    }

    m_ImportBytes.store(0, std::memory_order_relaxed);
    m_ImportTotal.store(total, std::memory_order_relaxed);
    m_ImportDone.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(m_ImportMutex);
        m_ImportResult.clear();
        m_ImportSuccess = false;
    }
    m_Status = "Importing " + PathToUtf8(source.filename());
    m_ImportWorker = std::jthread([this, copies = std::move(copies), bundleRoot, gltfBundle, target, total](std::stop_token stop) {
        std::string result;
        bool success = false;
        std::vector<fs::path> temporaries;
        if (gltfBundle) {
            std::error_code createError;
            fs::create_directory(bundleRoot, createError);
            if (createError)
                result = "Cannot create glTF import folder: " + createError.message();
        }
        for (const CopyItem& copy : copies) {
            if (!result.empty() || stop.stop_requested())
                break;
            std::error_code createError;
            fs::create_directories(copy.target.parent_path(), createError);
            if (createError) {
                result = "Cannot create import target folder: " + createError.message();
                break;
            }
            fs::path temporary = copy.target;
            temporary += ".importing.tmp";
            temporaries.push_back(temporary);
            std::ifstream input(copy.source, std::ios::binary);
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!input || !output) {
                result = "Cannot open import source or temporary file: " + PathToUtf8(copy.source.filename());
                break;
            }
            std::array<char, 1024 * 1024> buffer{};
            while (!stop.stop_requested()) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize count = input.gcount();
                if (count > 0) {
                    output.write(buffer.data(), count);
                    if (!output) {
                        result = "Import failed while writing " + PathToUtf8(copy.target.filename());
                        break;
                    }
                    m_ImportBytes.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
                }
                if (input.eof())
                    break;
                if (!input) {
                    result = "Import failed while reading " + PathToUtf8(copy.source.filename());
                    break;
                }
            }
            output.flush();
            if (result.empty() && stop.stop_requested())
                result = "Import cancelled";
            else if (result.empty() && !output)
                result = "Import failed while finalizing " + PathToUtf8(copy.target.filename());
            output.close();
            input.close();
            if (!result.empty())
                break;
        }
        if (result.empty() && stop.stop_requested())
            result = "Import cancelled";
        else if (result.empty() && m_ImportBytes.load(std::memory_order_relaxed) != total)
            result = "Import source changed while files were being copied";
        if (result.empty()) {
            std::error_code commitError;
            for (std::size_t i = 0; i < copies.size(); ++i) {
                fs::path temporary = copies[i].target;
                temporary += ".importing.tmp";
                if (fs::exists(copies[i].target, commitError) || commitError) {
                    result = "Import destination appeared during copy: " + PathToUtf8(copies[i].target.filename());
                    break;
                }
                fs::rename(temporary, copies[i].target, commitError);
                if (commitError) {
                    result = "Could not finalize import: " + commitError.message();
                    break;
                }
            }
            success = result.empty();
            if (success)
                result = "Imported " + PathToUtf8(target.filename());
        }
        if (!success) {
            for (const fs::path& temporary : temporaries) {
                std::error_code removeError;
                fs::remove(temporary, removeError);
            }
            if (gltfBundle) {
                std::error_code removeError;
                fs::remove_all(bundleRoot, removeError);
            }
        }
        {
            std::scoped_lock lock(m_ImportMutex);
            m_ImportResult = std::move(result);
            m_ImportSuccess = success;
        }
        m_ImportDone.store(true, std::memory_order_release);
    });
}

void Editor::UpdateContentImport()
{
    if (!m_ImportWorker.joinable() || !m_ImportDone.load(std::memory_order_acquire))
        return;
    m_ImportWorker.join();
    {
        std::scoped_lock lock(m_ImportMutex);
        m_Status = m_ImportResult;
        if (m_ImportSuccess && m_ImportTargetDir == m_ContentDir)
            RefreshContent();
    }
}

bool Editor::RelocateContentAsset(const fs::path& source, const fs::path& target)
{
    std::error_code ec;
    if (!fs::exists(source, ec) || ec || fs::exists(target, ec)) {
        m_Status = ec ? "Cannot inspect relocation paths: " + ec.message() : "Relocation target already exists or source is missing";
        return false;
    }
    const bool wasDirectory = fs::is_directory(source, ec);
    if (ec) {
        m_Status = "Cannot inspect source: " + ec.message();
        return false;
    }
    fs::rename(source, target, ec);
    if (ec) {
        m_Status = "Relocation failed: " + ec.message();
        return false;
    }

    std::size_t storedReferences = 0;
    std::string referenceError;
    const bool repairedStored = RepairStoredReferences(ContentRoot(), source, target, wasDirectory,
                                                        storedReferences, referenceError);
    if (!repairedStored) {
        std::error_code rollbackError;
        fs::rename(target, source, rollbackError);
        m_Status = rollbackError ? "Relocation completed but reference repair and rollback failed: " + referenceError
                                 : "Relocation cancelled because references could not be repaired: " + referenceError;
        return false;
    }
    m_Status = "Moved " + PathToUtf8(source.filename()) + " to " + PathToUtf8(target.parent_path().filename());
    if (storedReferences > 0)
        m_Status += std::format("; repaired {} stored reference(s)", storedReferences);
    if (!referenceError.empty())
        m_Status += "; reference scan note: " + referenceError;

    if (m_Ctx.project) {
        const fs::path start = m_Ctx.project->StartScene();
        if (const auto relocated = RelocateReference(PathToUtf8(start), source, target, wasDirectory)) {
            m_Ctx.project->settings.startScene = m_Ctx.project->Relative(*relocated);
            m_ProjectDirty = !m_Ctx.project->Save();
        }
    }
    if (const auto relocated = RelocateReference(PathToUtf8(m_ScenePath), source, target, wasDirectory))
        m_ScenePath = *relocated;

    std::unordered_map<ModelHandle, ModelHandle> relocatedModels;
    const auto relocateModel = [&](ModelHandle oldHandle) -> std::optional<ModelHandle> {
        if (const auto it = relocatedModels.find(oldHandle); it != relocatedModels.end())
            return it->second;
        const ModelSource modelSource = m_Ctx.assets.Source(oldHandle);
        if (modelSource.file.empty())
            return std::nullopt;
        const auto path = RelocateReference(PathToUtf8(modelSource.file), source, target, wasDirectory);
        if (!path)
            return std::nullopt;
        const std::size_t ownedRefs = static_cast<std::size_t>(std::ranges::count(m_Ctx.modelRefs, oldHandle));
        const ModelHandle newHandle = m_Ctx.assets.LoadModel(*path);
        for (std::size_t i = 1; i < std::max<std::size_t>(ownedRefs, 1); ++i)
            (void)m_Ctx.assets.LoadModel(*path);
        if (ownedRefs == 0) {
            m_Ctx.modelRefs.push_back(newHandle);
        } else {
            for (ModelHandle& handle : m_Ctx.modelRefs)
                if (handle == oldHandle)
                    handle = newHandle;
            for (std::size_t i = 1; i < ownedRefs; ++i)
                m_Ctx.assets.Release(oldHandle);
            m_Ctx.modelRefs.push_back(oldHandle); // keep the old source alive for scene undo
        }
        relocatedModels.emplace(oldHandle, newHandle);
        return newHandle;
    };
    std::vector<StateEdit> edits;
    Registry& registry = m_Ctx.scene.GetRegistry();
    registry.ViewOf<Hierarchy>().Each([&](Entity entity, Hierarchy&) {
        ScriptComponent* script = registry.TryGet<ScriptComponent>(entity);
        AudioSource* audio = registry.TryGet<AudioSource>(entity);
        UiWidget* widget = registry.TryGet<UiWidget>(entity);
        PrefabInstance* prefab = registry.TryGet<PrefabInstance>(entity);
        MeshRenderer* mesh = registry.TryGet<MeshRenderer>(entity);
        ModelInstance* instance = registry.TryGet<ModelInstance>(entity);
        const auto meshModel = mesh ? relocateModel(mesh->model) : std::optional<ModelHandle>{};
        const auto instanceModel = instance ? relocateModel(instance->model) : std::optional<ModelHandle>{};
        const auto scriptPath = script ? RelocateReference(script->graph, source, target, wasDirectory)
                                        : std::optional<fs::path>{};
        const auto audioPath = audio ? RelocateReference(audio->sound, source, target, wasDirectory)
                                     : std::optional<fs::path>{};
        const auto imagePath = widget ? RelocateReference(widget->image, source, target, wasDirectory)
                                      : std::optional<fs::path>{};
        const auto prefabPath = prefab ? RelocateReference(prefab->prefab, source, target, wasDirectory)
                                       : std::optional<fs::path>{};
        if (!meshModel && !instanceModel && !scriptPath && !audioPath && !imagePath && !prefabPath)
            return;
        const std::string before = SnapshotEntityState(m_Ctx.scene, entity);
        if (meshModel) mesh->model = *meshModel;
        if (instanceModel) instance->model = *instanceModel;
        if (scriptPath) script->graph = PathToUtf8(*scriptPath);
        if (audioPath) audio->sound = PathToUtf8(*audioPath);
        if (imagePath) widget->image = PathToUtf8(*imagePath);
        if (prefabPath) prefab->prefab = PathToUtf8(*prefabPath);
        edits.push_back({UuidOf(entity), before});
    });
    if (!edits.empty())
        PushStateChange("Repair references after relocation", std::move(edits));
    if (const auto relocated = RelocateReference(PathToUtf8(m_ContentSelected), source, target, wasDirectory))
        SelectContentFile(*relocated);
    if (m_ContentDir == source || (wasDirectory && !m_ContentDir.lexically_relative(source).empty() &&
                                   *m_ContentDir.lexically_relative(source).begin() != ".."))
        m_ContentDir = target / m_ContentDir.lexically_relative(source);
    RefreshContent();
    return true;
}

void Editor::DrawContentBrowser()
{
    UpdateContentImport();
    if (!ImGui::Begin("Content", &m_ShowContent)) {
        ImGui::End();
        return;
    }
    if (m_ContentScanTime < 0.0 || ImGui::GetTime() - m_ContentScanTime > 1.0) // cheap: one directory
        RefreshContent();
    const fs::path  root = ContentRoot();
    std::error_code ec;

    // Toolbar: up, breadcrumb, new, filter.
    ImGui::BeginDisabled(m_ContentDir == root);
    if (ImGui::Button("Up")) {
        ReleaseContentPreview();
        m_ContentSelected.clear();
        m_ContentDir = m_ContentDir.parent_path();
        RefreshContent();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ New"))
        ImGui::OpenPopup("content new");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_ImportWorker.joinable() || m_DialogPurpose != DialogPurpose::None || m_FileDialog->IsOpen());
    if (ImGui::Button("Import file...")) {
        m_ImportTargetDir = m_ContentDir;
        m_DialogPurpose = DialogPurpose::ImportContent;
        m_FileDialog->Open("Import into Content", FileDialog::Mode::Open, fs::current_path(),
                           {".glb", ".gltf", ".png", ".jpg", ".jpeg", ".ktx2", ".wav", ".ogg", ".mp3", ".flac"});
    }
    ImGui::EndDisabled();
    if (m_ImportWorker.joinable()) {
        const std::uint64_t total = m_ImportTotal.load(std::memory_order_relaxed);
        const std::uint64_t copied = m_ImportBytes.load(std::memory_order_relaxed);
        const float progress = total == 0 ? 1.0f : std::clamp(static_cast<float>(copied) / static_cast<float>(total), 0.0f, 1.0f);
        ImGui::SameLine();
        ImGui::ProgressBar(progress, ImVec2(130.0f, 0.0f), "Importing");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Preview kind");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::BeginCombo("##imageImportKind", ToString(m_TextureImportKind))) {
        constexpr std::array kinds{TextureKind::Color, TextureKind::Linear, TextureKind::Normal};
        for (TextureKind kind : kinds) {
            if (ImGui::Selectable(ToString(kind), kind == m_TextureImportKind)) {
                m_TextureImportKind = kind;
                if (!m_ContentSelected.empty()) {
                    const fs::path selected = m_ContentSelected;
                    ReleaseContentPreview();
                    m_ContentSelected.clear();
                    SelectContentFile(selected);
                }
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginPopup("content new")) {
        if (ImGui::MenuItem("Folder")) {
            fs::create_directory(UniquePath(m_ContentDir, "NewFolder", ""), ec);
            RefreshContent();
        }
        if (ImGui::MenuItem("Blueprint")) {
            const fs::path file = UniquePath(m_ContentDir, "NewBlueprint", ".ugraph");
            if (m_Graphs->New(file)) {
                m_ShowBlueprint = true;
                m_Graphs->Focus();
            }
            RefreshContent();
        }
        // Blueprint types / libraries: shared by every blueprint of the project (name = file name).
        const auto newType = [&](const char* stem, const char* extension, const std::function<void(const fs::path&, const std::string&)>& write) {
            const fs::path file = UniquePath(m_ContentDir, stem, extension);
            try {
                write(file, PathToUtf8(file.stem()));
            } catch (const std::exception& e) {
                m_Status = std::string("Cannot create: ") + e.what();
                return;
            }
            ReloadScriptRegistry();
            RefreshContent();
            OpenAsset(file);
        };
        if (ImGui::MenuItem("Blueprint library"))
            newType("NewLibrary", ".ugraph", [](const fs::path& file, const std::string&) {
                ScriptGraph library;
                library.library = true;
                library.AddFunction("MyFunction");
                SaveScriptGraph(file, library);
            });
        if (ImGui::MenuItem("Enum"))
            newType("NewEnum", ".uenum", [](const fs::path& file, const std::string& name) {
                ScriptRegistry::SaveEnumFile(file, {name, {"First", "Second"}, file});
            });
        if (ImGui::MenuItem("Struct"))
            newType("NewStruct", ".ustruct", [](const fs::path& file, const std::string& name) {
                ScriptRegistry::SaveStructFile(file, {name, {{"Value", PinType::Float, 0.0f}}, file});
            });
        if (ImGui::MenuItem("Interface"))
            newType("NewInterface", ".uinterface", [](const fs::path& file, const std::string& name) {
                ScriptRegistry::SaveInterfaceFile(file, {name, {{"Interact", {}, {}}}, file});
            });
        if (ImGui::MenuItem("Scene")) {
            std::ofstream(UniquePath(m_ContentDir, "NewScene", ".scene.json")) << "{\n  \"version\": 1,\n  \"entities\": []\n}\n";
            RefreshContent();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        RefreshContent();
    ImGui::SameLine();
    if (ImGui::Button("Show in folder"))
        (void)OpenInFileBrowser(m_ContentDir);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##filter", "Filter", &m_ContentFilter);
    ImGui::SameLine();
    const fs::path relative = m_ContentDir.lexically_relative(root.parent_path());
    ImGui::TextDisabled("%s", PathToUtf8(relative.empty() ? m_ContentDir : relative).c_str());

    // Items.
    const std::string filter = Lower(m_ContentFilter);
    std::optional<fs::path> enter;
    if (ImGui::BeginChild("items", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        const float width   = ImGui::GetContentRegionAvail().x;
        const float cell    = 150.0f;
        const int   columns = std::max(1, static_cast<int>(width / cell));
        if (ImGui::BeginTable("grid", columns)) {
            for (const ContentItem& item : m_ContentItems) {
                if (!filter.empty() && Lower(item.label).find(filter) == std::string::npos)
                    continue;
                ImGui::TableNextColumn();
                ImGui::PushID(item.label.c_str());
                const int  kind     = static_cast<int>(item.kind);
                const bool selected = m_ContentSelected == item.path;
                ImGui::PushStyleColor(ImGuiCol_Text, KindColor(kind));
                ImGui::TextUnformatted(kKindTags[kind]);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Selectable(item.label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    SelectContentFile(item.path);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (item.kind == ContentItem::Kind::Folder)
                            enter = item.path;
                        else
                            OpenAsset(item.path);
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", PathToUtf8(item.path).c_str());
                if (item.kind != ContentItem::Kind::Folder && ImGui::BeginDragDropSource()) {
                    const std::string payload = PathToUtf8(item.path);
                    ImGui::SetDragDropPayload(kContentPayload, payload.c_str(), payload.size() + 1);
                    ImGui::Text("%s %s", kKindTags[kind], item.label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("item")) {
                    SelectContentFile(item.path);
                    if (item.kind != ContentItem::Kind::Other && item.kind != ContentItem::Kind::Texture &&
                        item.kind != ContentItem::Kind::Sound && ImGui::MenuItem(item.kind == ContentItem::Kind::Folder ? "Open" : "Open / place")) {
                        if (item.kind == ContentItem::Kind::Folder)
                            enter = item.path;
                        else
                            OpenAsset(item.path);
                    }
                    if (item.kind == ContentItem::Kind::Scene && m_Ctx.project &&
                        ImGui::MenuItem("Set as start scene")) {
                        m_Ctx.project->settings.startScene = m_Ctx.project->Relative(item.path);
                        m_Status = m_Ctx.project->Save() ? "Start scene: " + item.label : "Saving the project failed";
                    }
                    if (item.kind == ContentItem::Kind::Blueprint && Selected() != NullEntity &&
                        ImGui::MenuItem("Assign to selection"))
                        AssignScript(Selected(), item.path);
                    if (item.kind == ContentItem::Kind::Sound) {
                        if (m_Ctx.audio && ImGui::MenuItem(m_Ctx.audio->Previewing() ? "Stop preview" : "Preview"))
                            OpenAsset(item.path);
                        if (ImGui::MenuItem("Place audio source"))
                            CreateAudioEntity(item.path, PlacementPoint(std::max(m_Ctx.camera.moveSpeed, 1.0f) * 2.0f));
                        if (Selected() != NullEntity && ImGui::MenuItem("Assign to selection"))
                            AssignSound(Selected(), item.path);
                    }
                    if (ImGui::MenuItem("Rename...")) {
                        m_RenameTarget = item.path;
                        m_RenameText   = item.label;
                    }
                    if (ImGui::MenuItem("Move to folder...")) {
                        m_MoveTarget = item.path;
                        m_DialogPurpose = DialogPurpose::MoveContent;
                        m_FileDialog->Open("Move Content item", FileDialog::Mode::Folder, ContentRoot(), {});
                    }
                    if (ImGui::MenuItem("Delete..."))
                        m_DeleteTarget = item.path;
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (m_ContentItems.empty())
            ImGui::TextDisabled("Empty folder. Use + New, or put models / textures here.");
        if (m_ContentSelected.empty() || !fs::exists(m_ContentSelected, ec)) {
            if (!m_ContentSelected.empty())
                ReleaseContentPreview();
            m_ContentSelected.clear();
        } else if (m_ContentSelected.parent_path() == m_ContentDir) {
            const std::string selectedName = Lower(PathToUtf8(m_ContentSelected.filename()));
            if (EndsWith(selectedName, ".glb") || EndsWith(selectedName, ".gltf")) {
                ImGui::SeparatorText("Model preview");
                if (const Model* model = m_Ctx.assets.Get(m_ContentPreviewModel)) {
                    const ImVec2 origin = ImGui::GetCursorScreenPos();
                    const ImVec2 avail = ImGui::GetContentRegionAvail();
                    const ImVec2 size(std::min(avail.x, 320.0f), 180.0f);
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(25, 29, 36, 255), 4.0f);
                    const glm::vec3 extent = glm::max(model->boundsMax - model->boundsMin, glm::vec3(0.001f));
                    constexpr float yawCos = 0.819152f, yawSin = 0.573576f;
                    const float projectedWidth = extent.x * yawCos + extent.z * yawSin;
                    const float projectedHeight = extent.y + extent.z * 0.3f;
                    const float scale = 0.72f * std::min(size.x / std::max(projectedWidth, 0.001f),
                                                        size.y / std::max(projectedHeight, 0.001f));
                    const glm::vec3 center = (model->boundsMin + model->boundsMax) * 0.5f;
                    const glm::vec2 screenCenter(origin.x + size.x * 0.5f, origin.y + size.y * 0.52f);
                    const std::size_t triangleCount = std::min(model->collisionIndices.size() / 3, std::size_t{600});
                    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
                        ImVec2 points[3];
                        bool valid = true;
                        for (std::size_t corner = 0; corner < 3; ++corner) {
                            const std::uint32_t index = model->collisionIndices[triangle * 3 + corner];
                            if (index >= model->collisionPositions.size()) { valid = false; break; }
                            const glm::vec3& p = model->collisionPositions[index];
                            const float projectedX = (p.x - center.x) * yawCos + (p.z - center.z) * yawSin;
                            const float projectedY = (p.y - center.y) - (p.z - center.z) * 0.3f;
                            points[corner] = ImVec2(screenCenter.x + projectedX * scale,
                                                    screenCenter.y - projectedY * scale);
                        }
                        if (valid)
                            draw->AddPolyline(points, 3, IM_COL32(115, 190, 245, 180), 1.0f, ImDrawFlags_Closed);
                    }
                    ImGui::Dummy(size);
                    ImGui::Text("%zu mesh(es), %zu material(s)", model->meshes.size(), model->previewMaterials.size());
                    for (std::size_t i = 0; i < model->previewMaterials.size(); ++i) {
                        const MaterialData& material = model->previewMaterials[i];
                        const glm::vec4 color = glm::clamp(material.baseColorFactor, glm::vec4(0.0f), glm::vec4(1.0f));
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::ColorButton("##materialPreview", ImVec4(color.r, color.g, color.b, color.a),
                                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(22, 22));
                        ImGui::SameLine();
                        ImGui::Text("%s  M %.2f  R %.2f", material.name.empty() ? "Material" : material.name.c_str(),
                                     material.metallic, material.roughness);
                        ImGui::PopID();
                    }
                } else if (m_Ctx.assets.State(m_ContentPreviewModel) == AssetState::Failed) {
                    ImGui::TextDisabled("Model failed to load; see the log for details.");
                } else {
                    ImGui::TextDisabled("Loading model preview...");
                }
            } else if (IsSoundFile(m_ContentSelected)) {
                ImGui::SeparatorText("Audio preview");
                if (const std::shared_ptr<const SoundData> sound = m_Ctx.assets.Get(m_ContentPreviewSound)) {
                    const ImVec2 origin = ImGui::GetCursorScreenPos();
                    const ImVec2 avail = ImGui::GetContentRegionAvail();
                    const ImVec2 size(std::min(avail.x, 320.0f), 72.0f);
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(25, 29, 36, 255), 4.0f);
                    const std::size_t buckets = std::max<std::size_t>(1, static_cast<std::size_t>(size.x / 3.0f));
                    if (!m_ContentPreviewWaveformReady) {
                        m_ContentPreviewWaveform.assign(buckets, 0.0f);
                        if (!sound->samples.empty() && sound->channels > 0) {
                            for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
                                const std::size_t firstFrame = bucket * sound->frames / buckets;
                                const std::size_t lastFrame = std::max(firstFrame + 1, (bucket + 1) * sound->frames / buckets);
                                for (std::size_t frame = firstFrame; frame < std::min<std::uint64_t>(lastFrame, sound->frames); ++frame)
                                    for (std::uint32_t channel = 0; channel < sound->channels; ++channel) {
                                        const std::size_t sample = frame * sound->channels + channel;
                                        if (sample < sound->samples.size())
                                            m_ContentPreviewWaveform[bucket] = std::max(m_ContentPreviewWaveform[bucket],
                                                                                        std::abs(sound->samples[sample]));
                                    }
                            }
                        }
                        m_ContentPreviewWaveformReady = true;
                    }
                    if (!sound->samples.empty() && sound->channels > 0) {
                        for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
                            const float halfHeight = std::clamp(m_ContentPreviewWaveform[bucket], 0.025f, 1.0f) * (size.y * 0.43f);
                            const float x = origin.x + (static_cast<float>(bucket) + 0.5f) * size.x / static_cast<float>(buckets);
                            draw->AddLine(ImVec2(x, origin.y + size.y * 0.5f - halfHeight),
                                          ImVec2(x, origin.y + size.y * 0.5f + halfHeight), IM_COL32(95, 205, 165, 255), 2.0f);
                        }
                    } else {
                        draw->AddText(ImVec2(origin.x + 10.0f, origin.y + 27.0f), IM_COL32(175, 185, 200, 255),
                                      sound->Streamed() ? "Streamed audio" : "No waveform data");
                    }
                    ImGui::Dummy(size);
                    ImGui::Text("%.1f s  |  %u Hz  |  %u channel(s)", sound->Duration(), sound->sampleRate, sound->channels);
                    if (m_Ctx.audio && ImGui::Button(m_Ctx.audio->Previewing() ? "Stop audio preview" : "Play"))
                        OpenAsset(m_ContentSelected);
                } else if (m_Ctx.assets.State(m_ContentPreviewSound) == AssetState::Failed) {
                    ImGui::TextDisabled("Audio failed to load; see the log for details.");
                } else {
                    ImGui::TextDisabled("Loading audio preview...");
                }
            } else if (EndsWith(selectedName, ".png") || EndsWith(selectedName, ".jpg") || EndsWith(selectedName, ".jpeg") ||
                EndsWith(selectedName, ".ktx2")) {
                ImGui::SeparatorText("Preview");
                if (const Texture* texture = m_Ctx.assets.Get(m_ContentPreviewHandle)) {
                    const std::uint32_t revision = m_Ctx.assets.Revision(m_ContentPreviewHandle);
                    if (m_ContentPreviewTexture && revision != m_ContentPreviewRevision) {
                        m_ImGui->RemoveTexture(m_ContentPreviewTexture);
                        m_ContentPreviewTexture = 0;
                    }
                    if (!m_ContentPreviewTexture) {
                        m_ContentPreviewTexture = m_ImGui->AddTexture(texture->image.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        m_ContentPreviewRevision = revision;
                    }
                    const float scale = std::min(1.0f, 320.0f / static_cast<float>(std::max({texture->width, texture->height, 1u})));
                    ImGui::Image(ImTextureRef(m_ContentPreviewTexture),
                                 ImVec2(static_cast<float>(texture->width) * scale,
                                        static_cast<float>(texture->height) * scale));
                } else if (m_Ctx.assets.State(m_ContentPreviewHandle) == AssetState::Failed) {
                    ImGui::TextDisabled("Image failed to load; see the log for details.");
                } else {
                    ImGui::TextDisabled("Loading image preview...");
                }
            } else {
                ImGui::SeparatorText("Selected file");
                ImGui::TextWrapped("%s", PathToUtf8(m_ContentSelected.filename()).c_str());
                std::error_code sizeError;
                const std::uintmax_t size = fs::file_size(m_ContentSelected, sizeError);
                if (!sizeError)
                    ImGui::TextDisabled("%llu bytes", static_cast<unsigned long long>(size));
                if (IsSoundFile(m_ContentSelected) && m_Ctx.audio &&
                    ImGui::Button(m_Ctx.audio->Previewing() ? "Stop audio preview" : "Preview audio"))
                    OpenAsset(m_ContentSelected);
            }
        }
    }
    ImGui::EndChild();
    if (enter) {
        ReleaseContentPreview();
        m_ContentSelected.clear();
        m_ContentDir = *enter;
        RefreshContent();
    }

    // Rename / delete.
    if (!m_RenameTarget.empty())
        ImGui::OpenPopup("Rename");
    if (ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool enterPressed = ImGui::InputText("##name", &m_RenameText, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Rename") || enterPressed) && !m_RenameText.empty()) {
            const fs::path previous = m_RenameTarget;
            const fs::path target = m_RenameTarget.parent_path() / PathFromUtf8(m_RenameText);
            const bool wasDirectory = fs::is_directory(previous, ec);
            if (fs::exists(target, ec)) {
                m_Status = "'" + m_RenameText + "' already exists";
            } else {
                fs::rename(m_RenameTarget, target, ec);
                m_Status = ec ? "Rename failed: " + ec.message() : "Renamed to " + m_RenameText;
                if (!ec) {
                    std::size_t storedReferences = 0;
                    std::string referenceError;
                    const bool repairedStored = RepairStoredReferences(ContentRoot(), previous, target, wasDirectory,
                                                                        storedReferences, referenceError);
                    if (!repairedStored) {
                        ec.clear();
                        fs::rename(target, previous, ec);
                        m_Status = ec ? "Rename completed but references could not be repaired or rolled back: " + referenceError
                                      : "Rename cancelled because references could not be repaired: " + referenceError;
                    } else {
                        if (storedReferences > 0)
                            m_Status += std::format("; repaired {} stored reference(s)", storedReferences);
                        if (!referenceError.empty())
                            m_Status += "; reference scan note: " + referenceError;
                        if (m_Ctx.project) {
                            const fs::path start = m_Ctx.project->StartScene();
                            if (const auto relocated = RelocateReference(PathToUtf8(start), previous, target, wasDirectory)) {
                                m_Ctx.project->settings.startScene = m_Ctx.project->Relative(*relocated);
                                m_ProjectDirty = !m_Ctx.project->Save();
                            }
                        }
                        if (const auto relocated = RelocateReference(PathToUtf8(m_ScenePath), previous, target, wasDirectory))
                            m_ScenePath = *relocated;

                        std::unordered_map<ModelHandle, ModelHandle> relocatedModels;
                        const auto relocateModel = [&](ModelHandle oldHandle) -> std::optional<ModelHandle> {
                            if (const auto it = relocatedModels.find(oldHandle); it != relocatedModels.end())
                                return it->second;
                            const ModelSource source = m_Ctx.assets.Source(oldHandle);
                            if (source.file.empty())
                                return std::nullopt;
                            const auto path = RelocateReference(PathToUtf8(source.file), previous, target, wasDirectory);
                            if (!path)
                                return std::nullopt;
                            const std::size_t ownedRefs = static_cast<std::size_t>(
                                std::ranges::count(m_Ctx.modelRefs, oldHandle));
                            const ModelHandle newHandle = m_Ctx.assets.LoadModel(*path);
                            for (std::size_t i = 1; i < std::max<std::size_t>(ownedRefs, 1); ++i)
                                (void)m_Ctx.assets.LoadModel(*path);
                            if (ownedRefs == 0) {
                                m_Ctx.modelRefs.push_back(newHandle);
                            } else {
                                for (ModelHandle& handle : m_Ctx.modelRefs)
                                    if (handle == oldHandle)
                                        handle = newHandle;
                                for (std::size_t i = 1; i < ownedRefs; ++i)
                                    m_Ctx.assets.Release(oldHandle);
                                m_Ctx.modelRefs.push_back(oldHandle); // keep the old source alive for scene undo
                            }
                            relocatedModels.emplace(oldHandle, newHandle);
                            return newHandle;
                        };
                        std::vector<StateEdit> edits;
                        Registry& registry = m_Ctx.scene.GetRegistry();
                        registry.ViewOf<Hierarchy>().Each([&](Entity entity, Hierarchy&) {
                        ScriptComponent* script = registry.TryGet<ScriptComponent>(entity);
                        AudioSource* audio = registry.TryGet<AudioSource>(entity);
                        UiWidget* widget = registry.TryGet<UiWidget>(entity);
                        PrefabInstance* prefab = registry.TryGet<PrefabInstance>(entity);
                        MeshRenderer* mesh = registry.TryGet<MeshRenderer>(entity);
                        ModelInstance* instance = registry.TryGet<ModelInstance>(entity);
                        const auto meshModel = mesh ? relocateModel(mesh->model) : std::optional<ModelHandle>{};
                        const auto instanceModel = instance ? relocateModel(instance->model) : std::optional<ModelHandle>{};
                        const auto scriptPath = script ? RelocateReference(script->graph, previous, target, wasDirectory)
                                                        : std::optional<fs::path>{};
                        const auto audioPath = audio ? RelocateReference(audio->sound, previous, target, wasDirectory)
                                                     : std::optional<fs::path>{};
                        const auto imagePath = widget ? RelocateReference(widget->image, previous, target, wasDirectory)
                                                      : std::optional<fs::path>{};
                        const auto prefabPath = prefab ? RelocateReference(prefab->prefab, previous, target, wasDirectory)
                                                       : std::optional<fs::path>{};
                        const bool moveScript = scriptPath.has_value();
                        const bool moveAudio = audioPath.has_value();
                        const bool moveImage = imagePath.has_value();
                        const bool movePrefab = prefabPath.has_value();
                        if (!moveScript && !moveAudio && !moveImage && !movePrefab && !meshModel && !instanceModel)
                            return;
                        const std::string before = SnapshotEntityState(m_Ctx.scene, entity);
                        if (moveScript)
                            script->graph = PathToUtf8(*scriptPath);
                        if (moveAudio)
                            audio->sound = PathToUtf8(*audioPath);
                        if (moveImage)
                            widget->image = PathToUtf8(*imagePath);
                        if (movePrefab)
                            prefab->prefab = PathToUtf8(*prefabPath);
                        if (meshModel)
                            mesh->model = *meshModel;
                        if (instanceModel)
                            instance->model = *instanceModel;
                        edits.push_back({UuidOf(entity), before});
                        });
                        if (!edits.empty())
                            PushStateChange("Repair references after rename", std::move(edits));
                        if (const auto relocated = RelocateReference(PathToUtf8(m_ContentSelected), previous, target, wasDirectory))
                            SelectContentFile(*relocated);
                    }
                }
            }
            m_RenameTarget.clear();
            RefreshContent();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_RenameTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!m_DeleteTarget.empty())
        ImGui::OpenPopup("Delete?");
    if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s'? This cannot be undone.", PathToUtf8(m_DeleteTarget.filename()).c_str());
        if (ImGui::Button("Delete")) {
            const fs::path deleted = m_DeleteTarget;
            fs::remove_all(m_DeleteTarget, ec);
            m_Status = ec ? "Delete failed: " + ec.message() : "Deleted " + PathToUtf8(m_DeleteTarget.filename());
            if (!ec && m_Ctx.project) {
                const fs::path start = m_Ctx.project->StartScene();
                const fs::path relative = start.lexically_normal().lexically_relative(deleted.lexically_normal());
                if (!relative.empty() && !relative.is_absolute() && relative.begin() != relative.end() && *relative.begin() != "..") {
                    m_Ctx.project->settings.startScene.clear();
                    m_ProjectDirty = !m_Ctx.project->Save();
                }
            }
            if (!ec && !m_ScenePath.empty()) {
                const fs::path relative = m_ScenePath.lexically_normal().lexically_relative(deleted.lexically_normal());
                if (relative.empty() || (!relative.is_absolute() && relative.begin() != relative.end() && *relative.begin() != ".."))
                    m_ScenePath.clear();
            }
            m_DeleteTarget.clear();
            RefreshContent();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_DeleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void Editor::DrawProjectSettings()
{
    if (!m_Ctx.project) {
        m_ShowProjectSettings = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(460.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &m_ShowProjectSettings)) {
        ImGui::End();
        return;
    }
    Project&         project = *m_Ctx.project;
    ProjectSettings& s       = project.settings;
    ImGui::TextDisabled("%s", PathToUtf8(project.File()).c_str());
    ImGui::InputText("Name", &s.name);
    if (!IsValidProjectName(s.name))
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Letters, digits, '_', '-' and spaces only");

    // Start scene: any scene file below Content/.
    if (ImGui::BeginCombo("Start scene", s.startScene.empty() ? "(none)" : s.startScene.c_str())) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(project.ContentDirectory(), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = Lower(PathToUtf8(it->path().filename()));
            if (!EndsWith(name, ".scene.json"))
                continue;
            const std::string relative = project.Relative(it->path());
            if (ImGui::Selectable(relative.c_str(), relative == s.startScene))
                s.startScene = relative;
        }
        ImGui::EndCombo();
    }
    int size[2] = {static_cast<int>(s.windowWidth), static_cast<int>(s.windowHeight)};
    if (ImGui::InputInt2("Window size", size)) {
        s.windowWidth  = static_cast<std::uint32_t>(std::clamp(size[0], 320, 16384));
        s.windowHeight = static_cast<std::uint32_t>(std::clamp(size[1], 200, 16384));
    }
    ImGui::Checkbox("Fullscreen", &s.fullscreen);
    ImGui::SameLine();
    ImGui::Checkbox("VSync", &s.vsync);
    ImGui::TextDisabled("Audio mixer: Renderer panel > Audio (stored here too).");
    // Input actions / axes for blueprints (key names as in Is Key Down, plus MouseLeft / Right / Middle).
    if (ImGui::CollapsingHeader("Input")) {
        InputMap& input = s.input;
        const auto keyCombo = [&](const char* id, std::string& key) {
            bool changed = false;
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::BeginCombo(id, key.c_str())) {
                std::vector<std::string> names(KeyNames().begin(), KeyNames().end());
                names.insert(names.end(), {"MouseLeft", "MouseRight", "MouseMiddle"});
                for (const std::string& k : names)
                    if (ImGui::Selectable(k.c_str(), k == key)) {
                        key     = k;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            return changed;
        };
        bool changed = false;
        ImGui::SeparatorText("Actions");
        std::optional<std::size_t> removeAction;
        for (std::size_t i = 0; i < input.actions.size(); ++i) {
            InputActionBinding& a = input.actions[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(120.0f);
            changed |= ImGui::InputText("##action", &a.name);
            std::optional<std::size_t> removeKey;
            for (std::size_t k = 0; k < a.keys.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                ImGui::SameLine();
                changed |= keyCombo("##key", a.keys[k]);
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    removeKey = k;
                ImGui::PopID();
            }
            if (removeKey) {
                a.keys.erase(a.keys.begin() + static_cast<std::ptrdiff_t>(*removeKey));
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ key")) {
                a.keys.push_back("Space");
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                removeAction = i;
            ImGui::PopID();
        }
        if (removeAction) {
            input.actions.erase(input.actions.begin() + static_cast<std::ptrdiff_t>(*removeAction));
            changed = true;
        }
        if (ImGui::SmallButton("+ Action")) {
            input.actions.push_back({"Action" + std::to_string(input.actions.size() + 1), {"Space"}});
            changed = true;
        }
        ImGui::SeparatorText("Axes");
        std::optional<std::size_t> removeAxis;
        for (std::size_t i = 0; i < input.axes.size(); ++i) {
            InputAxisBinding& a = input.axes[i];
            ImGui::PushID(static_cast<int>(i) + 1000);
            ImGui::SetNextItemWidth(120.0f);
            changed |= ImGui::InputText("##axis", &a.name);
            ImGui::SameLine();
            if (ImGui::SmallButton("+ key")) {
                a.keys.push_back({"W", 1.0f});
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                removeAxis = i;
            std::optional<std::size_t> removeKey;
            for (std::size_t k = 0; k < a.keys.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                ImGui::Indent();
                changed |= keyCombo("##key", a.keys[k].key);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f);
                changed |= ImGui::DragFloat("Scale", &a.keys[k].scale, 0.05f, -10.0f, 10.0f, "%.2f");
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    removeKey = k;
                ImGui::Unindent();
                ImGui::PopID();
            }
            if (removeKey) {
                a.keys.erase(a.keys.begin() + static_cast<std::ptrdiff_t>(*removeKey));
                changed = true;
            }
            ImGui::PopID();
        }
        if (removeAxis) {
            input.axes.erase(input.axes.begin() + static_cast<std::ptrdiff_t>(*removeAxis));
            changed = true;
        }
        if (ImGui::SmallButton("+ Axis")) {
            input.axes.push_back({"Axis" + std::to_string(input.axes.size() + 1), {{"W", 1.0f}, {"S", -1.0f}}});
            changed = true;
        }
        ImGui::TextDisabled("Right-click a key of an action to remove it. Used from the next Play.");
        m_ProjectDirty |= changed;
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!IsValidProjectName(s.name));
    if (ImGui::Button("Save")) {
        std::string error;
        const bool  saved = project.Save(&error);
        m_ProjectDirty    = m_ProjectDirty && !saved;
        m_Status          = saved ? "Project saved" : "Project save failed: " + error;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from file"))
        if (const auto loaded = Project::Load(project.File())) {
            project.settings = loaded->settings;
            if (m_Ctx.audio)
                m_Ctx.audio->Apply(project.settings.audio);
            m_ProjectDirty = false;
        }
    ImGui::End();
}


// --- Blueprint types (enums, structs, interfaces) ------------------------------------------------

struct Editor::TypeEdit {
    fs::path                                                   file;
    std::variant<ScriptEnum, ScriptStructDef, ScriptInterface> def;
    bool                                                       dirty = false;
};

void Editor::OpenTypeFile(const fs::path& file)
{
    const std::string name = Lower(PathToUtf8(file.filename()));
    try {
        auto edit  = std::make_shared<TypeEdit>();
        edit->file = file;
        if (EndsWith(name, ".uenum"))
            edit->def = ScriptRegistry::LoadEnumFile(file);
        else if (EndsWith(name, ".ustruct"))
            edit->def = ScriptRegistry::LoadStructFile(file);
        else
            edit->def = ScriptRegistry::LoadInterfaceFile(file);
        m_TypeEdit  = std::move(edit);
        m_ShowTypes = true;
    } catch (const std::exception& e) {
        m_Status = std::string("Cannot open: ") + e.what();
    }
}

void Editor::DrawBlueprintTypes()
{
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(ImVec2(center.x - 700.0f, center.y - 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Blueprint Types", &m_ShowTypes)) {
        ImGui::End();
        return;
    }
    // Left: everything the registry knows (from the project's files).
    if (ImGui::BeginChild("##typelist", ImVec2(200.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
        const auto entry = [&](const char* tag, const std::string& name, const fs::path& file) {
            const bool selected = m_TypeEdit && m_TypeEdit->file == file;
            if (ImGui::Selectable(std::format("{} {}##{}", tag, name, PathToUtf8(file)).c_str(), selected) && !file.empty())
                OpenTypeFile(file);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", file.empty() ? "registered in code" : PathToUtf8(file).c_str());
        };
        ImGui::SeparatorText("Enums");
        for (const std::string& n : ScriptRegistry::EnumNames())
            entry("E", n, ScriptRegistry::FindEnum(n)->file);
        ImGui::SeparatorText("Structs");
        for (const std::string& n : ScriptRegistry::StructNames())
            entry("S", n, ScriptRegistry::FindStruct(n)->file);
        ImGui::SeparatorText("Interfaces");
        for (const std::string& n : ScriptRegistry::InterfaceNames())
            entry("I", n, ScriptRegistry::FindInterface(n)->file);
        ImGui::SeparatorText("Libraries");
        for (const std::string& n : ScriptRegistry::LibraryNames())
            if (ImGui::Selectable(("L " + n).c_str()))
                OpenAsset(ScriptRegistry::FindLibrary(n)->file);
        ImGui::Spacing();
        if (ImGui::SmallButton("Reload all"))
            ReloadScriptRegistry();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##typeedit");
    if (!m_TypeEdit) {
        ImGui::TextDisabled("Pick a type, or create one in the Content browser (+ New).");
    } else {
        TypeEdit& t = *m_TypeEdit;
        ImGui::TextDisabled("%s", PathToUtf8(t.file).c_str());
        bool changed = false;
        // A parameter list (interface functions).
        const auto params = [&](const char* title, std::vector<ScriptParam>& list, int base) {
            ImGui::TextDisabled("%s", title);
            std::optional<std::size_t> remove;
            for (std::size_t i = 0; i < list.size(); ++i) {
                ImGui::PushID(base + static_cast<int>(i));
                ImGui::SetNextItemWidth(110.0f);
                changed |= ImGui::InputText("##p", &list[i].name);
                ImGui::SameLine();
                changed |= ScriptGraphEditor::EditType("##t", list[i].type, 110.0f);
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    remove = i;
                ImGui::PopID();
            }
            if (remove) {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(*remove));
                changed = true;
            }
            ImGui::PushID(base + 999);
            if (ImGui::SmallButton("+")) {
                list.push_back({"Value" + std::to_string(list.size() + 1), PinType::Float});
                changed = true;
            }
            ImGui::PopID();
        };
        if (auto* e = std::get_if<ScriptEnum>(&t.def)) {
            ImGui::Text("Enum %s", e->name.c_str());
            std::optional<std::size_t> remove;
            for (std::size_t i = 0; i < e->values.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%2zu", i);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(180.0f);
                changed |= ImGui::InputText("##v", &e->values[i]);
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    remove = i;
                ImGui::PopID();
            }
            if (remove) {
                e->values.erase(e->values.begin() + static_cast<std::ptrdiff_t>(*remove));
                changed = true;
            }
            if (ImGui::SmallButton("+ Value")) {
                e->values.push_back("Value" + std::to_string(e->values.size()));
                changed = true;
            }
            ImGui::TextDisabled("Values are stored by index: removing one shifts the later ones.");
        } else if (auto* st = std::get_if<ScriptStructDef>(&t.def)) {
            ImGui::Text("Struct %s", st->name.c_str());
            std::optional<std::size_t> remove;
            for (std::size_t i = 0; i < st->fields.size(); ++i) {
                ScriptStructField& f = st->fields[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(120.0f);
                changed |= ImGui::InputText("##f", &f.name);
                ImGui::SameLine();
                if (ScriptGraphEditor::EditType("##t", f.type, 110.0f)) {
                    f.value = DefaultValue(f.type);
                    changed = true;
                }
                ImGui::SameLine();
                if (f.type.kind != PinKind::Entity)
                    changed |= ScriptGraphEditor::EditValue("##d", f.value, f.type, 140.0f);
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    remove = i;
                ImGui::PopID();
            }
            if (remove) {
                st->fields.erase(st->fields.begin() + static_cast<std::ptrdiff_t>(*remove));
                changed = true;
            }
            if (ImGui::SmallButton("+ Field")) {
                st->fields.push_back({"Field" + std::to_string(st->fields.size() + 1), PinType::Float, 0.0f});
                changed = true;
            }
        } else if (auto* in = std::get_if<ScriptInterface>(&t.def)) {
            ImGui::Text("Interface %s", in->name.c_str());
            std::optional<std::size_t> remove;
            for (std::size_t i = 0; i < in->functions.size(); ++i) {
                ScriptInterfaceFunction& f = in->functions[i];
                ImGui::PushID(static_cast<int>(i) * 10000);
                ImGui::SetNextItemWidth(160.0f);
                changed |= ImGui::InputText("##fn", &f.name);
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    remove = i;
                ImGui::Indent();
                params("Inputs", f.inputs, 1000);
                params("Outputs", f.outputs, 2000);
                ImGui::Unindent();
                ImGui::PopID();
            }
            if (remove) {
                in->functions.erase(in->functions.begin() + static_cast<std::ptrdiff_t>(*remove));
                changed = true;
            }
            if (ImGui::SmallButton("+ Function")) {
                in->functions.push_back({"Function" + std::to_string(in->functions.size() + 1), {}, {}});
                changed = true;
            }
            ImGui::TextDisabled("Blueprints implementing it need matching functions (Compile shows them).");
        }
        t.dirty |= changed;
        ImGui::Separator();
        ImGui::BeginDisabled(!t.dirty);
        if (ImGui::Button("Save")) {
            try {
                std::visit(
                    [&](const auto& def) {
                        using T = std::decay_t<decltype(def)>;
                        if constexpr (std::is_same_v<T, ScriptEnum>)
                            ScriptRegistry::SaveEnumFile(t.file, def);
                        else if constexpr (std::is_same_v<T, ScriptStructDef>)
                            ScriptRegistry::SaveStructFile(t.file, def);
                        else
                            ScriptRegistry::SaveInterfaceFile(t.file, def);
                    },
                    t.def);
                t.dirty = false;
                const std::vector<std::string> problems = ReloadScriptRegistry();
                m_Status = problems.empty() ? "Saved " + PathToUtf8(t.file.filename()) : "Saved (with problems, see Blueprint Types)";
            } catch (const std::exception& e) {
                m_Status = std::string("Save failed: ") + e.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
            OpenTypeFile(t.file);
        ImGui::EndDisabled();
        // Problems of all definitions.
        for (const std::string& problem : ScriptRegistry::Validate())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.35f, 1.0f), "%s", problem.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace Engine
