#include "Engine/Script/ScriptValue.h"
#include "Engine/Script/ScriptRegistry.h"
#include "ScriptJson.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace Engine {

// --- Type names --------------------------------------------------------------------------------

namespace {
struct NameTable {
    std::mutex                                      mutex;
    std::deque<std::string>                         names{std::string()}; // id 0: none
    std::unordered_map<std::string, std::uint16_t>  ids;
};
NameTable& Names()
{
    static NameTable table;
    return table;
}

constexpr const char* kKindNames[] = {"exec", "bool", "int", "float", "vec3", "string", "entity"};

std::string BaseName(PinKind kind, std::uint16_t name)
{
    switch (kind) {
    case PinKind::Enum: return "enum:" + TypeNameOf(name);
    case PinKind::Struct: return "struct:" + TypeNameOf(name);
    default: return kKindNames[static_cast<std::size_t>(kind)];
    }
}

// "int" / "enum:Color" / "struct:Item" -> type; `rest` gets what follows (":..." after a base).
std::optional<PinType> ParseBase(std::string_view text, std::string_view* rest = nullptr)
{
    const auto user = [&](std::string_view prefix, PinKind kind) -> std::optional<PinType> {
        std::string_view name = text.substr(prefix.size());
        std::string_view tail;
        if (rest) { // a map key: the name ends at the next ':'
            const std::size_t colon = name.find(':');
            if (colon != std::string_view::npos) {
                tail = name.substr(colon);
                name = name.substr(0, colon);
            }
        }
        if (name.empty())
            return std::nullopt;
        if (rest)
            *rest = tail;
        return PinType{kind, PinContainer::None, PinKind::Int, InternTypeName(name), 0};
    };
    if (text.starts_with("enum:"))
        return user("enum:", PinKind::Enum);
    if (text.starts_with("struct:"))
        return user("struct:", PinKind::Struct);
    for (std::size_t i = 1; i < std::size(kKindNames); ++i) {
        const std::string_view n = kKindNames[i];
        if (rest ? text.starts_with(n) && (text.size() == n.size() || text[n.size()] == ':') : text == n) {
            if (rest)
                *rest = text.substr(n.size());
            return PinType{static_cast<PinKind>(i)};
        }
    }
    return std::nullopt;
}

std::string ScalarString(const ScriptValue& value);
} // namespace

std::uint16_t InternTypeName(std::string_view name)
{
    if (name.empty())
        return 0;
    NameTable&       t = Names();
    std::scoped_lock lock(t.mutex);
    const std::string key(name);
    if (const auto it = t.ids.find(key); it != t.ids.end())
        return it->second;
    const auto id = static_cast<std::uint16_t>(t.names.size());
    t.names.push_back(key);
    t.ids.emplace(key, id);
    return id;
}

const std::string& TypeNameOf(std::uint16_t id)
{
    NameTable&       t = Names();
    std::scoped_lock lock(t.mutex);
    return id < t.names.size() ? t.names[id] : t.names.front(); // deque: references stay valid
}

PinType PinType::Enum(std::string_view enumName) { return {PinKind::Enum, PinContainer::None, PinKind::Int, InternTypeName(enumName), 0}; }
PinType PinType::Struct(std::string_view structName)
{
    return {PinKind::Struct, PinContainer::None, PinKind::Int, InternTypeName(structName), 0};
}
PinType PinType::Map(PinType key, PinType value)
{
    return {value.kind, PinContainer::Map, key.kind, value.name, key.kind == PinKind::Enum ? key.name : std::uint16_t{0}};
}

std::string ToString(PinType type)
{
    switch (type.container) {
    case PinContainer::Array:
        return IsUserType(type) ? BaseName(type.kind, type.name) + "[]" : BaseName(type.kind, 0) + "Array";
    case PinContainer::Map: return "map:" + BaseName(type.keyKind, type.keyName) + ":" + BaseName(type.kind, type.name);
    case PinContainer::None: break;
    }
    return BaseName(type.kind, type.name);
}

