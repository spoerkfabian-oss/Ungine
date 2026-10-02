// Editor operations that change the scene (all undoable) and scene files.
#include "Editor/Editor.h"
#include "FileDialog.h"
#include "History.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/Window.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Script/ScriptSystem.h"
#include "Editor/ScriptGraphEditor.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <imgui.h>

#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <memory>

namespace Engine {

std::uint64_t Editor::UuidOf(Entity entity) const
{
    return m_Ctx.scene.GetRegistry().Get<Uuid>(entity).value;
}

void Editor::PushCommand(EditCommand command)
{
    if (m_PlayState == PlayState::Edit) // play-mode changes are discarded by Stop
        m_History->Push(std::move(command));
}

bool Editor::Undo()
{
    if (m_PlayState != PlayState::Edit)
        return false;
    m_GizmoEdit.reset();
    m_InspectorEdit.reset();
    const bool done = m_History->Undo();
    ValidateSelection();
    m_Ctx.scene.UpdateTransforms();
    return done;
}

bool Editor::Redo()
{
    if (m_PlayState != PlayState::Edit)
        return false;
    m_GizmoEdit.reset();
    m_InspectorEdit.reset();
    const bool done = m_History->Redo();
    ValidateSelection();
    m_Ctx.scene.UpdateTransforms();
    return done;
}

// Component edits: `before` was captured when the edit started, the current state is the result.
void Editor::PushStateChange(std::string label, std::vector<StateEdit> before)
{
    std::vector<StateEdit> after;
    bool                   changed = false;
    for (const StateEdit& b : before) {
        const Entity e = m_Ctx.scene.FindByUuid(b.uuid);
        if (e == NullEntity)
            continue;
        after.push_back({b.uuid, SnapshotEntityState(m_Ctx.scene, e)});
        changed |= after.back().before != b.before;
    }
    if (!changed)
        return;
    const auto apply = [scene = &m_Ctx.scene](const std::vector<StateEdit>& states) {
        for (const StateEdit& s : states)
            if (const Entity e = scene->FindByUuid(s.uuid); e != NullEntity)
                ApplyEntityState(*scene, e, s.before);
    };
    PushCommand({std::move(label), [apply, before] { apply(before); }, [apply, after] { apply(after); }});
}

// Entities that were just created: undo destroys them, redo restores the snapshot (same UUIDs).
void Editor::PushCreated(std::string label, std::span<const Entity> roots)
{
    std::vector<std::uint64_t> uuids;
    for (Entity e : roots)
        uuids.push_back(UuidOf(e));
    const std::string snapshot = SnapshotEntities(m_Ctx.scene, roots);
    Scene*            scene    = &m_Ctx.scene;
    PushCommand({std::move(label), [this, uuids] { DestroyByUuids(uuids); },
                 [scene, snapshot] { (void)RestoreEntities(*scene, snapshot, RestoreMode::Original); }});
}

void Editor::DestroyByUuids(std::span<const std::uint64_t> uuids)
{
    for (std::uint64_t uuid : uuids)
        if (const Entity e = m_Ctx.scene.FindByUuid(uuid); e != NullEntity)
            m_Ctx.scene.DestroyEntity(e);
    ValidateSelection();
}

Entity Editor::CreateEntity(std::string name, Entity parent)
{
    const Entity e = m_Ctx.scene.CreateEntity(std::move(name), parent);
    const Entity roots[] = {e};
    PushCreated("Create entity", roots);
    return e;
}

Entity Editor::CreateLight(LightType type, Entity parent)
{
    // Scaled like the fly camera (its speed follows the scene size in the Sandbox).
    const float scale = std::max(m_Ctx.camera.moveSpeed, 0.1f);
    const Light light{.type = type, .intensity = 4.0f * scale * scale, .range = 4.0f * scale};
    const Entity e = m_Ctx.scene.CreateEntity(type == LightType::Spot ? "Spot Light" : "Point Light", parent);
    Transform&   t = m_Ctx.scene.EditTransform(e);
    if (parent == NullEntity) // a little in front of the surface in view (BVH raycast)
        t.position = PlacementPoint(2.0f * scale) - m_Ctx.camera.Forward() * (0.5f * scale);
    if (type == LightType::Spot)
        t.rotation = glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)); // pointing down
    m_Ctx.scene.GetRegistry().Emplace<Light>(e, light);
    const Entity roots[] = {e};
    PushCreated(type == LightType::Spot ? "Create spot light" : "Create point light", roots);
    return e;
}

