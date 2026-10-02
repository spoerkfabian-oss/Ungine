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
};

// The graph as it runs: used library functions copied in (named "<Library>.<Function>"; library
// calls become function calls), Macro nodes replaced by copies of the macro's nodes (rewired to
// the Macro node's links; repeated for macros using macros), macro definitions dropped. Every new
// node gets an entry in `origins`. Returns an error message (empty: ok). Expects a valid graph.
[[nodiscard]] std::string ExpandScriptGraph(ScriptGraph& graph, std::unordered_map<std::uint32_t, ScriptOrigin>& origins);

// ValidateScriptGraph for expanded graphs (allows the dotted function names, no warnings).
[[nodiscard]] std::vector<ScriptDiagnostic> ValidateExpandedScriptGraph(const ScriptGraph& graph);

} // namespace Engine
