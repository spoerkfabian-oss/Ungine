#include "Engine/Core/FileSystem.h"
#include "Engine/Material/MaterialGraph.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace Engine {

namespace {

MaterialPinDesc Pin(std::string name, std::uint32_t width, glm::vec4 fallback = glm::vec4(0.0f), std::string implicit = {})
{
    return {std::move(name), width, fallback, std::move(implicit)};
}

glm::vec4 S(float x) { return glm::vec4(x, 0.0f, 0.0f, 0.0f); }

std::vector<MaterialPinDesc> ColorOutputs()
{
    return {Pin("RGB", 3), Pin("R", 1), Pin("G", 1), Pin("B", 1), Pin("A", 1), Pin("RGBA", 4)};
}

std::vector<MaterialNodeDesc> BuildNodeTypes()
{
    std::vector<MaterialNodeDesc> t;
    const auto add = [&](MaterialNodeDesc d) { t.push_back(std::move(d)); };

    add({kMaterialOutputNode, "Material", "Output", "Inputs of the material.",
         {Pin("BaseColor", 3, glm::vec4(0.8f, 0.8f, 0.8f, 0.0f)), Pin("Metallic", 1), Pin("Roughness", 1, S(0.5f)),
          Pin("Normal", 3, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)), Pin("Emissive", 3), Pin("Opacity", 1, S(1.0f)),
          Pin("OpacityMask", 1, S(1.0f)), Pin("AmbientOcclusion", 1, S(1.0f)), Pin("WorldPositionOffset", 3)},
         {}, MaterialNodeParam::None, MaterialNodeValue::None, false, true});

    // Constants
    add({"Constant", "Constant", "Constants", "A number.", {}, {Pin("Value", 1)}, MaterialNodeParam::None,
         MaterialNodeValue::Float});
    add({"Constant2", "Constant 2", "Constants", "A 2D vector.", {}, {Pin("Value", 2)}, MaterialNodeParam::None,
         MaterialNodeValue::Float2});
    add({"Constant3", "Constant 3", "Constants", "A 3D vector.", {}, {Pin("Value", 3)}, MaterialNodeParam::None,
         MaterialNodeValue::Float3});
    add({"Color", "Color", "Constants", "A color (linear RGBA).", {}, ColorOutputs(), MaterialNodeParam::None,
         MaterialNodeValue::Color});

    // Parameters
    add({"Param.Scalar", "Scalar Parameter", "Parameters", "A number instances and blueprints can change.", {},
         {Pin("Value", 1)}, MaterialNodeParam::Parameter});
    add({"Param.Vector", "Vector Parameter", "Parameters", "A color / vector instances and blueprints can change.", {},
         ColorOutputs(), MaterialNodeParam::Parameter});
    add({"Param.Texture", "Texture Parameter", "Parameters", "Samples a texture instances can replace.",
         {Pin("UV", 2, glm::vec4(0.0f), "UV0")}, ColorOutputs(), MaterialNodeParam::Parameter});

    // Textures
    add({"Texture.Sample", "Texture Sample", "Textures", "Samples a texture file (normal maps: decoded normal in RGB).",
         {Pin("UV", 2, glm::vec4(0.0f), "UV0")}, ColorOutputs(), MaterialNodeParam::Texture});

    // Scene inputs
    add({"Input.TexCoord", "Texture Coordinate", "Inputs", "Mesh UVs times the tiling.", {}, {Pin("UV", 2)},
         MaterialNodeParam::None, MaterialNodeValue::Float2});
    add({"Input.WorldPosition", "World Position", "Inputs", "Position of the surface (vertex stage: before the offset).",
         {}, {Pin("Position", 3)}});
    add({"Input.ObjectPosition", "Object Position", "Inputs", "Origin of the mesh instance.", {}, {Pin("Position", 3)}});
    add({"Input.VertexNormal", "Vertex Normal", "Inputs", "World space normal of the mesh.", {}, {Pin("Normal", 3)}});
    add({"Input.CameraPosition", "Camera Position", "Inputs", "World position of the camera.", {}, {Pin("Position", 3)}});
    add({"Input.CameraVector", "Camera Vector", "Inputs", "Direction from the surface to the camera.", {},
         {Pin("Direction", 3)}});
    add({"Input.Time", "Time", "Inputs", "Seconds since the renderer started.", {}, {Pin("Time", 1)}});
    add({"Input.ScreenPosition", "Screen Position", "Inputs", "Pixel position in 0..1 (pixel stage only).", {},
         {Pin("UV", 2)}, MaterialNodeParam::None, MaterialNodeValue::None, true});
    add({"Input.PixelDepth", "Pixel Depth", "Inputs", "Distance from the camera plane (pixel stage only).", {},
         {Pin("Depth", 1)}, MaterialNodeParam::None, MaterialNodeValue::None, true});

