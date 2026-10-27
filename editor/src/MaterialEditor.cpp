#include "Editor/MaterialEditor.h"
#include "Editor/FileDialog.h"
#include "Editor/ImGuiLayer.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Core/Platform.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace Engine {

namespace {

constexpr VkFormat    kPreviewFormat  = VK_FORMAT_R8G8B8A8_SRGB; // sampled by ImGui, like the viewport
constexpr const char* kContentPayload = "UNGINE_CONTENT";
constexpr std::size_t kMaxUndo        = 100;
constexpr float       kNodeWidth      = 180.0f;
constexpr float       kRowHeight      = 20.0f;
constexpr float       kHeaderHeight   = 24.0f;

ImU32 CategoryColor(const std::string& category)
{
    if (category == "Parameters")
        return IM_COL32(40, 110, 60, 255);
    if (category == "Textures")
        return IM_COL32(120, 60, 40, 255);
    if (category == "Inputs")
        return IM_COL32(90, 60, 120, 255);
    if (category == "Constants")
        return IM_COL32(60, 80, 110, 255);
    if (category == "Output")
        return IM_COL32(130, 100, 30, 255);
    return IM_COL32(55, 55, 65, 255);
}

ImU32 PinColor(std::uint32_t width)
{
    switch (width) {
    case 1: return IM_COL32(170, 170, 170, 255);
    case 2: return IM_COL32(120, 200, 120, 255);
    case 3: return IM_COL32(230, 200, 80, 255);
    case 4: return IM_COL32(230, 120, 200, 255);
    default: return IM_COL32(120, 170, 230, 255); // generic
    }
}

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const char* kKinds[] = {"Color", "Linear", "Normal"};

bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    return std::filesystem::weakly_canonical(a, ec) == std::filesystem::weakly_canonical(b, ec);
}

// The node's title: its type and what it refers to.
std::string NodeTitle(const MaterialNode& node, const MaterialNodeDesc& desc)
{
    if (desc.param == MaterialNodeParam::Parameter)
        return desc.title + ": " + (node.param.empty() ? "?" : node.param);
    if (desc.param == MaterialNodeParam::Texture)
        return desc.title + ": " + (node.param.empty() ? "(none)" : PathToUtf8(PathFromUtf8(node.param).filename()));
    if (desc.param == MaterialNodeParam::Mask)
        return desc.title + " (" + node.param + ")";
    char buffer[96];
    switch (desc.value) {
    case MaterialNodeValue::Float:
        std::snprintf(buffer, sizeof(buffer), "%s %.3g", desc.title.c_str(), node.value.x);
        return buffer;
    case MaterialNodeValue::Float2:
        std::snprintf(buffer, sizeof(buffer), "%s (%.3g, %.3g)", desc.title.c_str(), node.value.x, node.value.y);
        return buffer;
    default: return desc.title;
    }
}

} // namespace

MaterialEditor::MaterialEditor(const Context& context) : m_Ctx(context), m_Dialog(std::make_unique<FileDialog>()) {}

MaterialEditor::~MaterialEditor()
{
    Close();
    ReleaseTarget();
}

void MaterialEditor::ReleaseTarget()
{
    if (m_Image) {
        m_Ctx.imgui.RemoveTexture(m_Texture);
        m_Ctx.renderer.DeferRelease(std::move(m_Image));
        m_Texture = 0;
    }
}

void MaterialEditor::Open(const std::filesystem::path& file)
{
    if (m_Open)
        Close();
    m_File       = std::filesystem::absolute(file).lexically_normal();
    m_IsInstance = IsMaterialInstanceFile(m_File);
    std::error_code ec;
    m_Exists = std::filesystem::exists(m_File, ec);
    std::string error;
    m_Graph    = {};
    m_Instance = {};
    if (m_IsInstance) {
        if (auto loaded = LoadMaterialInstance(m_File, &error))
            m_Instance = std::move(*loaded);
    } else if (auto loaded = LoadMaterialGraph(m_File, &error)) {
        m_Graph = std::move(*loaded);
    } else {
        m_Graph = MakeDefaultMaterialGraph();
    }
    m_Status    = m_Exists ? error : std::string(m_IsInstance ? "New instance (not saved yet)" : "New material (not saved yet)");
    m_Saved     = m_Exists ? Current() : State{};
    m_Committed = Current();
    m_Undo.clear();
    m_Redo.clear();
    m_Applied.reset();
    m_Selected.clear();
    m_LinkDrag.reset();
    m_PendingLink.reset();
    m_ParentCache.reset();
    m_ParentCacheKey.clear();
    m_FrameAll = true;
    m_Open     = true;

    // Preview scene: the material on a sphere (box / plane) above a ground plane.
    m_Scene = std::make_unique<Scene>();
    if (!m_Renderer) {
        m_Renderer = std::make_unique<SceneRenderer>(m_Ctx.renderer, m_Ctx.renderer.GetContext(), m_Ctx.assets);
        m_Renderer->shadows.resolution   = 1024;
        m_Renderer->shadows.cascadeCount = 2;
        m_Renderer->shadows.maxDistance  = 10.0f;
        m_Renderer->localShadows.enabled = false;
        m_Renderer->culling.occlusion    = false;
    }
    if (!m_Ground)
        m_Ground = m_Ctx.assets.CreatePrimitive({.shape = PrimitiveShape::Plane, .size = 8.0f, .baseColor = {0.3f, 0.31f, 0.33f, 1.0f}});
    const Entity ground = m_Scene->CreateEntity("Ground");
    m_Scene->GetRegistry().Emplace<MeshRenderer>(ground, MeshRenderer{.model = m_Ground});
    SetPreviewShape(m_ShapeKind);
    Apply();
}

