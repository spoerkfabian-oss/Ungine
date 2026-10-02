#pragma once
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Engine {

// Value types of visual scripts. Arrays hold elements of one scalar type (no arrays of arrays).
enum class PinType : std::uint8_t {
    Exec, Bool, Int, Float, Vec3, String, Entity,
    BoolArray, IntArray, FloatArray, Vec3Array, StringArray, EntityArray,
    Count
};

inline constexpr std::uint8_t kArrayTypeOffset = 6; // BoolArray - Bool

[[nodiscard]] constexpr bool IsArray(PinType t) { return t >= PinType::BoolArray && t < PinType::Count; }
[[nodiscard]] constexpr PinType ElementType(PinType t)
{
    return IsArray(t) ? static_cast<PinType>(static_cast<std::uint8_t>(t) - kArrayTypeOffset) : t;
}
[[nodiscard]] constexpr PinType ArrayOf(PinType t)
{
    return t == PinType::Exec || IsArray(t) ? t : static_cast<PinType>(static_cast<std::uint8_t>(t) + kArrayTypeOffset);
}

struct ScriptArray;
// Arrays are values with copy-on-write storage: copies share the elements until one is changed.
using ScriptArrayPtr = std::shared_ptr<const ScriptArray>;

// A data value; the alternative index is PinType - 1 for scalars, arrays use the last one.
using ScriptValue = std::variant<bool, std::int32_t, float, glm::vec3, std::string, Entity, ScriptArrayPtr>;

struct ScriptArray {
    PinType                  element = PinType::Float;
    std::vector<ScriptValue> items; // all of type `element`
};

[[nodiscard]] const char*            ToString(PinType type);
[[nodiscard]] std::optional<PinType> PinTypeFromString(std::string_view name);
[[nodiscard]] PinType                TypeOf(const ScriptValue& value);
// false, 0, 0.0, (0,0,0), "", NullEntity (= self / none), empty array
[[nodiscard]] ScriptValue DefaultValue(PinType type);
// Implicit conversions between connected pins: same type, Int <-> Float, Bool -> Int / Float,
// anything -> String. Exec only to Exec; arrays only to the same array type (or String).
[[nodiscard]] bool        CanConvert(PinType from, PinType to);
[[nodiscard]] ScriptValue Convert(const ScriptValue& value, PinType to); // to a type CanConvert allows (else default)
[[nodiscard]] std::string ToDisplayString(const ScriptValue& value);
// Deep comparison (arrays by content).
[[nodiscard]] bool ValuesEqual(const ScriptValue& a, const ScriptValue& b);

[[nodiscard]] ScriptArrayPtr MakeArray(PinType element, std::vector<ScriptValue> items = {});
// The array of a value (an empty one for non-arrays).
[[nodiscard]] const ScriptArray& ArrayItems(const ScriptValue& value);
// Writable array of an array value: copies the shared elements first if needed.
[[nodiscard]] ScriptArray& MutableArray(ScriptValue& value, PinType element);

} // namespace Engine