std::optional<PinType> PinTypeFromString(std::string_view name)
{
    if (name == "exec")
        return PinType::Exec;
    if (name.starts_with("map:")) {
        std::string_view rest;
        const auto       key = ParseBase(name.substr(4), &rest);
        if (!key || !rest.starts_with(':'))
            return std::nullopt;
        const auto value = ParseBase(rest.substr(1));
        if (!value || !IsKeyType(*key))
            return std::nullopt;
        return PinType::Map(*key, *value);
    }
    if (name.ends_with("[]")) {
        const auto base = ParseBase(name.substr(0, name.size() - 2));
        return base ? std::optional(ArrayOf(*base)) : std::nullopt;
    }
    if (name.ends_with("Array") && !name.starts_with("enum:") && !name.starts_with("struct:")) {
        const auto base = ParseBase(name.substr(0, name.size() - 5));
        return base ? std::optional(ArrayOf(*base)) : std::nullopt;
    }
    return ParseBase(name);
}

std::string DisplayName(PinType type)
{
    const auto base = [](PinKind kind, std::uint16_t name) {
        return kind == PinKind::Enum || kind == PinKind::Struct ? TypeNameOf(name) : std::string(kKindNames[static_cast<std::size_t>(kind)]);
    };
    switch (type.container) {
    case PinContainer::Array: return base(type.kind, type.name) + "[]";
    case PinContainer::Map: return "Map<" + base(type.keyKind, type.keyName) + ", " + base(type.kind, type.name) + ">";
    case PinContainer::None: break;
    }
    return base(type.kind, type.name);
}

// --- Values ------------------------------------------------------------------------------------

namespace {
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

// Element / field values shown with their type (enum names).
std::string TypedString(const ScriptValue& value, PinType type)
{
    if (type.kind == PinKind::Enum && !IsContainer(type))
        if (const auto* i = std::get_if<std::int32_t>(&value)) {
            const std::string name = ScriptRegistry::EnumValueName(TypeNameOf(type.name), *i);
            return name.empty() ? std::to_string(*i) : name;
        }
    return ToDisplayString(value);
}

int KeyRank(const ScriptValue& v) { return static_cast<int>(v.index()); }
} // namespace

bool ScriptKeyLess::operator()(const ScriptValue& a, const ScriptValue& b) const
{
    if (a.index() != b.index())
        return KeyRank(a) < KeyRank(b);
    if (const auto* x = std::get_if<bool>(&a))
        return *x < std::get<bool>(b);
    if (const auto* x = std::get_if<std::int32_t>(&a))
        return *x < std::get<std::int32_t>(b);
    if (const auto* x = std::get_if<float>(&a))
        return *x < std::get<float>(b);
    if (const auto* x = std::get_if<std::string>(&a))
        return *x < std::get<std::string>(b);
    if (const auto* x = std::get_if<Entity>(&a))
        return static_cast<std::uint64_t>(*x) < static_cast<std::uint64_t>(std::get<Entity>(b));
    return false; // vectors / containers are no keys
}

PinType TypeOf(const ScriptValue& value)
{
    if (const auto* a = std::get_if<ScriptArrayPtr>(&value))
        return ArrayOf(*a ? (*a)->element : PinType::Float);
    if (const auto* s = std::get_if<ScriptStructPtr>(&value))
        return {PinKind::Struct, PinContainer::None, PinKind::Int, *s ? (*s)->type : std::uint16_t{0}, 0};
    if (const auto* m = std::get_if<ScriptMapPtr>(&value))
        return *m ? PinType::Map((*m)->key, (*m)->value) : PinType::Map(PinType::String, PinType::Int);
    return PinType{static_cast<PinKind>(value.index() + 1)};
}

bool ValueFits(const ScriptValue& value, PinType type)
{
    if (type.kind == PinKind::Enum && !IsContainer(type))
        return std::holds_alternative<std::int32_t>(value);
    return TypeOf(value) == type;
}

ScriptValue DefaultValue(PinType type)
{
    if (IsArray(type))
        return MakeArray(ElementType(type));
    if (IsMap(type))
        return MakeMap(KeyType(type), ElementType(type));
    switch (type.kind) {
    case PinKind::Bool: return false;
    case PinKind::Int:
    case PinKind::Enum: return std::int32_t{0};
    case PinKind::Float: return 0.0f;
    case PinKind::Vec3: return glm::vec3(0.0f);
    case PinKind::String: return std::string();
    case PinKind::Entity: return NullEntity;
    case PinKind::Struct: return MakeStruct(TypeNameOf(type.name));
    case PinKind::Exec: break;
    }
    return false;
}

