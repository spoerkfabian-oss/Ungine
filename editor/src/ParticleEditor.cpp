#include "Editor/ParticleEditor.h"
#include "Editor/FileDialog.h"
#include "Editor/ImGuiLayer.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Core/Platform.h"
#include "Engine/Particles/ParticleFields.h"
#include "Engine/Particles/ParticleWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>

namespace Engine {

namespace {

constexpr VkFormat    kPreviewFormat  = VK_FORMAT_R8G8B8A8_SRGB; // sampled by ImGui, like the viewport
constexpr const char* kContentPayload = "UNGINE_CONTENT";
constexpr std::size_t kMaxUndo        = 100;

// "maxSpeed" -> "Max Speed"
std::string Label(const char* name)
{
    std::string out;
    for (const char* c = name; *c; ++c) {
        if (c == name)
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        else {
            if (std::isupper(static_cast<unsigned char>(*c)))
                out += ' ';
            out += *c;
        }
    }
    return out;
}

template <class E, std::size_t N>
bool EnumCombo(const char* label, E& value, const std::array<E, N>& values)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, ToString(value))) {
        for (E v : values)
            if (ImGui::Selectable(ToString(v), v == value)) {
                value   = v;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

// Curve: plot (drag keys, double-click adds, right-click removes) + interpolation.
bool CurveWidget(const char* label, ParticleCurve& curve)
{
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 120.0f), 70.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("plot", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(30, 32, 38, 255));
    float lo = 0.0f, hi = 1.0f;
    for (const auto& k : curve.keys) {
        lo = std::min(lo, k.value);
        hi = std::max(hi, k.value);
    }
    if (hi - lo < 1e-3f)
        hi = lo + 1.0f;
    const float pad = (hi - lo) * 0.08f; // keys on the border stay visible
    lo -= pad;
    hi += pad;
    const auto toScreen = [&](float t, float v) {
        return ImVec2(origin.x + t * size.x, origin.y + size.y - (v - lo) / (hi - lo) * size.y);
    };
    const auto fromScreen = [&](ImVec2 p) {
        return glm::vec2(std::clamp((p.x - origin.x) / size.x, 0.0f, 1.0f), lo + (origin.y + size.y - p.y) / size.y * (hi - lo));
    };
    ImVec2 last = toScreen(0.0f, curve.Evaluate(0.0f));
    for (int i = 1; i <= 64; ++i) {
        const float  t    = static_cast<float>(i) / 64.0f;
        const ImVec2 next = toScreen(t, curve.Evaluate(t));
        draw->AddLine(last, next, IM_COL32(120, 200, 255, 255), 1.5f);
        last = next;
    }
    // Key under the mouse.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int          hot   = -1;
    for (std::size_t i = 0; i < curve.keys.size(); ++i) {
        const ImVec2 p = toScreen(curve.keys[i].time, curve.keys[i].value);
        if (std::abs(p.x - mouse.x) < 6.0f && std::abs(p.y - mouse.y) < 6.0f)
            hot = static_cast<int>(i);
        draw->AddCircleFilled(p, 4.0f, IM_COL32(255, 200, 80, 255));
    }
    static int dragging = -1;
    static ImGuiID dragOwner = 0;
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::IsItemActivated() && hot >= 0) {
        dragging  = hot;
        dragOwner = id;
    }
    if (dragging >= 0 && dragOwner == id) {
        if (ImGui::IsItemActive() && dragging < static_cast<int>(curve.keys.size())) {
            const glm::vec2 v = fromScreen(mouse);
            auto&           k = curve.keys[static_cast<std::size_t>(dragging)];
            k.time            = v.x;
            k.value           = v.y;
            changed           = true;
        } else {
            dragging = -1;
            std::ranges::stable_sort(curve.keys, {}, &ParticleCurve::Key::time);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot < 0) {
        const glm::vec2 v = fromScreen(mouse);
        curve.keys.push_back({v.x, curve.keys.empty() ? 1.0f : curve.Evaluate(v.x)});
        std::ranges::stable_sort(curve.keys, {}, &ParticleCurve::Key::time);
        changed = true;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hot >= 0) {
        curve.keys.erase(curve.keys.begin() + hot);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drag keys, double-click adds one, right-click removes it\n%.2f .. %.2f", lo, hi);
    int interp = static_cast<int>(curve.interp);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("##interp", &interp, "Linear\0Smooth\0Constant\0")) {
        curve.interp = static_cast<ParticleCurve::Interp>(interp);
        changed      = true;
    }
    ImGui::SameLine();
    if (ImGui::TreeNode("Keys")) {
        for (std::size_t i = 0; i < curve.keys.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(90.0f);
            changed |= ImGui::DragFloat("##t", &curve.keys[i].time, 0.005f, 0.0f, 1.0f, "t %.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            changed |= ImGui::DragFloat("##v", &curve.keys[i].value, 0.01f);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                curve.keys.erase(curve.keys.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Key")) {
            curve.keys.push_back({1.0f, curve.Evaluate(1.0f)});
            changed = true;
        }
        if (changed)
            std::ranges::stable_sort(curve.keys, {}, &ParticleCurve::Key::time);
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

bool GradientWidget(const char* label, ParticleGradient& gradient)
{
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 120.0f), 18.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Checkerboard under it (alpha), then the gradient in 48 steps.
    for (int i = 0; i < 24; ++i)
        draw->AddRectFilled(ImVec2(origin.x + size.x * static_cast<float>(i) / 24.0f, origin.y),
                            ImVec2(origin.x + size.x * static_cast<float>(i + 1) / 24.0f, origin.y + size.y),
                            (i % 2) ? IM_COL32(90, 90, 90, 255) : IM_COL32(150, 150, 150, 255));
    for (int i = 0; i < 48; ++i) {
        const float     t0 = static_cast<float>(i) / 48.0f, t1 = static_cast<float>(i + 1) / 48.0f;
        const glm::vec4 a = glm::clamp(gradient.Evaluate(t0), 0.0f, 1.0f);
        const glm::vec4 b = glm::clamp(gradient.Evaluate(t1), 0.0f, 1.0f);
        const auto      col = [](const glm::vec4& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); };
        draw->AddRectFilledMultiColor(ImVec2(origin.x + size.x * t0, origin.y), ImVec2(origin.x + size.x * t1, origin.y + size.y),
                                      col(a), col(b), col(b), col(a));
    }
    for (std::size_t i = 0; i < gradient.keys.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(80.0f);
        changed |= ImGui::DragFloat("##t", &gradient.keys[i].time, 0.005f, 0.0f, 1.0f, "t %.2f");
        ImGui::SameLine();
        changed |= ImGui::ColorEdit4("##c", &gradient.keys[i].color.x,
                                     ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs |
                                         ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            gradient.keys.erase(gradient.keys.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Key")) {
        gradient.keys.push_back({1.0f, gradient.Evaluate(1.0f)});
        changed = true;
    }
    if (changed)
        std::ranges::stable_sort(gradient.keys, {}, &ParticleGradient::Key::time);
    ImGui::PopID();
    return changed;
}

// Generic widgets for the reflected module fields.
struct FieldUi {
    const ParticleEffect& effect;
    bool                  changed = false;

    void operator()(const char* name, float& v, const ParticleFieldHint& h)
    {
        const std::string label = Label(name);
        const float lo = std::isfinite(h.min) ? h.min : -FLT_MAX, hi = std::isfinite(h.max) ? h.max : FLT_MAX;
        changed |= ImGui::DragFloat(label.c_str(), &v, h.speed, lo, hi, h.degrees ? "%.1f deg" : "%.3f", ImGuiSliderFlags_AlwaysClamp);
    }
    void operator()(const char* name, std::uint32_t& v, const ParticleFieldHint& h)
    {
        const std::string   label = Label(name);
        const std::uint32_t lo = 0, hi = std::isfinite(h.max) ? static_cast<std::uint32_t>(h.max) : 1000000u;
        changed |= ImGui::DragScalar(label.c_str(), ImGuiDataType_U32, &v, std::max(h.speed, 0.2f), &lo, &hi);
    }
    void operator()(const char* name, std::uint16_t& v, const ParticleFieldHint&)
    {
        const std::string label = Label(name);
        changed |= ImGui::InputScalar(label.c_str(), ImGuiDataType_U16, &v, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal);
    }
    void operator()(const char* name, bool& v, const ParticleFieldHint&) { changed |= ImGui::Checkbox(Label(name).c_str(), &v); }
    void operator()(const char* name, glm::vec3& v, const ParticleFieldHint& h)
    {
        const float lo = std::isfinite(h.min) ? h.min : -FLT_MAX, hi = std::isfinite(h.max) ? h.max : FLT_MAX;
        changed |= ImGui::DragFloat3(Label(name).c_str(), &v.x, std::max(h.speed, 0.01f), lo, hi);
    }
    void operator()(const char* name, glm::vec4& v, const ParticleFieldHint&)
    {
        changed |= ImGui::ColorEdit4(Label(name).c_str(), &v.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float |
                                                                    ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    }
    void operator()(const char* name, FloatRange& v, const ParticleFieldHint& h)
    {
        const float lo = std::isfinite(h.min) ? h.min : -FLT_MAX, hi = std::isfinite(h.max) ? h.max : FLT_MAX;
        if (ImGui::DragFloatRange2(Label(name).c_str(), &v.min, &v.max, std::max(h.speed, 0.01f), lo, hi, "min %.2f", "max %.2f",
                                   ImGuiSliderFlags_AlwaysClamp)) {
            v.max   = std::max(v.max, v.min);
            changed = true;
        }
    }
    void operator()(const char* name, ParticleCurve& curve, const ParticleFieldHint&) { changed |= CurveWidget(Label(name).c_str(), curve); }
    void operator()(const char* name, ParticleGradient& g, const ParticleFieldHint&) { changed |= GradientWidget(Label(name).c_str(), g); }
    void operator()(const char* name, ParticleShape& v, const ParticleFieldHint&)
    {
        constexpr std::array shapes{ParticleShape::Point, ParticleShape::Sphere, ParticleShape::Hemisphere,
                                    ParticleShape::Box,   ParticleShape::Cone,   ParticleShape::Circle};
        changed |= EnumCombo(Label(name).c_str(), v, shapes);
    }
    void operator()(const char* name, ParticleEvent& v, const ParticleFieldHint&)
    {
        constexpr std::array events{ParticleEvent::Spawn, ParticleEvent::Death, ParticleEvent::Collision, ParticleEvent::Trail};
        changed |= EnumCombo(Label(name).c_str(), v, events);
    }
    void operator()(const char* name, std::string& v, const ParticleFieldHint& h)
    {
        if (!h.emitter)
            return;
        if (ImGui::BeginCombo(Label(name).c_str(), v.empty() ? "(none)" : v.c_str())) {
            for (const ParticleEmitter& e : effect.emitters)
                if (ImGui::Selectable(e.name.c_str(), e.name == v)) {
                    v       = e.name;
                    changed = true;
                }
            ImGui::EndCombo();
        }
    }
};

} // namespace

ParticleEditor::ParticleEditor(const Context& context) : m_Ctx(context), m_Dialog(std::make_unique<FileDialog>()) {}

ParticleEditor::~ParticleEditor()
{
    Close();
    ReleaseTarget();
}

void ParticleEditor::ReleaseTarget()
{
    if (m_Image) {
        m_Ctx.imgui.RemoveTexture(m_Texture);
        m_Ctx.renderer.DeferRelease(std::move(m_Image));
        m_Texture = 0;
    }
}

void ParticleEditor::Open(const std::filesystem::path& file)
{
    if (m_Open)
        Close();
    m_File = std::filesystem::absolute(file).lexically_normal();
    std::string error;
    if (auto loaded = LoadParticleEffect(m_File, &error)) {
        m_Effect = std::move(*loaded);
        m_Status.clear();
    } else {
        m_Effect = MakeDefaultParticleEffect();
        m_Status = std::filesystem::exists(m_File) ? error : "New effect (not saved yet)";
    }
    m_Saved     = std::filesystem::exists(m_File) ? m_Effect : ParticleEffect{};
    m_Committed = m_Effect;
    m_Undo.clear();
    m_Redo.clear();
    m_Applied.reset();
    m_Emitter = 0;
    m_Open    = true;
    // Preview scene: the effect at the origin above an optional ground plane.
    m_Scene   = std::make_unique<Scene>();
    m_Preview = std::make_unique<ParticleWorld>(nullptr, &m_Ctx.assets, nullptr);
    if (!m_Renderer) {
        m_Renderer = std::make_unique<SceneRenderer>(m_Ctx.renderer, m_Ctx.renderer.GetContext(), m_Ctx.assets);
        m_Renderer->shadows.resolution   = 1024;
        m_Renderer->shadows.cascadeCount = 2;
        m_Renderer->shadows.maxDistance  = 25.0f;
        m_Renderer->localShadows.enabled = false;
        m_Renderer->culling.occlusion    = false;
    }
    m_Renderer->particles = m_Preview.get();
    const Entity effect   = m_Scene->CreateEntity("Effect");
    m_Scene->GetRegistry().Emplace<ParticleSystem>(effect, ParticleSystem{.effect = PathToUtf8(m_File)});
    if (!m_Ground)
        m_Ground = m_Ctx.assets.CreatePrimitive({.shape = PrimitiveShape::Plane, .size = 12.0f, .baseColor = {0.35f, 0.36f, 0.38f, 1.0f}});
    const Entity ground = m_Scene->CreateEntity("Ground");
    m_Scene->GetRegistry().Emplace<MeshRenderer>(ground, MeshRenderer{.model = m_Ground});
    Apply();
}

void ParticleEditor::Close()
{
    if (!m_Open)
        return;
    if (m_Ctx.sceneParticles)
        m_Ctx.sceneParticles->Provide(PathToUtf8(m_File), nullptr); // back to the file
    m_Open = false;
    m_Renderer.reset(); // GPU resources: deferred until the frames in flight are done
    ReleaseTarget();
    m_Preview.reset();
    m_Scene.reset();
    if (m_Ground) {
        m_Ctx.assets.Release(m_Ground);
        m_Ground = {};
    }
}

bool ParticleEditor::Dirty() const { return m_Open && m_Effect != m_Saved; }

bool ParticleEditor::Save(std::string* error)
{
    std::string why;
    if (!SaveParticleEffect(m_File, m_Effect, &why)) {
        m_Status = why;
        if (error)
            *error = why;
        return false;
    }
    m_Saved  = m_Effect;
    m_Status = "Saved";
    return true;
}

void ParticleEditor::Revert()
{
    m_Effect = m_Saved.emitters.empty() ? MakeDefaultParticleEffect() : m_Saved;
    Commit();
}

void ParticleEditor::Commit()
{
    if (m_Effect == m_Committed)
        return;
    m_Undo.push_back(m_Committed);
    if (m_Undo.size() > kMaxUndo)
        m_Undo.erase(m_Undo.begin());
    m_Redo.clear();
    m_Committed = m_Effect;
    Apply();
}

bool ParticleEditor::Undo()
{
    if (m_Undo.empty())
        return false;
    m_Redo.push_back(m_Committed);
    m_Effect    = m_Undo.back();
    m_Committed = m_Effect;
    m_Undo.pop_back();
    Apply();
    return true;
}

bool ParticleEditor::Redo()
{
    if (m_Redo.empty())
        return false;
    m_Undo.push_back(m_Committed);
    m_Effect    = m_Redo.back();
    m_Committed = m_Effect;
    m_Redo.pop_back();
    Apply();
    return true;
}

void ParticleEditor::Apply()
{
    if (!m_Open || (m_Applied && *m_Applied == m_Effect))
        return;
    m_Applied      = m_Effect;
    auto shared    = std::make_shared<const ParticleEffect>(m_Effect);
    const auto key = PathToUtf8(m_File);
    if (m_Preview)
        m_Preview->Provide(key, shared);
    if (m_Ctx.sceneParticles)
        m_Ctx.sceneParticles->Provide(key, shared);
    m_Problems = ValidateParticleEffect(m_Effect);
    m_Emitter  = std::min(m_Emitter, m_Effect.emitters.empty() ? std::size_t{0} : m_Effect.emitters.size() - 1);
}

bool ParticleEditor::PathField(const char* label, std::string& path, const std::vector<std::string>& extensions, int purpose)
{
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::SmallButton("..."))
    {
        m_DialogPurpose = purpose;
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
            std::string ext = PathToUtf8(dropped.extension());
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std::ranges::find(extensions, ext) != extensions.end()) {
                path    = PathToUtf8(dropped.lexically_normal());
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    return changed;
}

void ParticleEditor::Draw(float dt)
{
    m_PreviewVisible = false;
    if (!m_Open)
        return;
    ImGui::SetNextWindowSize(ImVec2(980.0f, 640.0f), ImGuiCond_FirstUseEver);
    const std::string title = "Particle Effect - " + PathToUtf8(m_File.filename()) + (Dirty() ? " *" : "") + "###particleeditor";
    bool open = true;
    if (!ImGui::Begin(title.c_str(), &open)) {
        ImGui::End();
        if (!open)
            Close();
        return;
    }
    DrawToolbar();
    const float  previewWidth = std::max(ImGui::GetContentRegionAvail().x * 0.45f, 200.0f);
    ImGui::BeginChild("stack", ImVec2(ImGui::GetContentRegionAvail().x - previewWidth - 8.0f, 0.0f), ImGuiChildFlags_Borders);
    DrawEmitters();
    if (m_Emitter < m_Effect.emitters.size())
        DrawEmitter(m_Effect.emitters[m_Emitter]);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("preview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    DrawPreview(dt);
    ImGui::EndChild();
    // Edits are committed (undo + live update) once no widget is active anymore; while dragging
    // the preview follows live.
    if (!ImGui::IsAnyItemActive())
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
    if (const auto chosen = m_Dialog->Draw(); chosen && m_Emitter < m_Effect.emitters.size()) {
        ParticleRender& r = m_Effect.emitters[m_Emitter].render;
        (m_DialogPurpose == 2 ? r.mesh : r.texture) = PathToUtf8(chosen->lexically_normal());
        Commit();
    }
    if (!open)
        Close();
}

void ParticleEditor::DrawToolbar()
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
    if (ImGui::Button("Restart") && m_Preview)
        m_Preview->Reset();
    ImGui::SameLine();
    ImGui::Checkbox("Pause", &m_Paused);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("Speed", &m_Speed, 0.0f, 3.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("Ground", &m_ShowGround);
    if (!m_Status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_Status.c_str());
    }
}

void ParticleEditor::DrawEmitters()
{
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Warmup (s)", &m_Effect.warmup, 0.05f, 0.0f, 30.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SeparatorText("Emitters");
    std::optional<std::size_t> remove, duplicate, up, down;
    for (std::size_t i = 0; i < m_Effect.emitters.size(); ++i) {
        ParticleEmitter& e = m_Effect.emitters[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##on", &e.enabled);
        ImGui::SameLine();
        const std::string label = e.name + "  (" + ToString(e.render.type) + ")";
        if (ImGui::Selectable(label.c_str(), m_Emitter == i, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(ImGui::GetContentRegionAvail().x - 100.0f, 0.0f)))
            m_Emitter = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("^"))
            up = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("v"))
            down = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("+"))
            duplicate = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = i;
        ImGui::PopID();
    }
    const auto unique = [&](std::string base) {
        std::string name = base;
        for (int n = 2; m_Effect.Find(name); ++n)
            name = base + " " + std::to_string(n);
        return name;
    };
    if (ImGui::Button("+ Emitter")) {
        m_Effect.emitters.push_back(MakeDefaultParticleEmitter(unique("Emitter")));
        m_Emitter = m_Effect.emitters.size() - 1;
    }
    if (remove) {
        m_Effect.emitters.erase(m_Effect.emitters.begin() + static_cast<std::ptrdiff_t>(*remove));
        m_Emitter = std::min(m_Emitter, m_Effect.emitters.empty() ? 0 : m_Effect.emitters.size() - 1);
    } else if (duplicate) {
        ParticleEmitter copy = m_Effect.emitters[*duplicate];
        copy.name            = unique(copy.name);
        m_Effect.emitters.insert(m_Effect.emitters.begin() + static_cast<std::ptrdiff_t>(*duplicate) + 1, std::move(copy));
        m_Emitter = *duplicate + 1;
    } else if (up && *up > 0) {
        std::swap(m_Effect.emitters[*up], m_Effect.emitters[*up - 1]);
        m_Emitter = *up - 1;
    } else if (down && *down + 1 < m_Effect.emitters.size()) {
        std::swap(m_Effect.emitters[*down], m_Effect.emitters[*down + 1]);
        m_Emitter = *down + 1;
    }
    for (const std::string& problem : m_Problems)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "! %s", problem.c_str());
}

void ParticleEditor::DrawEmitter(ParticleEmitter& emitter)
{
    ImGui::SeparatorText("Emitter");
    char name[128];
    std::snprintf(name, sizeof(name), "%s", emitter.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] &&
        !m_Effect.Find(name)) {
        // Sub emitters follow the rename.
        for (ParticleEmitter& other : m_Effect.emitters)
            for (ParticleModule& m : other.modules)
                if (auto* sub = std::get_if<SubEmitterModule>(&m.params); sub && sub->emitter == emitter.name)
                    sub->emitter = name;
        emitter.name = name;
    }
    std::uint32_t maxParticles = emitter.maxParticles;
    const std::uint32_t lo = 1, hi = kMaxParticlesPerEmitter;
    if (ImGui::DragScalar("Max particles", ImGuiDataType_U32, &maxParticles, 5.0f, &lo, &hi))
        emitter.maxParticles = std::clamp(maxParticles, lo, hi);
    ImGui::Checkbox("Local space", &emitter.localSpace);
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &emitter.loop);
    ImGui::DragFloat("Duration (s, 0 = forever)", &emitter.duration, 0.05f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Delay (s)", &emitter.delay, 0.05f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    for (int stage = 0; stage < static_cast<int>(ParticleStage::Count); ++stage)
        DrawStage(emitter, static_cast<ParticleStage>(stage));
    DrawRender(emitter);
}

