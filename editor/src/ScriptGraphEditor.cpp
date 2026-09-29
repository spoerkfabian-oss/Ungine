#include "Editor/ScriptGraphEditor.h"
#include "FileDialog.h"

#include "Engine/Core/Log.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptSystem.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace Engine {

namespace {

constexpr float kHeader    = 24.0f; // canvas units
constexpr float kRow       = 22.0f;
constexpr float kPad       = 8.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kPinHit    = 9.0f;
constexpr float kMinZoom   = 0.2f;
constexpr float kMaxZoom   = 2.0f;
constexpr std::size_t kMaxUndo = 200;

ImU32 PinColor(PinType type, float alpha = 1.0f)
{
    const auto c = [&](int r, int g, int b) { return IM_COL32(r, g, b, static_cast<int>(alpha * 255.0f)); };
    switch (type) {
    case PinType::Exec: return c(235, 235, 235);
    case PinType::Bool: return c(210, 50, 50);
    case PinType::Int: return c(40, 205, 160);
    case PinType::Float: return c(150, 225, 70);
    case PinType::Vec3: return c(245, 195, 45);
    case PinType::String: return c(235, 90, 210);
    case PinType::Entity: return c(70, 150, 255);
    }
    return c(255, 255, 255);
}

ImU32 HeaderColor(const NodeDesc* desc, const ScriptGraph& graph, const ScriptNode& node)
{
    if (!desc)
        return IM_COL32(120, 30, 30, 255);
    if (desc->kind == NodeKind::Event)
        return IM_COL32(140, 30, 30, 255);
    if (desc->category == "Flow")
        return IM_COL32(75, 75, 80, 255);
    if (desc->category == "Variables") {
        const ScriptVariable* v = graph.FindVariable(node.param);
        const ImVec4          c = ImGui::ColorConvertU32ToFloat4(PinColor(v ? v->type : PinType::Float));
        return ImGui::GetColorU32(ImVec4(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 1.0f));
    }
    if (desc->kind == NodeKind::Pure)
        return IM_COL32(40, 95, 55, 255);
    return IM_COL32(35, 75, 130, 255);
}

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string NodeTitle(const NodeDesc* desc, const ScriptNode& node)
{
    if (!desc)
        return "Unknown: " + node.type;
    switch (desc->paramKind) {
    case ParamKind::Variable: return desc->title + " " + node.param;
    case ParamKind::Key:
    case ParamKind::Choice: return desc->title + " (" + node.param + ")";
    case ParamKind::Text: return desc->title + ": " + node.param;
    default: return desc->title;
    }
}

float WidgetWidth(PinType type)
{
    switch (type) {
    case PinType::Bool: return 20.0f;
    case PinType::Int: return 50.0f;
    case PinType::Float: return 56.0f;
    case PinType::Vec3: return 156.0f;
    case PinType::String: return 96.0f;
    case PinType::Entity: return 30.0f;
    default: return 0.0f;
    }
}

struct NodeLayout {
    const ScriptNode*      node = nullptr;
    const NodeDesc*        desc = nullptr;
    std::string            title;
    glm::vec2              pos{0.0f}, size{0.0f};
    std::vector<PinInfo>   pins;
    std::vector<glm::vec2> pinPos;    // canvas
    std::vector<bool>      connected;
    std::vector<float>     labelWidth;

    [[nodiscard]] bool Contains(glm::vec2 p) const
    {
        return p.x >= pos.x && p.y >= pos.y && p.x <= pos.x + size.x && p.y <= pos.y + size.y;
    }
    [[nodiscard]] int PinIndex(const std::string& name, bool output) const
    {
        for (std::size_t i = 0; i < pins.size(); ++i)
            if (pins[i].output == output && pins[i].name == name)
                return static_cast<int>(i);
        return -1;
    }
};

// Text widths are measured at the window's font size (= canvas units at zoom 1).
NodeLayout Layout(const ScriptGraph& graph, const ScriptNode& node)
{
    NodeLayout l;
    l.node  = &node;
    l.desc  = FindScriptNodeType(node.type);
    l.title = NodeTitle(l.desc, node);
    l.pos   = node.position;
    l.pins  = NodePins(graph, node);
    l.pinPos.resize(l.pins.size());
    l.connected.resize(l.pins.size(), false);
    l.labelWidth.resize(l.pins.size(), 0.0f);
    for (const ScriptLink& link : graph.links) {
        if (link.fromNode == node.id)
            if (const int i = l.PinIndex(link.fromPin, true); i >= 0)
                l.connected[static_cast<std::size_t>(i)] = true;
        if (link.toNode == node.id)
            if (const int i = l.PinIndex(link.toPin, false); i >= 0)
                l.connected[static_cast<std::size_t>(i)] = true;
    }
    float inWidth = 0.0f, outWidth = 0.0f;
    int   inRows = 0, outRows = 0;
    for (std::size_t i = 0; i < l.pins.size(); ++i) {
        const PinInfo& p     = l.pins[i];
        const bool     named = !(p.type == PinType::Exec && (p.name == "In" || p.name == "Then" || p.name == "Out"));
        l.labelWidth[i]      = named ? ImGui::CalcTextSize(p.name.c_str()).x : 0.0f;
        float width          = l.labelWidth[i];
        if (!p.output && p.type != PinType::Exec && !l.connected[i])
            width += WidgetWidth(p.type) + 4.0f;
        if (p.output) {
            outWidth = std::max(outWidth, width);
            ++outRows;
        } else {
            inWidth = std::max(inWidth, width);
            ++inRows;
        }
    }
    const float titleWidth = ImGui::CalcTextSize(l.title.c_str()).x + 2.0f * kPad;
    l.size.x = std::max({titleWidth, inWidth + outWidth + 4.0f * kPad + 16.0f, 110.0f});
    l.size.y = kHeader + static_cast<float>(std::max({inRows, outRows, 1})) * kRow + kPad * 0.5f;
    int in = 0, out = 0;
    for (std::size_t i = 0; i < l.pins.size(); ++i) {
        const int row = l.pins[i].output ? out++ : in++;
        l.pinPos[i]   = {l.pins[i].output ? l.pos.x + l.size.x : l.pos.x, l.pos.y + kHeader + (static_cast<float>(row) + 0.5f) * kRow};
    }
    return l;
}

std::string UniqueVariableName(const ScriptGraph& graph, const std::string& base)
{
    std::string name = base;
    for (int i = 1; graph.FindVariable(name); ++i)
        name = base + std::to_string(i);
    return name;
}

// Value editor shared by inline pins, variables and details. Returns true when changed.
bool ValueWidget(const char* id, ScriptValue& value, PinType type, float width)
{
    if (TypeOf(value) != type)
        value = Convert(value, type);
    ImGui::SetNextItemWidth(width);
    switch (type) {
    case PinType::Bool: return ImGui::Checkbox(id, &std::get<bool>(value));
    case PinType::Int: return ImGui::DragInt(id, &std::get<std::int32_t>(value), 0.2f);
    case PinType::Float: return ImGui::DragFloat(id, &std::get<float>(value), 0.01f, 0.0f, 0.0f, "%.3g");
    case PinType::Vec3: return ImGui::DragFloat3(id, &std::get<glm::vec3>(value).x, 0.01f, 0.0f, 0.0f, "%.3g");
    case PinType::String: return ImGui::InputText(id, &std::get<std::string>(value));
    case PinType::Entity: ImGui::TextDisabled("self"); return false;
    case PinType::Exec: break;
    }
    return false;
}

} // namespace

