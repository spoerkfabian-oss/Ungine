#pragma once
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/GeometryPool.h"
#include "Engine/Renderer/GpuProfiler.h"
#include "Engine/Renderer/ShaderReload.h"
#include "Engine/Renderer/Vulkan/Bindless.h"
#include "Engine/Renderer/Vulkan/Swapchain.h"
#include "Engine/Renderer/Vulkan/Upload.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <memory>
#include <optional>
#include <vector>

namespace Engine {

class Window;

inline constexpr std::uint32_t kFramesInFlight      = 2;
inline constexpr VkFormat      kDepthFormat         = VK_FORMAT_D32_SFLOAT; // reverse-Z: clear to 0 (scene targets)
inline constexpr VkDeviceSize  kTransientBufferSize = 8ull << 20;          // per frame in flight

// Textures used when a material slot is empty (1x1) or its texture failed to load (Error: checker).
// Their texture table entries are fixed: table index == enum value.
enum class DefaultTexture : std::uint32_t { White, Black, FlatNormal, Error, Count };

struct TransientAllocation {
    void*           cpu = nullptr;
    VkDeviceAddress gpu = 0;
};

struct RendererDesc {
    bool             vsync = true;
    GeometryPoolDesc geometry{}; // capacities of the global vertex / index / material pools
    UploadQueueDesc  upload{};   // staging ring size, per-frame upload budget
};

// Everything a pass needs to record into the current frame.
// The swapchain image is in COLOR_ATTACHMENT_OPTIMAL when handed out and must be left so.
struct FrameContext {
    VkCommandBuffer cmd        = VK_NULL_HANDLE;
    VkImage         image      = VK_NULL_HANDLE;
    VkImageView     view       = VK_NULL_HANDLE;
    VkFormat        format     = VK_FORMAT_UNDEFINED;
    VkExtent2D      extent{};
    std::uint32_t   frameIndex = 0; // [0, kFramesInFlight) - index per-frame resources with this
    std::uint32_t   imageIndex = 0; // swapchain image index
};

class Renderer {
public:
    Renderer(VulkanContext& ctx, Window& window, EventBus& events, const RendererDesc& desc);
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Returns nullopt when the frame must be skipped (swapchain out of date).
    [[nodiscard]] std::optional<FrameContext> BeginFrame();
    void EndFrame(const FrameContext& frame);

    // Threading: bindless registry, upload queue, geometry pool (see their docs) and DefaultTextureIndex may be
    // used from worker threads. Everything else, DeferRelease/DeferCall included: main thread.
    [[nodiscard]] const VulkanContext& GetContext() const { return m_Ctx; }
    [[nodiscard]] const Swapchain&   GetSwapchain() const { return *m_Swapchain; }
    [[nodiscard]] BindlessRegistry&  GetBindless()        { return *m_Bindless; }
    [[nodiscard]] UploadQueue&       GetUploader()        { return *m_Upload; }
    [[nodiscard]] GpuProfiler&       Profiler()           { return *m_Profiler; } // "Frame" scope included
    [[nodiscard]] GeometryPool&      Geometry()           { return *m_Geometry; } // thread-safe (see its docs)

    // Deferred destruction: the resource (Buffer, Image, Pipeline, ...) is destroyed once the
    // GPU has finished every frame that might still reference it. Pass with std::move.
    // Resources from the UploadQueue additionally need IsReady(ticket) before release.
    template <std::movable T>
    void DeferRelease(T resource)
    {
        GarbageSlot().garbage.push_back(std::make_shared<T>(std::move(resource)));
    }
    // Per-frame linear allocator (host-visible, BDA). Valid only between BeginFrame/EndFrame;
    // memory is recycled when this frame slot comes around again.
    [[nodiscard]] TransientAllocation AllocateTransient(VkDeviceSize size, VkDeviceSize alignment = 256);

    template <class T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] VkDeviceAddress PushTransient(const T& value, VkDeviceSize alignment = 256)
    {
        const TransientAllocation a = AllocateTransient(sizeof(T), alignment);
        std::memcpy(a.cpu, &value, sizeof(T));
        return a.gpu;
    }

    [[nodiscard]] std::uint32_t DefaultTextureIndex(DefaultTexture t) const
    {
        return m_DefaultTextureSlots[static_cast<std::size_t>(t)];
    }

    // Same timing, arbitrary cleanup (e.g. freeing a bindless slot).
    void DeferCall(std::function<void()> fn) { GarbageSlot().deferred.push_back(std::move(fn)); }