void MaterialEditor::SetPreviewShape(int shape)
{
    m_ShapeKind = shape;
    if (!m_Scene)
        return;
    const PrimitiveShape kinds[] = {PrimitiveShape::Sphere, PrimitiveShape::Box, PrimitiveShape::Plane};
    const ModelHandle    model   = m_Ctx.assets.CreatePrimitive({.shape = kinds[std::clamp(shape, 0, 2)], .size = 1.0f});
    if (m_Shape)
        m_Ctx.assets.Release(m_Shape);
    m_Shape = model;
    Registry& r = m_Scene->GetRegistry();
    Entity    e = NullEntity;
    r.ViewOf<Name>().Each([&](Entity entity, Name& name) {
        if (name.value == "Preview")
            e = entity;
    });
    if (e == NullEntity) {
        e = m_Scene->CreateEntity("Preview");
        r.Emplace<MaterialOverrides>(e, MaterialOverrides{.materials = {PathToUtf8(m_File)}});
    }
    if (r.Has<MeshRenderer>(e))
        r.Get<MeshRenderer>(e).model = model;
    else
        r.Emplace<MeshRenderer>(e, MeshRenderer{.model = model});
    m_Scene->EditTransform(e).position = {0.0f, shape == 2 ? 0.02f : 0.6f, 0.0f};
    m_Scene->MarkChanged(e);
}

void MaterialEditor::Close()
{
    if (!m_Open)
        return;
    // The scene shows the file again.
    if (m_IsInstance)
        m_Ctx.assets.ProvideMaterialInstance(m_File, std::nullopt);
    else
        m_Ctx.assets.ProvideMaterial(m_File, std::nullopt);
    m_Open = false;
    m_Renderer.reset(); // GPU resources: deferred until the frames in flight are done
    ReleaseTarget();
    m_Scene.reset();
    for (ModelHandle* h : {&m_Shape, &m_Ground})
        if (*h) {
            m_Ctx.assets.Release(*h);
            *h = {};
        }
}

bool MaterialEditor::Dirty() const { return m_Open && (!m_Exists || Current() != m_Saved); }

bool MaterialEditor::Save(std::string* error)
{
    std::string why;
    const bool  ok = m_IsInstance ? SaveMaterialInstance(m_File, m_Instance, &why) : SaveMaterialGraph(m_File, m_Graph, &why);
    if (!ok) {
        m_Status = why;
        if (error)
            *error = why;
        return false;
    }
    m_Saved  = Current();
    m_Exists = true;
    m_Status = "Saved";
    return true;
}

void MaterialEditor::Revert()
{
    if (m_Exists)
        Restore(m_Saved);
    else
        Restore({MakeDefaultMaterialGraph(), {}});
    Commit();
}

void MaterialEditor::Restore(const State& state)
{
    m_Graph    = state.graph;
    m_Instance = state.instance;
    std::erase_if(m_Selected, [&](std::uint32_t id) { return !m_Graph.FindNode(id); });
}

void MaterialEditor::Commit()
{
    const State now = Current();
    if (now == m_Committed)
        return;
    m_Undo.push_back(m_Committed);
    if (m_Undo.size() > kMaxUndo)
        m_Undo.erase(m_Undo.begin());
    m_Redo.clear();
    m_Committed = now;
    Apply();
}

bool MaterialEditor::Undo()
{
    if (m_Undo.empty())
        return false;
    m_Redo.push_back(m_Committed);
    Restore(m_Undo.back());
    m_Undo.pop_back();
    m_Committed = Current();
    Apply();
    return true;
}

bool MaterialEditor::Redo()
{
    if (m_Redo.empty())
        return false;
    m_Undo.push_back(m_Committed);
    Restore(m_Redo.back());
    m_Redo.pop_back();
    m_Committed = Current();
    Apply();
    return true;
}

void MaterialEditor::Apply()
{
    if (!m_Open)
        return;
    const State now = Current();
    if (m_Applied && *m_Applied == now)
        return;
    m_Applied = now;
    if (m_IsInstance) {
        m_Ctx.assets.ProvideMaterialInstance(m_File, m_Instance);
        m_Problems.clear();
        if (m_Instance.parent.empty())
            m_Problems.push_back({0, "no parent material", true});
        else if (!ParentGraph())
            m_Problems.push_back({0, "parent material cannot be loaded", true});
    } else {
        m_Ctx.assets.ProvideMaterial(m_File, m_Graph);
        m_Problems = GenerateMaterialCode(m_Graph).problems;
    }
}

std::optional<MaterialGraph> MaterialEditor::ParentGraph() const
{
    if (m_Instance.parent.empty())
        return std::nullopt;
    if (m_ParentCacheKey == m_Instance.parent && m_ParentCache)
        return m_ParentCache;
    std::filesystem::path file = PathFromUtf8(m_Instance.parent);
    for (int depth = 0; depth < 16; ++depth) {
        if (IsMaterialFile(file)) {
            m_ParentCache    = LoadMaterialGraph(file);
            m_ParentCacheKey = m_Instance.parent;
            return m_ParentCache;
        }
        const auto next = LoadMaterialInstance(file);
        if (!next || next->parent.empty())
            break;
        file = PathFromUtf8(next->parent);
    }
    m_ParentCache.reset();
    m_ParentCacheKey = m_Instance.parent;
    return std::nullopt;
}