    // Math: generic width (0) follows the widest input; scalars broadcast.
    const auto binary = [&](const char* type, const char* title, const char* text, glm::vec4 a, glm::vec4 b) {
        add({type, title, "Math", text, {Pin("A", 0, a), Pin("B", 0, b)}, {Pin("Result", 0)}});
    };
    binary("Math.Add", "Add", "A + B", S(0.0f), S(0.0f));
    binary("Math.Subtract", "Subtract", "A - B", S(0.0f), S(0.0f));
    binary("Math.Multiply", "Multiply", "A * B", S(1.0f), S(1.0f));
    binary("Math.Divide", "Divide", "A / B", S(1.0f), S(1.0f));
    binary("Math.Min", "Min", "Smaller of A and B", S(0.0f), S(0.0f));
    binary("Math.Max", "Max", "Larger of A and B", S(0.0f), S(0.0f));
    binary("Math.Modulo", "Modulo", "A modulo B", S(0.0f), S(1.0f));
    add({"Math.Power", "Power", "Math", "Base ^ Exponent", {Pin("Base", 0, S(1.0f)), Pin("Exponent", 0, S(2.0f))},
         {Pin("Result", 0)}});
    add({"Math.Lerp", "Lerp", "Math", "Blend from A to B by Alpha",
         {Pin("A", 0, S(0.0f)), Pin("B", 0, S(1.0f)), Pin("Alpha", 0, S(0.5f))}, {Pin("Result", 0)}});
    add({"Math.Clamp", "Clamp", "Math", "Value limited to Min..Max",
         {Pin("Value", 0), Pin("Min", 0, S(0.0f)), Pin("Max", 0, S(1.0f))}, {Pin("Result", 0)}});
    add({"Math.Step", "Step", "Math", "0 below Edge, else 1", {Pin("Edge", 0, S(0.5f)), Pin("X", 0)}, {Pin("Result", 0)}});
    add({"Math.SmoothStep", "Smooth Step", "Math", "Smooth 0..1 between Min and Max",
         {Pin("Min", 0, S(0.0f)), Pin("Max", 0, S(1.0f)), Pin("X", 0)}, {Pin("Result", 0)}});
    const auto unary = [&](const char* type, const char* title, const char* text) {
        add({type, title, "Math", text, {Pin("X", 0)}, {Pin("Result", 0)}});
    };
    unary("Math.Saturate", "Saturate", "Clamped to 0..1");
    unary("Math.OneMinus", "One Minus", "1 - X");
    unary("Math.Abs", "Abs", "Absolute value");
    unary("Math.Floor", "Floor", "Rounded down");
    unary("Math.Ceil", "Ceil", "Rounded up");
    unary("Math.Frac", "Frac", "Fractional part");
    unary("Math.Sin", "Sine", "sin(X), radians");
    unary("Math.Cos", "Cosine", "cos(X), radians");
    unary("Math.Sqrt", "Square Root", "sqrt(X)");
    unary("Math.Negate", "Negate", "-X");
    unary("Math.Sign", "Sign", "-1, 0 or 1");

