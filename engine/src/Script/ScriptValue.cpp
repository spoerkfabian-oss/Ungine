#include "Engine/Script/ScriptValue.h"
#include "ScriptJson.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

namespace Engine {

namespace {
constexpr const char* kTypeNames[] = {"exec",      "bool",     "int",        "float",       "vec3",
                                      "string",    "entity",   "boolArray",  "intArray",    "floatArray",
                                      "vec3Array", "stringArray", "entityArray"};
static_assert(std::size(kTypeNames) == static_cast<std::size_t>(PinType::Count));

std::string ScalarString(const ScriptValue& value)
{
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int32_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, float>)
                return std::format("{:g}", v);
            else if constexpr (std::is_same_v<T, glm::vec3>)
                return std::format("({:g}, {:g}, {:g})", v.x, v.y, v.z);
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, Entity>)
                return v == NullEntity ? std::string("none") : std::format("entity {}:{}", EntityIndex(v), EntityGeneration(v));
            else
                return {};
        },
        value);
}
} // namespace

const char* ToString(PinType type)
{
    const auto i = static_cast<std::size_t>(type);
    return i < std::size(kTypeNames) ? kTypeNames[i] : "float";
}

std::optional<PinType> PinTypeFromString(std::string_view name)
{
    for (std::size_t i = 0; i < std::size(kTypeNames); ++i)
        if (name == kTypeNames[i])
            return static_cast<PinType>(i);
    return std::nullopt;
}

PinType TypeOf(const ScriptValue& value)
{
    if (const auto* a = std::get_if<ScriptArrayPtr>(&value))
        return ArrayOf(*a ? (*a)->element : PinType::Float);
    return static_cast<PinType>(value.index() + 1);
}

ScriptValue DefaultValue(PinType type)
{
    if (IsArray(type))
        return MakeArray(ElementType(type));
    switch (type) {
    case PinType::Bool: return false;
    case PinType::Int: return std::int32_t{0};
    case PinType::Float: return 0.0f;
    case PinType::Vec3: return glm::vec3(0.0f);
    case PinType::String: return std::string();
    case PinType::Entity: return NullEntity;
    default: break;
    }
    return false;
}

bool CanConvert(PinType from, PinType to)
{
    if (from == to)
        return true;
    if (from == PinType::Exec || to == PinType::Exec)
        return false;
    if (to == PinType::String)
        return true;
    if (IsArray(from) || IsArray(to))
        return false;
    const bool numeric = to == PinType::Int || to == PinType::Float;
    return numeric && (from == PinType::Int || from == PinType::Float || from == PinType::Bool);
}

std::string ToDisplayString(const ScriptValue& value)
{
    if (std::holds_alternative<ScriptArrayPtr>(value)) {
        const ScriptArray& a = ArrayItems(value);
        std::string        s = "[";
        for (std::size_t i = 0; i < a.items.size(); ++i) {
            if (i == 16) {
                s += std::format(", ... ({} items)", a.items.size());
                break;
            }
            if (i)
                s += ", ";
            s += ScalarString(a.items[i]);
        }
        return s + "]";
    }
    return ScalarString(value);
}

ScriptValue Convert(const ScriptValue& value, PinType to)
{
    const PinType from = TypeOf(value);
    if (from == to)
        return value;
    if (to == PinType::String)
        return ToDisplayString(value);
    if (to == PinType::Float) {
        if (const auto* i = std::get_if<std::int32_t>(&value))
            return static_cast<float>(*i);
        if (const auto* b = std::get_if<bool>(&value))
            return *b ? 1.0f : 0.0f;
    }
    if (to == PinType::Int) {
        if (const auto* f = std::get_if<float>(&value))
            return std::isfinite(*f) ? static_cast<std::int32_t>(std::clamp(*f, -2147483520.0f, 2147483520.0f))
                                     : std::int32_t{0};
        if (const auto* b = std::get_if<bool>(&value))
            return std::int32_t{*b ? 1 : 0};
    }
    return DefaultValue(to);
}

bool ValuesEqual(const ScriptValue& a, const ScriptValue& b)
{
    if (a.index() != b.index())
        return false;
    if (std::holds_alternative<ScriptArrayPtr>(a)) {
        const ScriptArray& x = ArrayItems(a);
        const ScriptArray& y = ArrayItems(b);
        return x.element == y.element && x.items.size() == y.items.size() &&
               std::equal(x.items.begin(), x.items.end(), y.items.begin(), ValuesEqual);
    }
    return a == b;
}

ScriptArrayPtr MakeArray(PinType element, std::vector<ScriptValue> items)
{
    auto a     = std::make_shared<ScriptArray>();
    a->element = ElementType(element);
    a->items   = std::move(items);
    return a;
}

const ScriptArray& ArrayItems(const ScriptValue& value)
{
    static const ScriptArray empty;
    if (const auto* a = std::get_if<ScriptArrayPtr>(&value); a && *a)
        return **a;
    return empty;
}

ScriptArray& MutableArray(ScriptValue& value, PinType element)
{
    auto* a = std::get_if<ScriptArrayPtr>(&value);
    if (!a || !*a) {
        value = MakeArray(element);
        a     = std::get_if<ScriptArrayPtr>(&value);
    } else if (a->use_count() > 1) { // shared: copy on write
        *a = std::make_shared<ScriptArray>(**a);
    }
    // The object was created non-const by MakeArray / make_shared: writing through it is fine.
    return const_cast<ScriptArray&>(**a);
}

// --- JSON ----------------------------------------------------------------------------------------

nlohmann::json ScriptValueToJson(const ScriptValue& value)
{
    return std::visit(
        [](const auto& v) -> nlohmann::json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, glm::vec3>) {
                return nlohmann::json::array({v.x, v.y, v.z});
            } else if constexpr (std::is_same_v<T, Entity>) {
                return nullptr; // entities are runtime values (self / none)
            } else if constexpr (std::is_same_v<T, ScriptArrayPtr>) {
                nlohmann::json list = nlohmann::json::array();
                if (v)
                    for (const ScriptValue& item : v->items)
                        list.push_back(ScriptValueToJson(item));
                return list;
            } else {
                return v;
            }
        },
        value);
}

ScriptValue ScriptValueFromJson(const nlohmann::json& j, PinType type)
{
    ScriptValue value = DefaultValue(type);
    try {
        if (IsArray(type)) {
            std::vector<ScriptValue> items;
            if (j.is_array())
                for (const nlohmann::json& item : j)
                    items.push_back(ScriptValueFromJson(item, ElementType(type)));
            return MakeArray(ElementType(type), std::move(items));
        }
        switch (type) {
        case PinType::Bool: value = j.get<bool>(); break;
        case PinType::Int: value = j.get<std::int32_t>(); break;
        case PinType::Float: value = j.get<float>(); break;
        case PinType::Vec3: value = glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()); break;
        case PinType::String: value = j.get<std::string>(); break;
        default: break;
        }
    } catch (const nlohmann::json::exception&) { // wrong type in the file: keep the default
    }
    return value;
}

} // namespace Engine
