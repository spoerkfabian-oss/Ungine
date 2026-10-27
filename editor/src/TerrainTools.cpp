// Terrain window: sculpt / paint / hole / foliage brushes with undo, layers, foliage layers,
// size settings, heightmap import / export; the Terrain inspector section; terrain creation.
#include "Editor/Editor.h"
#include "Editor/FileDialog.h"
#include "Editor/History.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Terrain/TerrainData.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <set>
#include <string>

namespace Engine {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kSlotAlbedo = 0, kSlotNormal = 1, kSlotOrm = 2, kSlotFoliageTexture = 3, kSlotFoliageMesh = 4;

// The settings part of a terrain (no height / splat / hole arrays): what the window's fields edit.
TerrainData SettingsOf(const TerrainData& d)
{
    TerrainData s;
    s.resolution      = d.resolution;
    s.size            = d.size;
    s.heightScale     = d.heightScale;
    s.splatResolution = d.splatResolution;
    s.layers          = d.layers;
    s.foliage         = d.foliage;
    s.triplanarSlope  = d.triplanarSlope;
    return s;
}

bool SameSettings(const TerrainData& a, const TerrainData& b)
{
    return a.size == b.size && a.heightScale == b.heightScale && a.triplanarSlope == b.triplanarSlope && a.layers == b.layers &&
           a.foliage == b.foliage;
}

// Geometry settings (size, height scale) need everything derived rebuilt; layer / foliage values
// are read every frame (0 = only "edited").
std::uint32_t SettingsKinds(const TerrainData& a, const TerrainData& b)
{
    return a.size != b.size || a.heightScale != b.heightScale ? TerrainChangeSettings : 0u;
}

void ApplySettings(TerrainData& target, const TerrainData& settings)
{
    target.size           = settings.size;
    target.heightScale    = settings.heightScale;
    target.triplanarSlope = settings.triplanarSlope;
    target.layers         = settings.layers;
    target.foliage        = settings.foliage;
    SanitizeTerrain(target);
}

// World position of a terrain-space point (height from the data).
glm::vec3 TerrainWorld(const TerrainData& data, const glm::vec3& origin, glm::vec2 local)
{
    return origin + glm::vec3(local.x, TerrainHeight(data, local), local.y);
}

} // namespace

bool Editor::TerrainToolActive() const
{
    if (!m_TerrainToolOn || m_PlayState != PlayState::Edit)
        return false;
    const Entity e = Selected();
    return e != NullEntity && m_Ctx.scene.GetRegistry().Has<Terrain>(e);
}

Entity Editor::CreateTerrain(const glm::vec3& position)
{
    if (m_PlayState != PlayState::Edit) {
        m_Status = "Stop playing to create terrains";
        return NullEntity;
    }
    std::error_code ec;
    const fs::path  directory = ContentRoot() / "Terrains";
    fs::create_directories(directory, ec);
    fs::path file = directory / (std::string("Terrain") + kTerrainExtension);
    for (int i = 2; fs::exists(file, ec); ++i)
        file = directory / std::format("Terrain_{}{}", i, kTerrainExtension);
    TerrainData data = MakeFlatTerrain(257, 128.0f, 64.0f, 0.0f);
    FoliageLayer grass; // ready to paint
    grass.densityMap.assign(static_cast<std::size_t>(data.splatResolution) * data.splatResolution, 0);
    data.foliage.push_back(std::move(grass));
    std::string error;
    if (!SaveTerrain(file, data, &error)) {
        m_Status = "Cannot create terrain: " + error;
        return NullEntity;
    }
    const Entity e = PlaceTerrain(file, position);
    m_Status       = "Created " + PathToUtf8(file.filename());
    return e;
}

Entity Editor::PlaceTerrain(const fs::path& file, const glm::vec3& position)
{
    std::error_code ec;
    const Entity    e = m_Ctx.scene.CreateEntity(PathToUtf8(file.stem()));
    m_Ctx.scene.EditTransform(e).position = position;
    m_Ctx.scene.GetRegistry().Emplace<Terrain>(e, Terrain{.data = PathToUtf8(fs::absolute(file, ec).lexically_normal())});
    const Entity roots[] = {e};
    PushCreated("Place terrain", roots);
    Select(e);
    m_ShowTerrain = true;
    return e;
}

