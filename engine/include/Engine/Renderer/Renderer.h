#pragma once
#include "Engine/Events/EventBus.h"
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
inline constexpr VkFormat      kDepthFormat         = VK_FORMAT_D32_SFLOAT; // reverse-Z: clear to 0
inline constexpr VkDeviceSize  kTransientBufferSize = 8ull << 20;          // per frame in flight

// 1x1 textures used when a material slot is empty.
enum class DefaultTexture : std::uint32_t { White, Black, FlatNormal, Count };

struct TransientAllocation {
    void*           cpu = nullptr;
    VkDeviceAddress gpu = 0;
};

struct RendererDesc {
    bool vsync = true;
};

// Everything a pass needs to record into the current frame.
// The swapchain image is in COLOR_ATTACHMENT_OPTIMAL and the depth buffer in
// DEPTH_ATTACHMENT_OPTIMAL (contents undefined -> clear it) when handed out.
struct FrameContext {
    VkCommandBuffer cmd        = VK_NULL_HANDLE;
    VkImage         image      = VK_NULL_HANDLE;
    VkImageView     view       = VK_NULL_HANDLE;
    VkFormat        format     = VK_FORMAT_UNDEFINED;
    VkExtent2D      extent{};
    std::uint32_t   frameIndex = 0; // [0, kFramesInFlight) - index per-frame resources with this
    std::uint32_t   imageIndex = 0; // swapchain image index
    VkImage         depthImage = VK_NULL_HANDLE;
    VkImageView     depthView  = VK_NULL_HANDLE;
    VkFormat        depthFormat = kDepthFormat;
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

    // Threading: bindless registry, upload queue (see its docs) and DefaultTextureIndex may be
    // used from worker threads. Everything else, DeferRelease/DeferCall included: main thread.
    [[nodiscard]] const VulkanContext& GetContext() const { return m_Ctx; }
    [[nodiscard]] const Swapchain&   GetSwapchain() const { return *m_Swapchain; }
    [[nodiscard]] BindlessRegistry&  GetBindless()        { return *m_Bindless; }
    [[nodiscard]] UploadQueue&       GetUploader()        { return *m_Upload; }

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
    };

    FrameData& GarbageSlot();
    static void CollectGarbage(FrameData& frame);

    void RecreateSwapchain();
    void CreateDepthBuffer();
    void CreateDefaultTextures();
    void GrowRenderFinishedSemaphores();

    VulkanContext&             m_Ctx;
    Window&                    m_Window;
    std::unique_ptr<Swapchain>        m_Swapchain;
    std::unique_ptr<BindlessRegistry> m_Bindless;
    std::unique_ptr<UploadQueue>      m_Upload;
    Image                             m_Depth;

    static constexpr std::size_t kDefaultTextureCount = static_cast<std::size_t>(DefaultTexture::Count);
    std::array<Image, kDefaultTextureCount>         m_DefaultTextures;
    std::array<std::uint32_t, kDefaultTextureCount> m_DefaultTextureSlots{};

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
