#pragma once
// Shared helpers of the blueprint tests: graph building, a script runner without a renderer.
#include "Test.h"

#include "Engine/Events/EventBus.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptSystem.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace BlueprintTest {

using namespace Engine;

struct Graph {
    ScriptGraph g;

    std::uint32_t Node(const char* type, std::string param = {}, std::string function = {})
    {
        return g.AddNode(type, {}, std::move(param), std::move(function));
    }
    void Link(std::uint32_t from, const char* fromPin, std::uint32_t to, const char* toPin)
    {
        const std::string error = g.Connect(from, fromPin, to, toPin);
        CHECK(error.empty());
        if (!error.empty())
            std::printf("  connect %s -> %s: %s\n", fromPin, toPin, error.c_str());
    }
    void Set(std::uint32_t node, const char* pin, ScriptValue value) { g.FindNode(node)->defaults[pin] = std::move(value); }
    std::uint32_t Find(const char* type, const std::string& function) const
    {
        for (const ScriptNode& n : g.nodes)
            if (n.type == type && n.function == function)
                return n.id;
        return 0;
    }
    bool Valid() const
    {
        const auto d = ValidateScriptGraph(g);
        for (const ScriptDiagnostic& x : d)
            if (x.error)
                std::printf("  diagnostic (node %u): %s\n", x.node, x.message.c_str());
        return std::ranges::none_of(d, [](const ScriptDiagnostic& x) { return x.error; });
    }
};

inline bool HasError(const ScriptGraph& g, std::string_view text)
{
    return std::ranges::any_of(ValidateScriptGraph(g), [&](const ScriptDiagnostic& d) {
        return d.error && d.message.find(text) != std::string::npos;
    });
}

struct Runner {
    EventBus     bus;
    Scene        scene;
    ScriptSystem scripts{bus};

    Entity Add(const char* name, const char* file, const ScriptGraph& graph)
    {
        const Entity e = scene.CreateEntity(name);
        scene.GetRegistry().Emplace<ScriptComponent>(e, ScriptComponent{file});
        scripts.Provide(file, graph);
        return e;
    }
    void Run(float seconds, float dt = 0.1f)
    {
        for (float t = 0.0f; t < seconds - 1e-4f; t += dt)
            scripts.Update(scene, dt);
    }
    bool Printed(std::string_view text) const
    {
        return std::ranges::any_of(scripts.Messages(), [&](const ScriptMessage& m) { return m.text == text; });
    }
    ScriptValue Var(const char* file, Entity e, std::string_view name) const
    {
        const auto w = scripts.Watch(file, e);
        if (w)
            for (const auto& [n, v] : w->variables)
                if (n == name)
                    return v;
        return false;
    }
};

inline std::uint32_t Print(Graph& g, const char* text = "", std::string function = {})
{
    const std::uint32_t n = g.Node("Debug.Print", {}, std::move(function));
    g.Set(n, "Text", std::string(text));
    g.Set(n, "Duration", 1000.0f);
    return n;
}


} // namespace BlueprintTest