void ParticleEditor::DrawStage(ParticleEmitter& emitter, ParticleStage stage)
{
    ImGui::PushID(static_cast<int>(stage));
    const bool open = ImGui::CollapsingHeader(ToString(stage), ImGuiTreeNodeFlags_DefaultOpen);
    // Add a module of this stage.
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.0f);
    if (ImGui::SmallButton("+"))
        ImGui::OpenPopup("add");
    if (ImGui::BeginPopup("add")) {
        for (std::size_t t = 0; t < static_cast<std::size_t>(ParticleModuleType::Count); ++t) {
            const ParticleModuleInfo& info = ModuleInfo(static_cast<ParticleModuleType>(t));
            if (info.stage != stage)
                continue;
            if (ImGui::Selectable(info.label))
                emitter.modules.push_back(MakeParticleModule(static_cast<ParticleModuleType>(t)));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", info.description);
        }
        ImGui::EndPopup();
    }
    if (open) {
        std::optional<std::size_t> remove, up, down;
        for (std::size_t i = 0; i < emitter.modules.size(); ++i) {
            ParticleModule&           m    = emitter.modules[i];
            const ParticleModuleInfo& info = ModuleInfo(m.Type());
            if (info.stage != stage)
                continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox("##on", &m.enabled);
            ImGui::SameLine();
            const bool expanded = ImGui::TreeNodeEx(info.label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", info.description);
            ImGui::SameLine();
            if (ImGui::SmallButton("^"))
                up = i;
            ImGui::SameLine();
            if (ImGui::SmallButton("v"))
                down = i;
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = i;
            if (expanded) {
                ImGui::BeginDisabled(!m.enabled);
                FieldUi ui{m_Effect};
                std::visit([&](auto& params) { VisitParticleFields(params, ui); }, m.params);
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // Moves swap with the neighbor of the same stage.
        const auto neighbor = [&](std::size_t from, int dir) -> std::optional<std::size_t> {
            for (std::ptrdiff_t j = static_cast<std::ptrdiff_t>(from) + dir; j >= 0 && j < static_cast<std::ptrdiff_t>(emitter.modules.size()); j += dir)
                if (ModuleInfo(emitter.modules[static_cast<std::size_t>(j)].Type()).stage == stage)
                    return static_cast<std::size_t>(j);
            return std::nullopt;
        };
        if (remove)
            emitter.modules.erase(emitter.modules.begin() + static_cast<std::ptrdiff_t>(*remove));
        else if (up) {
            if (const auto j = neighbor(*up, -1))
                std::swap(emitter.modules[*up], emitter.modules[*j]);
        } else if (down) {
            if (const auto j = neighbor(*down, 1))
                std::swap(emitter.modules[*down], emitter.modules[*j]);
        }
    }
    ImGui::PopID();
}

void ParticleEditor::DrawRender(ParticleEmitter& emitter)
{
    if (!ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ParticleRender& r = emitter.render;
    constexpr std::array types{ParticleRenderType::Sprite, ParticleRenderType::Mesh, ParticleRenderType::Ribbon, ParticleRenderType::None};
    EnumCombo("Type", r.type, types);
    constexpr std::array blends{ParticleBlend::Alpha, ParticleBlend::Additive};
    EnumCombo("Blend", r.blend, blends);
    if (r.type == ParticleRenderType::None)
        return;
    if (r.type != ParticleRenderType::Mesh) {
        PathField("Texture", r.texture, {".png", ".jpg", ".jpeg", ".ktx2"}, 1);
        if (r.texture.empty())
            ImGui::TextDisabled("No texture: a soft round sprite");
    }
    if (r.type == ParticleRenderType::Sprite) {
        int grid[2] = {static_cast<int>(r.columns), static_cast<int>(r.rows)};
        if (ImGui::DragInt2("Sheet columns / rows", grid, 0.1f, 1, 64)) {
            r.columns = static_cast<std::uint32_t>(std::clamp(grid[0], 1, 64));
            r.rows    = static_cast<std::uint32_t>(std::clamp(grid[1], 1, 64));
        }
        constexpr std::array facings{ParticleFacing::Camera, ParticleFacing::Velocity, ParticleFacing::Axis};
        EnumCombo("Facing", r.facing, facings);
        if (r.facing == ParticleFacing::Axis)
            ImGui::DragFloat3("Axis (normal)", &r.axis.x, 0.01f, -1.0f, 1.0f);
        if (r.facing == ParticleFacing::Velocity)
            ImGui::DragFloat("Stretch (per m/s)", &r.stretch, 0.005f, 0.0f, 10.0f);
        ImGui::DragFloat("Soft edge (m)", &r.softness, 0.01f, 0.0f, 10.0f);
        ImGui::Checkbox("Sort back to front", &r.sort);
    }
    if (r.type == ParticleRenderType::Mesh) {
        PathField("Mesh", r.mesh, {".glb", ".gltf"}, 2);
        int meshIndex = static_cast<int>(r.meshIndex);
        if (ImGui::DragInt("Mesh index", &meshIndex, 0.1f, 0, 1024))
            r.meshIndex = static_cast<std::uint32_t>(std::max(meshIndex, 0));
        ImGui::Checkbox("Align to velocity", &r.alignToVelocity);
        ImGui::SameLine();
        ImGui::Checkbox("Shade", &r.shade);
        ImGui::Checkbox("Sort back to front", &r.sort);
    }
    if (r.type == ParticleRenderType::Ribbon) {
        constexpr std::array uvs{RibbonUv::Stretch, RibbonUv::Tile};
        EnumCombo("Texture along", r.ribbonUv, uvs);
        if (r.ribbonUv == RibbonUv::Tile)
            ImGui::DragFloat("Tile length (m)", &r.ribbonTile, 0.01f, 0.01f, 100.0f);
        ImGui::TextDisabled("Width = particle size; trails: a Sub Emitter (Trail) feeds it");
    }
    ImGui::DragFloat("Camera fade (m)", &r.cameraFade, 0.01f, 0.0f, 100.0f);
}

void ParticleEditor::DrawPreview(float dt)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const auto   width = static_cast<std::uint32_t>(std::max(avail.x, 16.0f));
    const auto   height = static_cast<std::uint32_t>(std::max(avail.y - ImGui::GetTextLineHeightWithSpacing(), 16.0f));
    // Simulate: the preview world plays the effect regardless of the scene.
    if (m_Preview && m_Scene) {
        m_Scene->UpdateTransforms();
        if (!m_Paused)
            m_Preview->Update(*m_Scene, dt * m_Speed);
        else
            m_Preview->Update(*m_Scene, 0.0f);
        // Ground on / off.
        Registry& r = m_Scene->GetRegistry();
        r.ViewOf<Name>().Each([&](Entity e, Name& name) {
            if (name.value != "Ground" || r.Has<MeshRenderer>(e) == m_ShowGround)
                return;
            if (m_ShowGround)
                r.Emplace<MeshRenderer>(e, MeshRenderer{.model = m_Ground});
            else
                r.Remove<MeshRenderer>(e);
            m_Scene->MarkChanged(e);
        });
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
            m_Target.y += io.MouseDelta.y * 0.01f * m_Distance * 0.2f;
        if (io.MouseWheel != 0.0f)
            m_Distance = std::clamp(m_Distance * std::pow(0.9f, io.MouseWheel), 0.5f, 200.0f);
    }
    const ParticleWorldStats& s = m_Preview ? m_Preview->Stats() : ParticleWorldStats{};
    ImGui::Text("%u particles  %u emitters  %.2f ms   (LMB orbit, RMB height, wheel zoom)", s.particles, s.emitters, s.simulateMs);
}

void ParticleEditor::EnsureTarget(std::uint32_t width, std::uint32_t height)
{
    if (m_Image && m_Image.Extent().width == width && m_Image.Extent().height == height)
        return;
    ReleaseTarget();
    m_Image   = Image(m_Ctx.renderer.GetContext(), {.extent    = {width, height, 1},
                                                    .format    = kPreviewFormat,
                                                    .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                    .debugName = "ParticlePreview"});
    m_Texture = m_Ctx.imgui.AddTexture(m_Image.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); // rendered before the UI pass
}

void ParticleEditor::Render(const FrameContext& frame)
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
    camera.LookAt(m_Target);
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
}

} // namespace Engine