    // Vectors
    add({"Math.Dot", "Dot", "Vector", "Dot product", {Pin("A", 0), Pin("B", 0)}, {Pin("Result", 1)}});
    add({"Math.Cross", "Cross", "Vector", "Cross product", {Pin("A", 3), Pin("B", 3)}, {Pin("Result", 3)}});
    add({"Math.Normalize", "Normalize", "Vector", "Unit length", {Pin("X", 0, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f))},
         {Pin("Result", 0)}});
    add({"Math.Length", "Length", "Vector", "Length of X", {Pin("X", 0)}, {Pin("Result", 1)}});
    add({"Math.Distance", "Distance", "Vector", "Distance between A and B", {Pin("A", 0), Pin("B", 0)}, {Pin("Result", 1)}});
    add({"Math.Append", "Append", "Vector", "Components of A followed by B", {Pin("A", 0), Pin("B", 0)}, {Pin("Result", 0)}});
    add({"Math.Mask", "Component Mask", "Vector", "Selected components (e.g. \"rg\", \"xz\")", {Pin("X", 0)},
         {Pin("Result", 0)}, MaterialNodeParam::Mask});
    add({"Math.Split", "Split", "Vector", "Components as numbers", {Pin("X", 0)},
         {Pin("R", 1), Pin("G", 1), Pin("B", 1), Pin("A", 1)}});

    // Utility
    add({"Utility.Fresnel", "Fresnel", "Utility", "Reflectance at grazing angles (Schlick)",
         {Pin("Exponent", 1, S(5.0f)), Pin("BaseReflect", 1, S(0.04f)), Pin("Normal", 3, glm::vec4(0.0f), "Normal")},
         {Pin("Result", 1)}});
    add({"Utility.Panner", "Panner", "Utility", "UV moving with time",
         {Pin("UV", 2, glm::vec4(0.0f), "UV0"), Pin("Time", 1, glm::vec4(0.0f), "Time"),
          Pin("Speed", 2, glm::vec4(0.1f, 0.0f, 0.0f, 0.0f))},
         {Pin("UV", 2)}});
    add({"Utility.Rotator", "Rotator", "Utility", "UV rotating around Center (Speed in radians / second)",
         {Pin("UV", 2, glm::vec4(0.0f), "UV0"), Pin("Center", 2, glm::vec4(0.5f, 0.5f, 0.0f, 0.0f)),
          Pin("Time", 1, glm::vec4(0.0f), "Time"), Pin("Speed", 1, S(1.0f))},
         {Pin("UV", 2)}});
    add({"Utility.Desaturate", "Desaturate", "Utility", "Blend towards gray by Fraction",
         {Pin("Color", 3), Pin("Fraction", 1, S(1.0f))}, {Pin("Result", 3)}});
    add({"Utility.Noise", "Noise", "Utility", "Smooth value noise 0..1",
         {Pin("Position", 3, glm::vec4(0.0f), "WorldPosition"), Pin("Scale", 1, S(1.0f))}, {Pin("Result", 1)}});
    add({"Utility.NormalStrength", "Normal Strength", "Utility", "Flattens or sharpens a tangent space normal",
         {Pin("Normal", 3, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)), Pin("Strength", 1, S(1.0f))}, {Pin("Normal", 3)}});
    add({"Utility.SphereMask", "Sphere Mask", "Utility", "1 inside Radius around B, fading by Hardness",
         {Pin("A", 0), Pin("B", 0), Pin("Radius", 1, S(1.0f)), Pin("Hardness", 1, S(0.5f))}, {Pin("Result", 1)}});
    add({"Utility.If", "If", "Utility", "Greater / Equal / Less by comparing A with B",
         {Pin("A", 1), Pin("B", 1), Pin("Greater", 0, S(1.0f)), Pin("Equal", 0, S(0.0f)), Pin("Less", 0, S(0.0f))},
         {Pin("Result", 0)}});
    return t;
}

std::string F(float v)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(v));
    std::string s = buffer;
    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";
    if (s == "inf" || s == "-inf" || s == "nan" || s == "-nan")
        s = "0.0";
    return s;
}

std::string TypeName(std::uint32_t width)
{
    return width <= 1 ? "float" : "vec" + std::to_string(width);
}

std::string Literal(glm::vec4 v, std::uint32_t width)
{
    switch (width) {
    case 1: return F(v.x);
    case 2: return "vec2(" + F(v.x) + ", " + F(v.y) + ")";
    case 3: return "vec3(" + F(v.x) + ", " + F(v.y) + ", " + F(v.z) + ")";
    default: return "vec4(" + F(v.x) + ", " + F(v.y) + ", " + F(v.z) + ", " + F(v.w) + ")";
    }
}

struct Value {
    std::string   expr;
    std::uint32_t width = 1;
};

// Width conversion: same, broadcast a scalar, truncate, or pad with zeros.
std::string Coerce(const Value& v, std::uint32_t width)
{
    if (v.width == width)
        return v.expr;
    if (v.width == 1)
        return TypeName(width) + "(" + v.expr + ")";
    if (v.width > width) {
        static const char* masks[] = {"", ".x", ".xy", ".xyz"};
        return "(" + v.expr + ")" + masks[width];
    }
    std::string padded = TypeName(width) + "(" + v.expr;
    for (std::uint32_t i = v.width; i < width; ++i)
        padded += ", 0.0";
    return padded + ")";
}

enum class Stage { Vertex, Pixel };

class Emitter {
public:
    Emitter(const MaterialGraph& graph, Stage stage, std::vector<MaterialSlot>& slots, std::vector<MaterialProblem>& problems)
        : m_Graph(graph), m_Stage(stage), m_Slots(slots), m_Problems(problems)
    {
    }

    // Value of an input pin: its link, the node's inline value, or the pin's default.
    Value Input(const MaterialNode& node, const MaterialPinDesc& pin)
    {
        if (const MaterialLink* link = m_Graph.LinkInto(node.id, pin.name)) {
            const MaterialNode* from = m_Graph.FindNode(link->fromNode);
            if (!from) {
                Error(node.id, "input '" + pin.name + "' is linked to a missing node");
                return Fallback(node, pin);
            }
            const std::vector<Value>* outputs = Eval(*from);
            const MaterialNodeDesc*   desc    = FindMaterialNodeType(from->type);
            if (!outputs || !desc)
                return Fallback(node, pin);
            for (std::size_t i = 0; i < desc->outputs.size(); ++i)
                if (desc->outputs[i].name == link->fromPin)
                    return (*outputs)[i];
            Error(node.id, "input '" + pin.name + "' is linked to an unknown pin '" + link->fromPin + "'");
        }
        return Fallback(node, pin);
    }