    // Texture table: materials store table indices, the table maps them to bindless slots, so a
    // texture can be swapped (reload, streaming, placeholder) without touching any material.
    // Main thread. Entries [0, DefaultTexture::Count) are the default textures.
    [[nodiscard]] std::uint32_t AllocateTextureEntry(std::uint32_t bindlessSlot);
    void SetTextureEntry(std::uint32_t entry, std::uint32_t bindlessSlot); // visible from the next frame on
    void FreeTextureEntry(std::uint32_t entry); // deferred: in-flight frames may still read it
    [[nodiscard]] std::uint32_t TextureEntry(std::uint32_t entry) const { return m_TextureTable[entry]; }
    [[nodiscard]] std::uint32_t TextureEntryCount() const { return static_cast<std::uint32_t>(m_TextureTable.size()); }
    // uint[] of this frame (BeginFrame .. EndFrame): FrameUniforms::textureTable.
    [[nodiscard]] VkDeviceAddress TextureTableAddress() const;

    // Shader hot reload (development): BeginFrame polls the GLSL sources and recompiles changed
    // shaders; owners of pipelines rebuild them when ShaderGeneration() changes. Main thread.
    void SetShaderHotReload(bool enabled);
    [[nodiscard]] bool ShaderHotReloadEnabled() const { return m_ShaderHotReload; }
    [[nodiscard]] ShaderHotReload*       ShaderReloader() { return m_ShaderReload.get(); } // null until enabled
    [[nodiscard]] const ShaderHotReload* ShaderReloader() const { return m_ShaderReload.get(); }
    [[nodiscard]] std::uint64_t ShaderGeneration() const { return m_ShaderGeneration; }
    void NotifyShadersChanged() { ++m_ShaderGeneration; } // SPIR-V replaced by other means: rebuild pipelines

private:
    struct FrameData {
        VkCommandPool   pool           = VK_NULL_HANDLE;
        VkCommandBuffer cmd            = VK_NULL_HANDLE;
        VkFence         inFlight       = VK_NULL_HANDLE;
        VkSemaphore     imageAvailable = VK_NULL_HANDLE;
        // shared_ptr<void> keeps the real deleter: destroying it runs ~Buffer/~Image/...
        std::vector<std::shared_ptr<void>>   garbage;
        std::vector<std::function<void()>>   deferred;
        Buffer                               transient;
        VkDeviceSize                         transientOffset = 0;
        std::uint64_t                        uploadWait      = 0; // upload timeline value to wait on
        std::uint32_t                        frameScope      = ~0u;
        Buffer                               textureTable;         // copy of m_TextureTable
        std::uint64_t                        textureTableVersion = 0;
    };

    FrameData& GarbageSlot();
    static void CollectGarbage(FrameData& frame);

    void RecreateSwapchain();
    void CreateDefaultTextures();
    void GrowRenderFinishedSemaphores();
    void UpdateTextureTable(FrameData& frame);

    VulkanContext&             m_Ctx;
    Window&                    m_Window;
    std::unique_ptr<Swapchain>        m_Swapchain;
    std::unique_ptr<BindlessRegistry> m_Bindless;
    std::unique_ptr<UploadQueue>      m_Upload;
    std::unique_ptr<GpuProfiler>      m_Profiler;
    std::unique_ptr<GeometryPool>     m_Geometry;

    static constexpr std::size_t kDefaultTextureCount = static_cast<std::size_t>(DefaultTexture::Count);
    std::array<Image, kDefaultTextureCount>         m_DefaultTextures;
    std::array<std::uint32_t, kDefaultTextureCount> m_DefaultTextureSlots{};

    std::vector<std::uint32_t> m_TextureTable; // entry -> bindless slot
    std::vector<std::uint32_t> m_FreeTextureEntries;
    std::uint64_t              m_TextureTableVersion = 1;

    std::unique_ptr<ShaderHotReload> m_ShaderReload;
    bool                             m_ShaderHotReload  = false;
    std::uint64_t                    m_ShaderGeneration = 0;
    std::uint64_t                    m_ReloadGeneration = 0; // m_ShaderReload->Generation() already counted

    std::array<FrameData, kFramesInFlight> m_Frames{};
    // Per swapchain IMAGE, not per frame: presentation may still hold the semaphore
    // when the same frame slot comes around again.
    std::vector<VkSemaphore> m_RenderFinished;

    std::uint32_t m_FrameIndex    = 0;
    bool          m_ResizePending = false;
    bool          m_FrameActive   = false;

    Subscription m_ResizeSub; // last member: unsubscribes before anything else is torn down
};

} // namespace Engine