Entity Editor::CreatePrimitiveEntity(PrimitiveShape shape)
{
    const float       scale  = std::max(m_Ctx.camera.moveSpeed, 0.1f);
    const ModelHandle handle = m_Ctx.assets.CreatePrimitive({.shape = shape, .size = scale});
    m_Ctx.modelRefs.push_back(handle); // renders once the (async) upload is done
    static constexpr const char* kNames[] = {"Plane", "Cube", "Sphere", "Capsule"};
    const char*                  name     = kNames[static_cast<std::size_t>(shape)];
    const Entity                 e        = m_Ctx.scene.CreateEntity(name);
    const float lift = shape == PrimitiveShape::Capsule ? scale : 0.5f * scale; // rest on the surface
    m_Ctx.scene.EditTransform(e).position = PlacementPoint(3.0f * scale) + glm::vec3(0.0f, lift, 0.0f);
    m_Ctx.scene.GetRegistry().Emplace<MeshRenderer>(e, MeshRenderer{.model = handle, .meshIndex = 0});
    const Entity roots[] = {e};
    PushCreated(std::string("Create ") + name, roots);
    return e;
}

void Editor::DuplicateSelection()
{
    const std::vector<Entity> roots = SelectionRoots();
    if (roots.empty())
        return;
    const std::vector<Entity> copies =
        RestoreEntities(m_Ctx.scene, SnapshotEntities(m_Ctx.scene, roots), RestoreMode::Duplicate);
    PushCreated(copies.size() == 1 ? "Duplicate" : std::format("Duplicate {} entities", copies.size()), copies);
    m_Selection = copies;
    m_Ctx.scene.UpdateTransforms();
}

void Editor::DeleteSelection()
{
    const std::vector<Entity> roots = SelectionRoots();
    if (roots.empty())
        return;
    std::vector<std::uint64_t> uuids;
    for (Entity e : roots)
        uuids.push_back(UuidOf(e));
    // Parents and sibling indices are recorded, so undo puts everything back in place.
    const std::string snapshot = SnapshotEntities(m_Ctx.scene, roots);
    DestroyByUuids(uuids);
    Scene* scene = &m_Ctx.scene;
    PushCommand({roots.size() == 1 ? "Delete" : std::format("Delete {} entities", roots.size()),
                 [scene, snapshot] { (void)RestoreEntities(*scene, snapshot, RestoreMode::Original); },
                 [this, uuids] { DestroyByUuids(uuids); }});
}

void Editor::Reparent(Entity child, Entity parent)
{
    Registry& registry = m_Ctx.scene.GetRegistry();
    if (!registry.Valid(child) || (parent != NullEntity && !registry.Valid(parent)) || child == parent ||
        (parent != NullEntity && m_Ctx.scene.IsAncestor(child, parent)) ||
        registry.Get<Hierarchy>(child).parent == parent)
        return;

    struct Place {
        std::uint64_t parent = 0;
        std::size_t   index  = 0;
        std::string   state;
    };
    const auto place = [&](Entity e) {
        const Entity p = registry.Get<Hierarchy>(e).parent;
        return Place{p == NullEntity ? 0 : UuidOf(p), m_Ctx.scene.SiblingIndex(e), SnapshotEntityState(m_Ctx.scene, e)};
    };
    const Place before = place(child);

    // Keep the world transform (world matrices are from the last UpdateTransforms).
    const glm::mat4 childWorld  = registry.Get<WorldTransform>(child).matrix;
    const glm::mat4 parentWorld = parent != NullEntity ? registry.Get<WorldTransform>(parent).matrix : glm::mat4(1.0f);
    m_Ctx.scene.SetParent(child, parent);
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    if (glm::decompose(glm::inverse(parentWorld) * childWorld, scale, rotation, translation, skew, perspective)) {
        Transform& t = m_Ctx.scene.EditTransform(child);
        t.position   = translation;
        t.rotation   = glm::normalize(rotation);
        t.scale      = scale;
    }
    const Place after = place(child);

    const std::uint64_t uuid  = UuidOf(child);
    Scene*              scene = &m_Ctx.scene;
    const auto          apply = [scene, uuid](const Place& p) {
        const Entity e = scene->FindByUuid(uuid);
        if (e == NullEntity)
            return;
        scene->SetParent(e, scene->FindByUuid(p.parent), p.index);
        ApplyEntityState(*scene, e, p.state);
    };
    PushCommand({"Reparent", [apply, before] { apply(before); }, [apply, after] { apply(after); }});
}

