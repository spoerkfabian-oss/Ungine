#pragma once
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

// One mip level of a pre-built image (CreateTexture): byte range in the source data.
struct UploadMip {
    std::uint64_t offset = 0; // multiple of 16 (block-compressed formats)
    std::uint64_t size   = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

struct UploadQueueDesc {
    VkDeviceSize stagingRingSize = 64ull << 20; // persistent staging memory; larger uploads get their own buffer
    VkDeviceSize frameBudget     = 32ull << 20; // bytes Submit() sends per call (at least one batch)
};

struct UploadStats {
    VkDeviceSize  ringUsed           = 0;
    VkDeviceSize  ringCapacity       = 0;
    VkDeviceSize  submittedLastFrame = 0; // bytes sent by the last Submit()
    std::uint32_t queuedBatches      = 0; // recorded, waiting for Submit (budget)
    std::uint32_t inFlightBatches    = 0; // submitted, not acquired yet
    std::uint64_t dedicatedStaging   = 0; // uploads that did not fit the ring (total)
    std::uint64_t totalBytes         = 0; // everything uploaded so far
};

// Asynchronous uploads on the dedicated transfer queue (falls back to the graphics queue).
//
//   any thread : CreateBuffer / CreateTexture* / WriteBuffer copy the data into the staging ring
//                (own staging buffer if it does not fit) and record the copy into the open batch.
//                A batch that exceeds the frame budget is closed and a new one opened.
//   main thread: Submit() sends closed batches (and then the open one) to the transfer queue, at
//                most `frameBudget` bytes per call, so large loads stream over several frames;
//                each batch signals a timeline semaphore with its ticket.
//   main thread: RecordAcquires() (called by the Renderer at frame start) takes finished
//                batches, records the queue-family acquire barriers plus mip generation
//                (blits need a graphics queue) into the frame's command buffer, and returns
//                the timeline value that frame's submission waits on.
//
// A resource must not be used or destroyed before IsReady(ticket).
class UploadQueue {
public:
    explicit UploadQueue(const VulkanContext& ctx, const UploadQueueDesc& desc = {});
    ~UploadQueue(); // device must be idle

    UploadQueue(const UploadQueue&)            = delete;
    UploadQueue& operator=(const UploadQueue&) = delete;

    // --- Thread-safe ---------------------------------------------------------------------
    // `ticket` is raised (never lowered) to cover this upload, so one ticket can track many.
    [[nodiscard]] Buffer CreateBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage,
                                      UploadTicket& ticket, const char* debugName = nullptr);
    [[nodiscard]] Image  CreateTexture2D(const TextureDesc& desc, UploadTicket& ticket);
    // Every mip level provided (e.g. block-compressed): no blits, SHADER_READ_ONLY once acquired.
    [[nodiscard]] Image  CreateTexture(VkFormat format, std::uint32_t width, std::uint32_t height,
                                       std::span<const std::byte> data, std::span<const UploadMip> mips,
                                       UploadTicket& ticket, const char* debugName = nullptr);
    // Writes into part of an existing buffer created with BufferDesc::concurrent (no ownership
    // transfer: the frame's wait on the upload timeline makes the data visible).
    void WriteBuffer(VkBuffer dst, VkDeviceSize dstOffset, std::span<const std::byte> data, UploadTicket& ticket);

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

    [[nodiscard]] UploadStats Stats() const;
    [[nodiscard]] VkSemaphore Timeline() const { return m_Timeline; }
    [[nodiscard]] bool UsesOwnershipTransfer() const { return m_OwnershipTransfer; }

private:
    struct PendingAcquire {
        VkBuffer      buffer    = VK_NULL_HANDLE; // either a buffer ...
        VkImage       image     = VK_NULL_HANDLE; // ... or an image (mip 0 uploaded); neither: no barriers
        VkExtent2D    extent{};
        std::uint32_t mipLevels    = 1;
        std::uint32_t uploadedMips = 1; // < mipLevels: the rest is blitted on the graphics queue
    };

    struct Batch {
        std::uint64_t               value = 0; // timeline value signaled on completion
        VkCommandPool               pool  = VK_NULL_HANDLE;
        VkCommandBuffer             cmd   = VK_NULL_HANDLE;
        std::vector<Buffer>         staging; // uploads that did not fit the ring
        std::vector<PendingAcquire> acquires;
        VkDeviceSize                bytes     = 0;
        VkDeviceSize                ringBytes = 0; // ring space (incl. padding) freed when the batch completes
        VkDeviceSize                ringEnd   = 0; // ring head after its last allocation
        std::uint32_t               pendingWrites = 0; // uploads copying into their staging memory (unlocked)
        bool                        failed = false; // a recording/copy failure poisoned this batch; never submit it
    };

    // Copies `data` into staging memory, opens / closes batches for the budget and runs `record`
    // (under the lock) with the staging buffer and offset. Returns the batch's ticket.
    UploadTicket Record(std::span<const std::byte> data, const PendingAcquire& acquire,
                        const std::function<void(VkCommandBuffer, VkBuffer, VkDeviceSize)>& record);
    std::unique_ptr<Batch> AcquireBatchLocked();
    void                   OpenBatchLocked();
    [[nodiscard]] std::optional<VkDeviceSize> RingAllocateLocked(VkDeviceSize size, Batch& batch);
    void SubmitBatches(bool all);
    void SubmitBatch(std::unique_ptr<Batch> batch);
    void RecycleBatch(std::unique_ptr<Batch> batch);

    void RecordBatchEndBarriers(const Batch& batch) const;      // transfer queue: release / visibility
    void RecordAcquireBarriers(VkCommandBuffer cmd, const Batch& batch) const;
    static void RecordMipChain(VkCommandBuffer cmd, const PendingAcquire& image);

    const VulkanContext& m_Ctx;
    bool                 m_OwnershipTransfer = false;
    VkSemaphore          m_Timeline          = VK_NULL_HANDLE;

    UploadQueueDesc                     m_Desc;
    mutable std::mutex                  m_Mutex; // guards the batches, the ring, m_NextValue and m_Stats
    std::condition_variable m_WritesDone; // pendingWrites of some batch reached 0 (Flush waits)
    std::unique_ptr<Batch>              m_Open;
    std::deque<std::unique_ptr<Batch>>  m_Closed; // over the budget, waiting for Submit
    Buffer                              m_Ring;
    VkDeviceSize                        m_RingHead = 0, m_RingTail = 0, m_RingUsed = 0;
    UploadStats                         m_Stats;
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