bool CanConvert(PinType from, PinType to)
{
    if (from == to)
        return true;
    if (from.kind == PinKind::Exec || to.kind == PinKind::Exec)
        return false;
    if (to == PinType::String)
        return true;
    if (IsContainer(from) || IsContainer(to) || from.kind == PinKind::Struct || to.kind == PinKind::Struct)
        return false;
    if (from.kind == PinKind::Enum || to.kind == PinKind::Enum) // enums are ints
        return (from.kind == PinKind::Enum && to == PinType::Int) || (to.kind == PinKind::Enum && from == PinType::Int);
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
            s += TypedString(a.items[i], a.element);
        }
        return s + "]";
    }
    if (std::holds_alternative<ScriptStructPtr>(value)) {
        const ScriptStruct&    st  = StructOf(value);
        const ScriptStructDef* def = ScriptRegistry::FindStruct(TypeNameOf(st.type));
        std::string            s   = "{";
        for (std::size_t i = 0; i < st.fields.size(); ++i) {
            PinType type = TypeOf(st.fields[i].second);
            if (def)
                for (const ScriptStructField& f : def->fields)
                    if (f.name == st.fields[i].first)
                        type = f.type;
            s += (i ? ", " : "") + st.fields[i].first + ": " + TypedString(st.fields[i].second, type);
        }
        return s + "}";
    }
    if (std::holds_alternative<ScriptMapPtr>(value)) {
        const ScriptMap& m = MapOf(value);
        std::string      s = "{";
        std::size_t      i = 0;
        for (const auto& [k, v] : m.items) {
            if (i == 16) {
                s += std::format(", ... ({} entries)", m.items.size());
                break;
            }
            s += (i++ ? ", " : "") + TypedString(k, m.key) + ": " + TypedString(v, m.value);
        }
        return s + "}";
    }
    return ScalarString(value);
}

ScriptValue Convert(const ScriptValue& value, PinType to)
{
    if (ValueFits(value, to))
        return value;
    if (to == PinType::String)
        return ToDisplayString(value);
    if (to == PinType::Float) {
        if (const auto* i = std::get_if<std::int32_t>(&value))
            return static_cast<float>(*i);
        if (const auto* b = std::get_if<bool>(&value))
            return *b ? 1.0f : 0.0f;
    }
    if (to == PinType::Int || (to.kind == PinKind::Enum && !IsContainer(to))) {
        if (const auto* f = std::get_if<float>(&value))
            return std::isfinite(*f) ? static_cast<std::int32_t>(std::clamp(*f, -2147483520.0f, 2147483520.0f))
                                     : std::int32_t{0};
        if (const auto* b = std::get_if<bool>(&value))
            return std::int32_t{*b ? 1 : 0};
    }
    return DefaultValue(to);
}

ScriptValue ConvertFrom(const ScriptValue& value, PinType from, PinType to)
{
    if (to == PinType::String && from != to)
        return TypedString(value, from);
    return Convert(value, to);
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
    if (std::holds_alternative<ScriptStructPtr>(a)) {
        const ScriptStruct& x = StructOf(a);
        const ScriptStruct& y = StructOf(b);
        return x.type == y.type && x.fields.size() == y.fields.size() &&
               std::equal(x.fields.begin(), x.fields.end(), y.fields.begin(),
                          [](const auto& p, const auto& q) { return p.first == q.first && ValuesEqual(p.second, q.second); });
    }
    if (std::holds_alternative<ScriptMapPtr>(a)) {
        const ScriptMap& x = MapOf(a);
        const ScriptMap& y = MapOf(b);
        return x.key == y.key && x.value == y.value && x.items.size() == y.items.size() &&
               std::equal(x.items.begin(), x.items.end(), y.items.begin(), [](const auto& p, const auto& q) {
                   return ValuesEqual(p.first, q.first) && ValuesEqual(p.second, q.second);
               });
    }
    return a == b;
}

// --- Containers --------------------------------------------------------------------------------

namespace {
// Copy on write: the objects were created non-const (make_shared), so writing through them is fine.
template <class T, class Make>
T& Writable(ScriptValue& value, Make make)
{
    using Ptr = std::shared_ptr<const T>;
    auto* p   = std::get_if<Ptr>(&value);
    if (!p || !*p) {
        value = make();
        p     = std::get_if<Ptr>(&value);
    } else if (p->use_count() > 1) {
        *p = std::make_shared<T>(**p);
    }
    return const_cast<T&>(**p);
}
} // namespace

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
    return Writable<ScriptArray>(value, [&] { return MakeArray(element); });
}