// --- Prefabs ------------------------------------------------------------------------------------

std::vector<Entity> Editor::OutermostRoots(const std::vector<std::uint64_t>& uuids) const
{
    std::vector<Entity> entities;
    for (std::uint64_t uuid : uuids)
        if (const Entity e = m_Ctx.scene.FindByUuid(uuid); e != NullEntity && std::ranges::find(entities, e) == entities.end())
            entities.push_back(e);
    std::vector<Entity> roots; // SnapshotEntities: none may lie below another
    for (Entity e : entities)
        if (std::ranges::none_of(entities, [&](Entity other) { return other != e && m_Ctx.scene.IsAncestor(other, e); }))
            roots.push_back(e);
    return roots;
}

void Editor::PushSubtreesChange(std::string label, const std::vector<std::uint64_t>& uuids, std::string before)
{
    std::vector<std::uint64_t> rootUuids;
    for (Entity e : OutermostRoots(uuids))
        rootUuids.push_back(UuidOf(e));
    std::string after = SnapshotEntities(m_Ctx.scene, OutermostRoots(uuids));
    const auto  swap  = [this, rootUuids](const std::string& snapshot) {
        DestroyByUuids(rootUuids);
        (void)RestoreEntities(m_Ctx.scene, snapshot, RestoreMode::Original);
        ValidateSelection();
    };
    PushCommand({std::move(label), [swap, before = std::move(before)] { swap(before); },
                 [swap, after = std::move(after)] { swap(after); }});
}

bool Editor::CreatePrefabFrom(Entity root, const std::filesystem::path& file)
{
    if (!m_Ctx.scene.GetRegistry().Valid(root))
        return false;
    const std::vector<std::uint64_t> uuids{UuidOf(root)};
    std::string                      before = SnapshotEntities(m_Ctx.scene, OutermostRoots(uuids));
    try {
        CreatePrefab(file, m_Ctx.scene, &m_Ctx.assets, root);
    } catch (const std::exception& e) {
        m_Status = e.what();
        ENGINE_ERROR("{}", m_Status);
        return false;
    }
    PushSubtreesChange("Create prefab", uuids, std::move(before));
    m_Status = "Prefab " + PathToUtf8(file.filename());
    RefreshContent();
    return true;
}

Entity Editor::PlacePrefab(const std::filesystem::path& file, const glm::vec3& position)
{
    Transform t;
    t.position = position;
    Entity e   = NullEntity;
    try {
        e = InstantiatePrefab(m_Ctx.scene, &m_Ctx.assets, file, NullEntity, t, m_Ctx.modelRefs);
    } catch (const std::exception& ex) {
        m_Status = ex.what();
        ENGINE_ERROR("{}", m_Status);
        return NullEntity;
    }
    const Entity roots[] = {e};
    PushCreated("Place " + PathToUtf8(file.stem()), roots);
    Select(e);
    m_Ctx.scene.UpdateTransforms();
    m_Status = "Placed " + PathToUtf8(file.filename());
    return e;
}

bool Editor::RunPrefabOp(PrefabOp op, Entity entity, const std::string& key)
{
    const Entity root = PrefabInstanceRoot(m_Ctx.scene, entity);
    if (root == NullEntity)
        return false;
    Registry&                  registry = m_Ctx.scene.GetRegistry();
    std::vector<std::uint64_t> affected{UuidOf(root)};
    if (op == PrefabOp::Apply) { // every instance of the prefab is rebuilt
        const std::string file = registry.Get<PrefabInstance>(root).prefab;
        registry.ViewOf<PrefabInstance>().Each([&](Entity e, PrefabInstance& instance) {
            if (e != root && instance.prefab == file)
                affected.push_back(UuidOf(e));
        });
    }
    std::string before = SnapshotEntities(m_Ctx.scene, OutermostRoots(affected));
    const char* label  = op == PrefabOp::Apply ? "Apply to prefab" : op == PrefabOp::Revert ? "Revert to prefab" : "Unlink prefab";
    try {
        switch (op) {
        case PrefabOp::Apply: ApplyPrefabInstance(m_Ctx.scene, &m_Ctx.assets, root, m_Ctx.modelRefs); break;
        case PrefabOp::Revert: RevertPrefabOverrides(m_Ctx.scene, &m_Ctx.assets, entity, key, m_Ctx.modelRefs); break;
        case PrefabOp::Unlink: UnlinkPrefabInstance(m_Ctx.scene, root); break;
        }
    } catch (const std::exception& e) {
        m_Status = e.what();
        ENGINE_ERROR("{}", m_Status);
        return false;
    }
    PushSubtreesChange(label, affected, std::move(before));
    ValidateSelection();
    m_Ctx.scene.UpdateTransforms();
    m_Status = label;
    return true;
}