bool Editor::SaveTerrains()
{
    bool                  ok = true;
    std::set<std::string> done;
    TerrainStore&         store = TerrainStore::Instance();
    m_Ctx.scene.GetRegistry().ViewOf<Terrain>().Each([&](Entity, Terrain& t) {
        if (t.data.empty() || !done.insert(t.data).second || !store.Dirty(t.data))
            return;
        std::string error;
        if (!store.Save(t.data, &error)) {
            ENGINE_ERROR("Terrain: cannot save '{}': {}", t.data, error);
            ok = false;
        }
    });
    return ok;
}

void Editor::DiscardTerrainEdits()
{
    m_TerrainStroke.reset();
    TerrainStore& store = TerrainStore::Instance();
    m_Ctx.scene.GetRegistry().ViewOf<Terrain>().Each([&](Entity, Terrain& t) {
        if (store.Dirty(t.data))
            store.Reload(t.data);
    });
    m_TerrainCommitted.reset();
    m_TerrainEditPath.clear();
}

// --- Strokes --------------------------------------------------------------------------------------

void Editor::BeginTerrainStroke(Entity terrain)
{
    m_TerrainStroke.reset();
    const Terrain* component = m_Ctx.scene.GetRegistry().TryGet<Terrain>(terrain);
    if (!component)
        return;
    std::shared_ptr<TerrainData> data = TerrainStore::Instance().Get(component->data);
    if (!data)
        return;
    TerrainStroke stroke;
    stroke.entity = terrain;
    stroke.path   = component->data;
    stroke.before = *data; // whole copy for the stroke's duration; undo keeps only the touched area
    stroke.data   = std::move(data);
    m_TerrainStroke = std::move(stroke);
}

void Editor::TerrainDab(const glm::vec3& world, float dt)
{
    if (!m_TerrainStroke)
        return;
    TerrainStroke&  stroke = *m_TerrainStroke;
    const Registry& r      = m_Ctx.scene.GetRegistry();
    if (!r.Valid(stroke.entity) || !r.Has<WorldTransform>(stroke.entity))
        return;
    TerrainData&    data   = *stroke.data;
    const glm::vec3 origin = glm::vec3(r.Get<WorldTransform>(stroke.entity).matrix[3]) - glm::vec3(0.5f * data.size, 0.0f, 0.5f * data.size);
    TerrainBrush    brush  = m_TerrainBrush;
    const ImGuiIO&  io     = ImGui::GetIO();
    if (io.KeyCtrl) { // inverse: lower / erase
        if (brush.tool == TerrainTool::Raise)
            brush.tool = TerrainTool::Lower;
        else if (brush.tool == TerrainTool::Lower)
            brush.tool = TerrainTool::Raise;
        else
            brush.erase = !brush.erase;
    }
    const TerrainChanges changes = ApplyTerrainBrush(data, brush, glm::vec2(world.x - origin.x, world.z - origin.z), dt);
    if (changes.kinds == 0)
        return;
    stroke.kinds |= changes.kinds;
    stroke.rect = glm::vec4(glm::min(glm::vec2(stroke.rect), glm::vec2(changes.rect)),
                            glm::max(glm::vec2(stroke.rect.z, stroke.rect.w), glm::vec2(changes.rect.z, changes.rect.w)));
    TerrainStore::Instance().MarkDirty(stroke.path, changes.kinds, changes.rect);
}

void Editor::EndTerrainStroke()
{
    if (!m_TerrainStroke)
        return;
    TerrainStroke stroke = std::move(*m_TerrainStroke);
    m_TerrainStroke.reset();
    if (stroke.kinds == 0)
        return;
    const std::uint32_t layer  = m_TerrainBrush.tool == TerrainTool::Foliage ? m_TerrainBrush.layer : 0u;
    auto                before = std::make_shared<TerrainRegion>(CaptureTerrainRegion(stroke.before, stroke.kinds, stroke.rect, layer));
    auto                after  = std::make_shared<TerrainRegion>(CaptureTerrainRegion(*stroke.data, stroke.kinds, stroke.rect, layer));
    const auto apply = [this, path = stroke.path](const TerrainRegion& region) {
        std::shared_ptr<TerrainData> data = TerrainStore::Instance().Get(path);
        if (!data || !RestoreTerrainRegion(*data, region)) {
            m_Status = "Terrain changed size: cannot undo the stroke";
            return;
        }
        TerrainStore::Instance().MarkDirty(path, region.kinds, region.rect);
        RebaseTerrainSettings(path); // density maps are part of the settings baseline
    };
    PushCommand({.label = std::string("Terrain: ") + ToString(m_TerrainBrush.tool),
                 .undo  = [apply, before] { apply(*before); },
                 .redo  = [apply, after] { apply(*after); }});
    RebaseTerrainSettings(stroke.path); // density maps are part of the settings baseline
}