struct ScriptGraphEditor::Document {
    std::filesystem::path path;
    ScriptGraph           graph;
    std::vector<std::pair<std::string, std::uint64_t>> undo, redo; // graph JSON + revision
    std::uint64_t revision = 0, savedRevision = 0;
    glm::vec2     scroll{0.0f};
    float         zoom   = 1.0f;
    bool          framed = false; // view fitted once
    std::vector<std::uint32_t> selected;
    std::uint32_t              selectedComment = 0;
    std::vector<ScriptDiagnostic> diagnostics;
    std::uint64_t                 diagnosed = ~std::uint64_t{0};
    bool                          selectTab = false;
    glm::vec2                     viewSize{800.0f, 500.0f}; // canvas size last frame (framing)
};

struct ScriptGraphEditor::DragState {
    enum class Kind { None, Pan, Nodes, Comment, Resize, Link, Box } kind = Kind::None;
    int           button = 0;
    glm::vec2     start{0.0f}, last{0.0f}; // canvas
    std::string   before;
    std::uint64_t beforeRevision = 0;
    bool          moved          = false;
    // Link from a pin.
    std::uint32_t node   = 0;
    std::string   pin;
    bool          output = false;
    PinType       type   = PinType::Exec;
    // Comment drag: the nodes inside it move along.
    std::uint32_t              comment = 0;
    std::vector<std::uint32_t> carried;
    // Create menu.
    struct Pending {
        std::uint32_t node;
        std::string   pin;
        bool          output;
        PinType       type;
    };
    std::optional<Pending> pending;
    glm::vec2              menuPos{0.0f};
    std::string            search;
    bool                   openCreate = false, openNode = false, focusSearch = false;
    std::uint32_t          menuNode    = 0;
    std::optional<std::size_t> confirmClose;
};

ScriptGraphEditor::ScriptGraphEditor() : m_Drag(std::make_unique<DragState>()), m_Dialog(std::make_unique<FileDialog>()) {}
ScriptGraphEditor::~ScriptGraphEditor() = default;

// --- Documents ----------------------------------------------------------------------------------

ScriptGraphEditor::Document* ScriptGraphEditor::ActiveDoc()
{
    return m_Active < m_Docs.size() ? m_Docs[m_Active].get() : nullptr;
}
const ScriptGraphEditor::Document* ScriptGraphEditor::ActiveDoc() const
{
    return m_Active < m_Docs.size() ? m_Docs[m_Active].get() : nullptr;
}

bool ScriptGraphEditor::Open(const std::filesystem::path& file)
{
    const std::string key = ScriptSystem::Key(file);
    for (std::size_t i = 0; i < m_Docs.size(); ++i)
        if (ScriptSystem::Key(m_Docs[i]->path) == key) {
            Activate(i);
            return true;
        }
    auto doc = std::make_unique<Document>();
    try {
        doc->graph = LoadScriptGraph(file);
    } catch (const std::exception& e) {
        m_LastError = e.what();
        ENGINE_ERROR("Blueprint: {}", m_LastError);
        return false;
    }
    std::error_code ec;
    doc->path     = std::filesystem::absolute(file, ec).lexically_normal();
    doc->revision = doc->savedRevision = ++m_RevisionCounter;
    m_Docs.push_back(std::move(doc));
    Activate(m_Docs.size() - 1);
    return true;
}

bool ScriptGraphEditor::New(const std::filesystem::path& file)
{
    ScriptGraph graph;
    const std::uint32_t begin = graph.AddNode("Event.BeginPlay", {0.0f, 0.0f});
    const std::uint32_t tick  = graph.AddNode("Event.Tick", {0.0f, 140.0f});
    const std::uint32_t print = graph.AddNode("Debug.Print", {260.0f, 0.0f});
    graph.FindNode(print)->defaults["Text"] = std::string("Hello from ") + file.stem().string();
    graph.Connect(begin, "Out", print, "In");
    (void)tick;
    try {
        SaveScriptGraph(file, graph);
    } catch (const std::exception& e) {
        m_LastError = e.what();
        ENGINE_ERROR("Blueprint: {}", m_LastError);
        return false;
    }
    return Open(file);
}

bool ScriptGraphEditor::Save()
{
    Document* doc = ActiveDoc();
    if (!doc)
        return false;
    try {
        SaveScriptGraph(doc->path, doc->graph);
    } catch (const std::exception& e) {
        m_LastError = e.what();
        ENGINE_ERROR("Blueprint: {}", m_LastError);
        return false;
    }
    doc->savedRevision = doc->revision;
    return true;
}

bool ScriptGraphEditor::SaveAs(const std::filesystem::path& file)
{
    Document* doc = ActiveDoc();
    if (!doc)
        return false;
    std::error_code ec;
    doc->path = std::filesystem::absolute(file, ec).lexically_normal();
    return Save();
}

bool ScriptGraphEditor::SaveAll()
{
    bool              ok     = true;
    const std::size_t active = m_Active;
    for (std::size_t i = 0; i < m_Docs.size(); ++i)
        if (m_Docs[i]->revision != m_Docs[i]->savedRevision) {
            m_Active = i;
            ok       = Save() && ok;
        }
    m_Active = active;
    return ok;
}

void ScriptGraphEditor::Close(std::size_t index)
{
    if (index >= m_Docs.size())
        return;
    m_Docs.erase(m_Docs.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_Active >= m_Docs.size())
        m_Active = m_Docs.empty() ? 0 : m_Docs.size() - 1;
    m_WidgetSession.reset();
    *m_Drag = DragState{};
}

std::size_t                  ScriptGraphEditor::Count() const { return m_Docs.size(); }
std::optional<std::size_t>   ScriptGraphEditor::Active() const { return m_Docs.empty() ? std::nullopt : std::optional(m_Active); }
ScriptGraph*                 ScriptGraphEditor::Graph() { return ActiveDoc() ? &ActiveDoc()->graph : nullptr; }
const std::filesystem::path* ScriptGraphEditor::Path() const { return ActiveDoc() ? &ActiveDoc()->path : nullptr; }
bool ScriptGraphEditor::Dirty() const { return ActiveDoc() && ActiveDoc()->revision != ActiveDoc()->savedRevision; }
bool ScriptGraphEditor::AnyDirty() const
{
    return std::ranges::any_of(m_Docs, [](const auto& d) { return d->revision != d->savedRevision; });
}

void ScriptGraphEditor::Activate(std::size_t index)
{
    if (index >= m_Docs.size())
        return;
    if (index != m_Active) {
        m_WidgetSession.reset();
        *m_Drag = DragState{};
    }
    m_Active                  = index;
    m_Docs[index]->selectTab  = true;
}

// --- Undo ---------------------------------------------------------------------------------------

void ScriptGraphEditor::Changed(Document& doc) { doc.revision = ++m_RevisionCounter; }

void ScriptGraphEditor::PushUndo(Document& doc, std::string before, std::uint64_t revision)
{
    doc.undo.emplace_back(std::move(before), revision);
    if (doc.undo.size() > kMaxUndo)
        doc.undo.erase(doc.undo.begin());
    doc.redo.clear();
}

void ScriptGraphEditor::Edit(const char* /*label*/, const std::function<void(ScriptGraph&)>& change)
{
    Document* doc = ActiveDoc();
    if (!doc)
        return;
    std::string         before   = ScriptGraphToJson(doc->graph);
    const std::uint64_t revision = doc->revision;
    change(doc->graph);
    if (ScriptGraphToJson(doc->graph) == before)
        return; // nothing happened (e.g. a rejected connection)
    PushUndo(*doc, std::move(before), revision);
    Changed(*doc);
}

bool ScriptGraphEditor::Undo()
{
    Document* doc = ActiveDoc();
    if (!doc || doc->undo.empty())
        return false;
    m_WidgetSession.reset();
    doc->redo.emplace_back(ScriptGraphToJson(doc->graph), doc->revision);
    auto [json, revision] = std::move(doc->undo.back());
    doc->undo.pop_back();
    doc->graph    = ScriptGraphFromJson(json);
    doc->revision = revision;
    std::erase_if(doc->selected, [&](std::uint32_t id) { return !doc->graph.FindNode(id); });
    return true;
}

