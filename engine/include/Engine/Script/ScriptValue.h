#pragma once
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Engine {

// --- Types -------------------------------------------------------------------------------------
//
// A pin / variable type: a kind (Bool .. Entity, or a user Enum / Struct named by an interned id),
// optionally inside a container. Arrays hold one element type (no arrays of arrays / maps), maps
// a scalar key (bool, int, string, entity or enum) and a non-container value. User types are
// defined in ScriptRegistry (.uenum / .ustruct files).

enum class PinKind : std::uint8_t { Exec, Bool, Int, Float, Vec3, String, Entity, Enum, Struct };
enum class PinContainer : std::uint8_t { None, Array, Map };

// Names of user types are interned: PinType stays a small value (id 0 = no name).
[[nodiscard]] std::uint16_t      InternTypeName(std::string_view name);
[[nodiscard]] const std::string& TypeNameOf(std::uint16_t id);

struct PinType {
    PinKind       kind      = PinKind::Float;
    PinContainer  container = PinContainer::None;
    PinKind       keyKind   = PinKind::Int; // maps
    std::uint16_t name      = 0;            // Enum / Struct (of the value)
    std::uint16_t keyName   = 0;            // maps with enum keys

    static const PinType Exec, Bool, Int, Float, Vec3, String, Entity;
    static const PinType BoolArray, IntArray, FloatArray, Vec3Array, StringArray, EntityArray;
    [[nodiscard]] static PinType Enum(std::string_view enumName);
    [[nodiscard]] static PinType Struct(std::string_view structName);
    [[nodiscard]] static PinType Map(PinType key, PinType value); // key / value: no containers

    constexpr bool operator==(const PinType&) const = default;
};
inline constexpr PinType PinType::Exec{PinKind::Exec};
inline constexpr PinType PinType::Bool{PinKind::Bool};
inline constexpr PinType PinType::Int{PinKind::Int};
inline constexpr PinType PinType::Float{PinKind::Float};
inline constexpr PinType PinType::Vec3{PinKind::Vec3};
inline constexpr PinType PinType::String{PinKind::String};
inline constexpr PinType PinType::Entity{PinKind::Entity};
inline constexpr PinType PinType::BoolArray{PinKind::Bool, PinContainer::Array};
inline constexpr PinType PinType::IntArray{PinKind::Int, PinContainer::Array};
inline constexpr PinType PinType::FloatArray{PinKind::Float, PinContainer::Array};
inline constexpr PinType PinType::Vec3Array{PinKind::Vec3, PinContainer::Array};
inline constexpr PinType PinType::StringArray{PinKind::String, PinContainer::Array};
inline constexpr PinType PinType::EntityArray{PinKind::Entity, PinContainer::Array};

[[nodiscard]] constexpr bool IsArray(PinType t) { return t.container == PinContainer::Array; }
[[nodiscard]] constexpr bool IsMap(PinType t) { return t.container == PinContainer::Map; }
[[nodiscard]] constexpr bool IsContainer(PinType t) { return t.container != PinContainer::None; }
// The value type of an array / map (the type itself otherwise).
[[nodiscard]] constexpr PinType ElementType(PinType t)
{
    return {t.kind, PinContainer::None, PinKind::Int, t.name, 0};
}
[[nodiscard]] constexpr PinType KeyType(PinType t) { return {t.keyKind, PinContainer::None, PinKind::Int, t.keyName, 0}; }
[[nodiscard]] constexpr PinType ArrayOf(PinType t)
{
    return t.kind == PinKind::Exec || IsContainer(t) ? t : PinType{t.kind, PinContainer::Array, PinKind::Int, t.name, 0};
}
[[nodiscard]] constexpr bool IsUserType(PinType t) { return t.kind == PinKind::Enum || t.kind == PinKind::Struct; }
// Allowed as map keys.
[[nodiscard]] constexpr bool IsKeyType(PinType t)
{
    return !IsContainer(t) && (t.kind == PinKind::Bool || t.kind == PinKind::Int || t.kind == PinKind::String ||
                               t.kind == PinKind::Entity || t.kind == PinKind::Enum);
}
[[nodiscard]] inline const std::string& UserTypeName(PinType t) { return TypeNameOf(t.name); }

// Stored form: "bool".."entity", "enum:Color", "struct:Item"; arrays "intArray" (built-in kinds)
// or "struct:Item[]"; maps "map:<key>:<value>" (e.g. "map:string:struct:Item").
[[nodiscard]] std::string            ToString(PinType type);
[[nodiscard]] std::optional<PinType> PinTypeFromString(std::string_view name);
// For the UI: "int", "Item", "Item[]", "Map<string, int>".
[[nodiscard]] std::string DisplayName(PinType type);

// --- Values ------------------------------------------------------------------------------------

struct ScriptArray;
struct ScriptStruct;
struct ScriptMap;
// Containers and structs are values with copy-on-write storage: copies share the data until one
// of them is changed (Mutable*).
using ScriptArrayPtr  = std::shared_ptr<const ScriptArray>;
using ScriptStructPtr = std::shared_ptr<const ScriptStruct>;
using ScriptMapPtr    = std::shared_ptr<const ScriptMap>;

// A data value. Enum values are ints (the pin / variable type knows the enum).
using ScriptValue = std::variant<bool, std::int32_t, float, glm::vec3, std::string, Entity, ScriptArrayPtr,
                                 ScriptStructPtr, ScriptMapPtr>;

struct ScriptArray {
    PinType                  element = PinType::Float;
    std::vector<ScriptValue> items; // all of type `element`
};

struct ScriptStruct {
    std::uint16_t                                    type = 0; // interned struct name
    std::vector<std::pair<std::string, ScriptValue>> fields;   // by name (definitions may change)
};

// Map keys order: by value (bool / int / string / entity).
struct ScriptKeyLess {
    bool operator()(const ScriptValue& a, const ScriptValue& b) const;
};
struct ScriptMap {
    PinType                                           key   = PinType::String;
    PinType                                           value = PinType::Int;
    std::map<ScriptValue, ScriptValue, ScriptKeyLess> items;
};

// The type a value carries (enum values report Int).
[[nodiscard]] PinType TypeOf(const ScriptValue& value);
// Does `value` hold a value of `type` (enums: ints)?
[[nodiscard]] bool ValueFits(const ScriptValue& value, PinType type);
// false, 0, 0.0, (0,0,0), "", NullEntity (= self / none), enum 0, struct with the field defaults
// (ScriptRegistry), empty containers.
[[nodiscard]] ScriptValue DefaultValue(PinType type);
// Implicit conversions between connected pins: same type, Int <-> Float, Bool -> Int / Float,
// Enum <-> Int, anything -> String. Exec only to Exec; containers and structs only to the same type
// (or String).
[[nodiscard]] bool        CanConvert(PinType from, PinType to);
[[nodiscard]] ScriptValue Convert(const ScriptValue& value, PinType to); // to a type CanConvert allows (else default)
// Like Convert, knowing the source type (enum -> String gives the value's name).
[[nodiscard]] ScriptValue ConvertFrom(const ScriptValue& value, PinType from, PinType to);
[[nodiscard]] std::string ToDisplayString(const ScriptValue& value);
// Deep comparison (containers and structs by content).
[[nodiscard]] bool ValuesEqual(const ScriptValue& a, const ScriptValue& b);

[[nodiscard]] ScriptArrayPtr MakeArray(PinType element, std::vector<ScriptValue> items = {});
// The array of a value (an empty one for non-arrays).
[[nodiscard]] const ScriptArray& ArrayItems(const ScriptValue& value);
// Writable array of an array value: copies the shared elements first if needed.
[[nodiscard]] ScriptArray& MutableArray(ScriptValue& value, PinType element);

// A struct with its definition's fields at their defaults (unknown struct: no fields).
[[nodiscard]] ScriptStructPtr     MakeStruct(std::string_view structName);
[[nodiscard]] const ScriptStruct& StructOf(const ScriptValue& value); // empty for non-structs
[[nodiscard]] const ScriptValue*  StructField(const ScriptValue& value, std::string_view field);
// Sets (adds) a field; copies shared storage first. A non-struct value becomes `structName`.
void SetStructField(ScriptValue& value, std::string_view structName, std::string_view field, ScriptValue fieldValue);

[[nodiscard]] ScriptMapPtr     MakeMap(PinType key, PinType value);
[[nodiscard]] const ScriptMap& MapOf(const ScriptValue& value); // empty for non-maps
[[nodiscard]] ScriptMap&       MutableMap(ScriptValue& value, PinType key, PinType valueType);

} // namespace Engine