    Value Fallback(const MaterialNode& node, const MaterialPinDesc& pin)
    {
        const bool set = std::any_of(node.inputs.begin(), node.inputs.end(),
                                     [&](const MaterialPinValue& v) { return v.pin == pin.name; });
        if (!set && !pin.implicit.empty()) {
            if (pin.implicit == "UV0")
                return {"ctx.uv", 2};
            if (pin.implicit == "Time")
                return {"ctx.time", 1};
            if (pin.implicit == "Normal")
                return {"ctx.normal", 3};
            if (pin.implicit == "WorldPosition")
                return {"ctx.worldPos", 3};
        }
        const std::uint32_t width = pin.width == 0 ? 1 : pin.width;
        glm::vec4           value = m_Graph.InputValue(node, pin.name, pin.fallback);
        return {Literal(value, width), width};
    }

    const std::vector<Value>* Eval(const MaterialNode& node)
    {
        if (const auto it = m_Done.find(node.id); it != m_Done.end())
            return &it->second;
        if (m_Visiting.contains(node.id)) {
            Error(node.id, "is part of a cycle");
            return nullptr;
        }
        const MaterialNodeDesc* desc = FindMaterialNodeType(node.type);
        if (!desc) {
            Error(node.id, "unknown node type '" + node.type + "'");
            return nullptr;
        }
        if (desc->hidden) {
            Error(node.id, "the output node has no outputs");
            return nullptr;
        }
        if (desc->pixelOnly && m_Stage == Stage::Vertex) {
            Error(node.id, desc->title + " is not available for the world position offset");
            return nullptr;
        }
        m_Visiting.insert(node.id);
        std::vector<Value> inputs;
        inputs.reserve(desc->inputs.size());
        for (const MaterialPinDesc& pin : desc->inputs)
            inputs.push_back(Input(node, pin));
        std::vector<Value> outputs = Generate(node, *desc, inputs);
        m_Visiting.erase(node.id);
        if (outputs.size() != desc->outputs.size())
            outputs.resize(desc->outputs.size(), Value{"0.0", 1});
        // Every output becomes a local (evaluated once, however many links read it).
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const std::string name = "n" + std::to_string(node.id) + "_" + std::to_string(i);
            m_Body << "    " << TypeName(outputs[i].width) << " " << name << " = " << outputs[i].expr << ";\n";
            outputs[i].expr = name;
        }
        return &(m_Done[node.id] = std::move(outputs));
    }

    [[nodiscard]] std::string Body() const { return m_Body.str(); }

