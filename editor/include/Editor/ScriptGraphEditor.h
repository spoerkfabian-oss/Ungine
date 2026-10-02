#pragma once
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptSystem.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

class FileDialog;
class Scene;
class ScriptSystem;

// Blueprint-style node graph editor for visual scripts (.ugraph), drawn as the ImGui window
// "Blueprint". Several graphs are open as tabs, each with its own undo history. The sidebar
// switches between the event graph and the graph's functions (signature, locals), lists the
// variables (types incl. arrays, "instance editable") and shows the details of the selection and,
// while playing, the debugger watch (variables, locals, last pin values).
//
// Canvas: wheel zooms about the cursor, right / middle drag pans, left drag on the background
// selects in a box, dragging a node moves the selection, dragging from a pin creates a link (drop
// on a pin to connect, on the background to pick a node that is connected automatically), Alt+click
// on a pin breaks its links, right click opens the node search (context sensitive). Keys: Del,
// Ctrl+C/V/D, Ctrl+Z/Y, Ctrl+S, C (comment around the selection), F (frame selection / all),
// F9 (breakpoint), Shift+W/A/S/D (align top / left / bottom / right), double-click on a link
// (reroute knot), M (minimap). Unconnected inputs are edited inline. While playing, nodes and exec
// links that ran light up; at a breakpoint the graph opens on the paused node, F5 / F10 (or the
// toolbar) continue / step, hovering a pin shows its last value.
class ScriptGraphEditor {
public:
    ScriptGraphEditor();
    ~ScriptGraphEditor();

    ScriptGraphEditor(const ScriptGraphEditor&)            = delete;
    ScriptGraphEditor& operator=(const ScriptGraphEditor&) = delete;

    // Documents. Open focuses an already open file; New writes a template (BeginPlay + Tick).
    bool Open(const std::filesystem::path& file); // false: LastError()
    bool New(const std::filesystem::path& file);
    bool Save(); // active document
    bool SaveAs(const std::filesystem::path& file);
    bool SaveAll();
    void Close(std::size_t index);
    [[nodiscard]] std::size_t                  Count() const;
    [[nodiscard]] std::optional<std::size_t>   Active() const;
    void                                       Activate(std::size_t index);
    [[nodiscard]] ScriptGraph*                 Graph(); // active, null if none
    [[nodiscard]] const ScriptGraph*           Find(const std::filesystem::path& file) const; // open, null if not
    [[nodiscard]] const std::filesystem::path* Path() const;
    [[nodiscard]] bool                         Dirty() const; // active has unsaved changes
    [[nodiscard]] bool                         AnyDirty() const;
    [[nodiscard]] const std::string&           LastError() const { return m_LastError; }

    // Undoable change of the active graph (validated afterwards).
    void Edit(const char* label, const std::function<void(ScriptGraph&)>& change);
    bool Undo();
    bool Redo();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] const std::vector<ScriptDiagnostic>& Diagnostics(); // active graph, current

    // Selection of the active graph (node ids).
    [[nodiscard]] const std::vector<std::uint32_t>& SelectedNodes() const;
    void Select(std::vector<std::uint32_t> nodes);
    void DeleteSelection();
    void DuplicateSelection();
    [[nodiscard]] std::string CopySelection() const; // JSON of the selected nodes + their links
    void Paste(const std::string& clipboard, glm::vec2 at);
    void CommentSelection();
    enum class Align { Left, Right, Top, Bottom, CenterX, CenterY, DistributeX, DistributeY };
    void AlignSelection(Align how);
    void ToggleBreakpoints(); // selected exec nodes (undoable, saved with the graph)

    // The canvas scope of the active graph: a function name, empty for the event graph.
    void                             OpenScope(const std::string& function);
    [[nodiscard]] const std::string* Scope() const;

    // Moves the selected nodes into a new function / macro (named NewFunction / NewMacro, renamed in
    // the sidebar) called in their place. Returns the error, empty on success.
    std::string CollapseSelection(bool macro);

    // Find in Blueprints (Ctrl+F): node titles / params in the open graphs and the .ugraph files
    // below the search root (the project's content). References: nodes naming exactly `query`
    // (param or a text input, e.g. a variable, function, macro, event or dispatcher).
    struct SearchHit {
        std::filesystem::path file;
        std::string           scope; // function / macro, empty: event graph
        std::uint32_t         node = 0;
        std::string           label;
    };
    void SetSearchRoot(std::filesystem::path root) { m_SearchRoot = std::move(root); }
    void OpenSearch(std::string query, bool references = false);
    [[nodiscard]] std::vector<SearchHit> Search(const std::string& query, bool references) const;
    void ShowHit(const SearchHit& hit); // opens the graph on the node

    void OpenTimeline(const std::string& name); // timeline editor window (active graph)
    // Screen rectangles (x, y, width, height; window pixels) of the last frame: the canvas and the
    // timeline editor's curve view (zero when not shown) - for UI tests and tools.
    [[nodiscard]] glm::vec4 CanvasRect() const { return m_CanvasRect; }
    [[nodiscard]] glm::vec4 TimelineCurveRect() const { return m_TimelineCurveRect; }
    // Changes with every edit of any open graph (construction scripts run again).
    [[nodiscard]] std::uint64_t Revision() const { return m_RevisionCounter; }

    // The value editor of the graph editor (arrays as a popup list), for other panels.
    static bool EditValue(const char* id, ScriptValue& value, PinType type, float width);
    static bool EditType(const char* id, PinType& type, float width); // type picker (arrays, maps, user types)

    // Open graphs (unsaved edits included) replace their files in play mode.
    void ProvideTo(ScriptSystem& scripts) const;

    // The window. debug: execution highlight, runtime errors and the debugger while playing (may
    // be null); scene: the one the scripts run in (debugger continue / step).
    void Draw(bool* open, ScriptSystem* debug, Scene* scene);
    [[nodiscard]] bool Focused() const { return m_Focused; } // keyboard shortcuts belong to the graph
    void               Focus() { m_FocusRequested = true; }

