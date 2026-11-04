// Blueprint inheritance: resolving the parent chain (editor, validation) and merging it into one
// graph before compiling (the VM itself knows nothing about classes except IsA).
#include "ScriptExpand.h"
#include "Engine/Core/Platform.h"
#include "Engine/Script/ScriptNodes.h"

#include <algorithm>
#include <unordered_set>

namespace Engine {

namespace {

constexpr std::size_t kMaxParentDepth = 16;

std::filesystem::path Normal(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::absolute(p, ec).lexically_normal();
}

bool IsEventNode(const ScriptNode& n)
{
    const NodeDesc* desc = FindScriptNodeType(n.type);
    return n.function.empty() && desc && desc->kind == NodeKind::Event && n.type != "Function.Entry" &&
           n.type != "Macro.Inputs";
}

std::string Resolve(ScriptGraph& graph, const std::filesystem::path& file, const ScriptGraphLoader& load,
                    std::vector<std::filesystem::path>& stack)
{
    graph.inherited.reset();
    graph.inheritedFile.clear();
    if (graph.parent.empty())
        return {};
    if (graph.library)
        return "A library cannot have a parent class";
    if (stack.size() > kMaxParentDepth)
        return "Parent classes nested too deep (more than 16)";
    const std::filesystem::path parentFile = (file.parent_path() / PathFromUtf8(graph.parent)).lexically_normal();
    const std::filesystem::path normal     = Normal(parentFile);
    if (std::ranges::find(stack, normal) != stack.end())
        return "Parent class cycle through '" + graph.parent + "'";
    ScriptGraph parent;
    if (std::optional<ScriptGraph> provided = load ? load(parentFile) : std::nullopt) {
        parent = std::move(*provided);
    } else {
        try {
            parent = LoadScriptGraph(parentFile);
        } catch (const std::exception& e) {
            return "Parent class: " + std::string(e.what());
        }
    }
    if (parent.library)
        return "Parent class '" + graph.parent + "' is a library";
    stack.push_back(normal);
    std::string problem = Resolve(parent, parentFile, load, stack);
    stack.pop_back();
    if (!problem.empty())
        return "Parent class '" + graph.parent + "': " + problem;
    graph.inherited     = std::make_shared<const ScriptGraph>(std::move(parent));
    graph.inheritedFile = parentFile;
    return {};
}

template <class T>
bool HasName(const std::vector<T>& list, std::string_view name)
{
    return std::ranges::any_of(list, [&](const T& x) { return x.name == name; });
}

// Merges graph.inherited (recursively) into `graph`; ancestors' nodes get new ids and an origin.
void Merge(ScriptGraph& graph, std::unordered_map<std::uint32_t, ScriptOrigin>& origins,
           const std::function<std::string(const std::filesystem::path&)>& keyOf, std::vector<std::string>& classes)
{
    if (!graph.inherited)
        return;
    std::size_t depth = 0;
    for (const ScriptGraph* g = &graph; g->inherited; g = g->inherited.get())
        ++depth;
    const std::string super = "Super" + std::to_string(depth) + ":";

    ScriptGraph                                     parent = *graph.inherited;
    std::unordered_map<std::uint32_t, ScriptOrigin> parentOrigins;
    const std::string                               parentKey = keyOf(graph.inheritedFile);
    classes.push_back(parentKey);
    Merge(parent, parentOrigins, keyOf, classes);
    // Origins of the parent's nodes before any renaming (the debugger shows the parent's names).
    std::unordered_map<std::uint32_t, ScriptOrigin> before;
    for (const ScriptNode& n : parent.nodes) {
        const auto it = parentOrigins.find(n.id);
        before[n.id]  = it != parentOrigins.end() ? it->second : ScriptOrigin{{}, n.id, n.function, 0, parentKey, {}};
    }

    // Overridden functions: the parent's definition becomes "Super<depth>:<name>" (calls stay
    // virtual: they reach the child's version).
    for (const ScriptFunction& f : graph.functions) {
        const auto it = std::ranges::find(parent.functions, f.name, &ScriptFunction::name);
        if (it == parent.functions.end())
            continue;
        const std::string renamed = super + f.name;
        it->name                  = renamed;
        for (ScriptNode& n : parent.nodes)
            if (n.function == f.name) {
                n.function = renamed;
                if (n.type == "Function.Entry" || n.type == "Function.Return")
                    n.param = renamed;
            }
        for (ScriptComment& c : parent.comments)
            if (c.function == f.name)
                c.function = renamed;
    }
    // Overridden events: the parent's handlers become a custom event "Super<depth>:<key>" with the
    // event's outputs as parameters.
    std::unordered_set<std::string> overridden;
    for (const ScriptNode& n : graph.nodes)
        if (IsEventNode(n))
            overridden.insert(ScriptEventKey(n));
    for (ScriptNode& n : parent.nodes) {
        if (!IsEventNode(n) || !overridden.contains(ScriptEventKey(n)))
            continue;
        const std::string name = super + ScriptEventKey(n);
        if (!HasName(parent.events, name)) {
            ScriptEventDecl decl{name, {}};
            for (const PinInfo& p : NodePins(parent, n))
                if (p.output && p.type != PinType::Exec)
                    decl.params.push_back({p.name, p.type});
            parent.events.push_back(std::move(decl));
        }
        n.type  = "Event.Custom";
        n.param = name;
    }
    // The child's Call Parent nodes call the renamed versions.
    for (ScriptNode& n : graph.nodes) {
        if (n.type == "Function.CallParent" || n.type == "Function.CallParentPure") {
            n.type  = n.type == "Function.CallParent" ? "Function.Call" : "Function.CallPure";
            n.param = super + n.param;
        } else if (n.type == "Flow.CallParentEvent") {
            n.type  = "Flow.CallEvent";
            n.param = super + n.param;
        }
    }

    // The parent's nodes and comments get ids after the child's.
    std::unordered_map<std::uint32_t, std::uint32_t> ids;
    for (ScriptNode& n : parent.nodes) {
        const std::uint32_t id = graph.nextId++;
        ids[n.id]              = id;
        origins[id]            = before.at(n.id);
        n.id                   = id;
    }
    for (ScriptComment& c : parent.comments)
        c.id = graph.nextId++;
    for (ScriptLink& l : parent.links) {
        l.fromNode = ids.at(l.fromNode);
        l.toNode   = ids.at(l.toNode);
    }

    const auto prepend = [](auto& own, auto& inherited) {
        own.insert(own.begin(), std::make_move_iterator(inherited.begin()), std::make_move_iterator(inherited.end()));
    };
    prepend(graph.variables, parent.variables);
    for (const auto& [name, value] : graph.defaults)
        if (const auto v = std::ranges::find(graph.variables, name, &ScriptVariable::name); v != graph.variables.end())
            v->value = ValueFits(value, v->type) ? value : Convert(value, v->type);
    prepend(graph.functions, parent.functions);
    prepend(graph.macros, parent.macros);
    prepend(graph.events, parent.events);
    prepend(graph.dispatchers, parent.dispatchers);
    prepend(graph.timelines, parent.timelines);
    for (const std::string& i : parent.interfaces)
        if (std::ranges::find(graph.interfaces, i) == graph.interfaces.end())
            graph.interfaces.push_back(i);
    prepend(graph.nodes, parent.nodes);
    prepend(graph.links, parent.links);
    prepend(graph.comments, parent.comments);
    graph.defaults.clear();
    graph.parent.clear();
    graph.inherited.reset();
    graph.inheritedFile.clear();
}

} // namespace

std::string ResolveScriptParent(ScriptGraph& graph, const std::filesystem::path& file, const ScriptGraphLoader& load)
{
    std::vector<std::filesystem::path> stack{Normal(file)};
    return Resolve(graph, file, load, stack);
}

std::string ScriptParentPath(const std::filesystem::path& childFile, const std::filesystem::path& parentFile)
{
    const std::filesystem::path rel = Normal(parentFile).lexically_relative(Normal(childFile).parent_path());
    const std::u8string         s   = (rel.empty() ? Normal(parentFile) : rel).generic_u8string();
    return {s.begin(), s.end()};
}

std::vector<std::filesystem::path> ScriptParentChain(const ScriptGraph& graph)
{
    std::vector<std::filesystem::path> chain;
    for (const ScriptGraph* g = &graph; g->inherited; g = g->inherited.get())
        chain.push_back(g->inheritedFile);
    return chain;
}

std::string DispatcherSignatureMismatch(const ScriptEventDecl& dispatcher, const ScriptEventDecl* event)
{
    static const std::vector<ScriptParam> none;
    const std::vector<ScriptParam>&       params = event ? event->params : none;
    const std::string                     name   = event ? "'" + event->name + "'" : std::string("the event");
    if (params.size() != dispatcher.params.size())
        return "Dispatcher '" + dispatcher.name + "' passes " + std::to_string(dispatcher.params.size()) +
               " argument(s), " + name + " takes " + std::to_string(params.size());
    for (std::size_t i = 0; i < params.size(); ++i)
        if (!CanConvert(dispatcher.params[i].type, params[i].type))
            return "Dispatcher '" + dispatcher.name + "', argument " + std::to_string(i + 1) + ": " +
                   DisplayName(dispatcher.params[i].type) + " does not fit " + name + "'s " + DisplayName(params[i].type);
    return {};
}

std::string ScriptEventKey(const ScriptNode& node) { return node.param.empty() ? node.type : node.type + ":" + node.param; }

bool InheritedHandlesEvent(const ScriptGraph& graph, std::string_view key)
{
    for (const ScriptGraph* g = graph.inherited.get(); g; g = g->inherited.get())
        if (std::ranges::any_of(g->nodes, [&](const ScriptNode& n) { return IsEventNode(n) && ScriptEventKey(n) == key; }))
            return true;
    return false;
}

void FlattenScriptInheritance(ScriptGraph& graph, std::unordered_map<std::uint32_t, ScriptOrigin>& origins,
                              const std::function<std::string(const std::filesystem::path&)>& keyOf,
                              std::vector<std::string>& classes)
{
    Merge(graph, origins, keyOf, classes);
}

} // namespace Engine