private:
    void Error(std::uint32_t node, std::string message)
    {
        for (const MaterialProblem& p : m_Problems)
            if (p.node == node && p.message == message)
                return;
        m_Problems.push_back({node, std::move(message), true});
    }
    void Warning(std::uint32_t node, std::string message)
    {
        for (const MaterialProblem& p : m_Problems)
            if (p.node == node && p.message == message)
                return;
        m_Problems.push_back({node, std::move(message), false});
    }

    std::uint32_t ParameterSlot(const std::string& name)
    {
        for (std::uint32_t i = 0; i < m_Slots.size(); ++i)
            if (m_Slots[i].kind == MaterialSlotKind::Parameter && m_Slots[i].name == name)
                return i;
        return ~0u;
    }

    std::uint32_t TextureSlot(const MaterialNode& node)
    {
        for (std::uint32_t i = 0; i < m_Slots.size(); ++i)
            if (m_Slots[i].kind == MaterialSlotKind::Texture && m_Slots[i].node == node.id)
                return i;
        MaterialSlot slot;
        slot.kind        = MaterialSlotKind::Texture;
        slot.type        = MaterialParameterType::Texture;
        slot.node        = node.id;
        slot.texture     = node.param;
        slot.textureKind = node.textureKind;
        m_Slots.push_back(std::move(slot));
        return static_cast<std::uint32_t>(m_Slots.size() - 1);
    }

    // Sample of a slot's texture -> color outputs (RGB, R, G, B, A, RGBA).
    std::vector<Value> SampleOutputs(const MaterialNode& node, std::uint32_t slot, TextureKind kind, const Value& uv)
    {
        const std::string sample = "s" + std::to_string(node.id);
        const char*       fn     = m_Stage == Stage::Vertex ? "MaterialTextureLod" : "MaterialTexture";
        m_Body << "    vec4 " << sample << " = " << fn << "(ctx.params, " << slot << "u, " << Coerce(uv, 2) << ");\n";
        if (kind == TextureKind::Normal)
            m_Body << "    " << sample << " = vec4(MaterialDecodeNormal(" << sample << "), 1.0);\n";
        return {{sample + ".rgb", 3}, {sample + ".r", 1}, {sample + ".g", 1}, {sample + ".b", 1}, {sample + ".a", 1},
                {sample, 4}};
    }

    // Common width of generic inputs (0-width pins); errors on mismatching vectors.
    std::uint32_t Generic(const MaterialNode& node, const MaterialNodeDesc& desc, std::vector<Value>& inputs,
                          std::initializer_list<std::size_t> scalarOk = {})
    {
        std::uint32_t width = 1;
        for (std::size_t i = 0; i < inputs.size(); ++i)
            if (desc.inputs[i].width == 0)
                width = std::max(width, inputs[i].width);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (desc.inputs[i].width == 0) {
                if (inputs[i].width != 1 && inputs[i].width != width) {
                    Error(node.id, "input '" + desc.inputs[i].name + "' has " + std::to_string(inputs[i].width) +
                                       " components, expected 1 or " + std::to_string(width));
                }
                inputs[i].expr  = Coerce(inputs[i], width);
                inputs[i].width = width;
            } else {
                inputs[i].expr  = Coerce(inputs[i], desc.inputs[i].width);
                inputs[i].width = desc.inputs[i].width;
            }
        }
        (void)scalarOk;
        return width;
    }

    std::vector<Value> Generate(const MaterialNode& node, const MaterialNodeDesc& desc, std::vector<Value>& in)
    {
        const std::string& type = node.type;
        const auto         one  = [](std::string expr, std::uint32_t width) { return std::vector<Value>{{std::move(expr), width}}; };

        if (type == "Constant")
            return one(F(node.value.x), 1);
        if (type == "Constant2")
            return one(Literal(node.value, 2), 2);
        if (type == "Constant3")
            return one(Literal(node.value, 3), 3);
        if (type == "Color") {
            const std::string c = Literal(node.value, 4);
            return {{c + ".rgb", 3}, {F(node.value.r), 1}, {F(node.value.g), 1}, {F(node.value.b), 1}, {F(node.value.a), 1}, {c, 4}};
        }

        if (desc.param == MaterialNodeParam::Parameter) {
            const MaterialParameter* p = m_Graph.FindParameter(node.param);
            if (!p) {
                Error(node.id, node.param.empty() ? std::string("no parameter selected")
                                                  : "unknown parameter '" + node.param + "'");
                return {};
            }
            const MaterialParameterType expected = type == "Param.Scalar"   ? MaterialParameterType::Scalar
                                                   : type == "Param.Vector" ? MaterialParameterType::Vector
                                                                            : MaterialParameterType::Texture;
            if (p->type != expected) {
                Error(node.id, "parameter '" + p->name + "' is a " + ToString(p->type) + " parameter");
                return {};
            }
            const std::uint32_t slot = ParameterSlot(p->name);
            const std::string   read = "MaterialParam(ctx.params, " + std::to_string(slot) + "u)";
            if (type == "Param.Scalar")
                return one(read + ".x", 1);
            if (type == "Param.Vector") {
                const std::string v = "p" + std::to_string(node.id);
                m_Body << "    vec4 " << v << " = " << read << ";\n";
                return {{v + ".rgb", 3}, {v + ".r", 1}, {v + ".g", 1}, {v + ".b", 1}, {v + ".a", 1}, {v, 4}};
            }
            return SampleOutputs(node, slot, p->textureKind, in[0]);
        }
        if (type == "Texture.Sample") {
            if (node.param.empty())
                Warning(node.id, "no texture selected (samples white)");
            return SampleOutputs(node, TextureSlot(node), node.textureKind, in[0]);
        }

        if (type == "Input.TexCoord")
            return one("ctx.uv * " + Literal(node.value, 2), 2);
        if (type == "Input.WorldPosition")
            return one("ctx.worldPos", 3);
        if (type == "Input.ObjectPosition")
            return one("ctx.objectPosition", 3);
        if (type == "Input.VertexNormal")
            return one("ctx.normal", 3);
        if (type == "Input.CameraPosition")
            return one("ctx.cameraPosition", 3);
        if (type == "Input.CameraVector")
            return one("ctx.cameraVector", 3);
        if (type == "Input.Time")
            return one("ctx.time", 1);
        if (type == "Input.ScreenPosition")
            return one("ctx.screenUV", 2);
        if (type == "Input.PixelDepth")
            return one("ctx.pixelDepth", 1);

        static const std::map<std::string, std::string, std::less<>> binaryOps = {
            {"Math.Add", "+"}, {"Math.Subtract", "-"}, {"Math.Multiply", "*"}, {"Math.Divide", "/"}};
        static const std::map<std::string, std::string, std::less<>> binaryFns = {
            {"Math.Min", "min"}, {"Math.Max", "max"}, {"Math.Modulo", "mod"}, {"Math.Power", "MaterialPow"},
            {"Math.Step", "step"}};
        static const std::map<std::string, std::string, std::less<>> unaryFns = {
            {"Math.Saturate", "MaterialSaturate"}, {"Math.Abs", "abs"}, {"Math.Floor", "floor"}, {"Math.Ceil", "ceil"},
            {"Math.Frac", "fract"}, {"Math.Sin", "sin"}, {"Math.Cos", "cos"}, {"Math.Sqrt", "MaterialSqrt"},
            {"Math.Sign", "sign"}, {"Math.Normalize", "MaterialNormalize"}};
        if (const auto it = binaryOps.find(type); it != binaryOps.end()) {
            const std::uint32_t w = Generic(node, desc, in);
            return one("(" + in[0].expr + " " + it->second + " " + in[1].expr + ")", w);
        }
        if (const auto it = binaryFns.find(type); it != binaryFns.end()) {
            const std::uint32_t w = Generic(node, desc, in);
            return one(it->second + "(" + in[0].expr + ", " + in[1].expr + ")", w);
        }
        if (const auto it = unaryFns.find(type); it != unaryFns.end()) {
            const std::uint32_t w = Generic(node, desc, in);
            return one(it->second + "(" + in[0].expr + ")", w);
        }
        if (type == "Math.OneMinus") {
            const std::uint32_t w = Generic(node, desc, in);
            return one("(1.0 - " + in[0].expr + ")", w);
        }
        if (type == "Math.Negate") {
            const std::uint32_t w = Generic(node, desc, in);
            return one("(-" + in[0].expr + ")", w);
        }
        if (type == "Math.Lerp") {
            const std::uint32_t w = Generic(node, desc, in);
            return one("mix(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + ")", w);
        }
        if (type == "Math.Clamp") {
            const std::uint32_t w = Generic(node, desc, in);
            return one("clamp(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + ")", w);
        }
        if (type == "Math.SmoothStep") {
            const std::uint32_t w = Generic(node, desc, in);
            return one("smoothstep(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + ")", w);
        }
        if (type == "Math.Dot") {
            Generic(node, desc, in);
            return one(in[0].width == 1 ? "(" + in[0].expr + " * " + in[1].expr + ")"
                                        : "dot(" + in[0].expr + ", " + in[1].expr + ")",
                       1);
        }
        if (type == "Math.Cross") {
            Generic(node, desc, in);
            return one("cross(" + in[0].expr + ", " + in[1].expr + ")", 3);
        }
        if (type == "Math.Length") {
            Generic(node, desc, in);
            return one(in[0].width == 1 ? "abs(" + in[0].expr + ")" : "length(" + in[0].expr + ")", 1);
        }
        if (type == "Math.Distance") {
            Generic(node, desc, in);
            return one(in[0].width == 1 ? "abs(" + in[0].expr + " - " + in[1].expr + ")"
                                        : "distance(" + in[0].expr + ", " + in[1].expr + ")",
                       1);
        }
        if (type == "Math.Append") {
            const std::uint32_t w = in[0].width + in[1].width;
            if (w > 4) {
                Error(node.id, "the result would have " + std::to_string(w) + " components (at most 4)");
                return {};
            }
            return one(TypeName(w) + "(" + in[0].expr + ", " + in[1].expr + ")", w);
        }
        if (type == "Math.Mask") {
            std::string mask;
            for (const char c : node.param) {
                const char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                const std::string_view rgba = "rgba", xyzw = "xyzw";
                std::size_t index = rgba.find(lower);
                if (index == std::string_view::npos)
                    index = xyzw.find(lower);
                if (index == std::string_view::npos || mask.size() >= 4) {
                    Error(node.id, "component mask '" + node.param + "' is not one of r g b a / x y z w (at most 4)");
                    return {};
                }
                if (index >= in[0].width) {
                    Error(node.id, "component '" + std::string(1, c) + "' does not exist in a " +
                                       std::to_string(in[0].width) + "-component input");
                    return {};
                }
                mask += xyzw[index];
            }
            if (mask.empty()) {
                Error(node.id, "empty component mask");
                return {};
            }
            if (in[0].width == 1) // a scalar has no swizzle in GLSL: repeat it
                return one(mask.size() == 1 ? in[0].expr : TypeName(static_cast<std::uint32_t>(mask.size())) + "(" + in[0].expr + ")",
                           static_cast<std::uint32_t>(mask.size()));
            return one("(" + in[0].expr + ")." + mask, static_cast<std::uint32_t>(mask.size()));
        }
        if (type == "Math.Split") {
            const Value v{Coerce(in[0], 4), 4};
            const std::string s = "v" + std::to_string(node.id);
            m_Body << "    vec4 " << s << " = " << v.expr << ";\n";
            return {{s + ".x", 1}, {s + ".y", 1}, {s + ".z", 1}, {s + ".w", 1}};
        }

        if (type == "Utility.Fresnel")
            return one("MaterialFresnel(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + ", ctx.cameraVector)", 1);
        if (type == "Utility.Panner")
            return one("(" + in[0].expr + " + " + in[2].expr + " * " + in[1].expr + ")", 2);
        if (type == "Utility.Rotator")
            return one("MaterialRotate(" + in[0].expr + ", " + in[1].expr + ", " + in[2].expr + " * " + in[3].expr + ")", 2);
        if (type == "Utility.Desaturate")
            return one("MaterialDesaturate(" + in[0].expr + ", " + in[1].expr + ")", 3);
        if (type == "Utility.Noise")
            return one("MaterialNoise(" + in[0].expr + " * " + in[1].expr + ")", 1);
        if (type == "Utility.NormalStrength")
            return one("normalize(vec3((" + in[0].expr + ").xy * " + in[1].expr + ", (" + in[0].expr + ").z))", 3);
        if (type == "Utility.SphereMask") {
            Generic(node, desc, in);
            const std::string d = in[0].width == 1 ? "abs(" + in[0].expr + " - " + in[1].expr + ")"
                                                   : "distance(" + in[0].expr + ", " + in[1].expr + ")";
            return one("MaterialSphereMask(" + d + ", " + in[2].expr + ", " + in[3].expr + ")", 1);
        }
        if (type == "Utility.If") {
            const std::uint32_t w = Generic(node, desc, in);
            return one("(" + in[0].expr + " > " + in[1].expr + " ? " + in[2].expr + " : (" + in[0].expr + " < " +
                           in[1].expr + " ? " + in[4].expr + " : " + in[3].expr + "))",
                       w);
        }
        Error(node.id, "no code generator for '" + type + "'");
        return {};
    }

    const MaterialGraph&                           m_Graph;
    Stage                                          m_Stage;
    std::vector<MaterialSlot>&                     m_Slots;
    std::vector<MaterialProblem>&                  m_Problems;
    std::map<std::uint32_t, std::vector<Value>>    m_Done;
    std::set<std::uint32_t>                        m_Visiting;
    std::ostringstream                             m_Body;
};

} // namespace