void Editor::ApplyPendingEdits()
{
    if (const auto op = std::exchange(m_PendingPrefabOp, std::nullopt))
        if (const Entity e = m_Ctx.scene.FindByUuid(op->uuid); e != NullEntity)
            RunPrefabOp(op->op, e, op->key);
    if (!m_PendingDelete.empty()) {
        m_Selection = std::exchange(m_PendingDelete, {});
        ValidateSelection();
        DeleteSelection();
    }
    if (std::exchange(m_ReparentPending, false))
        Reparent(m_ReparentChild, m_ReparentTo);
    if (std::exchange(m_PendingDuplicate, false))
        DuplicateSelection();
}

// --- Scene files ------------------------------------------------------------------------------

void Editor::ReleaseModelRefs()
{
    for (ModelHandle h : m_Ctx.modelRefs)
        if (m_Ctx.assets.State(h) != AssetState::Invalid)
            m_Ctx.assets.Release(h);
    m_Ctx.modelRefs.clear();
}

void Editor::RequestSceneChange(std::function<void()> action)
{
    Stop(); // the play state is never saved: continue from the edit scene
    if (!HasUnsavedChanges()) {
        action();
        return;
    }
    m_PendingSceneChange = std::move(action);
    m_ConfirmDiscard     = true;
}

void Editor::NewScene()
{
    m_Ctx.scene.Clear();
    ReleaseModelRefs();
    m_History->Clear();
    m_Selection.clear();
    m_GizmoEdit.reset();
    m_InspectorEdit.reset();
    m_ScenePath.clear();
    m_Status = "New scene";
}

bool Editor::OpenScene(const std::filesystem::path& file)
{
    // Parse into a scratch scene first: a broken file must not destroy the current one. Its model
    // refs are held until the real load has taken its own (cache hits: nothing loads twice).
    std::vector<ModelHandle> probe;
    try {
        Scene scratch;
        probe = LoadSceneFile(file, scratch, m_Ctx.assets);
    } catch (const std::exception& e) {
        ENGINE_ERROR("Open scene failed: {}", e.what());
        m_Status = "Open failed (see log)";
        return false;
    }
    NewScene();
    bool loaded = true;
    try {
        const SceneFileOptions options{.renderer = &m_Ctx.sceneRenderer,
                                       .camera   = &m_Ctx.camera,
                                       .physics  = m_Ctx.physics ? &m_Ctx.physics->settings : nullptr};
        const auto             handles = LoadSceneFile(file, m_Ctx.scene, m_Ctx.assets, options);
        m_Ctx.modelRefs.insert(m_Ctx.modelRefs.end(), handles.begin(), handles.end());
    } catch (const std::exception& e) { // e.g. the file changed in between
        ENGINE_ERROR("Open scene failed: {}", e.what());
        m_Status = "Open failed (see log)";
        loaded   = false;
    }
    for (ModelHandle h : probe)
        m_Ctx.assets.Release(h);
    if (!loaded)
        return false;
    m_ScenePath = file;
    m_Status    = "Opened " + file.filename().string();
    m_Ctx.scene.UpdateTransforms();
    ENGINE_INFO("Opened scene '{}'", file.string());
    return true;
}

bool Editor::SaveScene(const std::filesystem::path& file)
{
    if (m_PlayState != PlayState::Edit) {
        m_Status = "Stop playing to save";
        return false;
    }
    try {
        SaveSceneFile(file, m_Ctx.scene, m_Ctx.assets,
                      {.renderer = &m_Ctx.sceneRenderer,
                       .camera   = &m_Ctx.camera,
                       .physics  = m_Ctx.physics ? &m_Ctx.physics->settings : nullptr});
    } catch (const std::exception& e) {
        ENGINE_ERROR("Save scene failed: {}", e.what());
        m_Status = "Save failed (see log)";
        return false;
    }
    m_ScenePath = file;
    m_History->MarkSaved();
    m_Status = "Saved " + file.filename().string();
    ENGINE_INFO("Saved scene '{}'", file.string());
    return true;
}