void Editor::RebaseTerrainSettings(const std::string& path)
{
    if (path != m_TerrainEditPath)
        return; // another terrain: its baseline is taken when the window shows it
    if (const std::shared_ptr<TerrainData> data = TerrainStore::Instance().Get(path))
        m_TerrainCommitted = SettingsOf(*data);
}

bool Editor::ApplyTerrainStroke(std::span<const glm::vec3> worldPoints, float secondsPerPoint)
{
    if (!TerrainToolActive() || worldPoints.empty())
        return false;
    BeginTerrainStroke(Selected());
    if (!m_TerrainStroke)
        return false;
    for (const glm::vec3& p : worldPoints)
        TerrainDab(p, secondsPerPoint);
    const bool changed = m_TerrainStroke->kinds != 0;
    EndTerrainStroke();
    return changed;
}

bool Editor::UpdateTerrainBrush(float x, float y, float width, float height, bool clicked)
{
    const Entity    e         = Selected();
    const Terrain*  component = m_Ctx.scene.GetRegistry().TryGet<Terrain>(e);
    std::shared_ptr<TerrainData> data = component ? TerrainStore::Instance().Get(component->data) : nullptr;
    m_TerrainCursor.reset();
    if (!data) {
        EndTerrainStroke();
        return false;
    }
    const glm::vec3  origin = glm::vec3(m_Ctx.scene.GetRegistry().Get<WorldTransform>(e).matrix[3]) -
                              glm::vec3(0.5f * data->size, 0.0f, 0.5f * data->size);
    const ImGuiIO&   io     = ImGui::GetIO();
    const CameraData cam    = m_Ctx.camera.GetData(width / std::max(height, 1.0f));
    const glm::mat4  viewProj = cam.projection * cam.view;
    const glm::vec2  ndc((io.MousePos.x - x) / width * 2.0f - 1.0f, 1.0f - (io.MousePos.y - y) / height * 2.0f);
    const glm::vec4  far = glm::inverse(viewProj) * glm::vec4(ndc, 0.5f, 1.0f);
    const glm::vec3  dir = glm::normalize(glm::vec3(far) / far.w - cam.position);
    // The surface under the cursor (holes: the plane of the brush's last height, so they can be filled).
    const glm::vec3 local = cam.position - origin;
    if (const auto hit = RaycastTerrain(*data, local, dir, 100000.0f)) {
        m_TerrainCursor = cam.position + dir * *hit;
    } else if (m_TerrainBrush.tool == TerrainTool::Holes && std::abs(dir.y) > 1e-4f) {
        const float t = (m_TerrainPlaneY - cam.position.y) / dir.y; // the height the brush last touched
        if (t > 0.0f)
            m_TerrainCursor = cam.position + dir * t;
    }
    if (m_TerrainCursor && !m_TerrainStroke)
        m_TerrainPlaneY = m_TerrainCursor->y;
    const bool hovered = io.MousePos.x >= x && io.MousePos.y >= y && io.MousePos.x < x + width && io.MousePos.y < y + height;
    if (clicked && m_TerrainCursor && hovered) {
        if (io.KeyShift && m_TerrainBrush.tool == TerrainTool::Flatten) { // pick the flatten height
            m_TerrainBrush.target = m_TerrainCursor->y - origin.y;
        } else {
            BeginTerrainStroke(e);
            TerrainDab(*m_TerrainCursor, std::max(io.DeltaTime, 1.0f / 120.0f));
        }
    } else if (m_TerrainStroke && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (m_TerrainCursor)
            TerrainDab(*m_TerrainCursor, std::min(io.DeltaTime, 0.1f));
    } else if (m_TerrainStroke) {
        EndTerrainStroke();
    }

    // Brush circle on the surface (inner ring: where the falloff starts).
    if (m_TerrainCursor && hovered) {
        ImDrawList* list = ImGui::GetWindowDrawList();
        const auto  ring = [&](float radius, ImU32 color) {
            ImVec2 points[48];
            int    count = 0;
            for (int i = 0; i < 48; ++i) {
                const float     a = static_cast<float>(i) / 48.0f * 6.2831853f;
                const glm::vec2 p = glm::vec2(m_TerrainCursor->x - origin.x, m_TerrainCursor->z - origin.z) +
                                    glm::vec2(std::cos(a), std::sin(a)) * radius;
                const glm::vec4 clip = viewProj * glm::vec4(TerrainWorld(*data, origin, glm::clamp(p, glm::vec2(0.0f), glm::vec2(data->size))) +
                                                                glm::vec3(0.0f, 0.05f, 0.0f), 1.0f);
                if (clip.w <= 1e-4f)
                    return;
                points[count++] = ImVec2(x + (clip.x / clip.w * 0.5f + 0.5f) * width, y + (0.5f - clip.y / clip.w * 0.5f) * height);
            }
            list->AddPolyline(points, count, color, 2.0f, ImDrawFlags_Closed);
        };
        const bool  inverse = io.KeyCtrl;
        const ImU32 color   = inverse ? IM_COL32(255, 110, 90, 230) : IM_COL32(120, 220, 255, 230);
        ring(m_TerrainBrush.radius, color);
        if (m_TerrainBrush.falloff > 0.02f && m_TerrainBrush.falloff < 0.98f)
            ring(m_TerrainBrush.radius * (1.0f - m_TerrainBrush.falloff), (color & 0x00FFFFFFu) | 0x70000000u);
    }
    return clicked && hovered && m_TerrainCursor.has_value();
}

