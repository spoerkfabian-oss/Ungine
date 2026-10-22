#include "Editor/AnimGraphEditor.h"
#include "Editor/FileDialog.h"
#include "Editor/ImGuiLayer.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Model.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Core/Platform.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace Engine {

namespace {

constexpr VkFormat    kPreviewFormat  = VK_FORMAT_R8G8B8A8_SRGB;
constexpr const char* kContentPayload = "UNGINE_CONTENT";
constexpr std::size_t kMaxUndo        = 100;
constexpr glm::vec2   kStateSize{150.0f, 46.0f};
constexpr glm::vec2   kAnyPosition{-190.0f, 0.0f};

ImVec2 V(glm::vec2 v) { return {v.x, v.y}; }
glm::vec2 G(ImVec2 v) { return {v.x, v.y}; }

float SegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b)
{
    const glm::vec2 ab = b - a;
    const float     len2 = glm::dot(ab, ab);
    const float     t    = len2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(a + ab * t - p);
}

template <class E, std::size_t N>
bool EnumCombo(const char* label, E& value, const std::array<E, N>& values, const char* (*name)(E))
{
    bool changed = false;
    if (ImGui::BeginCombo(label, name(value))) {
        for (E v : values)
            if (ImGui::Selectable(name(v), v == value)) {
                value   = v;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

const char* LayerModeName(AnimLayerMode mode) { return mode == AnimLayerMode::Additive ? "Additive" : "Override"; }

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

AnimGraphEditor::AnimGraphEditor(const Context& context) : m_Ctx(context), m_Dialog(std::make_unique<FileDialog>()) {}

AnimGraphEditor::~AnimGraphEditor()
{
    Close();
    ReleaseTarget();
}

void AnimGraphEditor::ReleaseTarget()
{
    if (m_Image) {
        m_Ctx.imgui.RemoveTexture(m_Texture);
        m_Ctx.renderer.DeferRelease(std::move(m_Image));
        m_Texture = 0;
    }
}

const Model* AnimGraphEditor::PreviewModel() const
{
    return m_Model && m_Ctx.assets.State(m_Model) == AssetState::Ready ? m_Ctx.assets.Get(m_Model) : nullptr;
}

void AnimGraphEditor::Open(const std::filesystem::path& file)
{
    if (m_Open)
        Close();
    m_File = std::filesystem::absolute(file).lexically_normal();
    std::string error;
    if (auto loaded = LoadAnimGraph(m_File, &error)) {
        m_Graph = std::move(*loaded);
        m_Status.clear();
    } else {
        m_Graph  = MakeDefaultAnimGraph();
        m_Status = std::filesystem::exists(m_File) ? error : "New animation graph (not saved yet)";
    }
    m_Saved     = std::filesystem::exists(m_File) ? m_Graph : AnimGraph{};
    m_Committed = m_Graph;
    m_Undo.clear();
    m_Redo.clear();
    m_Applied.reset();
    m_Layer = 0, m_State = -1, m_Transition = -1, m_LinkFrom = -2;
    m_PreviewValues.clear();
    m_IkTargets.clear();
    for (const AnimParameter& p : m_Graph.parameters)
        m_PreviewValues[p.name] = p.type == AnimParamType::Trigger ? 0.0f : p.defaultValue;
    m_Open   = true;
    m_Framed = false;

    m_Scene   = std::make_unique<Scene>();
    m_Preview = std::make_unique<AnimationSystem>(&m_Ctx.assets);
    m_Preview->fireNotifyActions = false;
    if (!m_Renderer) {
        m_Renderer = std::make_unique<SceneRenderer>(m_Ctx.renderer, m_Ctx.renderer.GetContext(), m_Ctx.assets);
        m_Renderer->shadows.resolution   = 1024;
        m_Renderer->shadows.cascadeCount = 2;
        m_Renderer->localShadows.enabled = false;
        m_Renderer->culling.occlusion    = false;
    }
    if (!m_Ground)
        m_Ground = m_Ctx.assets.CreatePrimitive({.shape = PrimitiveShape::Plane, .size = 1.0f, .baseColor = {0.35f, 0.36f, 0.38f, 1.0f}});
    const Entity ground = m_Scene->CreateEntity("Ground");
    m_Scene->GetRegistry().Emplace<MeshRenderer>(ground, MeshRenderer{.model = m_Ground});
    m_PreviewRoot = NullEntity;
    m_ModelPath.clear();
    Apply();
}

void AnimGraphEditor::Close()
{
    if (!m_Open)
        return;
    if (m_Ctx.sceneAnimation)
        m_Ctx.sceneAnimation->Provide(PathToUtf8(m_File), nullptr);
    m_Open = false;
    m_Renderer.reset();
    ReleaseTarget();
    m_Preview.reset();
    m_Scene.reset();
    m_PreviewRoot = NullEntity;
    for (ModelHandle* h : {&m_Model, &m_Ground})
        if (*h) {
            m_Ctx.assets.Release(*h);
            *h = {};
        }
    m_ModelPath.clear();
}

bool AnimGraphEditor::Dirty() const { return m_Open && m_Graph != m_Saved; }

bool AnimGraphEditor::Save(std::string* error)
{
    std::string why;
    if (!SaveAnimGraph(m_File, m_Graph, &why)) {
        m_Status = why;
        if (error)
            *error = why;
        return false;
    }
    m_Saved  = m_Graph;
    m_Status = "Saved";
    return true;
}

void AnimGraphEditor::Revert()
{
    m_Graph = m_Saved.layers.empty() ? MakeDefaultAnimGraph() : m_Saved;
    Commit();
}

void AnimGraphEditor::Commit()
{
    if (m_Graph == m_Committed)
        return;
    m_Undo.push_back(m_Committed);
    if (m_Undo.size() > kMaxUndo)
        m_Undo.erase(m_Undo.begin());
    m_Redo.clear();
    m_Committed = m_Graph;
    Apply();
}

bool AnimGraphEditor::Undo()
{
    if (m_Undo.empty())
        return false;
    m_Redo.push_back(m_Committed);
    m_Graph     = m_Undo.back();
    m_Committed = m_Graph;
    m_Undo.pop_back();
    Apply();
    return true;
}

bool AnimGraphEditor::Redo()
{
    if (m_Redo.empty())
        return false;
    m_Undo.push_back(m_Committed);
    m_Graph     = m_Redo.back();
    m_Committed = m_Graph;
    m_Redo.pop_back();
    Apply();
    return true;
}

void AnimGraphEditor::Apply()
{
    if (!m_Open)
        return;
    m_Layer = std::min(m_Layer, m_Graph.layers.empty() ? std::size_t{0} : m_Graph.layers.size() - 1);
    const AnimLayer* layer = m_Layer < m_Graph.layers.size() ? &m_Graph.layers[m_Layer] : nullptr;
    if (!layer || m_State >= static_cast<int>(layer->states.size()))
        m_State = -1;
    if (!layer || m_Transition >= static_cast<int>(layer->transitions.size()))
        m_Transition = -1;
    if (m_Applied && *m_Applied == m_Graph)
        return;
    m_Applied      = m_Graph;
    auto shared    = std::make_shared<const AnimGraph>(m_Graph);
    const auto key = PathToUtf8(m_File);
    if (m_Preview)
        m_Preview->Provide(key, shared);
    if (m_Ctx.sceneAnimation)
        m_Ctx.sceneAnimation->Provide(key, shared);
    m_Problems = ValidateAnimGraph(m_Graph, PreviewModel());
    LoadPreviewModel();
}

void AnimGraphEditor::LoadPreviewModel()
{
    if (m_Graph.previewModel == m_ModelPath)
        return;
    if (m_PreviewRoot != NullEntity && m_Scene) {
        m_Scene->DestroyEntity(m_PreviewRoot);
        m_PreviewRoot = NullEntity;
    }
    if (m_Model) {
        m_Ctx.assets.Release(m_Model);
        m_Model = {};
    }
    m_ModelPath = m_Graph.previewModel;
    m_Framed    = false;
    if (!m_ModelPath.empty())
        m_Model = m_Ctx.assets.LoadModel(PathFromUtf8(m_ModelPath));
}

bool AnimGraphEditor::PathField(const char* label, std::string& path, const std::vector<std::string>& extensions, int purpose,
                                std::size_t index)
{
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::SmallButton("...")) {
        m_DialogPurpose = purpose;
        m_DialogIndex   = index;
        m_Dialog->Open(std::string("Choose ") + label, FileDialog::Mode::Open,
                       path.empty() ? m_File.parent_path() : PathFromUtf8(path).parent_path(), extensions);
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
            if (std::ranges::find(extensions, Lower(PathToUtf8(dropped.extension()))) != extensions.end()) {
                path    = PathToUtf8(dropped.lexically_normal());
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    return changed;
}

bool AnimGraphEditor::ClipCombo(const char* label, std::string& clip)
{
    const Model* model = PreviewModel();
    if (!model || model->animations.empty())
        return ImGui::InputText(label, &clip); // no preview model: type the clip name
    bool changed = false;
    if (ImGui::BeginCombo(label, clip.empty() ? "(none)" : clip.c_str())) {
        for (const AnimationClip& c : model->animations)
            if (ImGui::Selectable(c.name.c_str(), c.name == clip)) {
                clip    = c.name;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

bool AnimGraphEditor::BoneCombo(const char* label, std::string& bone)
{
    const Model* model = PreviewModel();
    if (!model)
        return ImGui::InputText(label, &bone);
    bool changed = false;
    if (ImGui::BeginCombo(label, bone.empty() ? "(none)" : bone.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("(none)", bone.empty()))
            bone.clear(), changed = true;
        for (std::size_t i = 0; i < model->nodes.size(); ++i) {
            const ModelNode& n = model->nodes[i];
            if (n.mesh >= 0 || n.name.empty())
                continue;
            int depth = 0;
            for (int p = n.parent; p >= 0 && depth < 16; p = model->nodes[static_cast<std::size_t>(p)].parent)
                ++depth;
            const std::string text = std::string(static_cast<std::size_t>(depth) * 2, ' ') + n.name;
            if (ImGui::Selectable(text.c_str(), n.name == bone)) {
                bone    = n.name;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool AnimGraphEditor::ParameterCombo(const char* label, std::string& parameter, bool numericOnly)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, parameter.empty() ? "(none)" : parameter.c_str())) {
        if (ImGui::Selectable("(none)", parameter.empty()))
            parameter.clear(), changed = true;
        for (const AnimParameter& p : m_Graph.parameters) {
            if (numericOnly && p.type != AnimParamType::Float && p.type != AnimParamType::Int)
                continue;
            if (ImGui::Selectable(p.name.c_str(), p.name == parameter)) {
                parameter = p.name;
                changed   = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void AnimGraphEditor::RenameState(AnimLayer& layer, const std::string& from, const std::string& to)
{
    for (AnimTransition& t : layer.transitions) {
        if (t.from == from)
            t.from = to;
        if (t.to == from)
            t.to = to;
    }
    if (layer.entryState == from)
        layer.entryState = to;
}

void AnimGraphEditor::Draw(float dt)
{
    m_PreviewVisible = false;
    if (!m_Open)
        return;
    ImGui::SetNextWindowSize(ImVec2(1200.0f, 700.0f), ImGuiCond_FirstUseEver);
    const std::string title = "Animation Graph - " + PathToUtf8(m_File.filename()) + (Dirty() ? " *" : "") + "###animgrapheditor";
    bool open = true;
    if (!ImGui::Begin(title.c_str(), &open)) {
        ImGui::End();
        if (!open)
            Close();
        return;
    }
    DrawToolbar();
    const float sidebar = 300.0f, details = 320.0f;
    ImGui::BeginChild("sidebar", ImVec2(sidebar, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    DrawSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    const float middle = std::max(ImGui::GetContentRegionAvail().x - details - 8.0f, 200.0f);
    ImGui::BeginChild("middle", ImVec2(middle, 0.0f));
    ImGui::BeginChild("canvas", ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.55f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas();
    ImGui::EndChild();
    ImGui::BeginChild("preview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    DrawPreview(dt);
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    DrawDetails();
    ImGui::EndChild();

    if (!ImGui::IsAnyItemActive() && m_Dragging < 0)
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
        const std::string path = PathToUtf8(chosen->lexically_normal());
        if (m_DialogPurpose == 1)
            m_Graph.previewModel = path;
        else if (m_Layer < m_Graph.layers.size() && m_State >= 0 &&
                 static_cast<std::size_t>(m_State) < m_Graph.layers[m_Layer].states.size()) {
            auto& notifies = m_Graph.layers[m_Layer].states[static_cast<std::size_t>(m_State)].notifies;
            if (m_DialogIndex < notifies.size())
                (m_DialogPurpose == 2 ? notifies[m_DialogIndex].sound : notifies[m_DialogIndex].effect) = path;
        }
        Commit();
    }
    if (!open)
        Close();
}

void AnimGraphEditor::DrawToolbar()
{
    if (ImGui::Button("Save"))
        Save();
    ImGui::SameLine();
    ImGui::BeginDisabled(!Dirty());
    if (ImGui::Button("Revert"))
        Revert();
    ImGui::EndDisabled();
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
    ImGui::SameLine();
    if (ImGui::Button("Restart") && m_Preview && m_PreviewRoot != NullEntity)
        m_Preview->Restart(m_PreviewRoot);
    ImGui::SameLine();
    ImGui::Checkbox("Pause", &m_Paused);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("Speed", &m_Speed, 0.0f, 3.0f, "%.2fx");
    if (!m_Status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_Status.c_str());
    }
}

void AnimGraphEditor::DrawSidebar()
{
    if (ImGui::CollapsingHeader("Graph", ImGuiTreeNodeFlags_DefaultOpen))
        DrawGraphSettings();
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
        DrawParameters();
    if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen))
        DrawLayers();
    if (ImGui::CollapsingHeader("IK", ImGuiTreeNodeFlags_DefaultOpen))
        DrawIk();
    if (!m_Problems.empty() && ImGui::CollapsingHeader("Problems", ImGuiTreeNodeFlags_DefaultOpen))
        for (const std::string& p : m_Problems)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", p.c_str());
}

void AnimGraphEditor::DrawGraphSettings()
{
    PathField("Preview model", m_Graph.previewModel, {".glb", ".gltf"}, 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Model shown in the preview; its clips and bones fill the lists");
    BoneCombo("Root motion bone", m_Graph.rootMotionBone);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Its movement moves the entity (or its character) when the controller applies root motion");
}

void AnimGraphEditor::DrawParameters()
{
    constexpr std::array types{AnimParamType::Float, AnimParamType::Int, AnimParamType::Bool, AnimParamType::Trigger};
    int remove = -1;
    for (std::size_t i = 0; i < m_Graph.parameters.size(); ++i) {
        AnimParameter& p = m_Graph.parameters[i];
        ImGui::PushID(static_cast<int>(i));
        std::string name = p.name;
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() &&
            !m_Graph.FindParameter(name)) {
            // References follow the rename.
            const std::string old = p.name;
            for (AnimLayer& layer : m_Graph.layers) {
                if (layer.weightParameter == old)
                    layer.weightParameter = name;
                for (AnimState& s : layer.states)
                    for (std::string* ref : {&s.speedParameter, &s.xParameter, &s.yParameter})
                        if (*ref == old)
                            *ref = name;
                for (AnimTransition& t : layer.transitions)
                    for (AnimCondition& c : t.conditions)
                        if (c.parameter == old)
                            c.parameter = name;
            }
            for (AnimIk& goal : m_Graph.ik)
                if (goal.weightParameter == old)
                    goal.weightParameter = name;
            m_PreviewValues[name] = m_PreviewValues[old];
            p.name                = name;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        EnumCombo("##type", p.type, types, AnimParamTypeName);
        ImGui::SameLine();
        float& live = m_PreviewValues[p.name];
        ImGui::SetNextItemWidth(70.0f);
        switch (p.type) {
        case AnimParamType::Float: ImGui::DragFloat("##value", &live, 0.02f); break;
        case AnimParamType::Int: {
            int v = static_cast<int>(live);
            if (ImGui::DragInt("##value", &v))
                live = static_cast<float>(v);
            break;
        }
        case AnimParamType::Bool: {
            bool v = live != 0.0f;
            if (ImGui::Checkbox("##value", &v))
                live = v ? 1.0f : 0.0f;
            break;
        }
        case AnimParamType::Trigger:
            if (ImGui::Button("Fire") && m_Preview && m_PreviewRoot != NullEntity)
                m_Preview->SetTrigger(m_PreviewRoot, p.name);
            break;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Preview value (the default is set with 'Use as default')");
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = static_cast<int>(i);
        if (p.type != AnimParamType::Trigger && live != p.defaultValue) {
            ImGui::SameLine();
            if (ImGui::SmallButton("def"))
                p.defaultValue = live;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Use as default (%.3g)", p.defaultValue);
        }
        ImGui::PopID();
    }
    if (remove >= 0)
        m_Graph.parameters.erase(m_Graph.parameters.begin() + remove);
    if (ImGui::Button("+ Parameter")) {
        std::string name = "Param";
        for (int n = 2; m_Graph.FindParameter(name); ++n)
            name = "Param " + std::to_string(n);
        m_Graph.parameters.push_back(AnimParameter{.name = name});
    }
}

void AnimGraphEditor::DrawLayers()
{
    for (std::size_t i = 0; i < m_Graph.layers.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(m_Graph.layers[i].name.c_str(), i == m_Layer, 0, ImVec2(160.0f, 0.0f)))
            m_Layer = i, m_State = -1, m_Transition = -1;
        ImGui::SameLine();
        if (ImGui::SmallButton("^") && i > 0)
            std::swap(m_Graph.layers[i], m_Graph.layers[i - 1]), m_Layer = i - 1;
        ImGui::SameLine();
        if (ImGui::SmallButton("v") && i + 1 < m_Graph.layers.size())
            std::swap(m_Graph.layers[i], m_Graph.layers[i + 1]), m_Layer = i + 1;
        ImGui::SameLine();
        ImGui::BeginDisabled(m_Graph.layers.size() <= 1);
        if (ImGui::SmallButton("x")) {
            m_Graph.layers.erase(m_Graph.layers.begin() + static_cast<std::ptrdiff_t>(i));
            m_Layer = 0, m_State = -1, m_Transition = -1;
            ImGui::EndDisabled();
            ImGui::PopID();
            break;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (ImGui::Button("+ Layer")) {
        AnimLayer layer{.name = "Layer " + std::to_string(m_Graph.layers.size() + 1), .entryState = "Idle"};
        layer.states.push_back(AnimState{.name = "Idle", .position = {40.0f, 40.0f}});
        m_Graph.layers.push_back(std::move(layer));
        m_Layer = m_Graph.layers.size() - 1;
    }
    if (m_Layer >= m_Graph.layers.size())
        return;
    AnimLayer& layer = m_Graph.layers[m_Layer];
    ImGui::Separator();
    ImGui::InputText("Name", &layer.name);
    constexpr std::array modes{AnimLayerMode::Override, AnimLayerMode::Additive};
    EnumCombo("Mode", layer.mode, modes, LayerModeName);
    ImGui::SliderFloat("Weight", &layer.weight, 0.0f, 1.0f);
    ParameterCombo("Weight parameter", layer.weightParameter, true);
    if (ImGui::BeginCombo("Entry state", layer.entryState.c_str())) {
        for (const AnimState& s : layer.states)
            if (ImGui::Selectable(s.name.c_str(), s.name == layer.entryState))
                layer.entryState = s.name;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Bone mask (empty: every bone)");
    int remove = -1;
    for (std::size_t i = 0; i < layer.mask.size(); ++i) {
        ImGui::PushID(static_cast<int>(1000 + i));
        ImGui::SetNextItemWidth(150.0f);
        BoneCombo("##bone", layer.mask[i].bone);
        ImGui::SameLine();
        ImGui::Checkbox("children", &layer.mask[i].includeChildren);
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0)
        layer.mask.erase(layer.mask.begin() + remove);
    if (ImGui::SmallButton("+ Mask bone"))
        layer.mask.push_back({});
}

void AnimGraphEditor::DrawIk()
{
    int remove = -1;
    for (std::size_t i = 0; i < m_Graph.ik.size(); ++i) {
        AnimIk& goal = m_Graph.ik[i];
        ImGui::PushID(static_cast<int>(2000 + i));
        const std::string header = goal.name + " (" + AnimIkKindName(goal.kind) + ")###goal";
        if (ImGui::TreeNode(header.c_str())) {
            std::string name = goal.name;
            if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty())
                goal.name = name;
            ImGui::Checkbox("Enabled", &goal.enabled);
            ImGui::SliderFloat("Weight", &goal.weight, 0.0f, 1.0f);
            ParameterCombo("Weight parameter", goal.weightParameter, true);
            switch (goal.kind) {
            case AnimIkKind::TwoBone:
                BoneCombo("End bone", goal.endBone);
                ImGui::Checkbox("Keep end rotation", &goal.keepEndRotation);
                break;
            case AnimIkKind::Fabrik:
                BoneCombo("Root bone", goal.rootBone);
                BoneCombo("End bone", goal.endBone);
                ImGui::SliderInt("Iterations", reinterpret_cast<int*>(&goal.iterations), 1, 64);
                ImGui::DragFloat("Tolerance", &goal.tolerance, 0.0001f, 1.0e-6f, 1.0f, "%.5f");
                break;
            case AnimIkKind::LookAt: {
                int drop = -1;
                for (std::size_t b = 0; b < goal.bones.size(); ++b) {
                    ImGui::PushID(static_cast<int>(b));
                    ImGui::SetNextItemWidth(170.0f);
                    BoneCombo("##bone", goal.bones[b]);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x"))
                        drop = static_cast<int>(b);
                    ImGui::PopID();
                }
                if (drop >= 0)
                    goal.bones.erase(goal.bones.begin() + drop);
                if (ImGui::SmallButton("+ Bone"))
                    goal.bones.emplace_back();
                ImGui::DragFloat3("Forward axis", &goal.forwardAxis.x, 0.01f, -1.0f, 1.0f);
                ImGui::SliderFloat("Max angle", &goal.maxAngle, 0.0f, 180.0f, "%.0f deg");
                break;
            }
            case AnimIkKind::FootPlacement: {
                int drop = -1;
                for (std::size_t b = 0; b < goal.feet.size(); ++b) {
                    ImGui::PushID(static_cast<int>(b));
                    ImGui::SetNextItemWidth(170.0f);
                    BoneCombo("##foot", goal.feet[b]);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x"))
                        drop = static_cast<int>(b);
                    ImGui::PopID();
                }
                if (drop >= 0)
                    goal.feet.erase(goal.feet.begin() + drop);
                if (ImGui::SmallButton("+ Foot"))
                    goal.feet.emplace_back();
                BoneCombo("Pelvis", goal.pelvisBone);
                ImGui::DragFloat("Ray height", &goal.rayHeight, 0.01f, 0.0f, 100.0f);
                ImGui::DragFloat("Ray depth", &goal.rayDepth, 0.01f, 0.0f, 100.0f);
                ImGui::DragFloat("Max pelvis offset", &goal.maxPelvisOffset, 0.01f, 0.0f, 100.0f);
                ImGui::Checkbox("Align to ground", &goal.alignToNormal);
                ImGui::SliderFloat("Max foot angle", &goal.maxFootAngle, 0.0f, 90.0f, "%.0f deg");
                ImGui::DragFloat("Smoothing", &goal.smoothing, 0.1f, 0.0f, 1000.0f, "%.1f /s");
                ImGui::TextDisabled("Raycasts the scene's physics (no effect in the preview)");
                break;
            }
            }
            if (goal.kind != AnimIkKind::FootPlacement) {
                glm::vec3& target = m_IkTargets[goal.name];
                bool       on     = m_IkTargets.contains(goal.name + "#on");
                if (ImGui::Checkbox("Preview target", &on)) {
                    if (on)
                        m_IkTargets[goal.name + "#on"] = glm::vec3(1.0f);
                    else {
                        m_IkTargets.erase(goal.name + "#on");
                        if (m_Preview && m_PreviewRoot != NullEntity)
                            m_Preview->ClearIkTarget(m_PreviewRoot, goal.name);
                    }
                }
                if (on)
                    ImGui::DragFloat3("Target (world)", &target.x, 0.01f);
            }
            if (ImGui::SmallButton("Remove goal"))
                remove = static_cast<int>(i);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0)
        m_Graph.ik.erase(m_Graph.ik.begin() + remove);
    if (ImGui::Button("+ IK goal"))
        ImGui::OpenPopup("add_ik");
    if (ImGui::BeginPopup("add_ik")) {
        for (const AnimIkKind kind : {AnimIkKind::TwoBone, AnimIkKind::FootPlacement, AnimIkKind::LookAt, AnimIkKind::Fabrik})
            if (ImGui::MenuItem(AnimIkKindName(kind))) {
                AnimIk goal{.name = std::string(AnimIkKindName(kind)) + " " + std::to_string(m_Graph.ik.size() + 1), .kind = kind};
                m_Graph.ik.push_back(std::move(goal));
            }
        ImGui::EndPopup();
    }
}

void AnimGraphEditor::DrawCanvas()
{
    if (m_Layer >= m_Graph.layers.size()) {
        ImGui::TextDisabled("No layer");
        return;
    }
    AnimLayer&   layer  = m_Graph.layers[m_Layer];
    ImDrawList*  draw   = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size   = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("canvas", ImVec2(std::max(size.x, 50.0f), std::max(size.y, 50.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool      hovered = ImGui::IsItemHovered();
    const glm::vec2 base    = G(origin) + m_Pan;
    const glm::vec2 mouse   = G(ImGui::GetIO().MousePos);
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 30, 36, 255));
    for (float x = std::fmod(m_Pan.x, 32.0f); x < size.x; x += 32.0f)
        draw->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(40, 42, 50, 255));
    for (float y = std::fmod(m_Pan.y, 32.0f); y < size.y; y += 32.0f)
        draw->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(40, 42, 50, 255));

    const auto stateRect = [&](int index) {
        const glm::vec2 p = index < 0 ? kAnyPosition : layer.states[static_cast<std::size_t>(index)].position;
        return std::pair{base + p, base + p + kStateSize};
    };
    const auto center = [&](int index) {
        const auto [a, b] = stateRect(index);
        return (a + b) * 0.5f;
    };
    const auto stateIndex = [&](const std::string& name) {
        for (std::size_t i = 0; i < layer.states.size(); ++i)
            if (layer.states[i].name == name)
                return static_cast<int>(i);
        return -2;
    };
    const auto stateAt = [&](glm::vec2 p) {
        for (int i = static_cast<int>(layer.states.size()) - 1; i >= -1; --i) {
            const auto [a, b] = stateRect(i);
            if (p.x >= a.x && p.y >= a.y && p.x <= b.x && p.y <= b.y)
                return i;
        }
        return -2;
    };

    // Runtime status of the preview.
    std::string current, fading;
    float       progress = 0.0f;
    if (m_Preview && m_PreviewRoot != NullEntity)
        if (const auto info = m_Preview->Inspect(m_PreviewRoot); info && m_Layer < info->layers.size()) {
            current  = info->layers[m_Layer].state;
            fading   = info->layers[m_Layer].fromState;
            progress = info->layers[m_Layer].normalizedTime - std::floor(info->layers[m_Layer].normalizedTime);
        }

    // Transitions: arrows from centre to centre (offset sideways so both directions show).
    int hotTransition = -1;
    for (std::size_t i = 0; i < layer.transitions.size(); ++i) {
        const AnimTransition& t    = layer.transitions[i];
        const int             from = t.from.empty() ? -1 : stateIndex(t.from);
        const int             to   = stateIndex(t.to);
        if (from == -2 || to < 0)
            continue;
        glm::vec2       a = center(from), b = center(to);
        const glm::vec2 d = b - a;
        if (glm::length(d) < 1.0f)
            continue;
        const glm::vec2 n = glm::normalize(glm::vec2(-d.y, d.x)) * 6.0f;
        a += n, b += n;
        const bool selected = static_cast<int>(i) == m_Transition;
        const bool hot      = hovered && SegmentDistance(mouse, a, b) < 5.0f;
        if (hot)
            hotTransition = static_cast<int>(i);
        const ImU32 color = selected ? IM_COL32(255, 210, 90, 255) : hot ? IM_COL32(220, 220, 255, 255) : IM_COL32(170, 175, 190, 255);
        draw->AddLine(V(a), V(b), color, selected ? 3.0f : 2.0f);
        const glm::vec2 mid = (a + b) * 0.5f, dir = glm::normalize(d), side(-dir.y, dir.x);
        draw->AddTriangleFilled(V(mid + dir * 7.0f), V(mid - dir * 5.0f + side * 6.0f), V(mid - dir * 5.0f - side * 6.0f), color);
    }
    // Pending transition (from the context menu): a line to the mouse.
    if (m_LinkFrom != -2)
        draw->AddLine(V(center(m_LinkFrom)), V(mouse), IM_COL32(255, 210, 90, 200), 2.0f);

    // States.
    for (int i = -1; i < static_cast<int>(layer.states.size()); ++i) {
        const auto [a, b] = stateRect(i);
        const bool  any    = i < 0;
        const AnimState* s = any ? nullptr : &layer.states[static_cast<std::size_t>(i)];
        const bool  selected = i == m_State && !any;
        const bool  entry    = s && s->name == layer.entryState;
        const bool  active   = s && s->name == current;
        ImU32 fill = any ? IM_COL32(60, 45, 70, 255) : IM_COL32(52, 58, 72, 255);
        if (active)
            fill = IM_COL32(40, 90, 60, 255);
        else if (s && s->name == fading)
            fill = IM_COL32(40, 70, 55, 255);
        draw->AddRectFilled(V(a), V(b), fill, 6.0f);
        draw->AddRect(V(a), V(b), selected ? IM_COL32(255, 210, 90, 255) : entry ? IM_COL32(255, 150, 50, 255) : IM_COL32(110, 115, 130, 255),
                      6.0f, selected || entry ? 2.5f : 1.0f);
        const std::string label = any ? std::string("Any State")
                                      : s->name + (s->kind == AnimStateKind::Clip ? "" : s->kind == AnimStateKind::BlendSpace1D ? "  [1D]" : "  [2D]");
        draw->AddText(ImVec2(a.x + 8.0f, a.y + 6.0f), IM_COL32(235, 235, 240, 255), label.c_str());
        if (s) {
            const std::string sub = s->kind == AnimStateKind::Clip ? s->clip : std::to_string(s->samples.size()) + " samples";
            draw->AddText(ImVec2(a.x + 8.0f, a.y + 24.0f), IM_COL32(160, 165, 180, 255), sub.c_str());
        }
        if (active)
            draw->AddRectFilled(ImVec2(a.x + 4.0f, b.y - 5.0f), ImVec2(a.x + 4.0f + (b.x - a.x - 8.0f) * progress, b.y - 2.0f),
                                IM_COL32(120, 230, 140, 255));
    }
    draw->PopClipRect();

    // Interaction.
    const int hotState = hovered ? stateAt(mouse) : -2;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (m_LinkFrom != -2) {
            if (hotState >= 0 && hotState != m_LinkFrom) {
                layer.transitions.push_back(AnimTransition{
                    .from = m_LinkFrom < 0 ? std::string() : layer.states[static_cast<std::size_t>(m_LinkFrom)].name,
                    .to = layer.states[static_cast<std::size_t>(hotState)].name, .hasExitTime = true});
                m_Transition = static_cast<int>(layer.transitions.size()) - 1;
                m_State      = -1;
            }
            m_LinkFrom = -2;
        } else if (hotState >= 0) {
            m_State = hotState, m_Transition = -1, m_Dragging = hotState;
        } else if (hotTransition >= 0) {
            m_Transition = hotTransition, m_State = -1;
        } else
            m_State = -1, m_Transition = -1;
    }
    if (m_Dragging >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && static_cast<std::size_t>(m_Dragging) < layer.states.size())
            layer.states[static_cast<std::size_t>(m_Dragging)].position += G(ImGui::GetIO().MouseDelta);
        else
            m_Dragging = -1;
    }
    if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)))
        m_Pan += G(ImGui::GetIO().MouseDelta);
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && glm::length(G(ImGui::GetMouseDragDelta(ImGuiMouseButton_Right))) < 4.0f) {
        m_LinkFrom = -2;
        if (hotState >= -1) {
            if (hotState >= 0)
                m_State = hotState, m_Transition = -1;
            m_MenuState = hotState;
            ImGui::OpenPopup("state_menu");
        } else {
            ImGui::OpenPopup("canvas_menu");
        }
    }
    static glm::vec2 menuAt{0.0f};
    if (ImGui::IsPopupOpen("canvas_menu") == false)
        menuAt = mouse - base - kStateSize * 0.5f;
    if (ImGui::BeginPopup("canvas_menu")) {
        const auto add = [&](const char* text, AnimStateKind kind) {
            if (!ImGui::MenuItem(text))
                return;
            std::string name = kind == AnimStateKind::Clip ? "State" : "Blend";
            for (int n = 2; stateIndex(name) != -2; ++n)
                name = (kind == AnimStateKind::Clip ? "State " : "Blend ") + std::to_string(n);
            AnimState state{.name = name, .kind = kind, .position = menuAt};
            if (kind != AnimStateKind::Clip && !m_Graph.parameters.empty())
                state.xParameter = state.yParameter = m_Graph.parameters.front().name;
            layer.states.push_back(std::move(state));
            if (layer.entryState.empty())
                layer.entryState = name;
            m_State = static_cast<int>(layer.states.size()) - 1, m_Transition = -1;
        };
        add("Add clip state", AnimStateKind::Clip);
        add("Add 1D blend space", AnimStateKind::BlendSpace1D);
        add("Add 2D blend space", AnimStateKind::BlendSpace2D);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("state_menu")) {
        const int target = m_MenuState;
        if (ImGui::MenuItem("Make transition"))
            m_LinkFrom = target;
        if (target >= 0) {
            AnimState& s = layer.states[static_cast<std::size_t>(target)];
            if (ImGui::MenuItem("Set as entry", nullptr, s.name == layer.entryState))
                layer.entryState = s.name;
            if (ImGui::MenuItem("Delete state")) {
                const std::string name = s.name;
                std::erase_if(layer.transitions, [&](const AnimTransition& t) { return t.from == name || t.to == name; });
                layer.states.erase(layer.states.begin() + target);
                if (layer.entryState == name)
                    layer.entryState = layer.states.empty() ? std::string() : layer.states.front().name;
                m_State = -1;
            }
        }
        ImGui::EndPopup();
    }
    if ((hovered || ImGui::IsWindowFocused()) && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !ImGui::GetIO().WantTextInput) {
        if (m_Transition >= 0 && static_cast<std::size_t>(m_Transition) < layer.transitions.size()) {
            layer.transitions.erase(layer.transitions.begin() + m_Transition);
            m_Transition = -1;
        } else if (m_State >= 0 && static_cast<std::size_t>(m_State) < layer.states.size()) {
            const std::string name = layer.states[static_cast<std::size_t>(m_State)].name;
            std::erase_if(layer.transitions, [&](const AnimTransition& t) { return t.from == name || t.to == name; });
            layer.states.erase(layer.states.begin() + m_State);
            if (layer.entryState == name)
                layer.entryState = layer.states.empty() ? std::string() : layer.states.front().name;
            m_State = -1;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        m_LinkFrom = -2;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + 4.0f));
    ImGui::TextDisabled("%s  -  right-click: add state / make transition, drag: move, RMB/MMB drag: pan, Del: remove",
                        layer.name.c_str());
}

void AnimGraphEditor::DrawDetails()
{
    if (m_Layer >= m_Graph.layers.size())
        return;
    AnimLayer& layer = m_Graph.layers[m_Layer];
    if (m_State >= 0 && static_cast<std::size_t>(m_State) < layer.states.size())
        DrawState(layer.states[static_cast<std::size_t>(m_State)]);
    else if (m_Transition >= 0 && static_cast<std::size_t>(m_Transition) < layer.transitions.size())
        DrawTransition(layer.transitions[static_cast<std::size_t>(m_Transition)]);
    else
        ImGui::TextDisabled("Select a state or a transition");
}

void AnimGraphEditor::DrawState(AnimState& state)
{
    AnimLayer& layer = m_Graph.layers[m_Layer];
    ImGui::SeparatorText("State");
    std::string name = state.name;
    if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && !layer.FindState(name)) {
        RenameState(layer, state.name, name);
        state.name = name;
    }
    constexpr std::array kinds{AnimStateKind::Clip, AnimStateKind::BlendSpace1D, AnimStateKind::BlendSpace2D};
    EnumCombo("Kind", state.kind, kinds, AnimStateKindName);
    if (state.kind == AnimStateKind::Clip)
        ClipCombo("Clip", state.clip);
    ImGui::DragFloat("Speed", &state.speed, 0.01f, -10.0f, 10.0f);
    ParameterCombo("Speed parameter", state.speedParameter, true);
    ImGui::Checkbox("Loop", &state.loop);
    if (state.kind != AnimStateKind::Clip)
        DrawBlendSpace(state);

    ImGui::SeparatorText("Notifies");
    int remove = -1;
    for (std::size_t i = 0; i < state.notifies.size(); ++i) {
        AnimNotify& n = state.notifies[i];
        ImGui::PushID(static_cast<int>(3000 + i));
        ImGui::InputText("Name", &n.name);
        ImGui::SliderFloat("Time", &n.time, 0.0f, 1.0f, "%.3f");
        BoneCombo("Bone", n.bone);
        PathField("Sound", n.sound, {".wav", ".ogg", ".mp3", ".flac", ".ucue"}, 2, i);
        if (!n.sound.empty())
            ImGui::SliderFloat("Volume", &n.volume, 0.0f, 2.0f);
        PathField("Effect", n.effect, {".uparticle"}, 3, i);
        if (ImGui::SmallButton("Remove notify"))
            remove = static_cast<int>(i);
        ImGui::Separator();
        ImGui::PopID();
    }
    if (remove >= 0)
        state.notifies.erase(state.notifies.begin() + remove);
    if (ImGui::Button("+ Notify"))
        state.notifies.push_back(AnimNotify{.name = "Notify " + std::to_string(state.notifies.size() + 1)});
}

void AnimGraphEditor::DrawBlendSpace(AnimState& state)
{
    const bool twoD = state.kind == AnimStateKind::BlendSpace2D;
    ImGui::SeparatorText(twoD ? "2D blend space" : "1D blend space");
    ParameterCombo("X parameter", state.xParameter, true);
    if (twoD)
        ParameterCombo("Y parameter", state.yParameter, true);
    int remove = -1;
    for (std::size_t i = 0; i < state.samples.size(); ++i) {
        AnimBlendSample& s = state.samples[i];
        ImGui::PushID(static_cast<int>(4000 + i));
        ImGui::SetNextItemWidth(110.0f);
        ClipCombo("##clip", s.clip);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(twoD ? 90.0f : 50.0f);
        if (twoD)
            ImGui::DragFloat2("##pos", &s.position.x, 0.01f);
        else
            ImGui::DragFloat("##pos", &s.position.x, 0.01f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(45.0f);
        ImGui::DragFloat("##speed", &s.speed, 0.01f, 0.01f, 10.0f, "x%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0)
        state.samples.erase(state.samples.begin() + remove);
    if (ImGui::SmallButton("+ Sample"))
        state.samples.push_back(AnimBlendSample{.position = state.samples.empty() ? glm::vec2(0.0f) : state.samples.back().position + glm::vec2(1.0f, 0.0f)});

    // Plot: samples and the preview parameter point (drag it to try the blend).
    if (state.samples.empty())
        return;
    glm::vec2 lo = state.samples[0].position, hi = lo;
    for (const AnimBlendSample& s : state.samples)
        lo = glm::min(lo, s.position), hi = glm::max(hi, s.position);
    const glm::vec2 span = glm::max(hi - lo, glm::vec2(1.0f));
    lo -= span * 0.1f, hi += span * 0.1f;
    const float  width  = std::max(ImGui::GetContentRegionAvail().x, 120.0f);
    const float  height = twoD ? width * 0.7f : 36.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("plot", ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(30, 32, 38, 255));
    const auto toScreen = [&](glm::vec2 p) {
        const glm::vec2 t = (p - lo) / (hi - lo);
        return ImVec2(origin.x + t.x * width, twoD ? origin.y + (1.0f - t.y) * height : origin.y + height * 0.5f);
    };
    for (const AnimBlendSample& s : state.samples) {
        const ImVec2 p = toScreen(s.position);
        draw->AddCircleFilled(p, 4.0f, IM_COL32(255, 200, 80, 255));
        draw->AddText(ImVec2(p.x + 5.0f, p.y - 14.0f), IM_COL32(200, 200, 210, 255), s.clip.c_str());
    }
    float& x = m_PreviewValues[state.xParameter];
    float  dummy = 0.0f;
    float& y = twoD ? m_PreviewValues[state.yParameter] : dummy;
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        x = lo.x + std::clamp((m.x - origin.x) / width, 0.0f, 1.0f) * (hi.x - lo.x);
        if (twoD)
            y = lo.y + std::clamp(1.0f - (m.y - origin.y) / height, 0.0f, 1.0f) * (hi.y - lo.y);
    }
    const ImVec2 point = toScreen({x, y});
    draw->AddCircle(point, 6.0f, IM_COL32(255, 90, 90, 255), 0, 2.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drag to set the preview parameter%s", twoD ? "s" : "");
}

void AnimGraphEditor::DrawTransition(AnimTransition& transition)
{
    ImGui::SeparatorText("Transition");
    ImGui::Text("%s  ->  %s", transition.from.empty() ? "Any State" : transition.from.c_str(), transition.to.c_str());
    ImGui::DragFloat("Duration", &transition.duration, 0.01f, 0.0f, 10.0f, "%.2f s");
    ImGui::Checkbox("Exit time", &transition.hasExitTime);
    if (transition.hasExitTime)
        ImGui::DragFloat("At", &transition.exitTime, 0.01f, 0.0f, 100.0f, "%.2f (normalized)");
    ImGui::Checkbox("Can be interrupted", &transition.canInterrupt);
    ImGui::SeparatorText("Conditions (all)");
    constexpr std::array numeric{AnimCompare::Greater, AnimCompare::Less, AnimCompare::GreaterEqual, AnimCompare::LessEqual,
                                 AnimCompare::Equal, AnimCompare::NotEqual};
    constexpr std::array boolean{AnimCompare::IsTrue, AnimCompare::IsFalse};
    int remove = -1;
    for (std::size_t i = 0; i < transition.conditions.size(); ++i) {
        AnimCondition& c = transition.conditions[i];
        ImGui::PushID(static_cast<int>(5000 + i));
        ImGui::SetNextItemWidth(110.0f);
        ParameterCombo("##param", c.parameter, false);
        const AnimParameter* p = m_Graph.FindParameter(c.parameter);
        ImGui::SameLine();
        if (p && p->type == AnimParamType::Trigger) {
            c.op = AnimCompare::Triggered;
            ImGui::TextDisabled("triggered");
        } else if (p && p->type == AnimParamType::Bool) {
            if (c.op != AnimCompare::IsTrue && c.op != AnimCompare::IsFalse)
                c.op = AnimCompare::IsTrue;
            ImGui::SetNextItemWidth(70.0f);
            EnumCombo("##op", c.op, boolean, AnimCompareName);
        } else {
            if (std::ranges::find(numeric, c.op) == numeric.end())
                c.op = AnimCompare::Greater;
            ImGui::SetNextItemWidth(50.0f);
            EnumCombo("##op", c.op, numeric, AnimCompareName);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            ImGui::DragFloat("##value", &c.value, 0.01f);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0)
        transition.conditions.erase(transition.conditions.begin() + remove);
    if (ImGui::SmallButton("+ Condition"))
        transition.conditions.push_back(AnimCondition{.parameter = m_Graph.parameters.empty() ? std::string() : m_Graph.parameters.front().name});
    ImGui::Spacing();
    if (ImGui::Button("Delete transition") && m_Layer < m_Graph.layers.size()) {
        auto& list = m_Graph.layers[m_Layer].transitions;
        list.erase(list.begin() + m_Transition);
        m_Transition = -1;
    }
}

void AnimGraphEditor::DrawPreview(float dt)
{
    const ImVec2 avail  = ImGui::GetContentRegionAvail();
    const auto   width  = static_cast<std::uint32_t>(std::max(avail.x, 16.0f));
    const auto   height = static_cast<std::uint32_t>(std::max(avail.y - ImGui::GetTextLineHeightWithSpacing(), 16.0f));
    const Model* model  = PreviewModel();
    if (m_Scene && m_Preview) {
        // The preview instance appears once the model is ready.
        if (model && m_PreviewRoot == NullEntity) {
            m_PreviewRoot = InstantiateModel(*m_Scene, m_Model, *model);
            m_Scene->GetRegistry().Emplace<AnimationController>(m_PreviewRoot, AnimationController{.graph = PathToUtf8(m_File)});
            m_Problems = ValidateAnimGraph(m_Graph, model); // clips / bones can be checked now
        }
        if (model && !m_Framed) {
            const glm::vec3 lo = model->boundsMin, hi = model->boundsMax;
            const float     extent = std::max(glm::length(hi - lo), 0.01f);
            m_Target   = (lo + hi) * 0.5f;
            m_Distance = extent * 1.2f;
            Registry& r = m_Scene->GetRegistry();
            r.ViewOf<Name>().Each([&](Entity e, Name& n) {
                if (n.value == "Ground") {
                    Transform& t = m_Scene->EditTransform(e);
                    t.position   = {m_Target.x, lo.y, m_Target.z};
                    t.scale      = glm::vec3(extent * 3.0f);
                }
            });
            m_Framed = true;
        }
        if (m_PreviewRoot != NullEntity) {
            for (const AnimParameter& p : m_Graph.parameters)
                if (p.type != AnimParamType::Trigger)
                    m_Preview->SetFloat(m_PreviewRoot, p.name, m_PreviewValues[p.name]);
            for (const AnimIk& goal : m_Graph.ik)
                if (m_IkTargets.contains(goal.name + "#on"))
                    m_Preview->SetIkTarget(m_PreviewRoot, goal.name, m_IkTargets[goal.name]);
        }
        m_Scene->UpdateTransforms();
        m_Preview->Update(*m_Scene, m_Paused ? 0.0f : dt * m_Speed);
        m_Scene->UpdateTransforms();
    }
    EnsureTarget(width, height);
    ImGui::Image(ImTextureRef(m_Texture), ImVec2(static_cast<float>(width), static_cast<float>(height)));
    m_PreviewVisible = true;
    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            m_Yaw -= io.MouseDelta.x * 0.01f;
            m_Pitch = std::clamp(m_Pitch + io.MouseDelta.y * 0.01f, -1.4f, 1.4f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
            m_Target.y += io.MouseDelta.y * 0.002f * m_Distance;
        if (io.MouseWheel != 0.0f)
            m_Distance = std::clamp(m_Distance * std::pow(0.9f, io.MouseWheel), 0.01f, 10000.0f);
    }
    if (!model)
        ImGui::TextDisabled(m_Graph.previewModel.empty() ? "Choose a preview model (Graph section)" : "Loading the preview model...");
    else if (const auto info = m_Preview->Inspect(m_PreviewRoot); info && !info->layers.empty())
        ImGui::Text("%s  %.2f%s   (LMB orbit, RMB height, wheel zoom)", info->layers[std::min(m_Layer, info->layers.size() - 1)].state.c_str(),
                    info->layers[std::min(m_Layer, info->layers.size() - 1)].normalizedTime,
                    info->layers[std::min(m_Layer, info->layers.size() - 1)].transition >= 0.0f ? "  (fading)" : "");
    else
        ImGui::TextDisabled("Not running (see Problems)");
}

void AnimGraphEditor::EnsureTarget(std::uint32_t width, std::uint32_t height)
{
    if (m_Image && m_Image.Extent().width == width && m_Image.Extent().height == height)
        return;
    ReleaseTarget();
    m_Image   = Image(m_Ctx.renderer.GetContext(), {.extent    = {width, height, 1},
                                                    .format    = kPreviewFormat,
                                                    .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                    .debugName = "AnimGraphPreview"});
    m_Texture = m_Ctx.imgui.AddTexture(m_Image.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void AnimGraphEditor::Render(const FrameContext& frame)
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
    FlyCamera camera;
    camera.position = m_Target + m_Distance * glm::vec3(std::cos(m_Pitch) * std::sin(m_Yaw), std::sin(m_Pitch),
                                                        std::cos(m_Pitch) * std::cos(m_Yaw));
    camera.nearPlane = std::max(m_Distance * 0.01f, 0.001f);
    camera.LookAt(m_Target);
    const VkExtent2D extent = m_Image.Extent2D();
    m_Renderer->shadows.maxDistance = m_Distance * 4.0f;
    m_Renderer->Render(frame, *m_Scene, camera.GetData(static_cast<float>(extent.width) / static_cast<float>(extent.height)),
                       {.image = m_Image.Handle(), .view = m_Image.View(), .format = kPreviewFormat, .extent = extent});
    CmdImageBarrier(cmd, {.image     = m_Image.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
}

} // namespace Engine