bool MaterialEditor::TextureField(const char* label, std::string& path, TextureKind* kind)
{
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::SmallButton("...")) {
        m_DialogPurpose = 1;
        m_DialogTarget  = &path;
        m_Dialog->Open(std::string("Choose ") + label, FileDialog::Mode::Open,
                       path.empty() ? m_File.parent_path() : PathFromUtf8(path).parent_path(),
                       {".png", ".jpg", ".jpeg", ".ktx2"});
    }
    ImGui::SameLine();
    if (!path.empty() && ImGui::SmallButton("x")) {
        path.clear();
        changed = true;
    }
    if (!path.empty())
        ImGui::SameLine();
    ImGui::Text("%s: %s", label, path.empty() ? "(none)" : PathToUtf8(PathFromUtf8(path).filename()).c_str());
    if (ImGui::IsItemHovered() && !path.empty())
        ImGui::SetTooltip("%s", path.c_str());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPayload)) {
            const std::filesystem::path dropped = PathFromUtf8(static_cast<const char*>(payload->Data));
            const std::string           ext     = Lower(PathToUtf8(dropped.extension()));
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".ktx2") {
                path    = PathToUtf8(dropped.lexically_normal());
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (kind) {
        int k = static_cast<int>(*kind);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("Kind", &k, kKinds, 3)) {
            *kind   = static_cast<TextureKind>(k);
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

void MaterialEditor::Draw(float dt)
{
    (void)dt;
    m_PreviewVisible = false;
    if (!m_Open)
        return;
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1180.0f, 680.0f), ImGuiCond_FirstUseEver);
    const std::string title = std::string(m_IsInstance ? "Material Instance - " : "Material - ") +
                              PathToUtf8(m_File.filename()) + (Dirty() ? " *" : "") + "###materialeditor";
    bool open = true;
    if (!ImGui::Begin(title.c_str(), &open)) {
        ImGui::End();
        if (!open)
            Close();
        return;
    }
    DrawToolbar();
    const ImVec2 avail        = ImGui::GetContentRegionAvail();
    const float  sideWidth    = std::clamp(avail.x * 0.26f, 220.0f, 360.0f);
    const float  previewWidth = std::clamp(avail.x * 0.28f, 200.0f, 420.0f);
    ImGui::BeginChild("side", ImVec2(sideWidth, 0.0f), ImGuiChildFlags_Borders);
    if (m_IsInstance)
        DrawInstance();
    else
        DrawSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    if (!m_IsInstance) {
        ImGui::BeginChild("canvas", ImVec2(avail.x - sideWidth - previewWidth - 16.0f, 0.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
        DrawCanvas();
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("preview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    DrawPreview();
    ImGui::EndChild();

    // Edits are committed (undo + live update) once no widget is active anymore; while dragging
    // the preview follows live.
    if (!ImGui::IsAnyItemActive() && !m_DraggingNodes)
        Commit();
    else
        Apply();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            ImGui::GetIO().KeyShift ? Redo() : Undo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            Redo();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save();
    }
    ImGui::End();
    if (const auto chosen = m_Dialog->Draw()) {
        if (m_DialogPurpose == 1 && m_DialogTarget)
            *m_DialogTarget = PathToUtf8(chosen->lexically_normal());
        else if (m_DialogPurpose == 2)
            m_Instance.parent = PathToUtf8(chosen->lexically_normal());
        m_DialogTarget = nullptr;
        Commit();
    }
    if (!open)
        Close();
}

void MaterialEditor::DrawToolbar()
{
    if (ImGui::Button("Save"))
        Save();
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        Revert();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_Undo.empty());
    if (ImGui::Button("Undo"))
        Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_Redo.empty());
    if (ImGui::Button("Redo"))
        Redo();
    ImGui::EndDisabled();
    if (!m_IsInstance) {
        ImGui::SameLine();
        if (ImGui::Button("Frame"))
            m_FrameAll = true;
    }
    // Compile state of the material asset (shared with the scene).
    for (const MaterialInfo& info : m_Ctx.assets.Materials())
        if (SameFile(PathFromUtf8(info.path), m_File)) {
            ImGui::SameLine();
            if (info.reloading || info.state == AssetState::Loading)
                ImGui::TextDisabled("compiling...");
            else if (info.state == AssetState::Failed || !info.error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "error (see problems)");
            else
                ImGui::TextDisabled("compiled%s", info.shaderCacheHit ? " (cached)" : "");
            if (!info.error.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", info.error.c_str());
            break;
        }
    if (!m_Status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_Status.c_str());
    }
}

void MaterialEditor::DrawSidebar()
{
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
        DrawSettings();
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
        DrawParameters();
    if (ImGui::CollapsingHeader("Details", ImGuiTreeNodeFlags_DefaultOpen))
        DrawDetails();
    if (ImGui::CollapsingHeader("Problems", ImGuiTreeNodeFlags_DefaultOpen))
        DrawProblems();
}

void MaterialEditor::DrawSettings()
{
    MaterialSettings& s = m_Graph.settings;
    const char* blends[] = {"Opaque", "Masked", "Translucent", "Additive"};
    int         blend    = static_cast<int>(s.blendMode);
    if (ImGui::Combo("Blend mode", &blend, blends, 4))
        s.blendMode = static_cast<MaterialBlendMode>(blend);
    const char* models[] = {"Lit", "Unlit"};
    int         model    = static_cast<int>(s.shadingModel);
    if (ImGui::Combo("Shading", &model, models, 2))
        s.shadingModel = static_cast<MaterialShadingModel>(model);
    ImGui::Checkbox("Two-sided", &s.twoSided);
    if (s.blendMode == MaterialBlendMode::Opaque || s.blendMode == MaterialBlendMode::Masked)
        ImGui::Checkbox("Cast shadows", &s.castShadows);
    if (s.blendMode == MaterialBlendMode::Masked)
        ImGui::SliderFloat("Alpha cutoff", &s.alphaCutoff, 0.0f, 1.0f);
    ImGui::DragFloat("Max displacement (m)", &s.maxDisplacement, 0.01f, 0.0f, 100.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bound of the world position offset: culling boxes grow by it");
}

void MaterialEditor::DrawParameters()
{
    const auto add = [&](MaterialParameterType type, const char* base) {
        MaterialParameter p;
        p.type = type;
        p.name = base;
        for (int i = 2; m_Graph.FindParameter(p.name); ++i)
            p.name = std::string(base) + std::to_string(i);
        if (type == MaterialParameterType::Vector)
            p.value = glm::vec4(1.0f);
        m_Graph.parameters.push_back(p);
    };
    if (ImGui::SmallButton("+ Scalar"))
        add(MaterialParameterType::Scalar, "Scalar");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Vector"))
        add(MaterialParameterType::Vector, "Color");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Texture"))
        add(MaterialParameterType::Texture, "Texture");
    std::optional<std::size_t> remove;
    for (std::size_t i = 0; i < m_Graph.parameters.size(); ++i) {
        MaterialParameter& p = m_Graph.parameters[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        char name[64];
        std::snprintf(name, sizeof(name), "%s", p.name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
        if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] != 0)
            m_Graph.RenameParameter(p.name, name);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ToString(p.type));
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = i;
        switch (p.type) {
        case MaterialParameterType::Scalar:
            ImGui::SliderFloat("Value", &p.value.x, p.range.x, p.range.y);
            ImGui::DragFloat2("Range", &p.range.x, 0.05f);
            break;
        case MaterialParameterType::Vector: ImGui::ColorEdit4("Value", &p.value.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR); break;
        case MaterialParameterType::Texture: TextureField("Texture", p.texture, &p.textureKind); break;
        }
        if (ImGui::SmallButton("Place node")) {
            const char* type = p.type == MaterialParameterType::Scalar   ? "Param.Scalar"
                               : p.type == MaterialParameterType::Vector ? "Param.Vector"
                                                                         : "Param.Texture";
            const glm::vec2 center = glm::vec2(m_CanvasRect.z, m_CanvasRect.w) * 0.5f;
            const std::uint32_t id = m_Graph.AddNode(type, (center - m_Pan) / m_Zoom - glm::vec2(80.0f, 20.0f), p.name);
            m_Selected = {id};
        }
        ImGui::PopID();
    }
    if (remove) {
        const std::string name = m_Graph.parameters[*remove].name;
        m_Graph.parameters.erase(m_Graph.parameters.begin() + static_cast<std::ptrdiff_t>(*remove));
        (void)name; // nodes keep referring to it: reported as a problem until fixed
    }
}

void MaterialEditor::DrawDetails()
{
    if (m_Selected.size() != 1) {
        ImGui::TextDisabled(m_Selected.empty() ? "Select a node" : "%zu nodes selected", m_Selected.size());
        return;
    }
    MaterialNode* node = m_Graph.FindNode(*m_Selected.begin());
    const MaterialNodeDesc* desc = node ? FindMaterialNodeType(node->type) : nullptr;
    if (!node || !desc) {
        ImGui::TextDisabled("Unknown node");
        return;
    }
    ImGui::Text("%s", desc->title.c_str());
    ImGui::TextWrapped("%s", desc->description.c_str());
    switch (desc->param) {
    case MaterialNodeParam::Parameter:
        if (ImGui::BeginCombo("Parameter", node->param.c_str())) {
            for (const MaterialParameter& p : m_Graph.parameters) {
                const bool fits = (node->type == "Param.Scalar" && p.type == MaterialParameterType::Scalar) ||
                                  (node->type == "Param.Vector" && p.type == MaterialParameterType::Vector) ||
                                  (node->type == "Param.Texture" && p.type == MaterialParameterType::Texture);
                if (fits && ImGui::Selectable(p.name.c_str(), p.name == node->param))
                    node->param = p.name;
            }
            ImGui::EndCombo();
        }
        break;
    case MaterialNodeParam::Mask: {
        char mask[8];
        std::snprintf(mask, sizeof(mask), "%s", node->param.c_str());
        if (ImGui::InputText("Components", mask, sizeof(mask)))
            node->param = mask;
        break;
    }
    case MaterialNodeParam::Texture: TextureField("Texture", node->param, &node->textureKind); break;
    case MaterialNodeParam::None: break;
    }
    switch (desc->value) {
    case MaterialNodeValue::Float: ImGui::DragFloat("Value", &node->value.x, 0.01f); break;
    case MaterialNodeValue::Float2:
        ImGui::DragFloat2(node->type == "Input.TexCoord" ? "Tiling" : "Value", &node->value.x, 0.01f);
        break;
    case MaterialNodeValue::Float3: ImGui::DragFloat3("Value", &node->value.x, 0.01f); break;
    case MaterialNodeValue::Color: ImGui::ColorEdit4("Value", &node->value.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR); break;
    case MaterialNodeValue::None: break;
    }
    // Values of unconnected inputs.
    for (const MaterialPinDesc& pin : desc->inputs) {
        if (m_Graph.LinkInto(node->id, pin.name))
            continue;
        const bool set = std::any_of(node->inputs.begin(), node->inputs.end(), [&](const MaterialPinValue& v) { return v.pin == pin.name; });
        glm::vec4  v   = m_Graph.InputValue(*node, pin.name, pin.fallback);
        ImGui::PushID(pin.name.c_str());
        bool changed = false;
        if (!pin.implicit.empty() && !set) {
            ImGui::TextDisabled("%s: %s", pin.name.c_str(), pin.implicit.c_str());
            ImGui::SameLine();
            changed = ImGui::SmallButton("Set value");
        } else {
            const std::uint32_t width = pin.width == 0 ? 1 : pin.width;
            if (width == 1)
                changed = ImGui::DragFloat(pin.name.c_str(), &v.x, 0.01f);
            else if (width == 2)
                changed = ImGui::DragFloat2(pin.name.c_str(), &v.x, 0.01f);
            else if (width == 3)
                changed = pin.name.find("Color") != std::string::npos || pin.name == "BaseColor" || pin.name == "Emissive"
                              ? ImGui::ColorEdit3(pin.name.c_str(), &v.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                              : ImGui::DragFloat3(pin.name.c_str(), &v.x, 0.01f);
            else
                changed = ImGui::DragFloat4(pin.name.c_str(), &v.x, 0.01f);
            if (set) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) // back to the default
                    std::erase_if(node->inputs, [&](const MaterialPinValue& p) { return p.pin == pin.name; });
            }
        }
        if (changed)
            m_Graph.SetInputValue(*node, pin.name, v);
        ImGui::PopID();
    }
}

