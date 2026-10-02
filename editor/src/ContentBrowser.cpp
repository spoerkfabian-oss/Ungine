#include "Editor/Editor.h"
#include "Editor/ScriptGraphEditor.h"
#include "FileDialog.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
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
    default: return {0.65f, 0.65f, 0.65f, 1.0f};
    }
}

constexpr const char* kKindTags[] = {"DIR", "SCENE", "BP", "PFB", "MODEL", "TEX", "SND", "FILE"};

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
        m_ContentItems.push_back(std::move(item));
    }
    std::ranges::sort(m_ContentItems, [](const ContentItem& a, const ContentItem& b) {
        return a.kind != b.kind ? a.kind < b.kind : Lower(a.label) < Lower(b.label);
    });
    m_ContentScanTime = ImGui::GetTime();
}

void Editor::DrawContentBrowser()
{
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
        m_ContentDir = m_ContentDir.parent_path();
        RefreshContent();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ New"))
        ImGui::OpenPopup("content new");
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
                    m_ContentSelected = item.path;
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
                    m_ContentSelected = item.path;
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
    }
    ImGui::EndChild();
    if (enter) {
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
            const fs::path target = m_RenameTarget.parent_path() / PathFromUtf8(m_RenameText);
            if (fs::exists(target, ec)) {
                m_Status = "'" + m_RenameText + "' already exists";
            } else {
                fs::rename(m_RenameTarget, target, ec);
                m_Status = ec ? "Rename failed: " + ec.message() : "Renamed to " + m_RenameText;
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
            fs::remove_all(m_DeleteTarget, ec);
            m_Status = ec ? "Delete failed: " + ec.message() : "Deleted " + PathToUtf8(m_DeleteTarget.filename());
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

} // namespace Engine
