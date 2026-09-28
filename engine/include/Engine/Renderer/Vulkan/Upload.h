#pragma once
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Engine {

struct TextureDesc {
    const void*   pixels       = nullptr; // tightly packed, 4 bytes per pixel
    std::uint32_t width        = 0;
    std::uint32_t height       = 0;
    VkFormat      format       = VK_FORMAT_R8G8B8A8_SRGB; // use *_UNORM for normal/ORM maps
    bool          generateMips = true;
    const char*   debugName    = nullptr;
};

// Timeline value of the transfer batch an upload was recorded into. 0 = nothing to wait for.
using UploadTicket = std::uint64_t;

// Asynchronous uploads on the dedicated transfer queue (falls back to the graphics queue).
//
//   any thread : CreateBuffer / CreateTexture2D stage the data and record the copy into the
//                currently open batch.
//   main thread: Submit() sends the open batch to the transfer queue; it signals a timeline
//                semaphore with the batch's ticket.
//   main thread: RecordAcquires() (called by the Renderer at frame start) takes finished
//                batches, records the queue-family acquire barriers plus mip generation
//                (blits need a graphics queue) into the frame's command buffer, and returns
//                the timeline value that frame's submission waits on.
//
// A resource must not be used or destroyed before IsReady(ticket).
class UploadQueue {
public:
    explicit UploadQueue(const VulkanContext& ctx);
    ~UploadQueue(); // device must be idle

    UploadQueue(const UploadQueue&)            = delete;
    UploadQueue& operator=(const UploadQueue&) = delete;

    // --- Thread-safe ---------------------------------------------------------------------
    // `ticket` is raised (never lowered) to cover this upload, so one ticket can track many.
    [[nodiscard]] Buffer CreateBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage,
                                      UploadTicket& ticket, const char* debugName = nullptr);
    [[nodiscard]] Image  CreateTexture2D(const TextureDesc& desc, UploadTicket& ticket);

    // True once the graphics-side acquire has been recorded into a frame: usable in that
    // frame's commands and every later graphics submission.
    [[nodiscard]] bool IsReady(UploadTicket ticket) const
    {
        return ticket <= m_AcquiredValue.load(std::memory_order_acquire);
    }

    // --- Main thread ---------------------------------------------------------------------
    void Submit();
    [[nodiscard]] std::uint64_t RecordAcquires(VkCommandBuffer graphicsCmd);

    // Submit + wait + acquire on a one-shot graphics submission. Stalls: startup/tools only.
    void Flush();

    // Blocking one-shot graphics submission, optionally waiting on the upload timeline first.
    void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record, std::uint64_t waitValue = 0);

    [[nodiscard]] VkSemaphore Timeline() const { return m_Timeline; }
    [[nodiscard]] bool UsesOwnershipTransfer() const { return m_OwnershipTransfer; }

private:
    struct PendingAcquire {
        VkBuffer      buffer    = VK_NULL_HANDLE; // either a buffer ...
        VkImage       image     = VK_NULL_HANDLE; // ... or an image (mip 0 uploaded)
        VkExtent2D    extent{};
        std::uint32_t mipLevels = 1;
    };

    struct Batch {
        std::uint64_t               value = 0; // timeline value signaled on completion
        VkCommandPool               pool  = VK_NULL_HANDLE;
        VkCommandBuffer             cmd   = VK_NULL_HANDLE;
        std::vector<Buffer>         staging;
        std::vector<PendingAcquire> acquires;
    };

    // Opens a batch if needed, runs `record` on its command buffer (under the lock) and
    // takes ownership of the staging buffer. Returns the batch's ticket.
    UploadTicket Record(Buffer staging, const PendingAcquire& acquire,
                        const std::function<void(VkCommandBuffer)>& record);
    std::unique_ptr<Batch> AcquireBatchLocked();

    void RecordBatchEndBarriers(const Batch& batch) const;      // transfer queue: release / visibility
    void RecordAcquireBarriers(VkCommandBuffer cmd, const Batch& batch) const;
    static void RecordMipChain(VkCommandBuffer cmd, const PendingAcquire& image);

    const VulkanContext& m_Ctx;
    bool                 m_OwnershipTransfer = false;
    VkSemaphore          m_Timeline          = VK_NULL_HANDLE;

    std::mutex                          m_Mutex; // guards m_Open, m_FreeBatches, m_NextValue
    std::unique_ptr<Batch>              m_Open;
    std::vector<std::unique_ptr<Batch>> m_FreeBatches;
    std::uint64_t                       m_NextValue = 1;

    std::deque<std::unique_ptr<Batch>> m_InFlight; // main thread only, ascending values
    std::atomic<std::uint64_t>         m_AcquiredValue{0};

    // Graphics-queue one-shot submissions (ImmediateSubmit / Flush).
    VkCommandPool   m_ImmediatePool  = VK_NULL_HANDLE;
    VkCommandBuffer m_ImmediateCmd   = VK_NULL_HANDLE;
    VkFence         m_ImmediateFence = VK_NULL_HANDLE;
};

} // namespace Engine