const std::vector<MaterialNodeDesc>& MaterialNodeTypes()
{
    static const std::vector<MaterialNodeDesc> types = BuildNodeTypes();
    return types;
}

const MaterialNodeDesc* FindMaterialNodeType(std::string_view type)
{
    for (const MaterialNodeDesc& desc : MaterialNodeTypes())
        if (desc.type == type)
            return &desc;
    return nullptr;
}

bool MaterialCode::Ok() const
{
    return !glsl.empty() && std::none_of(problems.begin(), problems.end(), [](const MaterialProblem& p) { return p.error; });
}

MaterialCode GenerateMaterialCode(const MaterialGraph& graph)
{
    MaterialCode code;
    // Parameters first (instances / per-entity overrides address them by name), then textures of
    // Texture.Sample nodes, then the alpha cutoff.
    for (const MaterialParameter& p : graph.parameters) {
        MaterialSlot slot;
        slot.kind        = MaterialSlotKind::Parameter;
        slot.name        = p.name;
        slot.type        = p.type;
        slot.texture     = p.texture;
        slot.textureKind = p.textureKind;
        code.slots.push_back(std::move(slot));
    }

    const MaterialNode* output = graph.OutputNode();
    if (!output) {
        code.problems.push_back({0, "the material has no output node", true});
        return code;
    }
    const MaterialNodeDesc* outputDesc = FindMaterialNodeType(kMaterialOutputNode);

    Emitter pixel(graph, Stage::Pixel, code.slots, code.problems);
    std::ostringstream pixelOut;
    for (const MaterialPinDesc& pin : outputDesc->inputs) {
        if (pin.name == "WorldPositionOffset")
            continue;
        const Value v = pixel.Input(*output, pin);
        static const std::map<std::string, std::string, std::less<>> fields = {
            {"BaseColor", "baseColor"}, {"Metallic", "metallic"}, {"Roughness", "roughness"}, {"Normal", "normal"},
            {"Emissive", "emissive"}, {"Opacity", "opacity"}, {"OpacityMask", "opacityMask"},
            {"AmbientOcclusion", "occlusion"}};
        pixelOut << "    r." << fields.at(pin.name) << " = " << Coerce(v, pin.width) << ";\n";
        if (pin.name == "Normal" && graph.LinkInto(output->id, "Normal"))
            code.normal = true;
    }

    Emitter vertex(graph, Stage::Vertex, code.slots, code.problems);
    std::string vertexOut = "vec3(0.0)";
    if (graph.LinkInto(output->id, "WorldPositionOffset")) {
        for (const MaterialPinDesc& pin : outputDesc->inputs)
            if (pin.name == "WorldPositionOffset")
                vertexOut = Coerce(vertex.Input(*output, pin), 3);
        code.worldPositionOffset = true;
    }

    MaterialSlot cutoff;
    cutoff.kind = MaterialSlotKind::AlphaCutoff;
    code.slots.push_back(cutoff);

    // Warnings for unused nodes (not reached from the output).
    std::set<std::uint32_t> reached;
    std::vector<std::uint32_t> stack{output->id};
    while (!stack.empty()) {
        const std::uint32_t id = stack.back();
        stack.pop_back();
        if (!reached.insert(id).second)
            continue;
        for (const MaterialLink& l : graph.links)
            if (l.toNode == id)
                stack.push_back(l.fromNode);
    }
    for (const MaterialNode& node : graph.nodes)
        if (!reached.contains(node.id))
            code.problems.push_back({node.id, "is not connected to the output", false});
    if (graph.settings.blendMode != MaterialBlendMode::Masked && graph.LinkInto(output->id, "OpacityMask"))
        code.problems.push_back({output->id, "Opacity Mask is only used by masked materials", false});
    const bool blended = graph.settings.blendMode == MaterialBlendMode::Translucent ||
                         graph.settings.blendMode == MaterialBlendMode::Additive;
    if (!blended && graph.LinkInto(output->id, "Opacity"))
        code.problems.push_back({output->id, "Opacity is only used by translucent / additive materials", false});
    if (code.worldPositionOffset && graph.settings.maxDisplacement <= 0.0f)
        code.problems.push_back({output->id, "world position offset without Max Displacement: displaced parts may be culled",
                                 false});

    if (std::any_of(code.problems.begin(), code.problems.end(), [](const MaterialProblem& p) { return p.error; }))
        return code;

    std::ostringstream glsl;
    glsl << "// Generated from a material graph (GenerateMaterialCode).\n";
    glsl << "#define MATERIAL_ALPHA_CUTOFF_SLOT " << (code.slots.size() - 1) << "u\n";
    if (code.worldPositionOffset)
        glsl << "#define MATERIAL_HAS_WPO 1\n";
    if (code.normal)
        glsl << "#define MATERIAL_HAS_NORMAL 1\n";
    glsl << "\nvec3 EvaluateMaterialOffset(MaterialContext ctx)\n{\n" << vertex.Body() << "    return " << vertexOut
         << ";\n}\n\n";
    glsl << "MaterialResult EvaluateMaterial(MaterialContext ctx)\n{\n    MaterialResult r;\n" << pixel.Body()
         << pixelOut.str() << "    return r;\n}\n";
    code.glsl = glsl.str();
    const std::string& text = code.glsl;
    code.hash = Fnv1a64(std::as_bytes(std::span(text.data(), text.size())));
    return code;
}