ScriptStructPtr MakeStruct(std::string_view structName)
{
    auto s  = std::make_shared<ScriptStruct>();
    s->type = InternTypeName(structName);
    if (const ScriptStructDef* def = ScriptRegistry::FindStruct(structName))
        for (const ScriptStructField& f : def->fields)
            s->fields.emplace_back(f.name, ValueFits(f.value, f.type) ? f.value : DefaultValue(f.type));
    return s;
}

const ScriptStruct& StructOf(const ScriptValue& value)
{
    static const ScriptStruct empty;
    if (const auto* s = std::get_if<ScriptStructPtr>(&value); s && *s)
        return **s;
    return empty;
}

const ScriptValue* StructField(const ScriptValue& value, std::string_view field)
{
    for (const auto& [name, v] : StructOf(value).fields)
        if (name == field)
            return &v;
    return nullptr;
}

void SetStructField(ScriptValue& value, std::string_view structName, std::string_view field, ScriptValue fieldValue)
{
    ScriptStruct& s = Writable<ScriptStruct>(value, [&] { return MakeStruct(structName); });
    for (auto& [name, v] : s.fields)
        if (name == field) {
            v = std::move(fieldValue);
            return;
        }
    s.fields.emplace_back(std::string(field), std::move(fieldValue));
}

ScriptMapPtr MakeMap(PinType key, PinType value)
{
    auto m   = std::make_shared<ScriptMap>();
    m->key   = ElementType(key);
    m->value = ElementType(value);
    return m;
}

const ScriptMap& MapOf(const ScriptValue& value)
{
    static const ScriptMap empty;
    if (const auto* m = std::get_if<ScriptMapPtr>(&value); m && *m)
        return **m;
    return empty;
}

ScriptMap& MutableMap(ScriptValue& value, PinType key, PinType valueType)
{
    return Writable<ScriptMap>(value, [&] { return MakeMap(key, valueType); });
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
            } else if constexpr (std::is_same_v<T, ScriptStructPtr>) {
                nlohmann::json object = nlohmann::json::object();
                if (v)
                    for (const auto& [name, field] : v->fields)
                        object[name] = ScriptValueToJson(field);
                return object;
            } else if constexpr (std::is_same_v<T, ScriptMapPtr>) {
                nlohmann::json pairs = nlohmann::json::array();
                if (v)
                    for (const auto& [key, item] : v->items)
                        pairs.push_back({ScriptValueToJson(key), ScriptValueToJson(item)});
                return pairs;
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
        if (IsMap(type)) {
            ScriptValue m   = MakeMap(KeyType(type), ElementType(type));
            ScriptMap&  map = MutableMap(m, KeyType(type), ElementType(type));
            if (j.is_array())
                for (const nlohmann::json& pair : j)
                    if (pair.is_array() && pair.size() == 2 && !pair[0].is_null())
                        map.items[ScriptValueFromJson(pair[0], KeyType(type))] = ScriptValueFromJson(pair[1], ElementType(type));
            return m;
        }
        switch (type.kind) {
        case PinKind::Bool: value = j.get<bool>(); break;
        case PinKind::Int:
        case PinKind::Enum: value = j.get<std::int32_t>(); break;
        case PinKind::Float: value = j.get<float>(); break;
        case PinKind::Vec3: value = glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()); break;
        case PinKind::String: value = j.get<std::string>(); break;
        case PinKind::Struct:
            if (j.is_object()) { // fields of the definition (typed), unknown ones by their JSON
                const std::string&     name = TypeNameOf(type.name);
                const ScriptStructDef* def  = ScriptRegistry::FindStruct(name);
                for (auto it = j.begin(); it != j.end(); ++it) {
                    PinType fieldType = PinType::String;
                    bool    known     = false;
                    if (def)
                        for (const ScriptStructField& f : def->fields)
                            if (f.name == it.key()) {
                                fieldType = f.type;
                                known     = true;
                            }
                    if (known)
                        SetStructField(value, name, it.key(), ScriptValueFromJson(*it, fieldType));
                }
            }
            break;
        default: break;
        }
    } catch (const nlohmann::json::exception&) { // wrong type in the file: keep the default
    }
    return value;
}

} // namespace Engine