private:
    struct Document;
    struct DragState;

    [[nodiscard]] Document* ActiveDoc();
    [[nodiscard]] const Document* ActiveDoc() const;
    void PushUndo(Document& doc, std::string before, std::uint64_t revision);
    void Changed(Document& doc); // new revision, diagnostics outdated
    void DrawToolbar(ScriptSystem* debug, Scene* scene);
    void DrawSidebar(Document& doc);
    void DrawFunctions(Document& doc);
    void DrawMacro(Document& doc, ScriptMacro& macro);
    void DrawMembers(Document& doc); // events, dispatchers, interfaces, timelines
    void DrawTimeline(Document& doc);
    void DrawSearch();
    void DrawWatch(Document& doc, const ScriptSystem* debug);
    void SyncDebugger(ScriptSystem* debug);
    void DrawDetails(Document& doc);
    void DrawCanvas(Document& doc, const ScriptSystem* debug);
    void DrawCreateMenu(Document& doc);
    void DrawResults(Document& doc, const ScriptSystem* debug);
    void HandleKeys(Document& doc, ScriptSystem* debug, Scene* scene);
    void FrameNodes(Document& doc, bool selectionOnly);
    void DrawDialog();

    std::vector<std::unique_ptr<Document>> m_Docs;
    std::size_t                            m_Active = 0;
    std::unique_ptr<DragState>             m_Drag;
    std::unique_ptr<FileDialog>            m_Dialog;
    enum class DialogPurpose { None, Open, New, SaveAs } m_DialogPurpose = DialogPurpose::None;
    std::string   m_LastError;
    bool          m_Focused        = false;
    bool          m_FocusRequested = false;
    std::uint64_t m_RevisionCounter = 0;
    // Widget edits (inline values, details, variables): one undo step per interaction.
    std::optional<std::pair<std::string, std::uint64_t>> m_WidgetSession;
    bool                                                 m_WidgetTouched = false;
    std::string                                          m_FrameJson; // active graph at the start of the frame
    Scene*                                               m_Scene = nullptr; // this frame's (names in the watch)
    std::optional<ScriptWatch>                           m_Watch;          // the watched instance, this frame
    // Find in Blueprints.
    std::filesystem::path  m_SearchRoot;
    std::string            m_SearchQuery;
    bool                   m_SearchReferences = false, m_SearchOpen = false, m_SearchFocus = false;
    std::vector<SearchHit> m_SearchHits;
    // Search: project graphs parsed once per file change (key -> (mtime, graph)).
    mutable std::map<std::string, std::pair<std::filesystem::file_time_type, std::shared_ptr<const ScriptGraph>>> m_SearchCache;
    // Graphs handed to a script system by ProvideTo (by key), so closed ones can be taken back.
    mutable std::map<std::string, std::filesystem::path> m_Provided;
    glm::vec4 m_CanvasRect{0.0f}, m_TimelineCurveRect{0.0f};
    // Timeline editor.
    std::string m_Timeline;
    int         m_TimelineTrack = 0, m_TimelineKey = -1, m_TimelineComponent = 0;
    bool        m_TimelineDragging = false;
};

} // namespace Engine