void Editor::HandleTerrainDialog(int purpose, const fs::path& file)
{
    const Entity   e         = Selected();
    Terrain*       component = e != NullEntity ? m_Ctx.scene.GetRegistry().TryGet<Terrain>(e) : nullptr;
    TerrainStore&  store     = TerrainStore::Instance();
    std::error_code ec;
    if (static_cast<DialogPurpose>(purpose) == DialogPurpose::AssignTerrain) {
        if (!component)
            return;
        const std::vector<StateEdit> before{{UuidOf(e), SnapshotEntityState(m_Ctx.scene, e)}};
        component->data = PathToUtf8(fs::absolute(file, ec).lexically_normal());
        PushStateChange("Assign terrain", before);
        return;
    }
    std::shared_ptr<TerrainData> data = component ? store.Get(component->data) : nullptr;
    if (!data)
        return;
    std::string error;
    switch (static_cast<DialogPurpose>(purpose)) {
    case DialogPurpose::ImportHeightmap: {
        auto before = std::make_shared<std::vector<std::uint16_t>>(data->heights);
        if (!ImportHeightmap(*data, file, &error)) {
            m_Status = "Import failed: " + error;
            return;
        }
        auto       after = std::make_shared<std::vector<std::uint16_t>>(data->heights);
        const auto apply = [path = component->data](const std::vector<std::uint16_t>& heights) {
            std::shared_ptr<TerrainData> target = TerrainStore::Instance().Get(path);
            if (target && target->heights.size() == heights.size()) {
                target->heights = heights;
                TerrainStore::Instance().MarkDirty(path, TerrainChangeHeights);
            }
        };
        store.MarkDirty(component->data, TerrainChangeHeights);
        PushCommand({.label = "Import heightmap", .undo = [apply, before] { apply(*before); }, .redo = [apply, after] { apply(*after); }});
        m_Status = "Imported " + PathToUtf8(file.filename());
        break;
    }
    case DialogPurpose::ExportHeightmap: {
        fs::path target = file;
        if (target.extension() != ".png" && target.extension() != ".raw" && target.extension() != ".r16")
            target += ".png";
        m_Status = ExportHeightmap(*data, target, &error) ? "Exported " + PathToUtf8(target.filename()) : "Export failed: " + error;
        break;
    }
    case DialogPurpose::TerrainAsset: { // a layer / foliage path (the settings commit records the undo step)
        const std::uint32_t slot  = m_TerrainAssetSlot >> 24;
        const std::size_t   index = m_TerrainAssetSlot & 0xFFFFFFu;
        const std::string   path  = PathToUtf8(fs::absolute(file, ec).lexically_normal());
        if (slot <= kSlotOrm && index < data->layers.size())
            (slot == kSlotAlbedo ? data->layers[index].albedo : slot == kSlotNormal ? data->layers[index].normal : data->layers[index].orm) = path;
        else if (slot == kSlotFoliageTexture && index < data->foliage.size())
            data->foliage[index].texture = path;
        else if (slot == kSlotFoliageMesh && index < data->foliage.size())
            data->foliage[index].mesh = path;
        store.MarkDirty(component->data, 0);
        break;
    }
    default:
        break;
    }
}

// --- Settings undo --------------------------------------------------------------------------------

