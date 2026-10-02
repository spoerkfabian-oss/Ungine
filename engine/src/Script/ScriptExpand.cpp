#include "ScriptExpand.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptRegistry.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace Engine {

namespace {

constexpr int kMaxRounds = 32;

struct Expander {
    ScriptGraph&                                    graph;
    std::unordered_map<std::uint32_t, ScriptOrigin>& origins;
    std::unordered_set<std::string>                 localMacros; // definitions (not expanded in place, dropped)

    ScriptOrigin OriginOf(const ScriptNode& n) const
    {
        const auto it = origins.find(n.id);
        return it != origins.end() ? it->second : ScriptOrigin{{}, n.id, n.function, 0};
    }

    std::uint32_t Add(ScriptNode node, ScriptOrigin origin)
    {
        node.id = graph.nextId++;
        origins[node.id] = std::move(origin);
        graph.nodes.push_back(std::move(node));
        return graph.nodes.back().id;
    }

    // Nodes copied out of library `lib` refer to its functions / macros by their local names.
    static void Qualify(ScriptNode& n, const std::string& lib, const ScriptGraph& libGraph)
    {
        if (lib.empty())
            return;
        if (n.type == "Function.Call" || n.type == "Function.CallPure") {
            n.type  = n.type == "Function.Call" ? "Library.Call" : "Library.CallPure";
            n.param = lib + "." + n.param;
        } else if (n.type == "Macro.Use" && libGraph.FindMacro(n.param)) {
            n.param = lib + "." + n.param;
        }
    }

    // Copies "<Library>.<Function>" into the graph (once).
    void Import(const std::string& qualified)
    {
        if (graph.FindFunction(qualified))
            return;
        const std::size_t    dot = qualified.find('.');
        const std::string    lib = qualified.substr(0, dot);
        const ScriptLibrary* l   = ScriptRegistry::FindLibrary(lib);
        const ScriptFunction* f  = l ? l->graph.FindFunction(qualified.substr(dot + 1)) : nullptr;
        if (!f)
            return; // validated before
        ScriptFunction copy = *f;
        copy.name           = qualified;
        graph.functions.push_back(std::move(copy));
        std::unordered_map<std::uint32_t, std::uint32_t> ids;
        for (const ScriptNode& n : l->graph.nodes) {
            if (n.function != f->name)
                continue;
            ScriptNode c = n;
            c.function   = qualified;
            if (c.type == "Function.Entry" || c.type == "Function.Return")
                c.param = qualified;
            else
                Qualify(c, lib, l->graph);
            ids[n.id] = Add(std::move(c), {lib, n.id, f->name, 0});
        }
        for (const ScriptLink& link : l->graph.links)
            if (ids.contains(link.fromNode) && ids.contains(link.toNode))
                graph.links.push_back({ids[link.fromNode], link.fromPin, ids[link.toNode], link.toPin});
    }

    PinType PinTypeOf(std::uint32_t node, const std::string& pin, bool output) const
    {
        const ScriptNode* n = graph.FindNode(node);
        const auto        p = n ? FindPin(graph, *n, pin, output) : std::nullopt;
        return p ? p->type : PinType::Float;
    }

    // A data source routed through a Reroute node of `via` when its type differs (the conversion
    // then happens in two steps, like through the macro's pin).
    struct Source {
        std::uint32_t node = 0;
        std::string   pin;
        PinType       type;
    };
    Source Route(Source from, PinType via, const ScriptNode& use, const ScriptOrigin& origin)
    {
        if (from.type == via)
            return from;
        ScriptNode r;
        r.type     = "Utility.Reroute";
        r.param    = ToString(via);
        r.position = use.position;
        r.function = use.function;
        const std::uint32_t id = Add(std::move(r), origin);
        graph.links.push_back({from.node, from.pin, id, "In"});
        return {id, "Out", via};
    }

    void SetDefault(std::uint32_t node, const std::string& pin, const ScriptValue& value, PinType type)
    {
        const PinType to = PinTypeOf(node, pin, false);
        if (ScriptNode* n = graph.FindNode(node))
            n->defaults[pin] = CanConvert(type, to) ? ConvertFrom(value, type, to) : DefaultValue(to);
    }

