#pragma once
#include "Engine/Script/ScriptGraph.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

// Where a node of an expanded graph comes from (debugger: highlights, breakpoints, errors).
struct ScriptOrigin {
    std::string   library;  // empty: the graph itself, else the library it was copied from
    std::uint32_t node = 0; // node id there
    std::string   function; // its function / macro there (empty: event graph)
    std::uint32_t use  = 0; // the Macro node of the graph itself it was copied for (0: none)
    std::string   file;     // debugger key of the parent class graph it comes from (empty: see library)
    std::string   useFile;  // debugger key of `use`'s graph (empty: the graph itself)
};

// Inheritance: merges the resolved parent chain (graph.inherited) into the graph: the ancestors'
// nodes are added with new ids (origins: their class file via `keyOf`), overridden functions and
// events of a parent become "Super<n>:<name>" (functions) / custom events "Super<n>:<event key>",
// Call Parent nodes call them. `classes` receives the ancestors' keys (nearest first).
void FlattenScriptInheritance(ScriptGraph& graph, std::unordered_map<std::uint32_t, ScriptOrigin>& origins,
                              const std::function<std::string(const std::filesystem::path&)>& keyOf,
                              std::vector<std::string>& classes);

// The graph as it runs: used library functions copied in (named "<Library>.<Function>"; library
// calls become function calls), Macro nodes replaced by copies of the macro's nodes (rewired to
// the Macro node's links; repeated for macros using macros), macro definitions dropped. Every new
// node gets an entry in `origins`. Returns an error message (empty: ok). Expects a valid graph.
[[nodiscard]] std::string ExpandScriptGraph(ScriptGraph& graph, std::unordered_map<std::uint32_t, ScriptOrigin>& origins);

// ValidateScriptGraph for expanded graphs (allows the dotted function names, no warnings).
[[nodiscard]] std::vector<ScriptDiagnostic> ValidateExpandedScriptGraph(const ScriptGraph& graph);

} // namespace Engine
