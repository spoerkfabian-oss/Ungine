// Editor panels: Hierarchy, Inspector, Renderer settings, Stats, Assets.
#include "Editor/Editor.h"
#include "Editor/ScriptGraphEditor.h"
#include "Editor/FileDialog.h"
#include "Editor/History.h"
#include "Editor/ImGuiLayer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptSystem.h"

#include <imgui.h>
#include <imgui_internal.h> // ActiveIdWindow (inspector edit sessions)
#include <imgui_stdlib.h>   // InputText(std::string*)

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <numeric>
#include <system_error>
#include <utility>

namespace Engine {

namespace {

constexpr const char* kEntityPayload = "ENGINE_ENTITY";

const void* EntityId(Entity e)
{
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(static_cast<std::uint64_t>(e)));
}

const char* ToString(AssetState state)
{
    switch (state) {
    case AssetState::Loading:   return "Loading";
    case AssetState::Uploading: return "Uploading";
    case AssetState::Ready:     return "Ready";
    case AssetState::Failed:    return "Failed";
    default:                    return "Invalid";
    }
}

ImVec4 StateColor(AssetState state)
{
    switch (state) {
    case AssetState::Ready:  return {0.35f, 0.85f, 0.35f, 1.0f};
    case AssetState::Failed: return {0.95f, 0.30f, 0.25f, 1.0f};
    default:                 return {0.95f, 0.80f, 0.25f, 1.0f};
    }
}

// Label column on the left, widget filling the rest (inside a 2-column table).
bool PropertyRow(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    return true;
}

bool BeginProperties(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    return true;
}

bool DragFloatRow(const char* label, float* v, float speed, float min, float max, const char* fmt = "%.3f")
{
    PropertyRow(label);
    const bool changed = ImGui::DragFloat("##v", v, speed, min, max, fmt, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    return changed;
}

bool SliderFloatRow(const char* label, float* v, float min, float max, const char* fmt = "%.3f",
                    ImGuiSliderFlags flags = ImGuiSliderFlags_None)
{
    PropertyRow(label);
    const bool changed = ImGui::SliderFloat("##v", v, min, max, fmt, flags | ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    return changed;
}

bool SliderUintRow(const char* label, std::uint32_t* v, std::uint32_t min, std::uint32_t max)
{
    PropertyRow(label);
    int        value   = static_cast<int>(*v);
    const bool changed = ImGui::SliderInt("##v", &value, static_cast<int>(min), static_cast<int>(max), "%d",
                                          ImGuiSliderFlags_AlwaysClamp);
    if (changed)
        *v = static_cast<std::uint32_t>(value);
    ImGui::PopID();
    return changed;
}

bool CheckboxRow(const char* label, bool* v)
{
    PropertyRow(label);
    const bool changed = ImGui::Checkbox("##v", v);
    ImGui::PopID();
    return changed;
}

template <class E>
bool EnumComboRow(const char* label, E* value)
{
    PropertyRow(label);
    bool changed = false;
    if (ImGui::BeginCombo("##v", ToString(*value))) {
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(E::Count); ++i) {
            const E    option   = static_cast<E>(i);
            const bool selected = option == *value;
            if (ImGui::Selectable(ToString(option), selected)) {
                *value  = option;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

template <class E, std::size_t N>
bool ComboRow(const char* label, E* value, const char* const (&names)[N])
{
    PropertyRow(label);
    int        index   = static_cast<int>(*value);
    const bool changed = ImGui::Combo("##v", &index, names, static_cast<int>(N));
    if (changed)
        *value = static_cast<E>(index);
    ImGui::PopID();
    return changed;
}

constexpr const char* kBodyTypeNames[]      = {"Static", "Kinematic", "Dynamic"};
constexpr const char* kAttenuationNames[]   = {"Inverse", "Linear", "Exponential"};

// Buses a sound can play on (Master is the sum).
bool BusComboRow(const char* label, AudioBus* bus)
{
    PropertyRow(label);
    bool changed = false;
    if (ImGui::BeginCombo("##v", ToString(*bus))) {
        for (AudioBus option : {AudioBus::World, AudioBus::Music, AudioBus::Ui, AudioBus::Ambient})
            if (ImGui::Selectable(ToString(option), option == *bus)) {
                *bus    = option;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

bool ReverbRows(ReverbParams* r)
{
    bool changed = false;
    changed |= SliderFloatRow("Room size", &r->roomSize, 0.0f, 1.0f);
    changed |= SliderFloatRow("Damping", &r->damping, 0.0f, 1.0f);
    changed |= SliderFloatRow("Wet", &r->wet, 0.0f, 1.0f);
    changed |= SliderFloatRow("Width", &r->width, 0.0f, 1.0f);
    return changed;
}
constexpr const char* kColliderShapeNames[] = {"Box", "Sphere", "Capsule", "Mesh"};
constexpr const char* kActivityNames[]      = {"-", "static", "kinematic", "active", "sleeping", "character"};

bool Vec3Row(const char* label, glm::vec3* v, float speed, float resetValue, const char* fmt = "%.3f")
{
    PropertyRow(label);
    bool        changed = false;
    const float button  = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemInnerSpacing.x);
    changed |= ImGui::DragFloat3("##v", &v->x, speed, 0.0f, 0.0f, fmt);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::Button("R", ImVec2(button, button))) {
        *v      = glm::vec3(resetValue);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reset");
    ImGui::PopID();
    return changed;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------------------------

void Editor::DrawHierarchy()
{
    if (!ImGui::Begin("Hierarchy", &m_ShowHierarchy)) {
        ImGui::End();
        return;
    }
    Registry& registry = m_Ctx.scene.GetRegistry();

    if (ImGui::Button("+ Add"))
        ImGui::OpenPopup("add");
    if (ImGui::BeginPopup("add")) {
        if (ImGui::MenuItem("Entity"))
            Select(CreateEntity("Entity", NullEntity));
        if (ImGui::MenuItem("Cube"))
            Select(CreatePrimitiveEntity(PrimitiveShape::Box));
        if (ImGui::MenuItem("Plane"))
            Select(CreatePrimitiveEntity(PrimitiveShape::Plane));
        if (ImGui::MenuItem("Sphere"))
            Select(CreatePrimitiveEntity(PrimitiveShape::Sphere));
        if (ImGui::MenuItem("Capsule"))
            Select(CreatePrimitiveEntity(PrimitiveShape::Capsule));
        ImGui::Separator();
        if (ImGui::MenuItem("Point Light"))
            Select(CreateLight(LightType::Point, NullEntity));
        if (ImGui::MenuItem("Spot Light"))
            Select(CreateLight(LightType::Spot, NullEntity));
        ImGui::Separator();
        if (ImGui::MenuItem("Audio Source"))
            CreateAudioEntity({}, PlacementPoint(std::max(m_Ctx.camera.moveSpeed, 1.0f) * 2.0f));
        if (ImGui::MenuItem("Reverb Zone")) {
            const Entity e = m_Ctx.scene.CreateEntity("Reverb Zone");
            m_Ctx.scene.EditTransform(e).position = PlacementPoint(std::max(m_Ctx.camera.moveSpeed, 1.0f) * 2.0f);
            registry.Emplace<ReverbZone>(e);
            const Entity roots[] = {e};
            PushCreated("Create reverb zone", roots);
            Select(e);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu entities", registry.AliveCount());
    ImGui::Separator();

    std::vector<Entity> roots;
    registry.ViewOf<Hierarchy>().Each([&](Entity e, Hierarchy& h) {
        if (h.parent == NullEntity)
            roots.push_back(e);
    });
    std::ranges::sort(roots, {}, [](Entity e) { return EntityIndex(e); }); // stable order across frames

    ImGui::BeginChild("tree");
    for (Entity e : roots)
        DrawHierarchyNode(e);

    // Empty space: click clears the selection, dropping an entity makes it a root.
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        Select(NullEntity);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            m_ReparentChild   = *static_cast<const Entity*>(payload->Data);
            m_ReparentTo      = NullEntity;
            m_ReparentPending = true;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::DrawHierarchyNode(Entity entity)
{
    Registry& registry = m_Ctx.scene.GetRegistry();
    // Copies: creating entities from the context menu may reallocate the pools.
    const std::vector<Entity> children = registry.Get<Hierarchy>(entity).children;
    const std::string         name     = registry.Get<Name>(entity).value;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (IsSelected(entity))
        flags |= ImGuiTreeNodeFlags_Selected;
    const Entity prefabRoot  = PrefabInstanceRoot(m_Ctx.scene, entity);
    const bool   constructed = registry.Has<ConstructionOwned>(entity);
    const bool   tinted = constructed || prefabRoot != NullEntity || registry.Has<MeshRenderer>(entity) || registry.Has<Light>(entity);
    if (constructed) // made by a construction script: rebuilt, not saved
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
    else if (prefabRoot != NullEntity) // prefab instances: root bright blue, members lighter
        ImGui::PushStyleColor(ImGuiCol_Text, prefabRoot == entity ? ImVec4(0.35f, 0.65f, 1.0f, 1.0f) : ImVec4(0.6f, 0.78f, 1.0f, 1.0f));
    else if (tinted)
        ImGui::PushStyleColor(ImGuiCol_Text, registry.Has<Light>(entity) ? ImVec4(1.0f, 0.85f, 0.4f, 1.0f)
                                                                        : ImVec4(0.65f, 0.85f, 1.0f, 1.0f));
    const bool open = ImGui::TreeNodeEx(EntityId(entity), flags, "%s", name.empty() ? "(unnamed)" : name.c_str());
    if (tinted)
        ImGui::PopStyleColor();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        SelectFromClick(entity, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);

    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &entity, sizeof(entity));
        ImGui::Text("%s", name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            m_ReparentChild   = *static_cast<const Entity*>(payload->Data);
            m_ReparentTo      = entity;
            m_ReparentPending = true;
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem()) {
        if (!IsSelected(entity))
            Select(entity);
        if (ImGui::MenuItem("Create child"))
            Select(CreateEntity("Entity", entity));
        if (ImGui::MenuItem("Create point light"))
            Select(CreateLight(LightType::Point, entity));
        if (ImGui::MenuItem("Create spot light"))
            Select(CreateLight(LightType::Spot, entity));
        ImGui::Separator();
        if (ImGui::MenuItem("Focus", "F"))
            FocusSelected();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
            m_PendingDuplicate = true;
        if (ImGui::MenuItem("Delete", "Del"))
            m_PendingDelete = m_Selection; // after the tree was drawn
        ImGui::Separator();
        if (ImGui::MenuItem("Create prefab...", nullptr, false, m_PlayState == PlayState::Edit)) {
            m_PrefabTarget  = entity;
            m_DialogPurpose = DialogPurpose::CreatePrefab;
            std::error_code ec;
            const std::filesystem::path dir = ContentRoot() / "Prefabs";
            std::filesystem::create_directories(dir, ec);
            m_FileDialog->Open("Create prefab", FileDialog::Mode::Save, dir, {std::string(kPrefabExtension)},
                               name + std::string(kPrefabExtension));
        }
        if (prefabRoot != NullEntity) {
            const bool edit = m_PlayState == PlayState::Edit;
            if (prefabRoot != entity && ImGui::MenuItem("Select prefab root"))
                Select(prefabRoot);
            if (ImGui::MenuItem("Apply to prefab", nullptr, false, edit))
                m_PendingPrefabOp = PendingPrefabOp{PrefabOp::Apply, UuidOf(entity), {}};
            if (ImGui::MenuItem(prefabRoot == entity ? "Revert instance" : "Revert entity", nullptr, false, edit))
                m_PendingPrefabOp = PendingPrefabOp{PrefabOp::Revert, UuidOf(entity), {}};
            if (ImGui::MenuItem("Unlink prefab", nullptr, false, edit))
                m_PendingPrefabOp = PendingPrefabOp{PrefabOp::Unlink, UuidOf(entity), {}};
        }
        ImGui::EndPopup();
    }

    if (open) {
        for (Entity child : children)
            if (registry.Valid(child))
                DrawHierarchyNode(child);
        ImGui::TreePop();
    }
}

// ---------------------------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------------------------

void Editor::DrawInspector()
{
    if (!ImGui::Begin("Inspector", &m_ShowInspector)) {
        ImGui::End();
        return;
    }
    Registry& registry = m_Ctx.scene.GetRegistry();
    const Entity e     = Selected();
    if (e == NullEntity) {
        if (m_InspectorEdit) // the edited entity was deselected mid-edit
            PushStateChange("Edit properties", std::exchange(m_InspectorEdit, std::nullopt).value());
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }
    // Undo: the state before any widget of this frame touched it (see the end of this function).
    const std::string frameState = SnapshotEntityState(m_Ctx.scene, e);

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##name", &registry.Get<Name>(e).value);
    if (m_Selection.size() > 1)
        ImGui::TextDisabled("%zu selected - edits apply to all (values of the last one shown)", m_Selection.size());
    ImGui::TextDisabled("Entity %u (gen %u), uuid %016llx", EntityIndex(e), EntityGeneration(e),
                        static_cast<unsigned long long>(registry.Get<Uuid>(e).value));
    if (PrefabInstanceRoot(m_Ctx.scene, e) != NullEntity)
        DrawPrefabHeader(e);

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("transform")) {
        // Edited as a copy: writing back only on change keeps the entity (and its shadow caches) clean.
        const Transform original = m_Ctx.scene.GetTransform(e);
        Transform       t        = original;

        Vec3Row("Position", &t.position, 0.01f, 0.0f);

        // Refresh the Euler angles only when the rotation was changed elsewhere (gizmo, other entity).
        if (m_EulerEntity != e || m_EulerSource != t.rotation) {
            m_EulerEntity  = e;
            m_EulerDegrees = glm::degrees(glm::eulerAngles(t.rotation)) + glm::vec3(0.0f); // no "-0.0"
        }
        if (Vec3Row("Rotation", &m_EulerDegrees, 0.2f, 0.0f, "%.1f\xC2\xB0"))
            t.rotation = glm::normalize(glm::quat(glm::radians(m_EulerDegrees)));
        m_EulerSource = t.rotation;

        Vec3Row("Scale", &t.scale, 0.01f, 1.0f);
        if (t != original)
            m_Ctx.scene.SetTransform(e, t);

        const glm::vec3 world = registry.Get<WorldTransform>(e).matrix[3];
        PropertyRow("World pos");
        ImGui::TextDisabled("%.3f  %.3f  %.3f", world.x, world.y, world.z);
        ImGui::PopID();
        ImGui::EndTable();
    }

    if (MeshRenderer* mesh = registry.TryGet<MeshRenderer>(e);
        mesh && ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen) &&
        BeginProperties("mesh")) {
        const AssetState state = m_Ctx.assets.State(mesh->model);
        PropertyRow("Model");
        ImGui::TextColored(StateColor(state), "#%u  %s", mesh->model.index, ToString(state));
        ImGui::PopID();
        if (const Model* model = m_Ctx.assets.Get(mesh->model)) {
            PropertyRow("Asset");
            ImGui::TextUnformatted(model->name.c_str());
            ImGui::PopID();
            if (mesh->meshIndex < model->meshes.size()) {
                const Mesh&   m         = model->meshes[mesh->meshIndex];
                std::uint64_t triangles = 0;
                for (const Submesh& s : m.submeshes)
                    triangles += s.indexCount / 3;
                PropertyRow("Mesh");
                ImGui::Text("%u: %s", mesh->meshIndex, m.name.empty() ? "(unnamed)" : m.name.c_str());
                ImGui::PopID();
                PropertyRow("Submeshes");
                ImGui::Text("%zu  (%llu tris)", m.submeshes.size(), static_cast<unsigned long long>(triangles));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        if (ImGui::Button("Remove component"))
            registry.Remove<MeshRenderer>(e);
    }

    if (Light* light = registry.TryGet<Light>(e);
        light && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("light")) {
        PropertyRow("Type");
        int type = static_cast<int>(light->type);
        if (ImGui::Combo("##v", &type, "Point\0Spot\0"))
            light->type = static_cast<LightType>(type);
        ImGui::PopID();
        PropertyRow("Color");
        ImGui::ColorEdit3("##v", &light->color.x, ImGuiColorEditFlags_Float);
        ImGui::PopID();
        PropertyRow("Intensity");
        ImGui::DragFloat("##v", &light->intensity, 0.01f, 0.0f, 1e6f, "%.4g cd",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
        PropertyRow("Range");
        char rangeFormat[48];
        std::snprintf(rangeFormat, sizeof(rangeFormat), light->range > 0.0f ? "%%.3f" : "auto (%.3f)",
                      EffectiveRange(*light));
        ImGui::DragFloat("##v", &light->range, 0.01f, 0.0f, 1e5f, rangeFormat, ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = derived from the intensity (illuminance cutoff %.3f)", kLightCutoffIlluminance);
        ImGui::PopID();
        CheckboxRow("Cast shadows", &light->castShadows);
        if (light->type == LightType::Spot) {
            PropertyRow("Outer cone");
            ImGui::SliderAngle("##v", &light->outerConeAngle, 1.0f, 90.0f);
            ImGui::PopID();
            PropertyRow("Inner cone");
            ImGui::SliderAngle("##v", &light->innerConeAngle, 0.0f, glm::degrees(light->outerConeAngle));
            ImGui::PopID();
            light->innerConeAngle = std::min(light->innerConeAngle, light->outerConeAngle);
        }
        ImGui::EndTable();
        if (ImGui::Button("Remove light"))
            registry.Remove<Light>(e);
    }

    if (RigidBody* body = registry.TryGet<RigidBody>(e);
        body && ImGui::CollapsingHeader("Rigid Body", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("body")) {
        ComboRow("Type", &body->type, kBodyTypeNames);
        if (body->type == BodyType::Dynamic) {
            DragFloatRow("Mass", &body->mass, 0.05f, 0.001f, 1e6f, "%.3f kg");
            DragFloatRow("Gravity factor", &body->gravityFactor, 0.01f, -10.0f, 10.0f);
            DragFloatRow("Linear damping", &body->linearDamping, 0.005f, 0.0f, 10.0f);
            DragFloatRow("Angular damping", &body->angularDamping, 0.005f, 0.0f, 10.0f);
            CheckboxRow("Allow sleeping", &body->allowSleeping);
            CheckboxRow("Continuous (CCD)", &body->continuous);
        }
        if (m_Ctx.physics) {
            PropertyRow("State");
            const BodyActivity activity = m_Ctx.physics->Activity(e);
            ImGui::TextDisabled("%s", kActivityNames[static_cast<std::size_t>(activity)]);
            ImGui::PopID();
            if (activity == BodyActivity::Active || activity == BodyActivity::Sleeping) {
                const glm::vec3 v = m_Ctx.physics->LinearVelocity(e);
                PropertyRow("Velocity");
                ImGui::TextDisabled("%.2f  %.2f  %.2f", v.x, v.y, v.z);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        if (!registry.Has<Collider>(e))
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Needs a Collider");
        if (ImGui::Button("Remove rigid body"))
            registry.Remove<RigidBody>(e);
    }

    if (Collider* collider = registry.TryGet<Collider>(e);
        collider && ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("collider")) {
        ComboRow("Shape", &collider->shape, kColliderShapeNames);
        switch (collider->shape) {
        case ColliderShape::Box: Vec3Row("Half extents", &collider->halfExtents, 0.005f, 0.5f); break;
        case ColliderShape::Sphere: DragFloatRow("Radius", &collider->radius, 0.005f, 0.001f, 1e4f); break;
        case ColliderShape::Capsule:
            DragFloatRow("Radius", &collider->radius, 0.005f, 0.001f, 1e4f);
            DragFloatRow("Half height", &collider->halfHeight, 0.005f, 0.0f, 1e4f);
            break;
        case ColliderShape::Mesh:
            PropertyRow("Triangles");
            ImGui::TextDisabled(registry.Has<MeshRenderer>(e) ? "from the Mesh Renderer" : "needs a Mesh Renderer");
            ImGui::PopID();
            break;
        }
        if (collider->shape != ColliderShape::Mesh)
            Vec3Row("Center", &collider->center, 0.005f, 0.0f);
        DragFloatRow("Friction", &collider->friction, 0.005f, 0.0f, 10.0f);
        DragFloatRow("Restitution", &collider->restitution, 0.005f, 0.0f, 1.0f);
        CheckboxRow("Trigger", &collider->trigger);
        std::uint32_t layer = collider->layer;
        if (SliderUintRow("Layer", &layer, 0, kPhysicsLayers - 1))
            collider->layer = static_cast<std::uint8_t>(layer);
        ImGui::EndTable();
        if (collider->shape == ColliderShape::Mesh && registry.Has<RigidBody>(e) &&
            registry.Get<RigidBody>(e).type == BodyType::Dynamic)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Mesh colliders are static or kinematic");
        const std::optional<std::pair<glm::vec3, glm::vec3>> bounds = MeshBounds(e);
        if (bounds && collider->shape != ColliderShape::Mesh) {
            if (ImGui::Button("Fit to mesh")) {
                const Collider fitted = FitCollider(collider->shape, bounds->first, bounds->second);
                collider->halfExtents = fitted.halfExtents;
                collider->radius      = fitted.radius;
                collider->halfHeight  = fitted.halfHeight;
                collider->center      = fitted.center;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Remove collider"))
            registry.Remove<Collider>(e);
    }

    if (CharacterController* character = registry.TryGet<CharacterController>(e);
        character && ImGui::CollapsingHeader("Character Controller", ImGuiTreeNodeFlags_DefaultOpen) &&
        BeginProperties("character")) {
        DragFloatRow("Radius", &character->radius, 0.005f, 0.01f, 10.0f);
        DragFloatRow("Height", &character->height, 0.005f, 0.05f, 20.0f);
        PropertyRow("Max slope");
        ImGui::SliderAngle("##v", &character->maxSlope, 0.0f, 89.0f);
        ImGui::PopID();
        DragFloatRow("Step height", &character->stepHeight, 0.005f, 0.0f, 5.0f);
        DragFloatRow("Jump speed", &character->jumpSpeed, 0.05f, 0.0f, 100.0f, "%.2f m/s");
        if (m_Ctx.physics) {
            if (const std::optional<CharacterState> state = m_Ctx.physics->GetCharacterState(e)) {
                PropertyRow("State");
                ImGui::TextDisabled("%s, v %.2f m/s", state->onGround ? "on ground" : "in air", glm::length(state->velocity));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        if (ImGui::Button("Remove character controller"))
            registry.Remove<CharacterController>(e);
    }

    if (CameraComponent* cam = registry.TryGet<CameraComponent>(e);
        cam && ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("camera component")) {
        PropertyRow("Field of view");
        ImGui::SliderAngle("##v", &cam->fovY, 10.0f, 150.0f);
        ImGui::PopID();
        DragFloatRow("Near plane", &cam->nearPlane, 0.001f, 0.001f, 10.0f);
        CheckboxRow("Primary", &cam->primary);
        ImGui::EndTable();
        ImGui::TextDisabled("The player renders through the first primary camera.");
        if (ImGui::Button("Remove camera"))
            registry.Remove<CameraComponent>(e);
    }

    if (ScriptComponent* script = registry.TryGet<ScriptComponent>(e);
        script && ImGui::CollapsingHeader("Script (Blueprint)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##graph", "path/to/script.ugraph", &script->graph);
        std::error_code             ec;
        const std::filesystem::path dir = !script->graph.empty() ? std::filesystem::path(script->graph).parent_path()
                                          : !m_ScenePath.empty() ? m_ScenePath.parent_path()
                                                                 : std::filesystem::current_path(ec);
        if (ImGui::Button("Edit") && !script->graph.empty()) {
            if (m_Graphs->Open(script->graph)) {
                m_ShowBlueprint = true;
                m_Graphs->Focus();
            } else {
                m_Status = "Cannot open script (see log)";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            m_ScriptTarget  = e;
            m_DialogPurpose = DialogPurpose::AssignScript;
            m_FileDialog->Open("Choose script", FileDialog::Mode::Open, dir, {".ugraph"});
        }
        ImGui::SameLine();
        if (ImGui::Button("New...")) {
            m_ScriptTarget  = e;
            m_DialogPurpose = DialogPurpose::NewScript;
            m_FileDialog->Open("New script", FileDialog::Mode::Save, dir, {".ugraph"}, registry.Get<Name>(e).value + ".ugraph");
        }
        if (m_Ctx.scripts && m_Ctx.scripts->Running() && !script->graph.empty())
            if (const ScriptDebugInfo* info = m_Ctx.scripts->Debug(script->graph)) {
                const auto errors = std::ranges::count_if(info->diagnostics, [](const ScriptDiagnostic& d) { return d.error; });
                if (errors)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%d error(s) - see the Blueprint window", static_cast<int>(errors));
                else
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running");
            }
        DrawScriptVariables(*script);
        if (ImGui::Button("Remove script"))
            registry.Remove<ScriptComponent>(e);
    }

    if (Tags* tags = registry.TryGet<Tags>(e); tags && ImGui::CollapsingHeader("Tags", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < tags->values.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (i && ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(tags->values[i].c_str()).x + 40.0f)
                ImGui::SameLine();
            if (ImGui::SmallButton((tags->values[i] + "  x").c_str()))
                remove = i;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove tag");
            ImGui::PopID();
        }
        if (remove)
            tags->values.erase(tags->values.begin() + static_cast<std::ptrdiff_t>(*remove));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##newtag", "add a tag (Enter)", &m_NewTag, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (!m_NewTag.empty() && !tags->Has(m_NewTag))
                tags->values.push_back(m_NewTag);
            m_NewTag.clear();
            ImGui::SetKeyboardFocusHere(-1);
        }
        if (ImGui::Button("Remove tags"))
            registry.Remove<Tags>(e);
    }

    if (AudioSource* audio = registry.TryGet<AudioSource>(e);
        audio && ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##sound", "path/to/sound.wav", &audio->sound);
        const std::filesystem::path dir = !audio->sound.empty() ? PathFromUtf8(audio->sound).parent_path()
                                                                : ContentRoot();
        if (ImGui::Button("Browse...##sound")) {
            m_SoundTarget   = e;
            m_DialogPurpose = DialogPurpose::AssignSound;
            m_FileDialog->Open("Choose sound", FileDialog::Mode::Open, dir, {".wav", ".ogg", ".mp3", ".flac"});
        }
        if (m_Ctx.audio) {
            ImGui::SameLine();
            if (m_Ctx.audio->Previewing()) {
                if (ImGui::Button("Stop preview"))
                    m_Ctx.audio->StopPreview();
            } else if (ImGui::Button("Preview") && !audio->sound.empty()) {
                m_Ctx.audio->Preview(audio->sound);
            }
        }
        if (BeginProperties("audio")) {
            BusComboRow("Bus", &audio->bus);
            DragFloatRow("Volume", &audio->volume, 0.01f, 0.0f, 4.0f);
            DragFloatRow("Pitch", &audio->pitch, 0.01f, 0.01f, 16.0f);
            CheckboxRow("Loop", &audio->loop);
            CheckboxRow("Play on start", &audio->playOnStart);
            CheckboxRow("Stream", &audio->stream);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Always stream from the file (music); longer than %.0f s streams anyway",
                                  kSoundStreamThresholdSeconds);
            DragFloatRow("Fade in", &audio->fadeIn, 0.01f, 0.0f, 60.0f, "%.2f s");
            CheckboxRow("3D (spatial)", &audio->spatial);
            if (audio->spatial) {
                ComboRow("Attenuation", &audio->attenuation, kAttenuationNames);
                DragFloatRow("Min distance", &audio->minDistance, 0.01f, 0.01f, 1e4f);
                DragFloatRow("Max distance", &audio->maxDistance, 0.1f, audio->minDistance, 1e5f);
                DragFloatRow("Rolloff", &audio->rolloff, 0.01f, 0.0f, 10.0f);
                DragFloatRow("Doppler", &audio->doppler, 0.01f, 0.0f, 10.0f);
                CheckboxRow("Occlusion", &audio->occlusion);
            }
            if (m_Ctx.audio && m_Ctx.audio->Running()) {
                PropertyRow("State");
                ImGui::TextUnformatted(m_Ctx.audio->IsPlaying(e) ? "playing" : "stopped");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (m_Ctx.audio && m_Ctx.audio->Running()) {
            if (ImGui::Button("Play"))
                m_Ctx.audio->Play(e);
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                m_Ctx.audio->Stop(e, 0.1f);
            ImGui::SameLine();
        }
        if (ImGui::Button("Remove audio source"))
            registry.Remove<AudioSource>(e);
    }

    if (registry.Has<AudioListener>(e) && ImGui::CollapsingHeader("Audio Listener", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("The scene is heard from here (else from the primary camera).");
        if (ImGui::Button("Remove listener"))
            registry.Remove<AudioListener>(e);
    }

    if (ReverbZone* zone = registry.TryGet<ReverbZone>(e);
        zone && ImGui::CollapsingHeader("Reverb Zone", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("reverb")) {
        Vec3Row("Half extents", &zone->halfExtents, 0.01f, 5.0f);
        zone->halfExtents = glm::max(zone->halfExtents, glm::vec3(0.0f));
        DragFloatRow("Blend distance", &zone->blendDistance, 0.01f, 0.0f, 100.0f);
        ReverbRows(&zone->reverb);
        ImGui::EndTable();
        ImGui::TextDisabled("World-bus sounds get this room while the listener is inside.");
        if (ImGui::Button("Remove reverb zone"))
            registry.Remove<ReverbZone>(e);
    }

    ImGui::Separator();
    if (ImGui::Button("Add component"))
        ImGui::OpenPopup("add component");
    if (ImGui::BeginPopup("add component")) {
        if (ImGui::MenuItem("Point Light", nullptr, false, !registry.Has<Light>(e)))
            registry.Emplace<Light>(e, Light{.type = LightType::Point});
        if (ImGui::MenuItem("Spot Light", nullptr, false, !registry.Has<Light>(e)))
            registry.Emplace<Light>(e, Light{.type = LightType::Spot});
        ImGui::Separator();
        // Physics: a body with a collider fitted to the mesh (box), or just one of them.
        const auto fitted = [&](ColliderShape shape) {
            const auto bounds = MeshBounds(e);
            return bounds ? FitCollider(shape, bounds->first, bounds->second) : Collider{.shape = shape};
        };
        if (ImGui::MenuItem("Dynamic body (box)", nullptr, false, !registry.Has<RigidBody>(e) && !registry.Has<Collider>(e))) {
            registry.Emplace<RigidBody>(e);
            registry.Emplace<Collider>(e, fitted(ColliderShape::Box));
        }
        if (ImGui::MenuItem("Static body (mesh)", nullptr, false,
                            !registry.Has<RigidBody>(e) && !registry.Has<Collider>(e) && registry.Has<MeshRenderer>(e))) {
            registry.Emplace<RigidBody>(e, RigidBody{.type = BodyType::Static});
            registry.Emplace<Collider>(e, Collider{.shape = ColliderShape::Mesh});
        }
        if (ImGui::MenuItem("Rigid Body", nullptr, false, !registry.Has<RigidBody>(e)))
            registry.Emplace<RigidBody>(e);
        if (ImGui::BeginMenu("Collider", !registry.Has<Collider>(e))) {
            for (std::size_t i = 0; i < std::size(kColliderShapeNames); ++i)
                if (ImGui::MenuItem(kColliderShapeNames[i]))
                    registry.Emplace<Collider>(e, fitted(static_cast<ColliderShape>(i)));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Character Controller", nullptr, false, !registry.Has<CharacterController>(e)))
            registry.Emplace<CharacterController>(e);
        ImGui::Separator();
        if (ImGui::MenuItem("Script (Blueprint)", nullptr, false, !registry.Has<ScriptComponent>(e)))
            registry.Emplace<ScriptComponent>(e);
        if (ImGui::MenuItem("Camera", nullptr, false, !registry.Has<CameraComponent>(e)))
            registry.Emplace<CameraComponent>(e);
        if (ImGui::MenuItem("Tags", nullptr, false, !registry.Has<Tags>(e)))
            registry.Emplace<Tags>(e);
        ImGui::Separator();
        if (ImGui::MenuItem("Audio Source", nullptr, false, !registry.Has<AudioSource>(e)))
            registry.Emplace<AudioSource>(e);
        if (ImGui::MenuItem("Audio Listener", nullptr, false, !registry.Has<AudioListener>(e)))
            registry.Emplace<AudioListener>(e);
        if (ImGui::MenuItem("Reverb Zone", nullptr, false, !registry.Has<ReverbZone>(e)))
            registry.Emplace<ReverbZone>(e);
        ImGui::EndPopup();
    }

    // One undo step per edit: instant widgets (checkbox, combo, buttons) push right away, drags and
    // text fields when they are released. Detected by comparing the entity's state.
    // Multi-selection: the same change (changed fields, added / removed components) goes to the
    // other selected entities.
    const std::uint64_t    uuid    = registry.Get<Uuid>(e).value;
    const bool             editing = ImGui::IsAnyItemActive() && GImGui->ActiveIdWindow == ImGui::GetCurrentWindow();
    const std::string      after   = SnapshotEntityState(m_Ctx.scene, e);
    const bool             changed = after != frameState;
    std::vector<StateEdit> others;
    if (changed) {
        m_Ctx.scene.MarkChanged(e); // light / mesh edits: bounds and shadow caches
        for (Entity other : m_Selection) {
            if (other == e)
                continue;
            std::string before = SnapshotEntityState(m_Ctx.scene, other);
            if (ApplyEntityStateDiff(m_Ctx.scene, other, frameState, after))
                others.push_back({UuidOf(other), std::move(before)});
        }
    }
    const auto addOthers = [&](std::vector<StateEdit>& edit) { // first change of each entity only
        for (StateEdit& o : others)
            if (std::ranges::none_of(edit, [&](const StateEdit& x) { return x.uuid == o.uuid; }))
                edit.push_back(std::move(o));
    };
    if (m_InspectorEdit && m_InspectorEdit->front().uuid != uuid) // the primary changed mid-edit
        PushStateChange("Edit properties", std::exchange(m_InspectorEdit, std::nullopt).value());
    if (m_InspectorEdit) {
        addOthers(*m_InspectorEdit);
        if (!editing) // released: one step for the whole drag / typing session
            PushStateChange("Edit properties", std::exchange(m_InspectorEdit, std::nullopt).value());
    } else if (changed) {
        std::vector<StateEdit> edit{StateEdit{uuid, frameState}};
        addOthers(edit);
        if (editing)
            m_InspectorEdit = std::move(edit);
        else
            PushStateChange("Edit properties", std::move(edit));
    }
    ImGui::End();
}

std::optional<std::pair<glm::vec3, glm::vec3>> Editor::MeshBounds(Entity entity) const
{
    const MeshRenderer* mesh  = m_Ctx.scene.GetRegistry().TryGet<MeshRenderer>(entity);
    const Model*        model = mesh ? m_Ctx.assets.Get(mesh->model) : nullptr;
    if (!model || mesh->meshIndex >= model->meshes.size() || model->meshes[mesh->meshIndex].submeshes.empty())
        return std::nullopt;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const Submesh& sm : model->meshes[mesh->meshIndex].submeshes) {
        lo = glm::min(lo, sm.boundsMin);
        hi = glm::max(hi, sm.boundsMax);
    }
    return std::pair{lo, hi};
}

void Editor::DrawPrefabHeader(Entity entity)
{
    const Registry& registry = m_Ctx.scene.GetRegistry();
    const Entity    root     = PrefabInstanceRoot(m_Ctx.scene, entity);
    const auto&     instance = registry.Get<PrefabInstance>(root);
    const bool      edit     = m_PlayState == PlayState::Edit;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.2f, 0.32f, 1.0f));
    ImGui::BeginChild("prefab", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "Prefab %s", PathToUtf8(PathFromUtf8(instance.prefab).filename()).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", instance.prefab.c_str());
    if (!instance.built && instance.unresolved)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "The prefab file is missing: the instance keeps its saved data");
    if (root != entity) {
        ImGui::SameLine();
        ImGui::TextDisabled("member of '%s'", registry.Get<Name>(root).value.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select root"))
            Select(root);
    }
    // Overridden values of this entity (each can go back to the prefab's).
    const std::vector<std::string> keys = PrefabOverriddenKeys(m_Ctx.scene, &m_Ctx.assets, entity);
    if (!keys.empty()) {
        ImGui::TextDisabled("Overrides:");
        for (const std::string& key : keys) {
            ImGui::SameLine();
            ImGui::PushID(key.c_str());
            ImGui::BeginDisabled(!edit);
            if (ImGui::SmallButton((key + "  x").c_str()))
                m_PendingPrefabOp = PendingPrefabOp{PrefabOp::Revert, UuidOf(entity), key};
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Revert '%s' to the prefab", key.c_str());
            ImGui::PopID();
        }
    }
    if (root == entity) {
        const std::size_t changes = PrefabOverrides(m_Ctx.scene, &m_Ctx.assets, root).size();
        ImGui::BeginDisabled(!edit);
        if (ImGui::Button("Apply to prefab"))
            m_PendingPrefabOp = PendingPrefabOp{PrefabOp::Apply, UuidOf(entity), {}};
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes this instance into the prefab file; the other instances update (their overrides stay)");
        ImGui::SameLine();
        if (ImGui::Button("Revert all"))
            m_PendingPrefabOp = PendingPrefabOp{PrefabOp::Revert, UuidOf(entity), {}};
        ImGui::SameLine();
        if (ImGui::Button("Unlink"))
            m_PendingPrefabOp = PendingPrefabOp{PrefabOp::Unlink, UuidOf(entity), {}};
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(changes ? "%zu changed entit%s" : "matches the prefab", changes, changes == 1 ? "y" : "ies");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

const ScriptGraph* Editor::GraphFor(const std::string& file)
{
    if (file.empty())
        return nullptr;
    if (const ScriptGraph* open = m_Graphs->Find(PathFromUtf8(file)))
        return open; // unsaved edits count
    std::error_code ec;
    const auto      mtime = std::filesystem::last_write_time(PathFromUtf8(file), ec);
    if (ec)
        return nullptr;
    auto it = m_GraphCache.find(file);
    if (it == m_GraphCache.end() || it->second.first != mtime) {
        try {
            m_GraphCache[file] = {mtime, LoadScriptGraph(PathFromUtf8(file))};
        } catch (const std::exception&) {
            m_GraphCache.erase(file);
            return nullptr;
        }
        it = m_GraphCache.find(file);
    }
    return &it->second.second;
}

void Editor::DrawScriptVariables(ScriptComponent& script)
{
    const ScriptGraph* graph = GraphFor(script.graph);
    if (!graph)
        return;
    bool any = false;
    for (const ScriptVariable& v : graph->variables)
        any |= v.exposed;
    if (!any) {
        ImGui::TextDisabled("No instance editable variables (tick the box next to a variable in the Blueprint)");
        return;
    }
    if (!BeginProperties("scriptvars"))
        return;
    std::optional<std::string> reset;
    for (const ScriptVariable& v : graph->variables) {
        if (!v.exposed)
            continue;
        const auto overridden = script.variables.find(v.name);
        PropertyRow(v.name.c_str());
        if (overridden == script.variables.end())
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f); // the graph's default
        const float width = overridden != script.variables.end() ? -28.0f : -FLT_MIN;
        if (v.type == PinType::Entity) { // pick an entity of the scene (stored by UUID)
            const std::uint64_t current = overridden != script.variables.end() ? overridden->second.entityUuid : 0;
            const Entity        target  = current ? m_Ctx.scene.FindByUuid(current) : NullEntity;
            const std::string   preview = !current ? "(self / none)"
                                          : target != NullEntity ? m_Ctx.scene.GetRegistry().Get<Name>(target).value
                                                                 : "(missing)";
            ImGui::SetNextItemWidth(width);
            if (ImGui::BeginCombo("##entity", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("(self / none)", current == 0))
                    reset = v.name;
                int shown = 0;
                m_Ctx.scene.GetRegistry().ViewOf<Name>().Each([&](Entity other, Name& name) {
                    if (++shown > 2000)
                        return;
                    const std::uint64_t uuid = UuidOf(other);
                    ImGui::PushID(static_cast<int>(EntityIndex(other)));
                    if (ImGui::Selectable(name.value.c_str(), uuid == current))
                        script.variables[v.name] = ScriptVariableOverride{.value = NullEntity, .entityUuid = uuid};
                    ImGui::PopID();
                });
                ImGui::EndCombo();
            }
        } else {
            ScriptValue value = overridden != script.variables.end() ? Convert(overridden->second.value, v.type) : v.value;
            if (ScriptGraphEditor::EditValue("##value", value, v.type, width))
                script.variables[v.name] = ScriptVariableOverride{.value = std::move(value), .entityUuid = 0};
        }
        if (overridden == script.variables.end()) {
            ImGui::PopStyleVar();
        } else {
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                reset = v.name;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Back to the graph's value");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (reset)
        script.variables.erase(*reset);
}

// ---------------------------------------------------------------------------------------------
// Renderer settings
// ---------------------------------------------------------------------------------------------

void Editor::DrawRendererSettings()
{
    if (!ImGui::Begin("Renderer", &m_ShowRenderer)) {
        ImGui::End();
        return;
    }
    SceneRenderer& sr = m_Ctx.sceneRenderer;

    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("lighting")) {
        SkySettings& sky = sr.lighting.sky;
        // sunDirection is where the light travels; the UI edits where the sun is.
        const glm::vec3 toSun     = -glm::normalize(sky.sunDirection);
        float           elevation = std::asin(std::clamp(toSun.y, -1.0f, 1.0f));
        float           azimuth   = std::atan2(toSun.x, toSun.z);
        PropertyRow("Sun azimuth");
        bool sunChanged = ImGui::SliderAngle("##v", &azimuth, -180.0f, 180.0f);
        ImGui::PopID();
        PropertyRow("Sun elevation");
        sunChanged |= ImGui::SliderAngle("##v", &elevation, -10.0f, 89.0f); // zenith: azimuth undefined
        ImGui::PopID();
        if (sunChanged) {
            const glm::vec3 dir{std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                                std::cos(elevation) * std::cos(azimuth)};
            sky.sunDirection = -dir;
        }
        PropertyRow("Sun color");
        ImGui::ColorEdit3("##v", &sky.sunColor.x, ImGuiColorEditFlags_Float);
        ImGui::PopID();
        DragFloatRow("Sun intensity", &sky.sunIntensity, 0.02f, 0.0f, 100.0f);
        DragFloatRow("Sky intensity", &sky.skyIntensity, 0.01f, 0.0f, 20.0f);
        DragFloatRow("IBL intensity", &sr.lighting.iblIntensity, 0.01f, 0.0f, 10.0f);
        ImGui::EndTable();
        ImGui::TextDisabled("Sun/sky changes regenerate the IBL maps.");
    }

    if (ImGui::CollapsingHeader("Post processing", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("post")) {
        PostSettings& post = sr.post;
        EnumComboRow("Tonemapper", &post.tonemapper);
        CheckboxRow("Auto exposure", &post.autoExposure);
        SliderFloatRow(post.autoExposure ? "Compensation" : "Exposure", &post.exposure, 0.01f, 16.0f, "%.3f",
                       ImGuiSliderFlags_Logarithmic);
        if (post.autoExposure) {
            SliderFloatRow("Key", &post.exposureKey, 0.02f, 1.0f);
            SliderFloatRow("Adaptation", &post.adaptationSpeed, 0.1f, 10.0f, "%.2f /s");
            SliderFloatRow("Min log2 lum", &post.minLogLuminance, -16.0f, 0.0f, "%.1f");
            SliderFloatRow("Max log2 lum", &post.maxLogLuminance, 0.0f, 16.0f, "%.1f");
        }
        CheckboxRow("Bloom", &post.bloom);
        if (post.bloom) {
            SliderFloatRow("Bloom strength", &post.bloomStrength, 0.0f, 0.5f);
            SliderFloatRow("Bloom radius", &post.bloomRadius, 0.0005f, 0.02f, "%.4f");
        }
        EnumComboRow("Debug view", &post.debugView);
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Culling (GPU-driven)") && BeginProperties("culling")) {
        CullingSettings& c = m_Ctx.sceneRenderer.culling;
        CheckboxRow("GPU-driven", &c.gpuDriven);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Compute culling + indirect multi-draws (off: CPU BVH culling, one draw per submesh)");
        ImGui::BeginDisabled(!c.gpuDriven);
        CheckboxRow("Occlusion (Hi-Z)", &c.occlusion);
        CheckboxRow("Freeze culling", &c.freeze);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keep culling with the current camera and Hi-Z; move away to see what was culled");
        SliderUintRow("Hi-Z debug level", &c.hizDebugLevel, 0, 12);
        ImGui::EndDisabled();
        CheckboxRow("Mesh LODs", &c.lod);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Coarsest level whose simplification error stays below the pixel threshold (both paths, "
                              "shadows follow the camera)");
        ImGui::BeginDisabled(!c.lod);
        SliderFloatRow("LOD pixel error", &c.lodPixelError, 0.25f, 32.0f, "%.2f px", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
        PropertyRow("Force LOD");
        ImGui::SliderInt("##v", &c.forceLod, -1, static_cast<int>(kMaxLods) - 1, c.forceLod < 0 ? "auto" : "LOD %d",
                         ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Lights (clustered)") && BeginProperties("lights")) {
        CheckboxRow("Enabled", &sr.lights.enabled);
        DragFloatRow("Cluster far", &sr.lights.clusterFar, 1.0f, 1.0f, 100000.0f, "%.0f");
        PropertyRow("Visible");
        ImGui::Text("%u / %u (max %u)", sr.Stats().lights, sr.Stats().lightsTotal, kMaxVisibleLights);
        ImGui::PopID();
        ImGui::EndTable();
        ImGui::TextDisabled("Grid %ux%ux%u, %u lights per cluster. Debug view: Light clusters.", kClusterGridX,
                            kClusterGridY, kClusterGridZ, kClusterMaxLights);
    }

    if (ImGui::CollapsingHeader("Local light shadows") && BeginProperties("localShadows")) {
        LocalShadowSettings& s = sr.localShadows;
        CheckboxRow("Enabled", &s.enabled);
        const auto powerOfTwoCombo = [](const char* label, std::uint32_t* value, std::initializer_list<std::uint32_t> options) {
            PropertyRow(label);
            char current[16];
            std::snprintf(current, sizeof(current), "%u", *value);
            if (ImGui::BeginCombo("##v", current)) {
                for (std::uint32_t option : options) {
                    char text[16];
                    std::snprintf(text, sizeof(text), "%u", option);
                    if (ImGui::Selectable(text, option == *value))
                        *value = option;
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        };
        powerOfTwoCombo("Atlas size", &s.atlasSize, {1024, 2048, 4096, 8192});
        SliderUintRow("Max lights", &s.maxLights, 0, 64);
        powerOfTwoCombo("Max tile", &s.maxTileSize, {128, 256, 512, 1024, 2048});
        powerOfTwoCombo("Min tile", &s.minTileSize, {32, 64, 128, 256});
        SliderFloatRow("Depth bias", &s.depthBias, 0.0f, 10.0f, "%.2f");
        SliderFloatRow("Slope bias", &s.slopeBias, 0.0f, 10.0f, "%.2f");
        SliderFloatRow("Normal bias", &s.normalBias, 0.0f, 5.0f, "%.2f");
        SliderFloatRow("Filter radius", &s.filterRadius, 0.0f, 5.0f, "%.2f");
        PropertyRow("This frame");
        ImGui::Text("%u lights, %u tiles, %u draws", sr.Stats().shadowedLights, sr.Stats().shadowTiles,
                    sr.Stats().localShadowDraws);
        ImGui::PopID();
        ImGui::EndTable();
        ImGui::TextDisabled("Point light = 6 tiles. Debug view: Shadow atlas.");
    }

    if (ImGui::CollapsingHeader("Sun shadows") && BeginProperties("shadows")) {
        ShadowSettings& s = sr.shadows;
        CheckboxRow("Enabled", &s.enabled);
        SliderUintRow("Cascades", &s.cascadeCount, 1, kMaxCascades);
        PropertyRow("Resolution");
        static constexpr std::uint32_t kResolutions[] = {512, 1024, 2048, 4096};
        char                           current[16];
        std::snprintf(current, sizeof(current), "%u", s.resolution);
        if (ImGui::BeginCombo("##v", current)) {
            for (std::uint32_t r : kResolutions) {
                char label[16];
                std::snprintf(label, sizeof(label), "%u", r);
                if (ImGui::Selectable(label, r == s.resolution))
                    s.resolution = r;
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        DragFloatRow("Max distance", &s.maxDistance, 0.1f, 1.0f, 1000.0f, "%.1f");
        SliderFloatRow("Split lambda", &s.splitLambda, 0.0f, 1.0f, "%.2f");
        SliderFloatRow("Depth bias", &s.depthBias, 0.0f, 10.0f, "%.2f");
        SliderFloatRow("Slope bias", &s.slopeBias, 0.0f, 10.0f, "%.2f");
        SliderFloatRow("Normal bias", &s.normalBias, 0.0f, 5.0f, "%.2f");
        SliderFloatRow("Filter radius", &s.filterRadius, 0.0f, 5.0f, "%.2f");
        SliderFloatRow("Cascade blend", &s.cascadeBlend, 0.0f, 0.5f, "%.2f");
        CheckboxRow("Debug cascades", &s.debugCascades);
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Ambient occlusion") && BeginProperties("ao")) {
        AoSettings& ao = sr.ao;
        CheckboxRow("Enabled", &ao.enabled);
        SliderFloatRow("Radius", &ao.radius, 0.05f, 5.0f, "%.2f");
        SliderFloatRow("Falloff", &ao.falloff, 0.0f, 1.0f, "%.2f");
        SliderFloatRow("Power", &ao.power, 0.1f, 4.0f, "%.2f");
        SliderUintRow("Slices", &ao.sliceCount, 1, 4);
        SliderUintRow("Steps per side", &ao.stepsPerSide, 1, 8);
        SliderFloatRow("Sharpness", &ao.sharpness, 0.0f, 100.0f, "%.1f");
        ImGui::EndTable();
    }

    DrawAudioSettings();

    if (m_Ctx.physics && ImGui::CollapsingHeader("Physics") && BeginProperties("physics")) {
        PhysicsSettings& ps = m_Ctx.physics->settings;
        Vec3Row("Gravity", &ps.gravity, 0.05f, 0.0f);
        PropertyRow("Collision steps");
        ImGui::SliderInt("##v", &ps.collisionSteps, 1, 8);
        ImGui::PopID();
        DragFloatRow("Air control", &ps.airControl, 0.01f, 0.0f, 20.0f, "%.2f /s");
        CheckboxRow("Interpolate", &ps.interpolate);
        CheckboxRow("Show colliders", &m_ShowColliders);
        ImGui::EndTable();

        // Lower triangle of the symmetric layer matrix: row a, column b <= a.
        if (ImGui::TreeNode("Layer collision")) {
            static int shownLayers = 4;
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderInt("Layers shown", &shownLayers, 1, static_cast<int>(kPhysicsLayers));
            const int n = std::clamp(shownLayers, 1, static_cast<int>(kPhysicsLayers));
            if (ImGui::BeginTable("layers", n + 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                for (int b = 0; b < n; ++b) {
                    ImGui::TableSetColumnIndex(b + 1);
                    ImGui::Text("%d", b);
                }
                for (int a = 0; a < n; ++a) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", a);
                    for (int b = 0; b <= a; ++b) {
                        ImGui::TableSetColumnIndex(b + 1);
                        ImGui::PushID(a * static_cast<int>(kPhysicsLayers) + b);
                        bool collide = ps.LayersCollide(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
                        if (ImGui::Checkbox("##c", &collide))
                            ps.SetLayerCollision(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b), collide);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Camera") && BeginProperties("camera")) {
        FlyCamera& cam = m_Ctx.camera;
        PropertyRow("Field of view");
        ImGui::SliderAngle("##v", &cam.fovY, 20.0f, 120.0f);
        ImGui::PopID();
        DragFloatRow("Near plane", &cam.nearPlane, 0.001f, 0.001f, 10.0f);
        DragFloatRow("Move speed", &cam.moveSpeed, 0.05f, 0.1f, 100.0f, "%.2f m/s");
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Frame diagnostics
// ---------------------------------------------------------------------------------------------

void Editor::DrawStats()
{
    if (!ImGui::Begin("Stats", &m_ShowStats)) {
        ImGui::End();
        return;
    }
    const VulkanContext& ctx = m_Ctx.renderer.GetContext();
    ImGui::Text("%s", ctx.DeviceName().c_str());
    const std::uint32_t validationErrors = VulkanContext::ValidationErrorCount();
    if (validationErrors > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, 1.0f), "  %u validation errors", validationErrors);
    }

    // CPU frame time (ring buffer, oldest first via the offset).
    const auto  samples = static_cast<float>(std::ranges::count_if(m_FrameTimes, [](float ms) { return ms > 0.0f; }));
    const float average = std::accumulate(m_FrameTimes.begin(), m_FrameTimes.end(), 0.0f) / std::max(samples, 1.0f);
    const float peak    = *std::ranges::max_element(m_FrameTimes);
    char        overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%.2f ms avg (%.0f FPS), %.2f ms max", average,
                  average > 0.0f ? 1000.0f / average : 0.0f, peak);
    ImGui::PlotLines("##frametimes", m_FrameTimes.data(), static_cast<int>(kHistory), static_cast<int>(m_FrameTimeHead),
                     overlay, 0.0f, std::max(peak * 1.2f, 1.0f), ImVec2(-FLT_MIN, 70.0f));

    if (ImGui::BeginTable("columns", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();

        // GPU timings.
        ImGui::TableSetColumnIndex(0);
        ImGui::SeparatorText("GPU (ms)");
        const GpuProfiler& profiler = m_Ctx.renderer.Profiler();
        if (!profiler.Supported()) {
            ImGui::TextDisabled("Timestamps not supported");
        } else {
            const auto   timings = profiler.Results();
            const double frameMs = timings.empty() ? 0.0 : timings.front().milliseconds;
            if (ImGui::BeginTable("gpu", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                for (const GpuTiming& t : timings) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Indent(static_cast<float>(t.depth) * 10.0f + 0.001f);
                    ImGui::TextUnformatted(t.name);
                    ImGui::Unindent(static_cast<float>(t.depth) * 10.0f + 0.001f);
                    ImGui::TableSetColumnIndex(1);
                    char label[32];
                    std::snprintf(label, sizeof(label), "%.3f", t.milliseconds);
                    const float fraction = frameMs > 0.0 ? static_cast<float>(t.milliseconds / frameMs) : 0.0f;
                    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), label);
                }
                ImGui::EndTable();
            }
        }

        // Scene + memory.
        ImGui::TableSetColumnIndex(1);
        ImGui::SeparatorText("Scene");
        const SceneRenderStats& stats = m_Ctx.sceneRenderer.Stats();
        ImGui::Text("Draw calls     %u%s", stats.drawCalls, stats.gpuDriven ? " (indirect multi-draws)" : "");
        ImGui::Text("Culled         %u", stats.culled);
        ImGui::Text("Shadow draws   %u", stats.shadowDraws);
        ImGui::Text("Triangles      %llu", static_cast<unsigned long long>(stats.triangles));
        ImGui::Text("Lights         %u / %u", stats.lights, stats.lightsTotal);
        ImGui::Text("Shadowed       %u (%u tiles, %u re-rendered, %u draws)", stats.shadowedLights, stats.shadowTiles,
                    stats.shadowTilesRendered, stats.localShadowDraws);
        ImGui::Text("Entities       %zu", m_Ctx.scene.GetRegistry().AliveCount());
        ImGui::Text("Exposure       %.3f", stats.exposure);
        if (m_Ctx.sceneRenderer.post.autoExposure)
            ImGui::Text("Avg luminance  %.4f", stats.averageLuminance);

        ImGui::SeparatorText(stats.gpuDriven ? "GPU-driven culling (2 frames old)" : "GPU scene");
        ImGui::Text("Instances      %u  (%u draws, %u batches)", stats.instances, stats.draws, stats.batches);
        if (stats.gpuDriven) {
            ImGui::Text("Camera         %u tested: %u frustum, %u occluded", stats.gpuTested, stats.gpuFrustumCulled,
                        stats.gpuOccluded);
            ImGui::Text("Drawn          %u early + %u late", stats.gpuEarly, stats.gpuLate);
            ImGui::Text("Commands       %u  (all views)", stats.gpuCommands);
        }
        ImGui::Text("LOD > 0        %u camera draws", stats.lodDraws);
        ImGui::Text("Geometry pool  %.1f / %.1f M vertices, %.1f / %.1f M indices",
                    static_cast<double>(stats.geometryVertices) * 1e-6, static_cast<double>(stats.geometryVertexCapacity) * 1e-6,
                    static_cast<double>(stats.geometryIndices) * 1e-6, static_cast<double>(stats.geometryIndexCapacity) * 1e-6);

        ImGui::SeparatorText("CPU (ms)");
        const TransformUpdateStats& xf      = m_Ctx.scene.FrameTransformUpdate();
        const SpatialIndex&         spatial = m_Ctx.sceneRenderer.Spatial();
        ImGui::Text("Transforms     %.3f  (%u subtrees, %u matrices)", xf.milliseconds, xf.dirtyRoots, xf.updated);
        ImGui::Text("BVH sync       %.3f  (%u changed, %u re-inserted, %u pending)", stats.cpuSpatialMs,
                    spatial.LastSync().changed, spatial.LastSync().reinserted, spatial.LastSync().pending);
        ImGui::Text("GPU scene      %.3f  (instance updates)", stats.cpuGpuSceneMs);
        ImGui::Text("Culling        %.3f  (%u CPU draws)", stats.cpuCullingMs, stats.drawItems);

        ImGui::SeparatorText("BVH");
        ImGui::Text("Meshes         %zu  (height %d, %zu nodes)", spatial.MeshCount(), spatial.MeshTree().Height(),
                    spatial.MeshTree().NodeCount());
        ImGui::Text("Lights         %zu  (height %d)", spatial.LightCount(), spatial.LightTree().Height());
        ImGui::Checkbox("Show in viewport", &m_ShowBvh);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderInt("max depth", &m_BvhDepth, 0, 32);

        if (m_Ctx.physics) {
            const PhysicsStats& ps = m_Ctx.physics->Stats();
            ImGui::SeparatorText("Physics");
            ImGui::Text("Bodies         %u  (%u active), %u characters", ps.bodies, ps.activeBodies, ps.characters);
            ImGui::Text("Contacts       %u pairs", ps.contactPairs);
            ImGui::Text("Sync / step    %.3f / %.3f ms  (+%u -%u, %u meshes pending)", ps.syncMs, ps.stepMs, ps.created,
                        ps.removed, ps.pendingMeshes);
        }
        if (m_Ctx.scripts) {
            const ScriptStats& ss = m_Ctx.scripts->Stats();
            ImGui::SeparatorText("Scripts");
            ImGui::Text("Instances      %u  (%s)", ss.instances, m_Ctx.scripts->Running() ? "running" : "stopped");
            ImGui::Text("Last frame     %u events, %u nodes, %u waiting", ss.eventsFired, ss.nodesExecuted, ss.waiting);
            ImGui::Text("Errors         %u", ss.errors);
        }
        if (m_Ctx.audio) {
            const AudioSystemStats& as = m_Ctx.audio->Stats();
            const AudioStats&       es = m_Ctx.audio->Engine().Stats();
            ImGui::SeparatorText("Audio");
            ImGui::Text("Voices         %u  (%u streamed, %u dropped)", es.voices, es.streamed, es.dropped);
            ImGui::Text("Sources        %u  (%u playing, %u loading)", as.sources, as.playing, as.waiting);
            ImGui::Text("One-shots      %u", as.oneShots);
            ImGui::Text("Occlusion      %u rays, %u occluded", as.rays, as.occluded);
            ImGui::Text("Reverb         %u zone(s), wet %.2f, room %.2f", as.zones, as.reverb.wet, as.reverb.roomSize);
        }

        ImGui::SeparatorText("Memory");
        const VkPhysicalDeviceMemoryProperties* memory = nullptr;
        vmaGetMemoryProperties(ctx.Allocator(), &memory);
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(ctx.Allocator(), budgets.data());
        for (std::uint32_t i = 0; i < memory->memoryHeapCount; ++i) {
            const bool   deviceLocal = (memory->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            const double usage       = static_cast<double>(budgets[i].usage) / (1024.0 * 1024.0);
            const double budget      = static_cast<double>(budgets[i].budget) / (1024.0 * 1024.0);
            const double allocated   = static_cast<double>(budgets[i].statistics.allocationBytes) / (1024.0 * 1024.0);
            char         label[96];
            std::snprintf(label, sizeof(label), "%.0f / %.0f MB", usage, budget);
            ImGui::Text("Heap %u %s", i, deviceLocal ? "(device)" : "(host)");
            ImGui::ProgressBar(budget > 0.0 ? static_cast<float>(usage / budget) : 0.0f, ImVec2(-FLT_MIN, 0.0f), label);
            ImGui::TextDisabled("  engine: %.1f MB in %u allocations", allocated, budgets[i].statistics.allocationCount);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Assets
// ---------------------------------------------------------------------------------------------

namespace {
std::string Bytes(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= (1ull << 20))
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1 << 20));
    else
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / (1 << 10));
    return text;
}

// State plus "reloading" and the error of a failed reload (the old content stays in use).
void StateCell(AssetState state, bool reloading, const std::string& error)
{
    ImGui::TextColored(StateColor(state), "%s%s", ToString(state), reloading ? " ..." : "");
    if (!error.empty() && state == AssetState::Ready) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(StateColor(AssetState::Failed), "(!)");
    }
    if (ImGui::IsItemHovered() && (!error.empty() || reloading))
        ImGui::SetTooltip("%s%s%s", reloading ? "Reloading - the current content stays in use\n" : "",
                          state == AssetState::Ready && !error.empty() ? "Last reload failed:\n" : "", error.c_str());
}
} // namespace

void Editor::DrawAssets()
{
    if (!ImGui::Begin("Assets", &m_ShowAssets)) {
        ImGui::End();
        ReleaseTexturePreviews(false);
        return;
    }

    const ImGuiStyle& style   = ImGui::GetStyle();
    const float       buttons = ImGui::CalcTextSize("LoadBrowse...").x + style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons - style.ItemSpacing.x);
    const bool submit = ImGui::InputTextWithHint("##path", "path/to/model.glb", &m_LoadPath,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Load") || submit) && !m_LoadPath.empty())
        m_Ctx.modelRefs.push_back(m_Ctx.assets.LoadModel(std::filesystem::path(std::u8string(m_LoadPath.begin(), m_LoadPath.end()))));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        m_DialogPurpose = DialogPurpose::LoadModel;
        std::error_code             ec;
        const std::filesystem::path current(std::u8string(m_LoadPath.begin(), m_LoadPath.end()));
        m_FileDialog->Open("Load model", FileDialog::Mode::Open,
                           std::filesystem::is_directory(current.parent_path(), ec) ? current.parent_path()
                                                                                    : std::filesystem::current_path(),
                           {".glb", ".gltf"});
    }

    // Hot reload of assets and shaders, import settings, upload queue.
    bool hotReload = m_Ctx.assets.HotReload();
    if (ImGui::Checkbox("Hot reload", &hotReload)) {
        m_Ctx.assets.SetHotReload(hotReload);
        m_Ctx.renderer.SetShaderHotReload(hotReload);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Watch model, texture and shader sources: changed files are reloaded / recompiled live");
    if (const ShaderHotReload* shaders = m_Ctx.renderer.ShaderReloader(); shaders && m_Ctx.renderer.ShaderHotReloadEnabled()) {
        ImGui::SameLine();
        ImGui::TextDisabled("shaders: %zu watched, %u recompiled", shaders->WatchedShaders(), shaders->Recompiled());
        if (!shaders->LastError().empty()) {
            ImGui::SameLine();
            ImGui::TextColored(StateColor(AssetState::Failed), "compile error");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", shaders->LastError().c_str());
        }
    }
    const UploadStats upload = m_Ctx.renderer.GetUploader().Stats();
    ImGui::TextDisabled("Textures: %s  |  staging ring %s / %s, %u queued, %u in flight, %u direct  |  %u jobs",
                        m_Ctx.assets.CookSettings().compress ? "BC7 / BC5" : "RGBA8", Bytes(upload.ringUsed).c_str(),
                        Bytes(upload.ringCapacity).c_str(), upload.queuedBatches, upload.inFlightBatches,
                        static_cast<unsigned>(upload.dedicatedStaging), m_Ctx.assets.JobsInFlight());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Staging ring: recorded uploads not yet consumed by the transfer queue.\n"
                          "Queued: batches held back by the per-frame upload budget.\n"
                          "Direct: uploads larger than half the ring (own staging buffer).");

    ModelHandle toRelease;
    if (ImGui::BeginTabBar("assetTabs")) {
        if (ImGui::BeginTabItem("Models")) {
            DrawModelAssets(toRelease);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            DrawTextureAssets();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sounds")) {
            DrawSoundAssets();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ReleaseTexturePreviews(false);

    if (toRelease) { // entities that still use it render nothing
        m_Ctx.modelRefs.erase(std::ranges::find(m_Ctx.modelRefs, toRelease));
        m_Ctx.assets.Release(toRelease);
    }
}

void Editor::DrawSoundAssets()
{
    const std::vector<SoundInfo> sounds = m_Ctx.assets.Sounds();
    if (sounds.empty()) {
        ImGui::TextDisabled("No sounds loaded (audio sources load theirs when play starts, previews at once).");
        return;
    }
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("sounds", 6, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Refs", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableHeadersRow();
    for (const SoundInfo& info : sounds) {
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(info.handle.index));
        ImGui::TableNextColumn();
        const std::string name = PathToUtf8(PathFromUtf8(info.path).filename());
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", info.path.c_str());
        ImGui::TableNextColumn();
        StateCell(info.state, info.reloading, info.error);
        ImGui::TableNextColumn();
        ImGui::Text("%u", info.refCount);
        ImGui::TableNextColumn();
        if (info.sampleRate)
            ImGui::Text("%u Hz %s, %.1f s", info.sampleRate, info.channels == 1 ? "mono" : info.channels == 2 ? "stereo" : "multi",
                        info.duration);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(info.streamed ? "streamed" : Bytes(info.memoryBytes).c_str());
        ImGui::TableNextColumn();
        if (m_Ctx.audio && ImGui::SmallButton("Play"))
            m_Ctx.audio->Preview(PathFromUtf8(info.path));
        ImGui::SameLine();
        if (ImGui::SmallButton(info.state == AssetState::Failed ? "Retry" : "Reload"))
            m_Ctx.assets.Reload(info.handle);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void Editor::DrawAudioSettings()
{
    if (!m_Ctx.audio || !ImGui::CollapsingHeader("Audio"))
        return;
    AudioSettings settings = m_Ctx.audio->Settings();
    bool          changed  = false;
    if (BeginProperties("audio")) {
        for (std::size_t i = 0; i < kAudioBusCount; ++i) {
            PropertyRow(ToString(static_cast<AudioBus>(i)));
            ImGui::PushID(static_cast<int>(i));
            changed |= ImGui::Checkbox("##mute", &settings.muted[i]);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Muted");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::SliderFloat("##v", &settings.volume[i], 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::PopID();
            ImGui::PopID();
        }
        changed |= CheckboxRow("Occlusion", &settings.occlusion);
        changed |= SliderFloatRow("Occlusion strength", &settings.occlusionStrength, 0.0f, 1.0f);
        changed |= SliderUintRow("Rays per frame", &settings.occlusionRays, 1, 256);
        ImGui::EndTable();
    }
    if (changed) {
        m_Ctx.audio->Apply(settings);
        if (m_Ctx.project) {
            m_Ctx.project->settings.audio = settings;
            m_ProjectDirty                = true;
        }
    }
    const AudioEngine& engine = m_Ctx.audio->Engine();
    ImGui::TextDisabled("%s, %u Hz, %u ch", engine.HasDevice() ? engine.DeviceName().c_str() : "no output device (offline)",
                        engine.SampleRate(), engine.Channels());
    if (m_Ctx.project)
        ImGui::TextDisabled(m_ProjectDirty ? "Saved with the project (File > Save all)" : "Stored in the project file");
}

void Editor::DrawModelAssets(ModelHandle& toRelease)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("models", 6, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Refs", ImGuiTableColumnFlags_WidthFixed, 35.0f);
    ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 190.0f);
    ImGui::TableHeadersRow();

    std::uint64_t gpuTotal = 0;
    for (const ModelInfo& info : m_Ctx.assets.Models()) {
        ImGui::PushID(static_cast<int>(info.handle.index));
        ImGui::TableNextRow();
        gpuTotal += info.gpuBytes;

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(info.path.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nrevision %u, %u texture(s)", info.path.c_str(), info.revision, info.textures);

        ImGui::TableSetColumnIndex(1);
        StateCell(info.state, info.reloading, info.error);

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", info.refCount);

        ImGui::TableSetColumnIndex(3);
        if (info.state == AssetState::Ready)
            ImGui::Text("%u (+%u LODs)", info.triangles, info.lodLevels);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("LOD 0 triangles; simplified levels generated over all submeshes");

        ImGui::TableSetColumnIndex(4);
        if (info.state == AssetState::Ready) {
            ImGui::TextUnformatted(Bytes(info.gpuBytes).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("GPU (geometry pool): %s\nCPU (collision copy, meshes): %s",
                                  Bytes(info.gpuBytes).c_str(), Bytes(info.cpuBytes).c_str());
        }

        ImGui::TableSetColumnIndex(5);
        if (const Model* model = m_Ctx.assets.Get(info.handle)) {
            if (ImGui::SmallButton("Instantiate")) {
                const Entity roots[] = {InstantiateModel(m_Ctx.scene, info.handle, *model)};
                PushCreated("Instantiate " + model->name, roots);
                Select(roots[0]);
            }
            ImGui::SameLine();
        }
        if (info.reloadable && (info.state == AssetState::Ready || info.state == AssetState::Failed)) {
            ImGui::BeginDisabled(info.reloading);
            if (ImGui::SmallButton(info.state == AssetState::Failed ? "Retry" : "Reload"))
                (void)m_Ctx.assets.Reload(info.handle);
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (std::ranges::find(m_Ctx.modelRefs, info.handle) != m_Ctx.modelRefs.end()) {
            if (ImGui::SmallButton("Release"))
                toRelease = info.handle;
        } else {
            ImGui::TextDisabled("app");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Loaded by the application; only models loaded here can be released here");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Geometry: %s GPU", Bytes(gpuTotal).c_str());
}

void Editor::DrawTextureAssets()
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float footer = ImGui::GetTextLineHeightWithSpacing();
    if (!ImGui::BeginTable("textures", 8, flags, ImVec2(0.0f, -footer)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34.0f);
    ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 205.0f);
    ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 65.0f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Refs", ImGuiTableColumnFlags_WidthFixed, 35.0f);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableHeadersRow();

    std::uint64_t gpuTotal = 0;
    std::uint32_t cached   = 0, count = 0;
    for (const TextureInfo& info : m_Ctx.assets.Textures()) {
        ImGui::PushID(static_cast<int>(info.handle.index));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 34.0f);
        gpuTotal += info.gpuBytes;
        cached += info.cacheHit ? 1u : 0u;
        ++count;

        ImGui::TableSetColumnIndex(0);
        if (const std::uint64_t preview = TexturePreview(info.handle)) {
            ImGui::Image(ImTextureRef(preview), ImVec2(32.0f, 32.0f));
            if (ImGui::BeginItemTooltip()) {
                const float scale = 256.0f / static_cast<float>(std::max({info.width, info.height, 1u}));
                ImGui::Image(ImTextureRef(preview), ImVec2(static_cast<float>(info.width) * scale,
                                                           static_cast<float>(info.height) * scale));
                ImGui::EndTooltip();
            }
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(info.path.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s, table entry %u, revision %u", info.path.c_str(),
                              info.embedded ? "embedded in its model (reloads with it)" : "file", info.tableEntry,
                              info.revision);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(ToString(info.kind));

        ImGui::TableSetColumnIndex(3);
        if (info.state == AssetState::Ready) {
            ImGui::Text("%s %ux%u, %u mips", FormatName(info.format), info.width, info.height, info.mipLevels);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", info.cacheHit ? "Cooked data from the texture cache"
                                                      : "Cooked now (written to the texture cache)");
        }

        ImGui::TableSetColumnIndex(4);
        if (info.state == AssetState::Ready)
            ImGui::Text("%s%s", Bytes(info.gpuBytes).c_str(), info.cacheHit ? "" : "*");

        ImGui::TableSetColumnIndex(5);
        StateCell(info.state, info.reloading, info.error);

        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%u", info.refCount);

        ImGui::TableSetColumnIndex(7);
        if (!info.embedded && (info.state == AssetState::Ready || info.state == AssetState::Failed)) {
            ImGui::BeginDisabled(info.reloading);
            if (ImGui::SmallButton(info.state == AssetState::Failed ? "Retry" : "Reload"))
                (void)m_Ctx.assets.Reload(info.handle);
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("%u textures, %s GPU, %u from the cache (* = cooked this session)", count,
                        Bytes(gpuTotal).c_str(), cached);
}

std::uint64_t Editor::TexturePreview(TextureHandle handle)
{
    const Texture* texture = m_Ctx.assets.Get(handle);
    if (!texture)
        return 0;
    Preview&            preview  = m_TexturePreviews[handle];
    const std::uint32_t revision = m_Ctx.assets.Revision(handle);
    if (preview.texture && preview.revision != revision) { // reloaded: the old image goes away
        m_ImGui->RemoveTexture(preview.texture);
        preview.texture = 0;
    }
    if (!preview.texture) {
        preview.texture  = m_ImGui->AddTexture(texture->image.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        preview.revision = revision;
    }
    preview.used = true;
    return preview.texture;
}

void Editor::ReleaseTexturePreviews(bool all)
{
    // Unused this frame, or showing a released / replaced image: freed after the frames in flight.
    std::erase_if(m_TexturePreviews, [&](auto& entry) {
        Preview&   p     = entry.second;
        const bool stale = all || !p.used || m_Ctx.assets.Revision(entry.first) != p.revision;
        p.used           = false;
        if (stale && p.texture)
            m_ImGui->RemoveTexture(p.texture);
        return stale;
    });
}

} // namespace Engine