    // Replaces a Macro node by a copy of the macro's nodes.
    void Expand(std::uint32_t useId)
    {
        const ScriptNode use = *graph.FindNode(useId);
        // The macro: of the graph itself, or of a library (its nodes refer to the library's names).
        const ScriptGraph* source = &graph;
        std::string        lib, local = use.param;
        if (!graph.FindMacro(use.param)) {
            const std::size_t dot = use.param.find('.');
            lib                   = use.param.substr(0, dot);
            local                 = use.param.substr(dot + 1);
            const ScriptLibrary* l = ScriptRegistry::FindLibrary(lib);
            if (!l)
                return;
            source = &l->graph;
        }
        const ScriptMacro* macroPtr = source->FindMacro(local);
        if (!macroPtr)
            return;
        const ScriptMacro macro = *macroPtr;
        std::vector<ScriptNode> body;
        for (const ScriptNode& n : source->nodes)
            if (n.function == local)
                body.push_back(n);
        std::vector<ScriptLink> bodyLinks;
        for (const ScriptLink& l : source->links)
            if (std::ranges::any_of(body, [&](const ScriptNode& n) { return n.id == l.fromNode; }))
                bodyLinks.push_back(l);
        const ScriptOrigin useOrigin = OriginOf(use);
        // The Macro node the debugger highlights: the outermost one in the graph itself.
        const std::uint32_t top = origins.contains(use.id) ? useOrigin.use : use.id;

        std::uint32_t inputs = 0, outputs = 0;
        std::unordered_map<std::uint32_t, std::uint32_t> ids;
        for (const ScriptNode& n : body) {
            if (n.type == "Macro.Inputs") {
                inputs = n.id;
                continue;
            }
            if (n.type == "Macro.Outputs") {
                outputs = n.id;
                continue;
            }
            ScriptNode c = n;
            c.function   = use.function;
            Qualify(c, lib, *source);
            ids[n.id] = Add(std::move(c), {lib, n.id, local, top}); // definitions are never copies
        }

        // The Macro node's own links.
        std::vector<ScriptLink> into, outOf;
        for (const ScriptLink& l : graph.links) {
            if (l.toNode == use.id)
                into.push_back(l);
            if (l.fromNode == use.id)
                outOf.push_back(l);
        }
        std::erase_if(graph.links, [&](const ScriptLink& l) { return l.toNode == use.id || l.fromNode == use.id; });
        const auto typeOf = [](const std::vector<ScriptParam>& params, const std::string& name) {
            const auto it = std::ranges::find(params, name, &ScriptParam::name);
            return it != params.end() ? it->type : PinType::Float;
        };
        const auto inputValue = [&](const std::string& pin) -> ScriptValue {
            const auto it = use.defaults.find(pin);
            const PinType t = typeOf(macro.inputs, pin);
            return it != use.defaults.end() ? Convert(it->second, t) : DefaultValue(t);
        };
        // Data arriving at macro input `pin`: the outside source (routed through the pin's type) or nothing.
        std::unordered_map<std::string, std::optional<Source>> routed;
        const auto inputSource = [&](const std::string& pin) -> std::optional<Source> {
            if (const auto r = routed.find(pin); r != routed.end())
                return r->second;
            const auto it = std::ranges::find(into, pin, &ScriptLink::toPin);
            std::optional<Source> s;
            if (it != into.end())
                s = Route({it->fromNode, it->fromPin, PinTypeOf(it->fromNode, it->fromPin, true)}, typeOf(macro.inputs, pin),
                          use, useOrigin);
            return routed[pin] = s;
        };

        for (const ScriptLink& l : bodyLinks) {
            const bool fromInputs = l.fromNode == inputs, toOutputs = l.toNode == outputs && outputs != 0;
            if (!fromInputs && !toOutputs) {
                if (ids.contains(l.fromNode) && ids.contains(l.toNode))
                    graph.links.push_back({ids[l.fromNode], l.fromPin, ids[l.toNode], l.toPin});
                continue;
            }
            if (fromInputs && !toOutputs) { // macro input -> body
                const PinType t = typeOf(macro.inputs, l.fromPin);
                if (t.kind == PinKind::Exec) {
                    for (const ScriptLink& o : into)
                        if (o.toPin == l.fromPin)
                            graph.links.push_back({o.fromNode, o.fromPin, ids[l.toNode], l.toPin});
                } else if (const auto src = inputSource(l.fromPin)) {
                    graph.links.push_back({src->node, src->pin, ids[l.toNode], l.toPin});
                } else {
                    SetDefault(ids[l.toNode], l.toPin, inputValue(l.fromPin), t);
                }
                continue;
            }
            const PinType t = typeOf(macro.outputs, l.toPin);
            if (!fromInputs) { // body -> macro output
                for (const ScriptLink& o : outOf) {
                    if (o.fromPin != l.toPin)
                        continue;
                    if (t.kind == PinKind::Exec) {
                        graph.links.push_back({ids[l.fromNode], l.fromPin, o.toNode, o.toPin});
                    } else {
                        const Source s = Route({ids[l.fromNode], l.fromPin, PinTypeOf(ids[l.fromNode], l.fromPin, true)}, t,
                                               use, useOrigin);
                        graph.links.push_back({s.node, s.pin, o.toNode, o.toPin});
                    }
                }
                continue;
            }
            // Straight through: macro input -> macro output.
            for (const ScriptLink& o : outOf) {
                if (o.fromPin != l.toPin)
                    continue;
                if (t.kind == PinKind::Exec) {
                    for (const ScriptLink& i : into)
                        if (i.toPin == l.fromPin)
                            graph.links.push_back({i.fromNode, i.fromPin, o.toNode, o.toPin});
                } else if (auto src = inputSource(l.fromPin)) {
                    const Source s = Route(*src, t, use, useOrigin);
                    graph.links.push_back({s.node, s.pin, o.toNode, o.toPin});
                } else {
                    const PinType in = typeOf(macro.inputs, l.fromPin);
                    SetDefault(o.toNode, o.toPin, CanConvert(in, t) ? ConvertFrom(inputValue(l.fromPin), in, t) : DefaultValue(t), t);
                }
            }
        }
        // Data outputs with nothing connected inside: the Outputs node's value.
        const ScriptNode* outputsNode = nullptr;
        for (const ScriptNode& n : body)
            if (n.id == outputs)
                outputsNode = &n;
        for (const ScriptParam& p : macro.outputs) {
            if (p.type.kind == PinKind::Exec ||
                std::ranges::any_of(bodyLinks, [&](const ScriptLink& l) { return l.toNode == outputs && l.toPin == p.name; }))
                continue;
            ScriptValue value = DefaultValue(p.type);
            if (outputsNode)
                if (const auto it = outputsNode->defaults.find(p.name); it != outputsNode->defaults.end())
                    value = Convert(it->second, p.type);
            for (const ScriptLink& o : outOf)
                if (o.fromPin == p.name)
                    SetDefault(o.toNode, o.toPin, value, p.type);
        }
        std::erase_if(graph.nodes, [&](const ScriptNode& n) { return n.id == use.id; });
        origins.erase(use.id);
    }