void Editor::DrawDialogs()
{
    if (const std::optional<std::filesystem::path> path = m_FileDialog->Draw()) {
        switch (std::exchange(m_DialogPurpose, DialogPurpose::None)) {
        case DialogPurpose::NewScript:
            if (m_Graphs->New(*path)) {
                AssignScript(m_ScriptTarget, *path);
                m_ShowBlueprint = true;
                m_Graphs->Focus();
            }
            break;
        case DialogPurpose::AssignScript: AssignScript(m_ScriptTarget, *path); break;
        case DialogPurpose::AssignSound: AssignSound(m_SoundTarget, *path); break;
        case DialogPurpose::Package:
            if (m_Ctx.project)
                PackageProject(*path / PathFromUtf8(m_Ctx.project->settings.name));
            break;
        case DialogPurpose::CreatePrefab: {
            std::filesystem::path file = *path;
            if (file.extension() != kPrefabExtension)
                file += kPrefabExtension;
            CreatePrefabFrom(m_PrefabTarget, file);
            break;
        }
        case DialogPurpose::OpenScene: OpenScene(*path); break;
        case DialogPurpose::SaveScene: SaveScene(*path); break;
        case DialogPurpose::LoadModel: {
            const std::u8string u8 = path->u8string();
            m_LoadPath             = std::string(u8.begin(), u8.end());
            m_Ctx.modelRefs.push_back(m_Ctx.assets.LoadModel(*path));
            break;
        }
        case DialogPurpose::None: break;
        }
    }

    if (std::exchange(m_AskQuit, false))
        ImGui::OpenPopup("Quit?");
    if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("There are unsaved changes (scene or blueprints).");
        if (m_ScenePath.empty() && HasUnsavedChanges())
            ImGui::TextDisabled("The untitled scene has no file: use Save scene as... to keep it.");
        const auto quit = [&] {
            m_QuitConfirmed = true;
            m_Ctx.window.RequestClose();
            ImGui::CloseCurrentPopup();
        };
        if (ImGui::Button("Save all and quit", ImVec2(150.0f, 0.0f)) && SaveAll())
            quit();
        ImGui::SameLine();
        if (ImGui::Button("Quit without saving", ImVec2(150.0f, 0.0f)))
            quit();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (std::exchange(m_ConfirmDiscard, false))
        ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The scene has unsaved changes. Discard them?");
        if (ImGui::Button("Discard", ImVec2(100.0f, 0.0f))) {
            if (auto action = std::exchange(m_PendingSceneChange, nullptr))
                action();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            m_PendingSceneChange = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace Engine

namespace Engine {

// ---------------------------------------------------------------------------------------------
// Play mode
// ---------------------------------------------------------------------------------------------

void Editor::AssignScript(Entity entity, const std::filesystem::path& graph)
{
    if (!m_Ctx.scene.GetRegistry().Valid(entity))
        return;
    std::error_code ec;
    const std::u8string path = std::filesystem::absolute(graph, ec).lexically_normal().u8string();
    std::string         before = SnapshotEntityState(m_Ctx.scene, entity);
    m_Ctx.scene.GetRegistry().EmplaceOrReplace<ScriptComponent>(entity, ScriptComponent{std::string(path.begin(), path.end())});
    PushStateChange("Assign script", {StateEdit{UuidOf(entity), std::move(before)}});
}

void Editor::AssignSound(Entity entity, const std::filesystem::path& sound)
{
    Registry& r = m_Ctx.scene.GetRegistry();
    if (!r.Valid(entity))
        return;
    std::error_code ec;
    std::string     before = SnapshotEntityState(m_Ctx.scene, entity);
    AudioSource     source = r.Has<AudioSource>(entity) ? r.Get<AudioSource>(entity) : AudioSource{};
    source.sound           = PathToUtf8(std::filesystem::absolute(sound, ec).lexically_normal());
    r.EmplaceOrReplace<AudioSource>(entity, source);
    PushStateChange("Assign sound", {StateEdit{UuidOf(entity), std::move(before)}});
}

Entity Editor::CreateAudioEntity(const std::filesystem::path& sound, const glm::vec3& position)
{
    std::error_code ec;
    const Entity    e = m_Ctx.scene.CreateEntity(sound.empty() ? std::string("Audio Source") : PathToUtf8(sound.stem()));
    m_Ctx.scene.EditTransform(e).position = position;
    m_Ctx.scene.GetRegistry().Emplace<AudioSource>(
        e, AudioSource{.sound = sound.empty() ? std::string() : PathToUtf8(std::filesystem::absolute(sound, ec).lexically_normal())});
    const Entity roots[] = {e};
    PushCreated("Create audio source", roots);
    Select(e);
    return e;
}

void Editor::Play()
{
    if (!m_Ctx.physics && !m_Ctx.scripts && !m_Ctx.audio)
        return;
    if (m_PlayState == PlayState::Paused) {
        m_PlayState = PlayState::Playing;
        if (m_Ctx.audio)
            m_Ctx.audio->SetPaused(false);
        return;
    }
    if (m_PlayState != PlayState::Edit)
        return;
    ReloadScriptRegistry(); // type / library files may have changed
    // Finish edits in progress so they land in the history before it is frozen.
    if (m_InspectorEdit)
        PushStateChange("Edit properties", std::exchange(m_InspectorEdit, std::nullopt).value());
    if (m_GizmoEdit)
        PushStateChange("Transform", std::exchange(m_GizmoEdit, std::nullopt).value());

    std::vector<Entity> roots;
    m_Ctx.scene.GetRegistry().ViewOf<Hierarchy>().Each([&](Entity e, Hierarchy& h) {
        if (h.parent == NullEntity)
            roots.push_back(e);
    });
    std::reverse(roots.begin(), roots.end()); // views iterate backwards: keep creation order
    m_PlaySnapshot = SnapshotEntities(m_Ctx.scene, roots);
    if (m_Ctx.physics) {
        m_Ctx.physics->Reset(); // fresh bodies: no velocities or sleep state from editing
        m_Ctx.physics->Sync(m_Ctx.scene); // bodies exist for BeginPlay (impulses, raycasts)
    }
    if (m_Ctx.audio) {
        m_Ctx.audio->StopPreview();
        m_Ctx.audio->Begin(m_Ctx.scene); // before BeginPlay: scripts may play sources right away
    }
    if (m_Ctx.scripts) {
        m_Graphs->ProvideTo(*m_Ctx.scripts); // unsaved graph edits run too
        m_Ctx.scripts->Begin(m_Ctx.scene);
    }
    m_PlayState    = PlayState::Playing;
    m_StepRequested = false;
    m_Status       = "Playing";
}

void Editor::Pause()
{
    if (m_PlayState == PlayState::Playing) {
        m_PlayState = PlayState::Paused;
        if (m_Ctx.audio)
            m_Ctx.audio->SetPaused(true);
    }
}

void Editor::StepOnce()
{
    if (m_PlayState == PlayState::Playing)
        Pause();
    if (m_PlayState == PlayState::Paused)
        m_StepRequested = true;
}

void Editor::Stop()
{
    if (m_PlayState == PlayState::Edit)
        return;
    std::vector<std::uint64_t> selected;
    for (Entity e : m_Selection)
        if (m_Ctx.scene.GetRegistry().Valid(e))
            selected.push_back(UuidOf(e));

    if (m_Ctx.scripts)
        m_Ctx.scripts->End(m_Ctx.scene); // EndPlay, spawned models released
    if (m_Ctx.audio) {
        m_Ctx.audio->End(m_Ctx.scene);
        m_Ctx.audio->SetPaused(false);
    }
    m_Ctx.scene.Clear();
    (void)RestoreEntities(m_Ctx.scene, m_PlaySnapshot, RestoreMode::Original);
    m_Ctx.scene.UpdateTransforms();
    if (m_Ctx.physics)
        m_Ctx.physics->Reset();

    m_Selection.clear();
    for (std::uint64_t uuid : selected)
        if (const Entity e = m_Ctx.scene.FindByUuid(uuid); e != NullEntity)
            m_Selection.push_back(e);
    m_GizmoEdit.reset();
    m_InspectorEdit.reset();
    m_EulerEntity   = NullEntity;
    m_PlaySnapshot.clear();
    m_PlayState     = PlayState::Edit;
    m_StepRequested = false;
    m_Status        = "Stopped";
}

void Editor::FixedUpdate(float dt)
{
    if (!m_Ctx.physics)
        return; // scripts tick per frame (Update)
    if (m_Ctx.scripts && m_Ctx.scripts->DebugPaused())
        return; // stopped at a breakpoint: the world waits too
    if (m_PlayState == PlayState::Playing || (m_PlayState == PlayState::Paused && m_StepRequested)) {
        m_Ctx.physics->Step(m_Ctx.scene, dt);
        m_StepRequested = false;
    }
}

} // namespace Engine
