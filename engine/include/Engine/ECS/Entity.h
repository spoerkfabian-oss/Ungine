#pragma once
#include <cstdint>

namespace Engine {

// 32-bit slot index + 32-bit generation. A destroyed entity's handle never aliases a new one.
enum class Entity : std::uint64_t {};
inline constexpr Entity NullEntity{~std::uint64_t{0}};

[[nodiscard]] constexpr std::uint32_t EntityIndex(Entity e) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(e));
}
[[nodiscard]] constexpr std::uint32_t EntityGeneration(Entity e) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(e) >> 32);
}
[[nodiscard]] constexpr Entity MakeEntity(std::uint32_t index, std::uint32_t generation) noexcept
{
    return Entity{(std::uint64_t{generation} << 32) | index};
}

} // namespace Engine