bool ScriptGraphEditor::Redo()
{
    Document* doc = ActiveDoc();
    if (!doc || doc->redo.empty())
        return false;
    m_WidgetSession.reset();
    doc->undo.emplace_back(ScriptGraphToJson(doc->graph), doc->revision);
    auto [json, revision] = std::move(doc->redo.back());
    doc->redo.pop_back();
    doc->graph    = ScriptGraphFromJson(json);
    doc->revision = revision;
    std::erase_if(doc->selected, [&](std::uint32_t id) { return !doc->graph.FindNode(id); });
    return true;
}

bool ScriptGraphEditor::CanUndo() const { return ActiveDoc() && !ActiveDoc()->undo.empty(); }
bool ScriptGraphEditor::CanRedo() const { return ActiveDoc() && !ActiveDoc()->redo.empty(); }

const std::vector<ScriptDiagnostic>& ScriptGraphEditor::Diagnostics()
{
    static const std::vector<ScriptDiagnostic> none;
    Document* doc = ActiveDoc();
    if (!doc)
        return none;
    if (doc->diagnosed != doc->revision) {
        doc->diagnostics = ValidateScriptGraph(doc->graph);
        doc->diagnosed   = doc->revision;
    }
    return doc->diagnostics;
}

// --- Selection operations -----------------------------------------------------------------------

const std::vector<std::uint32_t>& ScriptGraphEditor::SelectedNodes() const
{
    static const std::vector<std::uint32_t> none;
    return ActiveDoc() ? ActiveDoc()->selected : none;
}

void ScriptGraphEditor::Select(std::vector<std::uint32_t> nodes)
{
    if (Document* doc = ActiveDoc()) {
        doc->selected        = std::move(nodes);
        doc->selectedComment = 0;
    }
}

void ScriptGraphEditor::DeleteSelection()
{
    Document* doc = ActiveDoc();
    if (!doc || (doc->selected.empty() && !doc->selectedComment))
        return;
    const std::vector<std::uint32_t> nodes   = doc->selected;
    const std::uint32_t              comment = doc->selectedComment;
    Edit("Delete", [&](ScriptGraph& g) {
        for (std::uint32_t id : nodes)
            g.RemoveNode(id);
        if (comment)
            g.RemoveComment(comment);
    });
    doc->selected.clear();
    doc->selectedComment = 0;
}

std::string ScriptGraphEditor::CopySelection() const
{
    const Document* doc = ActiveDoc();
    if (!doc || doc->selected.empty())
        return {};
    ScriptGraph part;
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id))
            part.nodes.push_back(*n);
    for (const ScriptLink& l : doc->graph.links)
        if (part.FindNode(l.fromNode) && part.FindNode(l.toNode))
            part.links.push_back(l);
    part.nextId = doc->graph.nextId;
    return ScriptGraphToJson(part);
}

void ScriptGraphEditor::Paste(const std::string& clipboard, glm::vec2 at)
{
    Document* doc = ActiveDoc();
    if (!doc || clipboard.empty())
        return;
    ScriptGraph part;
    try {
        part = ScriptGraphFromJson(clipboard);
    } catch (const std::exception&) {
        return; // not a graph
    }
    if (part.nodes.empty())
        return;
    glm::vec2 minPos(std::numeric_limits<float>::max());
    for (const ScriptNode& n : part.nodes)
        minPos = glm::min(minPos, n.position);
    std::vector<std::uint32_t> pasted;
    Edit("Paste", [&](ScriptGraph& g) {
        std::map<std::uint32_t, std::uint32_t> ids;
        for (const ScriptNode& n : part.nodes) {
            const std::uint32_t id = g.AddNode(n.type, n.position - minPos + at, n.param);
            g.FindNode(id)->defaults = n.defaults;
            ids[n.id]                = id;
            pasted.push_back(id);
        }
        for (const ScriptLink& l : part.links)
            (void)g.Connect(ids[l.fromNode], l.fromPin, ids[l.toNode], l.toPin);
    });
    doc->selected        = std::move(pasted);
    doc->selectedComment = 0;
}

void ScriptGraphEditor::DuplicateSelection()
{
    Document* doc = ActiveDoc();
    if (!doc || doc->selected.empty())
        return;
    glm::vec2 minPos(std::numeric_limits<float>::max());
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id))
            minPos = glm::min(minPos, n->position);
    Paste(CopySelection(), minPos + glm::vec2(30.0f, 30.0f));
}

void ScriptGraphEditor::CommentSelection()
{
    Document* doc = ActiveDoc();
    if (!doc)
        return;
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id)) {
            const NodeLayout l = Layout(doc->graph, *n);
            lo                 = glm::min(lo, l.pos);
            hi                 = glm::max(hi, l.pos + l.size);
        }
    if (lo.x > hi.x) { // nothing selected: a comment at the view center
        lo = -doc->scroll + glm::vec2(40.0f);
        hi = lo + glm::vec2(300.0f, 160.0f);
    }
    std::uint32_t id = 0;
    Edit("Comment", [&](ScriptGraph& g) { id = g.AddComment(lo - glm::vec2(20.0f, 44.0f), hi - lo + glm::vec2(40.0f, 64.0f)); });
    doc->selectedComment = id;
}

void ScriptGraphEditor::ProvideTo(ScriptSystem& scripts) const
{
    for (const auto& doc : m_Docs)
        scripts.Provide(doc->path, doc->graph);
}

// --- Window -------------------------------------------------------------------------------------