void MaterialEditor::DrawProblems()
{
    bool any = false;
    for (const MaterialProblem& p : m_Problems) {
        any = true;
        const ImVec4 color = p.error ? ImVec4(1.0f, 0.4f, 0.3f, 1.0f) : ImVec4(0.95f, 0.8f, 0.3f, 1.0f);
        std::string  text  = p.message;
        if (p.node)
            if (const MaterialNode* n = m_Graph.FindNode(p.node))
                if (const MaterialNodeDesc* d = FindMaterialNodeType(n->type))
                    text = d->title + ": " + text;
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (ImGui::Selectable((text + "##" + std::to_string(&p - m_Problems.data())).c_str()) && p.node)
            m_Selected = {p.node};
        ImGui::PopStyleColor();
    }
    // Compile errors of the asset (shader compiler).
    for (const MaterialInfo& info : m_Ctx.assets.Materials())
        if (SameFile(PathFromUtf8(info.path), m_File) && !info.error.empty()) {
            any = true;
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", info.error.c_str());
        }
    if (!any)
        ImGui::TextDisabled("No problems");
}

void MaterialEditor::DrawInstance()
{
    ImGui::TextUnformatted("Parent");
    ImGui::PushID("parent");
    if (ImGui::SmallButton("...")) {
        m_DialogPurpose = 2;
        m_Dialog->Open("Choose the parent material", FileDialog::Mode::Open,
                       m_Instance.parent.empty() ? m_File.parent_path() : PathFromUtf8(m_Instance.parent).parent_path(),
                       {".umat", ".uminst"});
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(m_Instance.parent.empty() ? "(none)" : PathToUtf8(PathFromUtf8(m_Instance.parent).filename()).c_str());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPayload)) {
            const std::filesystem::path dropped = PathFromUtf8(static_cast<const char*>(payload->Data));
            if ((IsMaterialFile(dropped) || IsMaterialInstanceFile(dropped)) && dropped.lexically_normal() != m_File)
                m_Instance.parent = PathToUtf8(dropped.lexically_normal());
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    ImGui::Separator();
    const std::optional<MaterialGraph> graph = ParentGraph();
    if (!graph) {
        ImGui::TextDisabled(m_Instance.parent.empty() ? "Choose a parent (.umat / .uminst)" : "The parent cannot be loaded");
        return;
    }
    ImGui::TextDisabled("Overrides (unchecked: the parent's value)");
    for (const MaterialParameter& p : graph->parameters) {
        ImGui::PushID(p.name.c_str());
        auto it = std::find_if(m_Instance.overrides.begin(), m_Instance.overrides.end(),
                               [&](const MaterialParameterValue& o) { return o.name == p.name && o.type == p.type; });
        bool overridden = it != m_Instance.overrides.end();
        if (ImGui::Checkbox("##on", &overridden)) {
            if (overridden)
                m_Instance.overrides.push_back({p.name, p.type, p.value, p.texture});
            else
                m_Instance.overrides.erase(it);
            ImGui::PopID();
            break; // iterators changed: next frame
        }
        ImGui::SameLine();
        if (!overridden) {
            ImGui::TextDisabled("%s (%s)", p.name.c_str(), ToString(p.type));
        } else {
            MaterialParameterValue& o = *it;
            switch (p.type) {
            case MaterialParameterType::Scalar: ImGui::SliderFloat(p.name.c_str(), &o.value.x, p.range.x, p.range.y); break;
            case MaterialParameterType::Vector:
                ImGui::ColorEdit4(p.name.c_str(), &o.value.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                break;
            case MaterialParameterType::Texture: TextureField(p.name.c_str(), o.texture, nullptr); break;
            }
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    for (const MaterialProblem& p : m_Problems)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", p.message.c_str());
}

glm::vec2 MaterialEditor::NodeScreenPosition(std::uint32_t id) const
{
    const MaterialNode* node = m_Graph.FindNode(id);
    if (!node)
        return glm::vec2(0.0f);
    return glm::vec2(m_CanvasRect.x, m_CanvasRect.y) + m_Pan + node->position * m_Zoom;
}

void MaterialEditor::DrawCanvas()
{
    ImDrawList*  dl     = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size   = ImGui::GetContentRegionAvail();
    m_CanvasRect        = glm::vec4(origin.x, origin.y, size.x, size.y);
    ImGui::InvisibleButton("canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool      hovered = ImGui::IsItemHovered();
    const ImGuiIO&  io      = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const glm::vec2 base(origin.x, origin.y);
    const auto toScreen = [&](glm::vec2 p) { return base + m_Pan + p * m_Zoom; };
    const auto toGraph  = [&](glm::vec2 s) { return (s - base - m_Pan) / m_Zoom; };

    if (m_FrameAll && !m_Graph.nodes.empty() && size.x > 10.0f) {
        glm::vec2 lo(1e9f), hi(-1e9f);
        for (const MaterialNode& n : m_Graph.nodes) {
            lo = glm::min(lo, n.position);
            hi = glm::max(hi, n.position + glm::vec2(kNodeWidth, 200.0f));
        }
        const glm::vec2 extent = glm::max(hi - lo, glm::vec2(1.0f));
        m_Zoom = std::clamp(std::min((size.x - 40.0f) / extent.x, (size.y - 40.0f) / extent.y), 0.3f, 1.2f);
        m_Pan  = glm::vec2(size.x, size.y) * 0.5f - (lo + hi) * 0.5f * m_Zoom;
        m_FrameAll = false;
    }

    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 28, 32, 255));
    const float grid = 32.0f * m_Zoom;
    for (float x = std::fmod(m_Pan.x, grid); x < size.x; x += grid)
        dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(40, 40, 46, 255));
    for (float y = std::fmod(m_Pan.y, grid); y < size.y; y += grid)
        dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(40, 40, 46, 255));

    // Pin positions (screen) for links and hit tests.
    struct PinHit {
        std::uint32_t node;
        std::string   pin;
        bool          output;
        glm::vec2     at;
        std::uint32_t width;
    };
    std::vector<PinHit> pins;
    std::map<std::uint32_t, std::pair<glm::vec2, glm::vec2>> nodeRects; // min, max (screen)
    for (const MaterialNode& node : m_Graph.nodes) {
        const MaterialNodeDesc* desc = FindMaterialNodeType(node.type);
        const std::size_t rows = desc ? std::max(desc->inputs.size(), desc->outputs.size()) : 1;
        const glm::vec2 lo = toScreen(node.position);
        const glm::vec2 hi = lo + glm::vec2(kNodeWidth, kHeaderHeight + kRowHeight * static_cast<float>(rows) + 6.0f) * m_Zoom;
        nodeRects[node.id] = {lo, hi};
        if (!desc)
            continue;
        for (std::size_t i = 0; i < desc->inputs.size(); ++i)
            pins.push_back({node.id, desc->inputs[i].name, false,
                            lo + glm::vec2(0.0f, kHeaderHeight + kRowHeight * (static_cast<float>(i) + 0.5f)) * m_Zoom,
                            desc->inputs[i].width});
        for (std::size_t i = 0; i < desc->outputs.size(); ++i)
            pins.push_back({node.id, desc->outputs[i].name, true,
                            glm::vec2(hi.x, lo.y) + glm::vec2(0.0f, kHeaderHeight + kRowHeight * (static_cast<float>(i) + 0.5f)) * m_Zoom,
                            desc->outputs[i].width});
    }
    const auto findPin = [&](std::uint32_t node, const std::string& pin, bool output) -> const PinHit* {
        for (const PinHit& p : pins)
            if (p.node == node && p.pin == pin && p.output == output)
                return &p;
        return nullptr;
    };
    const auto pinAt = [&](glm::vec2 s) -> const PinHit* {
        const float r = std::max(7.0f * m_Zoom, 5.0f);
        for (const PinHit& p : pins)
            if (glm::distance(p.at, s) <= r)
                return &p;
        return nullptr;
    };
    const auto bezier = [&](glm::vec2 a, glm::vec2 b, ImU32 color) {
        const float dx = std::max(std::abs(b.x - a.x) * 0.5f, 30.0f * m_Zoom);
        dl->AddBezierCubic(ImVec2(a.x, a.y), ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), ImVec2(b.x, b.y), color,
                           2.0f * std::max(m_Zoom, 0.6f));
    };

    // Links.
    for (const MaterialLink& link : m_Graph.links) {
        const PinHit* from = findPin(link.fromNode, link.fromPin, true);
        const PinHit* to   = findPin(link.toNode, link.toPin, false);
        if (from && to)
            bezier(from->at, to->at, PinColor(from->width));
    }

    // Nodes.
    const ImU32 problemNodes = IM_COL32(230, 80, 60, 255);
    for (const MaterialNode& node : m_Graph.nodes) {
        const MaterialNodeDesc* desc     = FindMaterialNodeType(node.type);
        const auto&             rect     = nodeRects[node.id];
        const bool              selected = m_Selected.contains(node.id);
        const bool              problem  = std::any_of(m_Problems.begin(), m_Problems.end(),
                                                       [&](const MaterialProblem& p) { return p.error && p.node == node.id; });
        dl->AddRectFilled(ImVec2(rect.first.x, rect.first.y), ImVec2(rect.second.x, rect.second.y), IM_COL32(45, 45, 52, 235), 5.0f);
        dl->AddRectFilled(ImVec2(rect.first.x, rect.first.y), ImVec2(rect.second.x, rect.first.y + kHeaderHeight * m_Zoom),
                          desc ? CategoryColor(desc->category) : problemNodes, 5.0f, ImDrawFlags_RoundCornersTop);
        dl->AddRect(ImVec2(rect.first.x, rect.first.y), ImVec2(rect.second.x, rect.second.y),
                    problem ? problemNodes : selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(20, 20, 24, 255), 5.0f,
                    selected || problem ? 2.0f : 1.0f);
        if (m_Zoom < 0.45f)
            continue; // too small for text
        const std::string title = desc ? NodeTitle(node, *desc) : "? " + node.type;
        dl->AddText(nullptr, 13.0f * m_Zoom, ImVec2(rect.first.x + 6.0f * m_Zoom, rect.first.y + 5.0f * m_Zoom),
                    IM_COL32(240, 240, 240, 255), title.c_str());
        if (!desc)
            continue;
        for (const PinHit& p : pins) {
            if (p.node != node.id)
                continue;
            const bool linked = p.output ? std::any_of(m_Graph.links.begin(), m_Graph.links.end(),
                                                       [&](const MaterialLink& l) { return l.fromNode == p.node && l.fromPin == p.pin; })
                                         : m_Graph.LinkInto(p.node, p.pin) != nullptr;
            const float r = 5.0f * std::max(m_Zoom, 0.6f);
            if (linked)
                dl->AddCircleFilled(ImVec2(p.at.x, p.at.y), r, PinColor(p.width));
            else
                dl->AddCircle(ImVec2(p.at.x, p.at.y), r, PinColor(p.width), 0, 1.5f);
            const ImVec2 textSize = ImGui::CalcTextSize(p.pin.c_str());
            const float  scale    = 12.0f / ImGui::GetFontSize() * m_Zoom;
            const ImVec2 at       = p.output ? ImVec2(p.at.x - textSize.x * scale - 9.0f * m_Zoom, p.at.y - 6.5f * m_Zoom)
                                             : ImVec2(p.at.x + 9.0f * m_Zoom, p.at.y - 6.5f * m_Zoom);
            dl->AddText(nullptr, 12.0f * m_Zoom, at, IM_COL32(200, 200, 205, 255), p.pin.c_str());
        }
    }

    // Interaction.
    const PinHit* hoveredPin = hovered ? pinAt(mouse) : nullptr;
    std::uint32_t hoveredNode = 0;
    if (hovered && !hoveredPin)
        for (auto it = m_Graph.nodes.rbegin(); it != m_Graph.nodes.rend(); ++it) {
            const auto& rect = nodeRects[it->id];
            if (mouse.x >= rect.first.x && mouse.x <= rect.second.x && mouse.y >= rect.first.y && mouse.y <= rect.second.y) {
                hoveredNode = it->id;
                break;
            }
        }
    if (hoveredPin && !m_LinkDrag)
        ImGui::SetTooltip("%s (%s)", hoveredPin->pin.c_str(),
                          hoveredPin->width == 0 ? "any width" : (std::to_string(hoveredPin->width) + " component(s)").c_str());

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredPin && !hoveredPin->output && (io.KeyAlt || io.KeyCtrl)) {
            m_Graph.Disconnect(hoveredPin->node, hoveredPin->pin);
        } else if (hoveredPin) {
            // Dragging from a linked input picks the link up (re-route it).
            if (!hoveredPin->output)
                if (const MaterialLink* l = m_Graph.LinkInto(hoveredPin->node, hoveredPin->pin)) {
                    m_LinkDrag = LinkDrag{l->fromNode, l->fromPin, true};
                    m_Graph.Disconnect(hoveredPin->node, hoveredPin->pin);
                }
            if (!m_LinkDrag)
                m_LinkDrag = LinkDrag{hoveredPin->node, hoveredPin->pin, hoveredPin->output};
        } else if (hoveredNode) {
            if (io.KeyCtrl || io.KeyShift) {
                if (!m_Selected.erase(hoveredNode))
                    m_Selected.insert(hoveredNode);
            } else if (!m_Selected.contains(hoveredNode)) {
                m_Selected = {hoveredNode};
            }
            m_DraggingNodes = true;
        } else {
            m_Selected.clear();
        }
    }
    if (m_DraggingNodes) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const glm::vec2 delta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / m_Zoom;
            for (const std::uint32_t id : m_Selected)
                if (MaterialNode* n = m_Graph.FindNode(id))
                    n->position += delta;
        } else {
            for (const std::uint32_t id : m_Selected) // snap to the grid when released
                if (MaterialNode* n = m_Graph.FindNode(id))
                    n->position = glm::round(n->position / 8.0f) * 8.0f;
            m_DraggingNodes = false;
        }
    }
    if (m_LinkDrag) {
        const PinHit* from = findPin(m_LinkDrag->node, m_LinkDrag->pin, m_LinkDrag->output);
        if (from)
            m_LinkDrag->output ? bezier(from->at, mouse, IM_COL32(255, 255, 255, 200)) : bezier(mouse, from->at, IM_COL32(255, 255, 255, 200));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (const PinHit* target = pinAt(mouse); target && target->output != m_LinkDrag->output) {
                if (m_LinkDrag->output)
                    m_Graph.Connect(m_LinkDrag->node, m_LinkDrag->pin, target->node, target->pin);
                else
                    m_Graph.Connect(target->node, target->pin, m_LinkDrag->node, m_LinkDrag->pin);
            } else if (hovered && !target) { // dropped on the canvas: palette, then connect
                m_PendingLink = m_LinkDrag;
                m_PaletteAt   = toGraph(mouse);
                m_Search[0]   = 0;
                ImGui::OpenPopup("palette");
            }
            m_LinkDrag.reset();
        }
    }
    // Pan (middle / right drag), zoom (wheel around the mouse), palette (right click).
    if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)))
        m_Pan += glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
    if (hovered && io.MouseWheel != 0.0f) {
        const glm::vec2 before = toGraph(mouse);
        m_Zoom = std::clamp(m_Zoom * std::pow(1.12f, io.MouseWheel), 0.25f, 2.5f);
        m_Pan  = mouse - base - before * m_Zoom;
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f) &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0.0f) {
        if (hoveredPin && !hoveredPin->output) {
            m_Graph.Disconnect(hoveredPin->node, hoveredPin->pin);
        } else {
            m_PendingLink.reset();
            m_PaletteAt = toGraph(mouse);
            m_Search[0] = 0;
            ImGui::OpenPopup("palette");
        }
    }
    if (ImGui::IsWindowFocused() && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
        for (const std::uint32_t id : m_Selected)
            m_Graph.RemoveNode(id);
        m_Selected.clear();
    }
    // Drop a texture / parameter from the content browser: a texture sample node.
    dl->PopClipRect();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPayload)) {
            const std::filesystem::path dropped = PathFromUtf8(static_cast<const char*>(payload->Data));
            const std::string           ext     = Lower(PathToUtf8(dropped.extension()));
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".ktx2")
                m_Selected = {m_Graph.AddNode("Texture.Sample", toGraph(mouse), PathToUtf8(dropped.lexically_normal()))};
        }
        ImGui::EndDragDropTarget();
    }
    DrawPalette();
}

