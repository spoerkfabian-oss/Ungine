#pragma once
#include <cstdint>
#include <functional>

namespace Engine {

// Typed, trivially copyable reference to an asset owned by the AssetManager.
// Stale handles (asset unloaded, slot reused) are detected via the generation.
template <class T>
struct AssetHandle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0; // 0 = null handle

    [[nodiscard]] explicit operator bool() const { return generation != 0; }
    friend bool operator==(AssetHandle, AssetHandle) = default;
};

struct Model;
struct Texture;
using ModelHandle   = AssetHandle<Model>;
using TextureHandle = AssetHandle<Texture>;

enum class AssetState : std::uint8_t {
    Invalid,   // null or stale handle
    Loading,   // CPU work on a worker thread (file IO, decoding)
    Uploading, // GPU resources created, transfer in flight
    Ready,
    Failed,    // see AssetFailedEvent / AssetManager::Error
};

} // namespace Engine

template <class T>
struct std::hash<Engine::AssetHandle<T>> {
    std::size_t operator()(Engine::AssetHandle<T> h) const noexcept
    {
        return std::hash<std::uint64_t>{}((std::uint64_t{h.generation} << 32) | h.index);
    }
};