void Editor::CommitTerrainSettings()
{
    const Entity   e         = Selected();
    const Terrain* component = e != NullEntity ? m_Ctx.scene.GetRegistry().TryGet<Terrain>(e) : nullptr;
    std::shared_ptr<TerrainData> data = component ? TerrainStore::Instance().Get(component->data) : nullptr;
    if (!data) {
        m_TerrainCommitted.reset();
        m_TerrainEditPath.clear();
        return;
    }
    if (component->data != m_TerrainEditPath || !m_TerrainCommitted) {
        m_TerrainEditPath  = component->data; // another terrain: the new baseline
        m_TerrainCommitted = SettingsOf(*data);
        return;
    }
    if (ImGui::IsAnyItemActive() || m_TerrainStroke || SameSettings(*m_TerrainCommitted, *data))
        return;
    auto       before = std::make_shared<TerrainData>(std::move(*m_TerrainCommitted));
    auto       after  = std::make_shared<TerrainData>(SettingsOf(*data));
    const auto apply  = [this, path = component->data](const TerrainData& settings, const TerrainData& other) {
        std::shared_ptr<TerrainData> target = TerrainStore::Instance().Get(path);
        if (!target)
            return;
        ApplySettings(*target, settings);
        TerrainStore::Instance().MarkDirty(path, SettingsKinds(settings, other) | TerrainChangeFoliage);
        RebaseTerrainSettings(path);
    };
    PushCommand({.label = "Terrain settings",
                 .undo  = [apply, before, after] { apply(*before, *after); },
                 .redo  = [apply, before, after] { apply(*after, *before); }});
    m_TerrainCommitted = *after;
}

// --- Window ---------------------------------------------------------------------------------------