void MaterialEditor::DrawPalette()
{
    if (!ImGui::BeginPopup("palette"))
        return;
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", "Search nodes", m_Search, sizeof(m_Search));
    const std::string filter = Lower(m_Search);
    std::string       category;
    std::string       chosen;
    bool              first = true;
    for (const MaterialNodeDesc& desc : MaterialNodeTypes()) {
        if (desc.hidden)
            continue;
        if (!filter.empty() && Lower(desc.title).find(filter) == std::string::npos &&
            Lower(desc.category).find(filter) == std::string::npos)
            continue;
        // A pending link: only nodes with a fitting pin.
        if (m_PendingLink && (m_PendingLink->output ? desc.inputs.empty() : desc.outputs.empty()))
            continue;
        if (desc.category != category) {
            category = desc.category;
            ImGui::SeparatorText(category.c_str());
        }
        if (ImGui::Selectable(desc.title.c_str()) || (first && !filter.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter)))
            chosen = desc.type;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", desc.description.c_str());
        first = false;
    }
    if (!chosen.empty()) {
        std::string param;
        if (chosen.starts_with("Param.")) { // the first parameter of that type
            const MaterialParameterType type = chosen == "Param.Scalar"   ? MaterialParameterType::Scalar
                                               : chosen == "Param.Vector" ? MaterialParameterType::Vector
                                                                          : MaterialParameterType::Texture;
            for (const MaterialParameter& p : m_Graph.parameters)
                if (p.type == type) {
                    param = p.name;
                    break;
                }
        }
        const std::uint32_t id = m_Graph.AddNode(chosen, m_PaletteAt, param);
        if (m_PendingLink) {
            const MaterialNodeDesc* desc = FindMaterialNodeType(chosen);
            if (m_PendingLink->output && !desc->inputs.empty())
                m_Graph.Connect(m_PendingLink->node, m_PendingLink->pin, id, desc->inputs.front().name);
            else if (!m_PendingLink->output && !desc->outputs.empty())
                m_Graph.Connect(id, desc->outputs.front().name, m_PendingLink->node, m_PendingLink->pin);
        }
        m_Selected = {id};
        m_PendingLink.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MaterialEditor::DrawPreview()
{
    const char* shapes[] = {"Sphere", "Box", "Plane"};
    int         shape    = m_ShapeKind;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::Combo("##shape", &shape, shapes, 3))
        SetPreviewShape(shape);
    ImGui::SameLine();
    if (ImGui::Checkbox("Ground", &m_ShowGround) && m_Scene) {
        Registry& r = m_Scene->GetRegistry();
        r.ViewOf<Name>().Each([&](Entity e, Name& name) {
            if (name.value != "Ground")
                return;
            if (m_ShowGround && !r.Has<MeshRenderer>(e))
                r.Emplace<MeshRenderer>(e, MeshRenderer{.model = m_Ground});
            else if (!m_ShowGround && r.Has<MeshRenderer>(e))
                r.Remove<MeshRenderer>(e);
            m_Scene->MarkChanged(e);
        });
    }
    const ImVec2 avail  = ImGui::GetContentRegionAvail();
    const auto   width  = static_cast<std::uint32_t>(std::max(avail.x, 16.0f));
    const auto   height = static_cast<std::uint32_t>(std::max(avail.y - ImGui::GetTextLineHeightWithSpacing(), 16.0f));
    if (m_Scene)
        m_Scene->UpdateTransforms();
    EnsureTarget(width, height);
    ImGui::Image(ImTextureRef(m_Texture), ImVec2(static_cast<float>(width), static_cast<float>(height)));
    m_PreviewVisible = true;
    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            m_Yaw -= io.MouseDelta.x * 0.01f;
            m_Pitch = std::clamp(m_Pitch + io.MouseDelta.y * 0.01f, -1.4f, 1.4f);
        }
        if (io.MouseWheel != 0.0f)
            m_Distance = std::clamp(m_Distance * std::pow(0.9f, io.MouseWheel), 0.8f, 30.0f);
    }
    ImGui::TextDisabled("LMB orbit, wheel zoom");
}

