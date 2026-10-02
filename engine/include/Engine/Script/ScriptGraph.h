#pragma once
#include "Engine/Script/ScriptValue.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

// Visual scripting ("Blueprints"): a graph of nodes connected by exec links (control flow) and
// data links (values). The graph is plain data, edited by the editor's graph panel, stored as JSON
// (.ugraph) and run by ScriptSystem. Node types and their pins come from the registry
// (ScriptNodes.h). Besides the event graph, a graph has functions: their nodes carry the function's
// name (ScriptNode::function), start at a Function Entry node and end at Return nodes.

struct PinInfo {
    std::string name;
    PinType     type   = PinType::Exec;
    bool        output = false;
};

struct ScriptNode {
    std::uint32_t id = 0;       // unique in its graph, never reused
    std::string   type;         // NodeDesc::type, e.g. "Math.AddFloat"
    glm::vec2     position{0.0f}; // canvas units (editor)
    std::string   param;        // variable / key / event / function name, count, type... (NodeDesc::param)
    std::map<std::string, ScriptValue> defaults; // values of unconnected data inputs (by pin name)
    std::string   function;     // the function this node belongs to; empty: the event graph
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
    std::string   function; // like ScriptNode::function
};

struct ScriptVariable {
    std::string name;
    PinType     type = PinType::Float;
    ScriptValue value = 0.0f; // initial value per instance
    bool        exposed = false; // "instance editable": overridden per entity (ScriptComponent::variables)
};

struct ScriptParam {
    std::string name;
    PinType     type = PinType::Float;
};

// A function of the graph. Impure functions are called from exec chains (Call node with exec pins),
// pure ones whenever an output is read. No latent nodes (Delay) and no recursion.
struct ScriptFunction {
    std::string                 name;
    std::vector<ScriptParam>    inputs;  // Entry node outputs / Call node inputs
    std::vector<ScriptParam>    outputs; // Return node inputs / Call node outputs
    std::vector<ScriptVariable> locals;  // reset at every call
    bool                        pure = false;
    std::string                 description;
};

struct ScriptGraph {
    std::vector<ScriptNode>     nodes;     // all scopes (event graph and functions)
    std::vector<ScriptLink>     links;     // between nodes of the same scope
    std::vector<ScriptComment>  comments;
    std::vector<ScriptVariable> variables;
    std::vector<ScriptFunction> functions;
    std::vector<std::uint32_t>  breakpoints; // node ids (debugger; saved with the graph)
    std::uint32_t               nextId = 1; // nodes and comments

    [[nodiscard]] ScriptNode*           FindNode(std::uint32_t id);
    [[nodiscard]] const ScriptNode*     FindNode(std::uint32_t id) const;
    [[nodiscard]] ScriptComment*        FindComment(std::uint32_t id);
    [[nodiscard]] ScriptVariable*       FindVariable(std::string_view name);
    [[nodiscard]] const ScriptVariable* FindVariable(std::string_view name) const;
    [[nodiscard]] ScriptFunction*       FindFunction(std::string_view name);
    [[nodiscard]] const ScriptFunction* FindFunction(std::string_view name) const;
    // A variable as seen from a scope: the function's locals first, then the graph's variables.
    [[nodiscard]] const ScriptVariable* FindVariableInScope(std::string_view function, std::string_view name) const;
    [[nodiscard]] ScriptVariable*       FindVariableInScope(std::string_view function, std::string_view name);

    // Editing. AddNode takes the node type's default param when `param` is empty.
    std::uint32_t AddNode(const std::string& type, glm::vec2 position, std::string param = {},
                          std::string function = {});
    void          RemoveNode(std::uint32_t id); // and its links
    std::uint32_t AddComment(glm::vec2 position, glm::vec2 size, std::string text = "Comment", std::string function = {});
    void          RemoveComment(std::uint32_t id);
    // Output pin -> input pin. Checks that both pins exist, type compatibility and self links;
    // replaces the existing link of a single-link pin. Returns an error message, empty on success.
    std::string Connect(std::uint32_t from, const std::string& fromPin, std::uint32_t to, const std::string& toPin);
    void        Disconnect(std::uint32_t node, const std::string& pin, bool output); // all links of the pin
    // Removes links whose pins no longer exist (node param / variable type changed).
    std::size_t RemoveDanglingLinks();
    // Variables (graph variables, or the locals of `function`): rename updates the nodes using
    // them; a type change resets the value and drops links that no longer fit.
    bool RenameVariable(const std::string& from, const std::string& to, const std::string& function = {});
    void SetVariableType(const std::string& name, PinType type, const std::string& function = {});
    // Functions. AddFunction creates the function with its Entry and Return node (name must be
    // free); rename / remove update every node of it and the calls; signature edits drop links
    // to pins that no longer exist. SetFunctionPure converts the call nodes.
    bool AddFunction(const std::string& name, glm::vec2 entryPosition = {0.0f, 0.0f});
    bool RenameFunction(const std::string& from, const std::string& to);
    void RemoveFunction(const std::string& name); // its nodes and comments and every call
    void SetFunctionPure(const std::string& name, bool pure);
    void FunctionSignatureChanged(const std::string& name); // after editing inputs / outputs
    [[nodiscard]] bool HasBreakpoint(std::uint32_t node) const;
    void               SetBreakpoint(std::uint32_t node, bool enabled);
};

[[nodiscard]] bool IsValidScriptName(std::string_view name); // letters, digits, '_', ' ' (not empty)

// Pins of a node as the registry resolves them (some depend on the param or the variable's type).
// Unknown node types have none.
[[nodiscard]] std::vector<PinInfo> NodePins(const ScriptGraph& graph, const ScriptNode& node);
[[nodiscard]] std::optional<PinInfo> FindPin(const ScriptGraph& graph, const ScriptNode& node, std::string_view pin,
                                             bool output);

// JSON (.ugraph, "version": 2; version 1 files load). Load throws std::runtime_error on format
// errors; unknown node types are kept (reported by validation).
[[nodiscard]] std::string ScriptGraphToJson(const ScriptGraph& graph);
[[nodiscard]] ScriptGraph ScriptGraphFromJson(const std::string& text);
void                      SaveScriptGraph(const std::filesystem::path& file, const ScriptGraph& graph); // via .tmp
[[nodiscard]] ScriptGraph LoadScriptGraph(const std::filesystem::path& file);

struct ScriptDiagnostic {
    std::uint32_t node  = 0; // 0: the graph itself
    std::string   message;
    bool          error = true; // false: warning (runs anyway)
};

// Unknown node types, missing / mistyped variables, duplicate names, links to missing pins, between
// incompatible types or across scopes, several links on single-link pins, cycles through pure nodes,
// function errors (unknown, Entry count, events / latent nodes inside, recursion, call purity);
// warnings for events without anything connected. A graph with errors does not run.
[[nodiscard]] std::vector<ScriptDiagnostic> ValidateScriptGraph(const ScriptGraph& graph);

} // namespace Engine