    bool InDefinition(const ScriptNode& n) const { return localMacros.contains(n.function); }

    std::string Run()
    {
        for (const ScriptMacro& m : graph.macros)
            localMacros.insert(m.name);
        for (int round = 0;; ++round) {
            bool changed = false;
            // Library calls: copy the functions in (their bodies may call more: the loop sees the
            // appended nodes too), then call them like the graph's own functions.
            for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                if ((graph.nodes[i].type != "Library.Call" && graph.nodes[i].type != "Library.CallPure") ||
                    InDefinition(graph.nodes[i]))
                    continue;
                const std::string qualified = graph.nodes[i].param;
                Import(qualified);
                ScriptNode& n = graph.nodes[i];
                n.type        = n.type == "Library.Call" ? "Function.Call" : "Function.CallPure";
                changed       = true;
            }
            std::vector<std::uint32_t> uses;
            for (const ScriptNode& n : graph.nodes)
                if (n.type == "Macro.Use" && !InDefinition(n))
                    uses.push_back(n.id);
            for (std::uint32_t id : uses)
                Expand(id);
            changed = changed || !uses.empty();
            if (!changed)
                break;
            if (round == kMaxRounds)
                return "Macros nested too deep (a macro using itself?)";
        }
        std::erase_if(graph.links, [&](const ScriptLink& l) {
            const ScriptNode* from = graph.FindNode(l.fromNode);
            return !from || InDefinition(*from);
        });
        std::erase_if(graph.nodes, [&](const ScriptNode& n) { return InDefinition(n); });
        std::erase_if(graph.comments, [&](const ScriptComment& c) { return localMacros.contains(c.function); });
        graph.macros.clear();
        return {};
    }
};

} // namespace

std::string ExpandScriptGraph(ScriptGraph& graph, std::unordered_map<std::uint32_t, ScriptOrigin>& origins)
{
    return Expander{graph, origins, {}}.Run();
}

} // namespace Engine