void ScriptGraphEditor::Draw(bool* open, const ScriptSystem* debug)
{
    if (m_FocusRequested) {
        ImGui::SetNextWindowFocus();
        m_FocusRequested = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    const bool visible = ImGui::Begin("Blueprint", open);
    ImGui::PopStyleVar();
    if (!visible) {
        m_Focused = false;
        ImGui::End();
        DrawDialog();
        return;
    }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    DrawToolbar(debug);

    if (m_Docs.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No blueprint open. Create one with New... or open a .ugraph file,");
        ImGui::TextDisabled("or use \"Edit\" on a Script component in the Inspector.");
        ImGui::End();
        DrawDialog();
        return;
    }

    // Tabs, one per open graph.
    std::optional<std::size_t> close;
    if (ImGui::BeginTabBar("graphs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (std::size_t i = 0; i < m_Docs.size(); ++i) {
            Document&         d     = *m_Docs[i];
            bool              keep  = true;
            const std::string label = d.path.filename().string() + "###" + d.path.string();
            ImGuiTabItemFlags flags = d.revision != d.savedRevision ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (std::exchange(d.selectTab, false))
                flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(label.c_str(), &keep, flags)) {
                if (m_Active != i) {
                    m_WidgetSession.reset();
                    *m_Drag = DragState{};
                }
                m_Active = i;
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", d.path.string().c_str());
            if (!keep)
                close = i;
        }
        ImGui::EndTabBar();
    }
    if (close) {
        if (m_Docs[*close]->revision != m_Docs[*close]->savedRevision)
            m_Drag->confirmClose = close;
        else
            Close(*close);
    }
    if (m_Docs.empty()) {
        ImGui::End();
        return;
    }

    Document& doc = *m_Docs[m_Active];
    m_FrameJson   = ScriptGraphToJson(doc.graph); // widget edits this frame undo to this
    m_WidgetTouched = false;

    if (ImGui::BeginChild("sidebar", ImVec2(250.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
        DrawSidebar(doc);
        DrawDetails(doc);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("main", ImVec2(0.0f, 0.0f))) {
        DrawCanvas(doc, debug);
        DrawResults(doc, debug);
    }
    ImGui::EndChild();

    if (m_Focused)
        HandleKeys(doc);

    // Widget edits: one undo step per interaction (session until no item is active).
    if (m_WidgetTouched) {
        if (!m_WidgetSession)
            m_WidgetSession.emplace(m_FrameJson, doc.revision);
        Changed(doc);
    }
    if (m_WidgetSession && !ImGui::IsAnyItemActive()) {
        PushUndo(doc, std::move(m_WidgetSession->first), m_WidgetSession->second);
        m_WidgetSession.reset();
    }

    // Closing an unsaved graph.
    if (m_Drag->confirmClose)
        ImGui::OpenPopup("Close blueprint?");
    if (ImGui::BeginPopupModal("Close blueprint?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::size_t index = m_Drag->confirmClose.value_or(0);
        ImGui::Text("'%s' has unsaved changes.", index < m_Docs.size() ? m_Docs[index]->path.filename().string().c_str() : "");
        if (ImGui::Button("Save and close")) {
            m_Active = index;
            if (Save())
                Close(index);
            m_Drag->confirmClose.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            Close(index);
            m_Drag->confirmClose.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_Drag->confirmClose.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
    DrawDialog();
}

void ScriptGraphEditor::DrawToolbar(const ScriptSystem* debug)
{
    std::error_code ec;
    const std::filesystem::path dir = ActiveDoc() ? ActiveDoc()->path.parent_path() : std::filesystem::current_path(ec);
    if (ImGui::Button("New...")) {
        m_DialogPurpose = DialogPurpose::New;
        m_Dialog->Open("New blueprint", FileDialog::Mode::Save, dir, {".ugraph"}, "NewBlueprint.ugraph");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open...")) {
        m_DialogPurpose = DialogPurpose::Open;
        m_Dialog->Open("Open blueprint", FileDialog::Mode::Open, dir, {".ugraph"});
    }
    ImGui::BeginDisabled(!ActiveDoc());
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        Save();
    ImGui::SameLine();
    if (ImGui::Button("Save As...")) {
        m_DialogPurpose = DialogPurpose::SaveAs;
        m_Dialog->Open("Save blueprint as", FileDialog::Mode::Save, dir, {".ugraph"},
                       ActiveDoc() ? ActiveDoc()->path.filename().string() : std::string());
    }
    ImGui::SameLine();
    const std::size_t errors = ActiveDoc() ? static_cast<std::size_t>(std::ranges::count_if(
                                                 Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }))
                                           : 0;
    ImGui::PushStyleColor(ImGuiCol_Button, errors ? ImVec4(0.6f, 0.15f, 0.15f, 1.0f) : ImVec4(0.15f, 0.45f, 0.2f, 1.0f));
    if (ImGui::Button(errors ? "Compile (errors)" : "Compile (ok)") && ActiveDoc())
        ActiveDoc()->diagnosed = ~std::uint64_t{0}; // validate again now
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanUndo());
    if (ImGui::Button("Undo"))
        Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRedo());
    if (ImGui::Button("Redo"))
        Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame") && ActiveDoc())
        FrameNodes(*ActiveDoc(), false);
    ImGui::EndDisabled();
    if (const Document* doc = ActiveDoc()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d%%  %zu nodes", static_cast<int>(std::round(doc->zoom * 100.0f)), doc->graph.nodes.size());
    }
    if (debug && debug->Running()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  Playing - execution highlighted");
    }
}

void ScriptGraphEditor::DrawDialog()
{
    const std::optional<std::filesystem::path> file = m_Dialog->Draw();
    if (!file)
        return;
    switch (m_DialogPurpose) {
    case DialogPurpose::Open: Open(*file); break;
    case DialogPurpose::New: New(*file); break;
    case DialogPurpose::SaveAs: SaveAs(*file); break;
    case DialogPurpose::None: break;
    }
    m_DialogPurpose = DialogPurpose::None;
}

// --- Sidebar: variables + details ---------------------------------------------------------------

void ScriptGraphEditor::DrawSidebar(Document& doc)
{
    ImGui::SeparatorText("Variables");
    ScriptGraph& g = doc.graph;
    if (ImGui::SmallButton("+ Variable")) {
        const std::string name = UniqueVariableName(g, "NewVar");
        Edit("Add variable", [&](ScriptGraph& graph) { graph.variables.push_back({name, PinType::Float, 0.0f}); });
    }
    std::optional<std::string> remove;
    const glm::vec2            center = -doc.scroll + glm::vec2(80.0f, 80.0f);
    for (std::size_t i = 0; i < g.variables.size(); ++i) {
        ScriptVariable& v = g.variables[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PinColor(v.type)));
        ImGui::Bullet();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        std::string name = v.name;
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("##name", &name);
        if (ImGui::IsItemDeactivatedAfterEdit() && name != v.name) {
            const std::string from = v.name;
            Edit("Rename variable", [&](ScriptGraph& graph) { graph.RenameVariable(from, name); });
            ImGui::PopID();
            break; // references changed
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Get")) {
            const std::string var = v.name;
            Edit("Add Get", [&](ScriptGraph& graph) { doc.selected = {graph.AddNode("Variable.Get", center, var)}; });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Set")) {
            const std::string var = v.name;
            Edit("Add Set", [&](ScriptGraph& graph) { doc.selected = {graph.AddNode("Variable.Set", center, var)}; });
        }
        // Type + initial value.
        ImGui::Indent();
        int type = static_cast<int>(v.type) - 1;
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::Combo("##type", &type, "bool\0int\0float\0vec3\0string\0entity\0")) {
            const std::string var     = v.name;
            const PinType     newType = static_cast<PinType>(type + 1);
            Edit("Variable type", [&](ScriptGraph& graph) { graph.SetVariableType(var, newType); });
            ImGui::Unindent();
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (v.type != PinType::Entity)
            m_WidgetTouched |= ValueWidget("##value", v.value, v.type, -24.0f);
        else
            ImGui::TextDisabled("none");
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = v.name;
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (remove) {
        const std::string var = *remove;
        Edit("Remove variable", [&](ScriptGraph& graph) {
            std::erase_if(graph.variables, [&](const ScriptVariable& v) { return v.name == var; });
            std::vector<std::uint32_t> users;
            for (const ScriptNode& n : graph.nodes)
                if ((n.type == "Variable.Get" || n.type == "Variable.Set") && n.param == var)
                    users.push_back(n.id);
            for (std::uint32_t id : users)
                graph.RemoveNode(id);
        });
        std::erase_if(doc.selected, [&](std::uint32_t id) { return !doc.graph.FindNode(id); });
    }
}

void ScriptGraphEditor::DrawDetails(Document& doc)
{
    ImGui::SeparatorText("Details");
    ScriptGraph& g = doc.graph;
    if (ScriptComment* c = doc.selectedComment ? g.FindComment(doc.selectedComment) : nullptr) {
        ImGui::TextUnformatted("Comment");
        ImGui::SetNextItemWidth(-FLT_MIN);
        m_WidgetTouched |= ImGui::InputTextMultiline("##text", &c->text, ImVec2(-FLT_MIN, 60.0f));
        m_WidgetTouched |= ImGui::ColorEdit3("Color", &c->color.x, ImGuiColorEditFlags_NoInputs);
        return;
    }
    if (doc.selected.size() != 1) {
        ImGui::TextDisabled(doc.selected.empty() ? "Select a node" : "%zu nodes selected", doc.selected.size());
        return;
    }
    ScriptNode* node = g.FindNode(doc.selected.front());
    if (!node)
        return;
    const NodeDesc* desc = FindScriptNodeType(node->type);
    ImGui::TextUnformatted(desc ? desc->title.c_str() : node->type.c_str());
    ImGui::TextDisabled("%s  (id %u)", node->type.c_str(), node->id);
    if (desc && !desc->tooltip.empty())
        ImGui::TextWrapped("%s", desc->tooltip.c_str());
    if (!desc)
        return;

    // Param: may change the pins -> structural edit.
    std::string param = node->param;
    bool        commit = false;
    switch (desc->paramKind) {
    case ParamKind::None: break;
    case ParamKind::Text:
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##param", &param);
        commit = ImGui::IsItemDeactivatedAfterEdit();
        break;
    case ParamKind::Variable:
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            for (const ScriptVariable& v : g.variables)
                if (ImGui::Selectable(v.name.c_str(), v.name == param)) {
                    param  = v.name;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    case ParamKind::Key:
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (const std::string& k : KeyNames())
                if (ImGui::Selectable(k.c_str(), k == param)) {
                    param  = k;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    case ParamKind::Count: {
        int count = std::clamp(std::atoi(param.c_str()), 2, 16);
        if (ImGui::SliderInt(desc->paramLabel.c_str(), &count, 2, 16)) {
            param  = std::to_string(count);
            commit = true;
        }
        break;
    }
    case ParamKind::Choice:
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            for (const std::string& c : desc->paramChoices)
                if (ImGui::Selectable(c.c_str(), c == param)) {
                    param  = c;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    if (commit && param != node->param) {
        const std::uint32_t id = node->id;
        Edit("Change parameter", [&](ScriptGraph& graph) {
            graph.FindNode(id)->param = param;
            graph.RemoveDanglingLinks();
        });
        return;
    }

    // Unconnected inputs (same values as inline on the canvas).
    const NodeLayout l = Layout(g, *node);
    bool             header = false;
    for (std::size_t i = 0; i < l.pins.size(); ++i) {
        const PinInfo& p = l.pins[i];
        if (p.output || p.type == PinType::Exec || p.type == PinType::Entity || l.connected[i])
            continue;
        if (!std::exchange(header, true))
            ImGui::SeparatorText("Inputs");
        auto it = node->defaults.find(p.name);
        if (it == node->defaults.end())
            it = node->defaults.emplace(p.name, PinDefault(*node, desc, p)).first;
        ImGui::PushID(p.name.c_str());
        m_WidgetTouched |= ValueWidget(p.name.c_str(), it->second, p.type, 140.0f);
        ImGui::PopID();
    }
}

// --- Canvas -------------------------------------------------------------------------------------

void ScriptGraphEditor::DrawCanvas(Document& doc, const ScriptSystem* debug)
{
    constexpr float kResultsHeight = 110.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 28, 32, 255));
    const bool shown = ImGui::BeginChild("canvas", ImVec2(0.0f, -kResultsHeight), ImGuiChildFlags_Borders,
                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                             ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();
    if (!shown) {
        ImGui::EndChild();
        return;
    }
    ScriptGraph&  g      = doc.graph;
    ImGuiIO&      io     = ImGui::GetIO();
    const ImVec2  origin = ImGui::GetCursorScreenPos();
    const ImVec2  avail  = ImGui::GetContentRegionAvail();
    const glm::vec2 size(std::max(avail.x, 50.0f), std::max(avail.y, 50.0f));
    doc.viewSize = size;
    if (!doc.framed) {
        doc.framed = true;
        doc.scroll = glm::vec2(60.0f, 60.0f);
        if (!g.nodes.empty())
            FrameNodes(doc, false);
    }
    float& zoom = doc.zoom;
    const auto toScreen = [&](glm::vec2 p) {
        return ImVec2(origin.x + (p.x + doc.scroll.x) * zoom, origin.y + (p.y + doc.scroll.y) * zoom);
    };
    const auto toCanvas = [&](ImVec2 s) {
        return glm::vec2((s.x - origin.x) / zoom - doc.scroll.x, (s.y - origin.y) / zoom - doc.scroll.y);
    };

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", ImVec2(size.x, size.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool      hovered = ImGui::IsItemHovered();
    const glm::vec2 mouse   = toCanvas(io.MousePos);
    ImDrawList*     draw    = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // Grid.
    const float grid = 32.0f * zoom;
    if (grid > 6.0f) {
        const float ox = std::fmod(doc.scroll.x * zoom, grid), oy = std::fmod(doc.scroll.y * zoom, grid);
        for (float x = ox; x < size.x; x += grid)
            draw->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(45, 45, 52, 255));
        for (float y = oy; y < size.y; y += grid)
            draw->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(45, 45, 52, 255));
    }

    ImFont*     font     = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * zoom;
    const bool  text     = zoom > 0.35f;
    const double now     = debug ? debug->Time() : 0.0;
    const ScriptDebugInfo* info = debug && debug->Running() ? debug->Debug(doc.path) : nullptr;
    const auto glow = [&](double t) { return info ? std::clamp(1.0 - (now - t) / 0.5, 0.0, 1.0) : 0.0; };

    // Comments (behind everything).
    for (const ScriptComment& c : g.comments) {
        const ImVec2 a = toScreen(c.position), b = toScreen(c.position + c.size);
        const ImU32  fill = ImGui::GetColorU32(ImVec4(c.color.r, c.color.g, c.color.b, 0.18f));
        const ImU32  head = ImGui::GetColorU32(ImVec4(c.color.r, c.color.g, c.color.b, 0.7f));
        draw->AddRectFilled(a, b, fill, 4.0f * zoom);
        draw->AddRectFilled(a, ImVec2(b.x, a.y + kHeader * zoom), head, 4.0f * zoom, ImDrawFlags_RoundCornersTop);
        draw->AddRect(a, b, doc.selectedComment == c.id ? IM_COL32(255, 200, 60, 255) : head, 4.0f * zoom,
                      doc.selectedComment == c.id ? 2.0f : 1.0f);
        draw->AddTriangleFilled(ImVec2(b.x - 12.0f * zoom, b.y), b, ImVec2(b.x, b.y - 12.0f * zoom), head);
        if (text)
            draw->AddText(font, fontSize * 1.1f, ImVec2(a.x + 6.0f * zoom, a.y + 3.0f * zoom), IM_COL32(255, 255, 255, 230),
                          c.text.c_str());
    }

    // Layouts (nodes are drawn in order: later = on top).
    std::vector<NodeLayout>             layouts;
    std::map<std::uint32_t, std::size_t> layoutOf;
    layouts.reserve(g.nodes.size());
    for (const ScriptNode& n : g.nodes) {
        layoutOf[n.id] = layouts.size();
        layouts.push_back(Layout(g, n));
    }

    // Links.
    const auto bezier = [&](ImVec2 p0, ImVec2 p3, ImU32 color, float thickness) {
        const float dx = std::max(std::abs(p3.x - p0.x) * 0.5f, 40.0f * zoom);
        draw->AddBezierCubic(p0, ImVec2(p0.x + dx, p0.y), ImVec2(p3.x - dx, p3.y), p3, color, thickness);
    };
    for (const ScriptLink& link : g.links) {
        const auto a = layoutOf.find(link.fromNode), b = layoutOf.find(link.toNode);
        if (a == layoutOf.end() || b == layoutOf.end())
            continue;
        const NodeLayout& la = layouts[a->second];
        const NodeLayout& lb = layouts[b->second];
        const int         pa = la.PinIndex(link.fromPin, true), pb = lb.PinIndex(link.toPin, false);
        if (pa < 0 || pb < 0)
            continue;
        const PinType type = la.pins[static_cast<std::size_t>(pa)].type;
        double        lit  = 0.0;
        if (info && type == PinType::Exec)
            if (const auto t = info->linkTimes.find({link.fromNode, link.fromPin}); t != info->linkTimes.end())
                lit = glow(t->second);
        const float thickness = (type == PinType::Exec ? 3.0f : 2.0f) * std::max(zoom, 0.5f) * (1.0f + static_cast<float>(lit));
        const ImU32 color     = lit > 0.0 ? ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.2f, 1.0f)) : PinColor(type, 0.85f);
        bezier(toScreen(la.pinPos[static_cast<std::size_t>(pa)]), toScreen(lb.pinPos[static_cast<std::size_t>(pb)]), color, thickness);
    }

    // Hover: topmost pin / node / comment under the mouse.
    struct PinHit {
        std::uint32_t node = 0;
        int           pin  = -1;
    } pinHit;
    std::uint32_t nodeHit = 0;
    for (auto it = layouts.rbegin(); it != layouts.rend() && hovered; ++it) {
        for (std::size_t p = 0; p < it->pins.size(); ++p)
            if (glm::distance(mouse, it->pinPos[p]) <= kPinHit) {
                pinHit = {it->node->id, static_cast<int>(p)};
                break;
            }
        if (pinHit.pin >= 0)
            break;
        if (it->Contains(mouse)) {
            nodeHit = it->node->id;
            break;
        }
    }
    std::uint32_t commentHeader = 0, commentResize = 0;
    if (hovered && pinHit.pin < 0 && !nodeHit)
        for (auto it = g.comments.rbegin(); it != g.comments.rend(); ++it) {
            const glm::vec2 corner = it->position + it->size;
            if (mouse.x >= corner.x - 14.0f && mouse.y >= corner.y - 14.0f && mouse.x <= corner.x && mouse.y <= corner.y) {
                commentResize = it->id;
                break;
            }
            if (mouse.x >= it->position.x && mouse.x <= corner.x && mouse.y >= it->position.y &&
                mouse.y <= it->position.y + kHeader) {
                commentHeader = it->id;
                break;
            }
        }

    // Nodes.
    const std::set<std::uint32_t> selected(doc.selected.begin(), doc.selected.end());
    std::set<std::uint32_t>       errorNodes;
    for (const ScriptDiagnostic& d : Diagnostics())
        if (d.error && d.node)
            errorNodes.insert(d.node);
    if (info)
        for (const ScriptDiagnostic& d : info->diagnostics)
            if (d.error && d.node)
                errorNodes.insert(d.node);
    const float rounding = 6.0f * zoom;
    for (NodeLayout& l : layouts) {
        const ScriptNode& n = *l.node;
        const ImVec2      a = toScreen(l.pos), b = toScreen(l.pos + l.size);
        if (b.x < origin.x || b.y < origin.y || a.x > origin.x + size.x || a.y > origin.y + size.y)
            continue; // off screen
        double lit = 0.0;
        if (info)
            if (const auto t = info->nodeTimes.find(n.id); t != info->nodeTimes.end())
                lit = glow(t->second);
        if (lit > 0.0)
            draw->AddRect(ImVec2(a.x - 4.0f, a.y - 4.0f), ImVec2(b.x + 4.0f, b.y + 4.0f),
                          ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.2f, static_cast<float>(lit))), rounding + 3.0f, 4.0f);
        draw->AddRectFilled(a, b, IM_COL32(22, 22, 26, 235), rounding);
        draw->AddRectFilled(a, ImVec2(b.x, a.y + kHeader * zoom), HeaderColor(l.desc, g, n), rounding, ImDrawFlags_RoundCornersTop);
        const bool isSelected = selected.contains(n.id);
        const bool isError    = errorNodes.contains(n.id);
        draw->AddRect(a, b,
                      isError      ? IM_COL32(255, 60, 60, 255)
                      : isSelected ? IM_COL32(255, 200, 60, 255)
                                   : IM_COL32(70, 70, 80, 255),
                      rounding, isSelected || isError ? 2.5f : 1.0f);
        if (text)
            draw->AddText(font, fontSize, ImVec2(a.x + kPad * zoom, a.y + 4.0f * zoom), IM_COL32(255, 255, 255, 255),
                          l.title.c_str());

        for (std::size_t p = 0; p < l.pins.size(); ++p) {
            const PinInfo& pin   = l.pins[p];
            const ImVec2   c     = toScreen(l.pinPos[p]);
            const ImU32    color = PinColor(pin.type);
            const float    r     = kPinRadius * zoom;
            const bool     hot   = pinHit.node == n.id && pinHit.pin == static_cast<int>(p);
            if (pin.type == PinType::Exec) {
                const ImVec2 t0(c.x - r, c.y - r), t1(c.x - r, c.y + r), t2(c.x + r, c.y);
                if (l.connected[p] || hot)
                    draw->AddTriangleFilled(t0, t1, t2, color);
                else
                    draw->AddTriangle(t0, t1, t2, color, 1.5f);
            } else {
                if (l.connected[p] || hot)
                    draw->AddCircleFilled(c, r, color);
                else
                    draw->AddCircle(c, r, color, 0, 1.5f);
            }
            if (!text || l.labelWidth[p] <= 0.0f)
                continue;
            const float ty = c.y - fontSize * 0.5f;
            if (pin.output)
                draw->AddText(font, fontSize, ImVec2(c.x - (kPad + 6.0f + l.labelWidth[p]) * zoom, ty),
                              IM_COL32(220, 220, 220, 255), pin.name.c_str());
            else
                draw->AddText(font, fontSize, ImVec2(c.x + (kPad + 6.0f) * zoom, ty), IM_COL32(220, 220, 220, 255),
                              pin.name.c_str());
        }

        // Inline values of unconnected data inputs.
        if (zoom < 0.5f)
            continue;
        ScriptNode& editable = *g.FindNode(n.id);
        ImGui::PushID(static_cast<int>(n.id));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * zoom, 1.0f * zoom));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(2.0f * zoom, 2.0f * zoom));
        for (std::size_t p = 0; p < l.pins.size(); ++p) {
            const PinInfo& pin = l.pins[p];
            if (pin.output || pin.type == PinType::Exec || l.connected[p])
                continue;
            const ImVec2 c = toScreen(l.pinPos[p]);
            ImGui::SetCursorScreenPos(ImVec2(c.x + (kPad + 10.0f + l.labelWidth[p]) * zoom, c.y - ImGui::GetFrameHeight() * 0.5f));
            auto it = editable.defaults.find(pin.name);
            if (it == editable.defaults.end()) {
                ScriptValue value = PinDefault(editable, l.desc, pin);
                ImGui::PushID(pin.name.c_str());
                if (ValueWidget("##v", value, pin.type, WidgetWidth(pin.type) * zoom)) {
                    editable.defaults[pin.name] = std::move(value);
                    m_WidgetTouched             = true;
                }
                ImGui::PopID();
            } else {
                ImGui::PushID(pin.name.c_str());
                m_WidgetTouched |= ValueWidget("##v", it->second, pin.type, WidgetWidth(pin.type) * zoom);
                ImGui::PopID();
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
        ImGui::PopID();
    }

    // --- Interaction ---
    DragState&    drag         = *m_Drag;
    std::uint32_t bringToFront = 0; // clicked node: drawn last from the next frame on
    if (hovered && io.MouseWheel != 0.0f && drag.kind == DragState::Kind::None) {
        const glm::vec2 before = mouse;
        zoom                   = std::clamp(zoom * std::pow(1.15f, io.MouseWheel), kMinZoom, kMaxZoom);
        doc.scroll = glm::vec2((io.MousePos.x - origin.x) / zoom, (io.MousePos.y - origin.y) / zoom) - before;
    }
    const auto beginDrag = [&](DragState::Kind kind, int button) {
        drag.kind           = kind;
        drag.button         = button;
        drag.start          = mouse;
        drag.last           = mouse;
        drag.before         = m_FrameJson;
        drag.beforeRevision = doc.revision;
        drag.moved          = false;
    };
    if (hovered && drag.kind == DragState::Kind::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (pinHit.pin >= 0) {
                const NodeLayout& l   = layouts[layoutOf[pinHit.node]];
                const PinInfo&    pin = l.pins[static_cast<std::size_t>(pinHit.pin)];
                if (io.KeyAlt) {
                    const std::uint32_t id = pinHit.node;
                    Edit("Break links", [&](ScriptGraph& graph) { graph.Disconnect(id, pin.name, pin.output); });
                } else {
                    beginDrag(DragState::Kind::Link, 0);
                    drag.node   = pinHit.node;
                    drag.pin    = pin.name;
                    drag.output = pin.output;
                    drag.type   = pin.type;
                }
            } else if (nodeHit) {
                const bool isSelected = selected.contains(nodeHit);
                if (io.KeyCtrl || io.KeyShift) {
                    if (isSelected)
                        std::erase(doc.selected, nodeHit);
                    else
                        doc.selected.push_back(nodeHit);
                } else if (!isSelected) {
                    doc.selected = {nodeHit};
                }
                doc.selectedComment = 0;
                bringToFront        = nodeHit;
                beginDrag(DragState::Kind::Nodes, 0);
            } else if (commentResize) {
                doc.selectedComment = commentResize;
                doc.selected.clear();
                beginDrag(DragState::Kind::Resize, 0);
                drag.comment = commentResize;
            } else if (commentHeader) {
                doc.selectedComment = commentHeader;
                doc.selected.clear();
                beginDrag(DragState::Kind::Comment, 0);
                drag.comment = commentHeader;
                drag.carried.clear();
                if (const ScriptComment* c = g.FindComment(commentHeader))
                    for (const NodeLayout& l : layouts)
                        if (l.pos.x >= c->position.x && l.pos.y >= c->position.y &&
                            l.pos.x + l.size.x <= c->position.x + c->size.x && l.pos.y + l.size.y <= c->position.y + c->size.y)
                            drag.carried.push_back(l.node->id);
            } else {
                if (!io.KeyCtrl && !io.KeyShift)
                    doc.selected.clear();
                doc.selectedComment = 0;
                beginDrag(DragState::Kind::Box, 0);
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            beginDrag(DragState::Kind::Pan, 1);
            drag.menuNode = nodeHit;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            beginDrag(DragState::Kind::Pan, 2);
        }
    }

    switch (drag.kind) {
    case DragState::Kind::None: break;
    case DragState::Kind::Pan:
        if (ImGui::IsMouseDown(drag.button)) {
            doc.scroll += glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / zoom;
            if (const ImVec2 d = ImGui::GetMouseDragDelta(drag.button); d.x * d.x + d.y * d.y > 16.0f)
                drag.moved = true;
        } else {
            if (drag.button == 1 && !drag.moved && hovered) { // right click: menus
                drag.menuPos = mouse;
                if (drag.menuNode) {
                    if (!selected.contains(drag.menuNode))
                        doc.selected = {drag.menuNode};
                    drag.openNode = true;
                } else {
                    drag.pending.reset();
                    drag.openCreate = true;
                }
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    case DragState::Kind::Nodes:
    case DragState::Kind::Comment:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            glm::vec2 delta = mouse - drag.last;
            drag.last       = mouse;
            if (delta != glm::vec2(0.0f)) {
                drag.moved = true;
                const std::vector<std::uint32_t>& ids = drag.kind == DragState::Kind::Nodes ? doc.selected : drag.carried;
                for (std::uint32_t id : ids)
                    if (ScriptNode* n = g.FindNode(id))
                        n->position += delta;
                if (drag.kind == DragState::Kind::Comment)
                    if (ScriptComment* c = g.FindComment(drag.comment))
                        c->position += delta;
            }
        } else {
            if (drag.moved) {
                // Snap to a 8-unit grid on release.
                const auto snap = [](glm::vec2 p) { return glm::round(p / 8.0f) * 8.0f; };
                for (std::uint32_t id : drag.kind == DragState::Kind::Nodes ? doc.selected : drag.carried)
                    if (ScriptNode* n = g.FindNode(id))
                        n->position = snap(n->position);
                PushUndo(doc, std::move(drag.before), drag.beforeRevision);
                Changed(doc);
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    case DragState::Kind::Resize:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (ScriptComment* c = g.FindComment(drag.comment)) {
                c->size    = glm::max(mouse - c->position, glm::vec2(120.0f, 60.0f));
                drag.moved = true;
            }
        } else {
            if (drag.moved) {
                PushUndo(doc, std::move(drag.before), drag.beforeRevision);
                Changed(doc);
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    case DragState::Kind::Link: {
        const auto from = layoutOf.find(drag.node);
        if (from == layoutOf.end()) {
            drag.kind = DragState::Kind::None;
            break;
        }
        const NodeLayout& l   = layouts[from->second];
        const int         pin = l.PinIndex(drag.pin, drag.output);
        if (pin < 0) {
            drag.kind = DragState::Kind::None;
            break;
        }
        const ImVec2 p = toScreen(l.pinPos[static_cast<std::size_t>(pin)]);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Compatible target under the mouse: highlight.
            ImU32 color = PinColor(drag.type, 0.9f);
            if (pinHit.pin >= 0 && pinHit.node != drag.node) {
                const PinInfo& target = layouts[layoutOf[pinHit.node]].pins[static_cast<std::size_t>(pinHit.pin)];
                const bool ok = target.output != drag.output &&
                                (drag.output ? CanConvert(drag.type, target.type) : CanConvert(target.type, drag.type));
                color = ok ? IM_COL32(120, 255, 120, 255) : IM_COL32(255, 80, 80, 255);
            }
            if (drag.output)
                bezier(p, io.MousePos, color, 2.5f * std::max(zoom, 0.5f));
            else
                bezier(io.MousePos, p, color, 2.5f * std::max(zoom, 0.5f));
        } else {
            if (pinHit.pin >= 0 && pinHit.node != drag.node) {
                const PinInfo&      target   = layouts[layoutOf[pinHit.node]].pins[static_cast<std::size_t>(pinHit.pin)];
                const std::uint32_t other    = pinHit.node;
                const std::string   otherPin = target.name;
                if (target.output != drag.output)
                    Edit("Connect", [&](ScriptGraph& graph) {
                        const std::string error = drag.output ? graph.Connect(drag.node, drag.pin, other, otherPin)
                                                              : graph.Connect(other, otherPin, drag.node, drag.pin);
                        if (!error.empty())
                            m_LastError = error;
                    });
            } else if (pinHit.pin < 0 && !nodeHit && hovered) {
                // Dropped on the background: pick a node to connect.
                drag.pending    = DragState::Pending{drag.node, drag.pin, drag.output, drag.type};
                drag.menuPos    = mouse;
                drag.openCreate = true;
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    }
    case DragState::Kind::Box: {
        const glm::vec2 lo = glm::min(drag.start, mouse), hi = glm::max(drag.start, mouse);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            draw->AddRectFilled(toScreen(lo), toScreen(hi), IM_COL32(90, 150, 255, 40));
            draw->AddRect(toScreen(lo), toScreen(hi), IM_COL32(90, 150, 255, 200));
        } else {
            for (const NodeLayout& l : layouts)
                if (l.pos.x <= hi.x && l.pos.y <= hi.y && l.pos.x + l.size.x >= lo.x && l.pos.y + l.size.y >= lo.y &&
                    std::ranges::find(doc.selected, l.node->id) == doc.selected.end())
                    doc.selected.push_back(l.node->id);
            drag.kind = DragState::Kind::None;
        }
        break;
    }
    }

    // Tooltip: node description / pin type.
    if (hovered && drag.kind == DragState::Kind::None) {
        if (pinHit.pin >= 0) {
            const PinInfo& pin = layouts[layoutOf[pinHit.node]].pins[static_cast<std::size_t>(pinHit.pin)];
            ImGui::SetTooltip("%s (%s)%s", pin.name.c_str(), ToString(pin.type), "\nAlt+click: break links");
        } else if (nodeHit) {
            const NodeLayout& l = layouts[layoutOf[nodeHit]];
            for (const ScriptDiagnostic& d : Diagnostics())
                if (d.node == nodeHit)
                    ImGui::SetTooltip("%s", d.message.c_str());
            if (!errorNodes.contains(nodeHit) && l.desc && !l.desc->tooltip.empty() && io.KeyCtrl)
                ImGui::SetTooltip("%s", l.desc->tooltip.c_str());
        }
    }

    draw->PopClipRect();
    if (bringToFront) { // after the layouts (they point into g.nodes) are no longer used
        const auto it = std::ranges::find_if(g.nodes, [&](const ScriptNode& n) { return n.id == bringToFront; });
        if (it != g.nodes.end() && it + 1 != g.nodes.end())
            std::rotate(it, it + 1, g.nodes.end());
    }
    DrawCreateMenu(doc);
    ImGui::EndChild();
}

void ScriptGraphEditor::DrawCreateMenu(Document& doc)
{
    DragState& drag = *m_Drag;
    if (std::exchange(drag.openCreate, false)) {
        drag.search.clear();
        drag.focusSearch = true;
        ImGui::OpenPopup("##create");
    }
    if (std::exchange(drag.openNode, false))
        ImGui::OpenPopup("##node");

    if (ImGui::BeginPopup("##node")) {
        if (ImGui::MenuItem("Delete", "Del"))
            DeleteSelection();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
            DuplicateSelection();
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
            ImGui::SetClipboardText(CopySelection().c_str());
        if (ImGui::MenuItem("Break all links")) {
            const std::vector<std::uint32_t> nodes = doc.selected;
            Edit("Break links", [&](ScriptGraph& g) {
                std::erase_if(g.links, [&](const ScriptLink& l) {
                    return std::ranges::find(nodes, l.fromNode) != nodes.end() || std::ranges::find(nodes, l.toNode) != nodes.end();
                });
            });
        }
        if (ImGui::MenuItem("Comment selection", "C"))
            CommentSelection();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f), ImVec2(360.0f, 420.0f));
    if (!ImGui::BeginPopup("##create"))
        return;
    if (std::exchange(drag.focusSearch, false))
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search nodes...", &drag.search);
    const std::string filter = Lower(drag.search);
    ScriptGraph&      g      = doc.graph;

    struct Entry {
        std::string label, category, type, param;
    };
    std::vector<Entry> entries;
    for (const NodeDesc& d : ScriptNodeTypes()) {
        if (d.paramKind == ParamKind::Variable) { // one entry per variable
            for (const ScriptVariable& v : g.variables)
                entries.push_back({d.title + " " + v.name, "Variables", d.type, v.name});
            continue;
        }
        entries.push_back({d.title, d.category, d.type, {}});
    }
    // Context: only nodes with a pin that fits the dragged one.
    const auto fits = [&](const Entry& e) -> std::optional<std::string> {
        if (!drag.pending)
            return std::string();
        ScriptNode probe;
        probe.type  = e.type;
        probe.param = e.param.empty() ? (FindScriptNodeType(e.type) ? FindScriptNodeType(e.type)->paramDefault : "") : e.param;
        for (const PinInfo& p : NodePins(g, probe))
            if (p.output != drag.pending->output &&
                (drag.pending->output ? CanConvert(drag.pending->type, p.type) : CanConvert(p.type, drag.pending->type)))
                return p.name;
        return std::nullopt;
    };

    std::optional<Entry> chosen;
    std::string          chosenPin;
    const auto item = [&](const Entry& e) {
        const auto pin = fits(e);
        if (!pin)
            return;
        if (ImGui::Selectable(e.label.c_str())) {
            chosen    = e;
            chosenPin = *pin;
        }
    };
    ImGui::BeginChild("##list", ImVec2(0.0f, 300.0f));
    if (filter.empty()) {
        std::vector<std::string> categories;
        for (const Entry& e : entries)
            if (std::ranges::find(categories, e.category) == categories.end() && fits(e))
                categories.push_back(e.category);
        for (const std::string& category : categories)
            if (ImGui::TreeNodeEx(category.c_str(), drag.pending ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                for (const Entry& e : entries)
                    if (e.category == category)
                        item(e);
                ImGui::TreePop();
            }
        if (!drag.pending && ImGui::Selectable("Add comment"))
            chosen = Entry{"comment", "", "", ""};
    } else {
        for (const Entry& e : entries)
            if (Lower(e.label).find(filter) != std::string::npos || Lower(e.category).find(filter) != std::string::npos)
                item(e);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) // first match
            for (const Entry& e : entries)
                if ((Lower(e.label).find(filter) != std::string::npos) && fits(e)) {
                    chosen    = e;
                    chosenPin = *fits(e);
                    break;
                }
    }
    ImGui::EndChild();

    if (chosen) {
        const glm::vec2 at = drag.menuPos;
        if (chosen->label == "comment" && chosen->type.empty()) {
            Edit("Comment", [&](ScriptGraph& graph) { doc.selectedComment = graph.AddComment(at, {300.0f, 160.0f}); });
        } else {
            std::uint32_t id = 0;
            const auto    pending = drag.pending;
            Edit("Add node", [&](ScriptGraph& graph) {
                id = graph.AddNode(chosen->type, at, chosen->param);
                if (pending) {
                    if (pending->output)
                        (void)graph.Connect(pending->node, pending->pin, id, chosenPin);
                    else
                        (void)graph.Connect(id, chosenPin, pending->node, pending->pin);
                }
            });
            doc.selected = {id};
        }
        drag.pending.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ScriptGraphEditor::DrawResults(Document& doc, const ScriptSystem* debug)
{
    ImGui::BeginChild("results", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    std::vector<ScriptDiagnostic> all = Diagnostics();
    if (debug && debug->Running())
        if (const ScriptDebugInfo* info = debug->Debug(doc.path))
            for (const ScriptDiagnostic& d : info->diagnostics)
                if (std::ranges::none_of(all, [&](const ScriptDiagnostic& x) { return x.node == d.node && x.message == d.message; }))
                    all.push_back(d);
    if (all.empty())
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Compiled: no errors");
    if (!m_LastError.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "%s", m_LastError.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            m_LastError.clear();
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        const ScriptDiagnostic& d = all[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec4 color = d.error ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(1.0f, 0.85f, 0.4f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        const std::string label = std::format("{} {}{}", d.error ? "Error:" : "Warning:", d.node ? std::format("[node {}] ", d.node) : "",
                                              d.message);
        if (ImGui::Selectable(label.c_str()) && d.node && doc.graph.FindNode(d.node)) {
            doc.selected = {d.node};
            FrameNodes(doc, true);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void ScriptGraphEditor::HandleKeys(Document& doc)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || m_Dialog->IsOpen() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        return;
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift)
                Redo();
            else
                Undo();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            Redo();
        if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            ImGui::SetClipboardText(CopySelection().c_str());
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            const char* text = ImGui::GetClipboardText();
            Paste(text ? text : "", -doc.scroll + glm::vec2(80.0f, 80.0f));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D, false))
            DuplicateSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        DeleteSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !doc.selected.empty()) // like UE: around the selection
        CommentSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        FrameNodes(doc, !doc.selected.empty());
}

void ScriptGraphEditor::FrameNodes(Document& doc, bool selectionOnly)
{
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const ScriptNode& n : doc.graph.nodes) {
        if (selectionOnly && std::ranges::find(doc.selected, n.id) == doc.selected.end())
            continue;
        const NodeLayout l = Layout(doc.graph, n);
        lo                 = glm::min(lo, l.pos);
        hi                 = glm::max(hi, l.pos + l.size);
    }
    if (lo.x > hi.x)
        return;
    const glm::vec2 view   = doc.viewSize;
    const glm::vec2 extent = hi - lo + glm::vec2(80.0f);
    doc.zoom               = std::clamp(std::min(view.x / extent.x, view.y / extent.y), kMinZoom, 1.0f);
    doc.scroll             = view / (2.0f * doc.zoom) - (lo + hi) * 0.5f;
}

} // namespace Engine