void MaterialEditor::EnsureTarget(std::uint32_t width, std::uint32_t height)
{
    if (m_Image && m_Image.Extent().width == width && m_Image.Extent().height == height)
        return;
    ReleaseTarget();
    m_Image   = Image(m_Ctx.renderer.GetContext(), {.extent    = {width, height, 1},
                                                    .format    = kPreviewFormat,
                                                    .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                    .debugName = "MaterialPreview"});
    m_Texture = m_Ctx.imgui.AddTexture(m_Image.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); // rendered before the UI pass
}

void MaterialEditor::Render(const FrameContext& frame)
{
    if (!m_Open || !m_PreviewVisible || !m_Image || !m_Renderer || !m_Scene)
        return;
    const VkCommandBuffer cmd = frame.cmd;
    CmdImageBarrier(cmd, {.image     = m_Image.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    const glm::vec3 target(0.0f, m_ShapeKind == 2 ? 0.0f : 0.6f, 0.0f);
    FlyCamera camera;
    camera.position = target + m_Distance * glm::vec3(std::cos(m_Pitch) * std::sin(m_Yaw), std::sin(m_Pitch),
                                                      std::cos(m_Pitch) * std::cos(m_Yaw));
    camera.LookAt(target);
    const VkExtent2D extent = m_Image.Extent2D();
    m_Renderer->Render(frame, *m_Scene, camera.GetData(static_cast<float>(extent.width) / static_cast<float>(extent.height)),
                       {.image = m_Image.Handle(), .view = m_Image.View(), .format = kPreviewFormat, .extent = extent});
    CmdImageBarrier(cmd, {.image     = m_Image.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    ++m_PreviewFrames;
}

} // namespace Engine