void Editor::DrawTerrainTools()
{
    if (!ImGui::Begin("Terrain", &m_ShowTerrain)) {
        ImGui::End();
        return;
    }
    CommitTerrainSettings();
    const Entity e         = Selected();
    Terrain*     component = e != NullEntity ? m_Ctx.scene.GetRegistry().TryGet<Terrain>(e) : nullptr;
    if (!component) {
        ImGui::TextDisabled("Select a terrain to edit it.");
        if (ImGui::Button("Create terrain"))
            (void)CreateTerrain(glm::vec3(PlacementPoint(0.0f).x, 0.0f, PlacementPoint(0.0f).z));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("New 128 m terrain under the camera (Content/Terrains)");
        ImGui::End();
        return;
    }
    TerrainStore&                store = TerrainStore::Instance();
    std::shared_ptr<TerrainData> data  = store.Get(component->data);
    if (!data) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Terrain data not found: %s",
                           component->data.empty() ? "(none)" : component->data.c_str());
        ImGui::End();
        return;
    }
    TerrainData& t = *data;

    // File.
    ImGui::TextUnformatted(PathToUtf8(PathFromUtf8(component->data).filename()).c_str());
    ImGui::SameLine();
    const bool dirty = store.Dirty(component->data);
    if (dirty)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(unsaved)");
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Save")) {
        std::string error;
        m_Status = store.Save(component->data, &error) ? "Saved terrain" : "Terrain save failed: " + error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        store.Reload(component->data);
        m_TerrainCommitted.reset();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Import heightmap...")) {
        m_DialogPurpose = DialogPurpose::ImportHeightmap;
        m_FileDialog->Open("Import heightmap (16-bit PNG / RAW)", FileDialog::Mode::Open, ContentRoot(), {".png", ".raw", ".r16"});
    }
    ImGui::SameLine();
    if (ImGui::Button("Export...")) {
        m_DialogPurpose = DialogPurpose::ExportHeightmap;
        m_FileDialog->Open("Export heightmap", FileDialog::Mode::Save, ContentRoot(), {".png", ".raw", ".r16"});
    }

    // Brush.
    ImGui::SeparatorText("Brush");
    const bool editing = m_PlayState == PlayState::Edit;
    ImGui::BeginDisabled(!editing);
    if (ImGui::RadioButton("Select", !m_TerrainToolOn))
        m_TerrainToolOn = false;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(TerrainTool::Count); ++i) {
        const auto tool = static_cast<TerrainTool>(i);
        ImGui::SameLine();
        if (ImGui::RadioButton(ToString(tool), m_TerrainToolOn && m_TerrainBrush.tool == tool)) {
            m_TerrainToolOn     = true;
            m_TerrainBrush.tool = tool;
            if (tool != TerrainTool::Paint && tool != TerrainTool::Foliage)
                m_TerrainBrush.layer = 0;
        }
    }
    ImGui::EndDisabled();
    if (!editing)
        ImGui::TextDisabled("Brushes work in edit mode (stop playing).");
    ImGui::SliderFloat("Radius", &m_TerrainBrush.radius, 0.25f, 64.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Strength", &m_TerrainBrush.strength, 0.01f, 1.0f, "%.2f");
    ImGui::SliderFloat("Falloff", &m_TerrainBrush.falloff, 0.0f, 1.0f, "%.2f");
    if (m_TerrainBrush.tool == TerrainTool::Flatten) {
        ImGui::DragFloat("Target height", &m_TerrainBrush.target, 0.05f, 0.0f, t.heightScale, "%.2f m");
        ImGui::TextDisabled("Shift+click in the viewport picks the height.");
    }
    if (m_TerrainBrush.tool == TerrainTool::Holes || m_TerrainBrush.tool == TerrainTool::Foliage)
        ImGui::Checkbox("Erase", &m_TerrainBrush.erase);
    ImGui::TextDisabled("Left drag in the viewport; Ctrl inverts (lower / erase). Undo per stroke.");

    // Layers (paint target = the selected one).
    ImGui::SeparatorText("Layers");
    int removeLayer = -1;
    const auto assetField = [&](const char* label, std::string& path, std::uint32_t slot, std::size_t index, const char* const* extensions,
                                std::size_t extensionCount) {
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputTextWithHint("##path", label, &path);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UNGINE_CONTENT"))
                path = PathToUtf8(fs::absolute(PathFromUtf8(static_cast<const char*>(payload->Data))).lexically_normal());
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button("...")) {
            m_TerrainAssetSlot = (slot << 24) | static_cast<std::uint32_t>(index);
            m_DialogPurpose    = DialogPurpose::TerrainAsset;
            m_FileDialog->Open(std::string("Choose ") + label, FileDialog::Mode::Open,
                               path.empty() ? ContentRoot() : PathFromUtf8(path).parent_path(),
                               std::vector<std::string>(extensions, extensions + extensionCount));
        }
        ImGui::SameLine();
        if (ImGui::Button("x"))
            path.clear();
        ImGui::PopID();
    };
    static constexpr const char* kImageExtensions[] = {".png", ".jpg", ".jpeg", ".ktx2"};
    static constexpr const char* kModelExtensions[] = {".glb", ".gltf"};
    for (std::size_t i = 0; i < t.layers.size(); ++i) {
        TerrainLayer& layer = t.layers[i];
        ImGui::PushID(static_cast<int>(i));
        const bool selected = m_TerrainBrush.tool == TerrainTool::Paint && m_TerrainBrush.layer == i;
        if (ImGui::Selectable(std::format("{}  {}", i, layer.name).c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) {
            m_TerrainBrush.tool  = TerrainTool::Paint;
            m_TerrainBrush.layer = static_cast<std::uint32_t>(i);
            m_TerrainToolOn      = editing;
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        ImGui::BeginDisabled(t.layers.size() <= 1);
        if (ImGui::SmallButton("-"))
            removeLayer = static_cast<int>(i);
        ImGui::EndDisabled();
        if (selected || ImGui::TreeNode("Settings")) {
            ImGui::InputText("Name", &layer.name);
            assetField("Albedo", layer.albedo, kSlotAlbedo, i, kImageExtensions, std::size(kImageExtensions));
            assetField("Normal", layer.normal, kSlotNormal, i, kImageExtensions, std::size(kImageExtensions));
            assetField("ORM", layer.orm, kSlotOrm, i, kImageExtensions, std::size(kImageExtensions));
            ImGui::ColorEdit3("Tint", &layer.tint.x);
            ImGui::DragFloat("Tiling", &layer.tiling, 0.05f, 0.05f, 1000.0f, "%.2f m");
            ImGui::SliderFloat("Roughness", &layer.roughness, 0.0f, 1.0f);
            ImGui::SliderFloat("Metallic", &layer.metallic, 0.0f, 1.0f);
            ImGui::SliderFloat("Normal strength", &layer.normalStrength, 0.0f, 2.0f);
            if (!selected)
                ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(t.layers.size() >= kMaxTerrainLayers);
    if (ImGui::Button("+ Layer"))
        t.layers.push_back(TerrainLayer{.name = std::format("Layer {}", t.layers.size())});
    ImGui::EndDisabled();
    if (removeLayer >= 0) { // its weights go to layer 0 (or 1 when removing 0); higher layers move down
        auto       before = std::make_shared<TerrainData>(t);
        const auto index  = static_cast<std::uint32_t>(removeLayer);
        const std::uint32_t heir = index == 0 ? 1u : 0u;
        for (std::size_t texel = 0; texel * kMaxTerrainLayers < t.splat.size(); ++texel) {
            std::uint8_t* w = &t.splat[texel * kMaxTerrainLayers];
            w[heir] = static_cast<std::uint8_t>(std::min(255, w[heir] + w[index]));
            for (std::uint32_t l = index; l + 1 < kMaxTerrainLayers; ++l)
                w[l] = w[l + 1];
            w[kMaxTerrainLayers - 1] = 0;
        }
        t.layers.erase(t.layers.begin() + removeLayer);
        m_TerrainBrush.layer = std::min<std::uint32_t>(m_TerrainBrush.layer, static_cast<std::uint32_t>(t.layers.size() - 1));
        auto       after = std::make_shared<TerrainData>(t);
        const auto apply = [this, path = component->data](const TerrainData& state) {
            if (std::shared_ptr<TerrainData> target = TerrainStore::Instance().Get(path)) {
                *target = state;
                TerrainStore::Instance().MarkDirty(path, TerrainChangeSettings);
            }
            RebaseTerrainSettings(path);
        };
        store.MarkDirty(component->data, TerrainChangeSettings);
        PushCommand({.label = "Remove terrain layer", .undo = [apply, before] { apply(*before); }, .redo = [apply, after] { apply(*after); }});
        m_TerrainCommitted = SettingsOf(t);
    }

    // Foliage layers (paint target = the selected one).
    ImGui::SeparatorText("Foliage");
    int removeFoliage = -1;
    for (std::size_t i = 0; i < t.foliage.size(); ++i) {
        FoliageLayer& f = t.foliage[i];
        ImGui::PushID(1000 + static_cast<int>(i));
        const bool selected = m_TerrainBrush.tool == TerrainTool::Foliage && m_TerrainBrush.layer == i;
        if (ImGui::Selectable(std::format("{}  {} ({})", i, f.name, f.kind == FoliageKind::Mesh ? "mesh" : "grass").c_str(), selected,
                              ImGuiSelectableFlags_AllowOverlap)) {
            m_TerrainBrush.tool  = TerrainTool::Foliage;
            m_TerrainBrush.layer = static_cast<std::uint32_t>(i);
            m_TerrainToolOn      = editing;
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        if (ImGui::SmallButton("-"))
            removeFoliage = static_cast<int>(i);
        if (selected || ImGui::TreeNode("Settings")) {
            ImGui::InputText("Name", &f.name);
            int kind = static_cast<int>(f.kind);
            if (ImGui::Combo("Kind", &kind, "Grass\0Mesh\0"))
                f.kind = static_cast<FoliageKind>(kind);
            if (f.kind == FoliageKind::Grass) {
                assetField("Card texture", f.texture, kSlotFoliageTexture, i, kImageExtensions, std::size(kImageExtensions));
                ImGui::TextDisabled("Empty: procedural blades");
                ImGui::DragFloat("Width", &f.width, 0.005f, 0.01f, 10.0f, "%.3f m");
                ImGui::DragFloat("Height", &f.height, 0.01f, 0.01f, 20.0f, "%.2f m");
                ImGui::ColorEdit3("Base color", &f.baseColor.x);
                ImGui::ColorEdit3("Tip color", &f.tipColor.x);
            } else {
                assetField("Mesh", f.mesh, kSlotFoliageMesh, i, kModelExtensions, std::size(kModelExtensions));
                int meshIndex = static_cast<int>(f.meshIndex);
                if (ImGui::InputInt("Mesh index", &meshIndex))
                    f.meshIndex = static_cast<std::uint32_t>(std::max(meshIndex, 0));
                ImGui::Checkbox("Align to ground", &f.alignToNormal);
            }
            ImGui::DragFloat("Density", &f.density, 0.05f, 0.0f, 64.0f, "%.2f /m2");
            ImGui::DragFloatRange2("Scale", &f.minScale, &f.maxScale, 0.01f, 0.05f, 10.0f, "%.2f");
            ImGui::DragFloat("Wind", &f.wind, 0.01f, 0.0f, 5.0f, "%.2f m");
            ImGui::DragFloat("View distance", &f.viewDistance, 0.5f, 1.0f, 2000.0f, "%.1f m");
            ImGui::SliderFloat("Max slope", &f.maxSlope, 0.0f, 90.0f, "%.0f deg");
            ImGui::Checkbox("Cast shadows", &f.castShadows);
            int seed = static_cast<int>(f.seed);
            if (ImGui::InputInt("Seed", &seed))
                f.seed = static_cast<std::uint32_t>(seed);
            if (ImGui::Button("Fill everywhere"))
                std::ranges::fill(f.densityMap, std::uint8_t{255});
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
                std::ranges::fill(f.densityMap, std::uint8_t{0});
            if (!selected)
                ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(t.foliage.size() >= kMaxFoliageLayers);
    if (ImGui::Button("+ Foliage layer")) {
        FoliageLayer layer;
        layer.name = std::format("Foliage {}", t.foliage.size());
        layer.densityMap.assign(static_cast<std::size_t>(t.splatResolution) * t.splatResolution, 0);
        t.foliage.push_back(std::move(layer));
    }
    ImGui::EndDisabled();
    if (removeFoliage >= 0) {
        t.foliage.erase(t.foliage.begin() + removeFoliage);
        if (m_TerrainBrush.tool == TerrainTool::Foliage)
            m_TerrainBrush.layer = 0;
    }

    // Size.
    ImGui::SeparatorText("Terrain");
    ImGui::Text("%u x %u samples, splat %u x %u", t.resolution, t.resolution, t.splatResolution, t.splatResolution);
    ImGui::DragFloat("Size", &t.size, 0.5f, 1.0f, 100000.0f, "%.1f m");
    ImGui::DragFloat("Height scale", &t.heightScale, 0.1f, 0.1f, 10000.0f, "%.1f m");
    ImGui::SliderFloat("Triplanar from", &t.triplanarSlope, 0.0f, 90.0f, "%.0f deg");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Slope where side projections start (90: off)");

    // Live: geometry settings rebuild what depends on them; layer values are read every frame.
    if (m_TerrainCommitted && !SameSettings(*m_TerrainCommitted, t)) {
        const std::uint32_t kinds = SettingsKinds(*m_TerrainCommitted, t);
        store.MarkDirty(component->data, kinds | (m_TerrainCommitted->foliage != t.foliage ? TerrainChangeFoliage : 0u));
    }
    ImGui::End();
}

void Editor::DrawTerrainInspector(Entity e)
{
    Registry& registry = m_Ctx.scene.GetRegistry();
    Terrain*  terrain  = registry.TryGet<Terrain>(e);
    if (!terrain || !ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##terraindata", "path/to/terrain.uterrain", &terrain->data);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UNGINE_CONTENT")) {
            const fs::path file = PathFromUtf8(static_cast<const char*>(payload->Data));
            if (file.extension() == kTerrainExtension)
                terrain->data = PathToUtf8(fs::absolute(file).lexically_normal());
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::Button("Browse...##terrain")) {
        m_DialogPurpose = DialogPurpose::AssignTerrain;
        m_FileDialog->Open("Choose terrain", FileDialog::Mode::Open,
                           terrain->data.empty() ? ContentRoot() : PathFromUtf8(terrain->data).parent_path(), {kTerrainExtension});
    }
    ImGui::SameLine();
    if (ImGui::Button("Edit (Terrain window)")) {
        m_ShowTerrain = true;
        ImGui::SetWindowFocus("Terrain");
    }
    ImGui::Checkbox("Cast shadows", &terrain->castShadows);
    ImGui::SameLine();
    ImGui::Checkbox("Collision", &terrain->collision);
    ImGui::SameLine();
    ImGui::Checkbox("Navigation", &terrain->navigation);
    ImGui::DragFloat("LOD distance", &terrain->lodDistance, 0.5f, 1.0f, 10000.0f, "%.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Range of the finest level; every coarser one doubles it");
    ImGui::SliderFloat("Foliage density", &terrain->foliageDensity, 0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Foliage distance", &terrain->foliageDistance, 0.0f, 4.0f, "%.2fx");
    if (const auto data = TerrainStore::Instance().Get(terrain->data))
        ImGui::TextDisabled("%.0f m, %u samples, %zu layer(s), %zu foliage layer(s)", data->size, data->resolution, data->layers.size(),
                            data->foliage.size());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "No terrain data");
    if (ImGui::Button("Remove terrain"))
        registry.Remove<Terrain>(e);
}

} // namespace Engine