const char* ToString(MaterialShader shader)
{
    switch (shader) {
    case MaterialShader::Vertex: return "Vertex";
    case MaterialShader::Prepass: return "Prepass";
    case MaterialShader::PrepassPicking: return "PrepassPicking";
    case MaterialShader::Lit: return "Lit";
    case MaterialShader::Blend: return "Blend";
    case MaterialShader::ShadowVertex: return "ShadowVertex";
    case MaterialShader::ShadowMask: return "ShadowMask";
    case MaterialShader::Count: break;
    }
    return "?";
}

std::vector<MaterialShader> MaterialShaderVariants(const MaterialSettings& settings)
{
    if (settings.blendMode == MaterialBlendMode::Translucent || settings.blendMode == MaterialBlendMode::Additive)
        return {MaterialShader::Vertex, MaterialShader::Blend};
    std::vector<MaterialShader> variants{MaterialShader::Vertex, MaterialShader::Prepass, MaterialShader::PrepassPicking,
                                         MaterialShader::Lit};
    if (settings.castShadows) {
        variants.push_back(MaterialShader::ShadowVertex);
        if (settings.blendMode == MaterialBlendMode::Masked)
            variants.push_back(MaterialShader::ShadowMask);
    }
    return variants;
}

const char* MaterialShaderTemplate(MaterialShader shader)
{
    switch (shader) {
    case MaterialShader::Vertex: return "mesh.vert";
    case MaterialShader::Prepass: return "depth_normal.frag";
    case MaterialShader::PrepassPicking: return "depth_normal_id.frag";
    case MaterialShader::Lit: return "mesh.frag";
    case MaterialShader::Blend: return "mesh_blend.frag";
    case MaterialShader::ShadowVertex: return "shadow.vert";
    case MaterialShader::ShadowMask: return "shadow_mask.frag";
    case MaterialShader::Count: break;
    }
    return "";
}

bool MaterialShaderIsVertex(MaterialShader shader)
{
    return shader == MaterialShader::Vertex || shader == MaterialShader::ShadowVertex;
}

std::vector<std::pair<std::string, std::string>> MaterialShaderDefines(const MaterialSettings& settings, const MaterialCode&)
{
    std::vector<std::pair<std::string, std::string>> defines{{"CUSTOM_MATERIAL", "1"}};
    defines.emplace_back("MATERIAL_BLEND_MODE", std::to_string(static_cast<int>(settings.blendMode)));
    if (settings.shadingModel == MaterialShadingModel::Unlit)
        defines.emplace_back("MATERIAL_UNLIT", "1");
    if (settings.twoSided)
        defines.emplace_back("MATERIAL_TWO_SIDED", "1");
    return defines;
}

} // namespace Engine
