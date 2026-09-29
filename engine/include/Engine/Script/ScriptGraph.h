#pragma once
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Engine {

// Visual scripting ("Blueprints"): a graph of nodes connected by exec links (control flow) and
// data links (values). The graph is plain data, edited by the editor's graph panel, stored as JSON
// (.ugraph) and run by ScriptSystem. Node types and their pins come from the registry
// (ScriptNodes.h).

enum class PinType : std::uint8_t { Exec, Bool, Int, Float, Vec3, String, Entity };

// A data value; the alternative index is PinType - 1 (Exec carries no value).
using ScriptValue = std::variant<bool, std::int32_t, float, glm::vec3, std::string, Entity>;

[[nodiscard]] const char*            ToString(PinType type);
[[nodiscard]] std::optional<PinType> PinTypeFromString(std::string_view name);
[[nodiscard]] PinType                TypeOf(const ScriptValue& value);
[[nodiscard]] ScriptValue            DefaultValue(PinType type); // false, 0, 0.0, (0,0,0), "", NullEntity (= self)
// Implicit conversions between connected pins: same type, Int <-> Float, Bool -> Int / Float,
// anything -> String. Exec only to Exec.
[[nodiscard]] bool        CanConvert(PinType from, PinType to);
[[nodiscard]] ScriptValue Convert(const ScriptValue& value, PinType to); // to a type CanConvert allows (else default)
[[nodiscard]] std::string ToDisplayString(const ScriptValue& value);

struct PinInfo {
    std::string name;
    PinType     type   = PinType::Exec;
    bool        output = false;
};

struct ScriptNode {
    std::uint32_t id = 0;       // unique in its graph, never reused
    std::string   type;         // NodeDesc::type, e.g. "Math.AddFloat"
    glm::vec2     position{0.0f}; // canvas units (editor)
    std::string   param;        // variable / key / event name, output count, shape (NodeDesc::param)
    std::map<std::string, ScriptValue> defaults; // values of unconnected data inputs (by pin name)
};

// Output pin -> input pin. Exec: one link per output, any number per input. Data: any number per
// output, one per input.
struct ScriptLink {
    std::uint32_t fromNode = 0;
    std::string   fromPin;
    std::uint32_t toNode = 0;
    std::string   toPin;

    bool operator==(const ScriptLink&) const = default;
};

struct ScriptComment {
    std::uint32_t id = 0;
    glm::vec2     position{0.0f};
    glm::vec2     size{300.0f, 200.0f};
    std::string   text = "Comment";
    glm::vec3     color{0.3f, 0.45f, 0.7f};
};

struct ScriptVariable {
    std::string name;
    PinType     type = PinType::Float;
    ScriptValue value = 0.0f; // initial value per instance
};

struct ScriptGraph {
    std::vector<ScriptNode>     nodes;
    std::vector<ScriptLink>     links;
    std::vector<ScriptComment>  comments;
    std::vector<ScriptVariable> variables;
    std::uint32_t               nextId = 1; // nodes and comments

    [[nodiscard]] ScriptNode*           FindNode(std::uint32_t id);
    [[nodiscard]] const ScriptNode*     FindNode(std::uint32_t id) const;
    [[nodiscard]] ScriptComment*        FindComment(std::uint32_t id);
    [[nodiscard]] ScriptVariable*       FindVariable(std::string_view name);
    [[nodiscard]] const ScriptVariable* FindVariable(std::string_view name) const;

    // Editing. AddNode takes the node type's default param when `param` is empty.
    std::uint32_t AddNode(const std::string& type, glm::vec2 position, std::string param = {});
    void          RemoveNode(std::uint32_t id); // and its links
    std::uint32_t AddComment(glm::vec2 position, glm::vec2 size, std::string text = "Comment");
    void          RemoveComment(std::uint32_t id);
    // Output pin -> input pin. Checks that both pins exist, type compatibility and self links;
    // replaces the existing link of a single-link pin. Returns an error message, empty on success.
    std::string Connect(std::uint32_t from, const std::string& fromPin, std::uint32_t to, const std::string& toPin);
    void        Disconnect(std::uint32_t node, const std::string& pin, bool output); // all links of the pin
    // Removes links whose pins no longer exist (node param / variable type changed).
    std::size_t RemoveDanglingLinks();
    // Variables: rename updates Get/Set nodes; type change resets the value and drops their links.
    bool RenameVariable(const std::string& from, const std::string& to);
    void SetVariableType(const std::string& name, PinType type);
};

// Pins of a node as the registry resolves them (some depend on the param or the variable's type).
// Unknown node types have none.
[[nodiscard]] std::vector<PinInfo> NodePins(const ScriptGraph& graph, const ScriptNode& node);
[[nodiscard]] std::optional<PinInfo> FindPin(const ScriptGraph& graph, const ScriptNode& node, std::string_view pin,
                                             bool output);

// JSON (.ugraph, "version": 1). Load throws std::runtime_error on format errors; unknown node
// types are kept (reported by validation).
[[nodiscard]] std::string ScriptGraphToJson(const ScriptGraph& graph);
[[nodiscard]] ScriptGraph ScriptGraphFromJson(const std::string& text);
void                      SaveScriptGraph(const std::filesystem::path& file, const ScriptGraph& graph); // via .tmp
[[nodiscard]] ScriptGraph LoadScriptGraph(const std::filesystem::path& file);

struct ScriptDiagnostic {
    std::uint32_t node  = 0; // 0: the graph itself
    std::string   message;
    bool          error = true; // false: warning (runs anyway)
};

// Unknown node types, missing / mistyped variables, duplicate names, links to missing pins or
// between incompatible types, several links on single-link pins, cycles through pure nodes; warnings
// for events without anything connected. A graph with errors does not run.
[[nodiscard]] std::vector<ScriptDiagnostic> ValidateScriptGraph(const ScriptGraph& graph);

} // namespace Engine
