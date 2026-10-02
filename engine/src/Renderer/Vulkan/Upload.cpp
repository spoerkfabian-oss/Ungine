#include "Engine/Renderer/Vulkan/Upload.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>

namespace Engine {

namespace {
constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

// Every way graphics/compute work may read an uploaded buffer (BDA reads are storage reads).
constexpr VkPipelineStageFlags2 kBufferReadStages = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                                                    VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                                                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | kShaderStages;
constexpr VkAccessFlags2 kBufferReadAccess = VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                                             VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;

// Layout of the uploaded mips once the graphics queue owns them: blit source if a mip chain follows.
template <class Acquire>
VkImageLayout UploadedLayout(const Acquire& a)
{
    return a.uploadedMips < a.mipLevels ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkCommandPool CreatePool(VkDevice device, std::uint32_t family)
{
    VkCommandPoolCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; // reset as a whole
    info.queueFamilyIndex = family;
    VkCommandPool pool    = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &info, nullptr, &pool));
    return pool;
}

VkCommandBuffer AllocateCommandBuffer(VkDevice device, VkCommandPool pool)
{
    VkCommandBufferAllocateInfo info{};
    info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool        = pool;
    info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &info, &cmd));
    return cmd;
}

void BeginOneTimeCommands(VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
}

void PipelineBarriers(VkCommandBuffer cmd, const std::vector<VkBufferMemoryBarrier2>& buffers,
                      const std::vector<VkImageMemoryBarrier2>& images)
{
    if (buffers.empty() && images.empty())
        return;
    VkDependencyInfo dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buffers.size());
    dep.pBufferMemoryBarriers    = buffers.data();
    dep.imageMemoryBarrierCount  = static_cast<std::uint32_t>(images.size());
    dep.pImageMemoryBarriers     = images.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace

UploadQueue::UploadQueue(const VulkanContext& ctx, const UploadQueueDesc& desc)
    : m_Ctx(ctx)
    , m_OwnershipTransfer(ctx.TransferQueue().family != ctx.GraphicsQueue().family)
    , m_Desc(desc)
{
    if (desc.stagingRingSize > 0)
        m_Ring = Buffer(ctx, {.size      = desc.stagingRingSize,
                              .usage     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .memory    = MemoryUsage::Upload,
                              .debugName = "StagingRing"});
    m_Stats.ringCapacity = desc.stagingRingSize;
    const VkDevice dev = ctx.Device();

    VkSemaphoreTypeCreateInfo type{};
    type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue  = 0;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    VK_CHECK(vkCreateSemaphore(dev, &info, nullptr, &m_Timeline));
    SetDebugName(dev, VK_OBJECT_TYPE_SEMAPHORE, m_Timeline, "UploadTimeline");

    m_ImmediatePool  = CreatePool(dev, ctx.GraphicsQueue().family);
    m_ImmediateCmd   = AllocateCommandBuffer(dev, m_ImmediatePool);
    m_ImmediateFence = MakeFence(dev, false);
}

UploadQueue::~UploadQueue()
{
    // Staging buffers go with the Batch objects; only the pools need explicit destruction.
    const VkDevice dev     = m_Ctx.Device();
    const auto     destroy = [dev](const std::unique_ptr<Batch>& b) {
        if (b)
            vkDestroyCommandPool(dev, b->pool, nullptr);
    };
    destroy(m_Open);
    for (const auto& b : m_Closed)
        destroy(b);
    for (const auto& b : m_InFlight)
        destroy(b);
    for (const auto& b : m_FreeBatches)
        destroy(b);

    vkDestroyFence(dev, m_ImmediateFence, nullptr);
    vkDestroyCommandPool(dev, m_ImmediatePool, nullptr);
    vkDestroySemaphore(dev, m_Timeline, nullptr);
}

std::unique_ptr<UploadQueue::Batch> UploadQueue::AcquireBatchLocked()
{
    if (!m_FreeBatches.empty()) {
        std::unique_ptr<Batch> batch = std::move(m_FreeBatches.back());
        m_FreeBatches.pop_back();
        return batch;
    }
    auto batch  = std::make_unique<Batch>();
    batch->pool = CreatePool(m_Ctx.Device(), m_Ctx.TransferQueue().family);
    batch->cmd  = AllocateCommandBuffer(m_Ctx.Device(), batch->pool);
    return batch;
}

void UploadQueue::OpenBatchLocked()
{
    m_Open        = AcquireBatchLocked();
    m_Open->value = m_NextValue++; // opened in submission order -> values stay monotonic
    BeginOneTimeCommands(m_Open->cmd);
    // Staging memory and pool ranges may reuse memory that earlier (fenced, finished) work
    // wrote. That is safe, but sync validation does not follow host fence waits across memory
    // reuse: order the batch after all earlier writes on this queue explicitly.
    VkMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(m_Open->cmd, &dep);
}

std::optional<VkDeviceSize> UploadQueue::RingAllocateLocked(VkDeviceSize size, Batch& batch)
{
    const VkDeviceSize capacity = m_Ring ? m_Ring.Size() : 0;
    if (size == 0 || size > capacity / 2) // large uploads would starve the ring
        return std::nullopt;
    if (m_RingUsed == 0)
        m_RingHead = m_RingTail = 0;
    constexpr VkDeviceSize kAlign = 16; // BCn block size, 4-byte copy rule
    VkDeviceSize start = (m_RingHead + kAlign - 1) / kAlign * kAlign;
    VkDeviceSize waste = 0;
    if (m_RingHead >= m_RingTail) { // free: [head, capacity) and [0, tail)
        if (m_RingUsed > 0 && m_RingHead == m_RingTail)
            return std::nullopt; // full
        if (start + size <= capacity) {
            waste = start - m_RingHead;
        } else if (size <= m_RingTail) { // wrap: the end of the ring is skipped
            waste = capacity - m_RingHead;
            start = 0;
        } else {
            return std::nullopt;
        }
    } else { // free: [head, tail)
        if (start + size > m_RingTail)
            return std::nullopt;
        waste = start - m_RingHead;
    }
    m_RingHead = start + size;
    m_RingUsed += waste + size;
    batch.ringBytes += waste + size;
    batch.ringEnd = m_RingHead;
    return start;
}

UploadTicket UploadQueue::Record(std::span<const std::byte> data, const PendingAcquire& acquire,
                                 const std::function<void(VkCommandBuffer, VkBuffer, VkDeviceSize)>& record)
{
    assert(!data.empty());
    // 1) Under the lock: pick the batch and reserve staging memory. The batch cannot be
    //    submitted while it has pending writes.
    Batch*                      batch = nullptr;
    std::optional<VkDeviceSize> ringOffset;
    {
        std::scoped_lock lock{m_Mutex};
        // Budget: a full batch is closed; Submit() sends at most frameBudget bytes per frame.
        if (m_Open && m_Open->bytes > 0 && m_Open->bytes + data.size() > m_Desc.frameBudget)
            m_Closed.push_back(std::move(m_Open));
        if (!m_Open)
            OpenBatchLocked();
        batch      = m_Open.get();
        ringOffset = RingAllocateLocked(data.size(), *batch);
        batch->bytes += data.size();
        ++batch->pendingWrites;
    }

    // 2) Unlocked: copy into the ring or into an own staging buffer (large uploads, full ring),
    //    so workers do not serialize on big textures.
    Buffer staging;
    if (ringOffset) {
        std::memcpy(static_cast<std::byte*>(m_Ring.Mapped()) + *ringOffset, data.data(), data.size());
        m_Ring.Flush(*ringOffset, data.size());
    } else {
        staging = Buffer(m_Ctx, {.size = data.size(), .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 .memory = MemoryUsage::Upload, .debugName = "staging"});
        staging.Write(data.data(), data.size());
    }

    // 3) Under the lock again: record the copy (the command buffer is shared by the batch).
    std::scoped_lock lock{m_Mutex};
    record(batch->cmd, ringOffset ? m_Ring.Handle() : staging.Handle(), ringOffset.value_or(0));
    if (!ringOffset) {
        batch->staging.push_back(std::move(staging));
        ++m_Stats.dedicatedStaging;
    }
    batch->acquires.push_back(acquire);
    m_Stats.totalBytes += data.size();
    if (--batch->pendingWrites == 0)
        m_WritesDone.notify_all();
    return batch->value;
}

Buffer UploadQueue::CreateBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage, UploadTicket& ticket,
                                 const char* debugName)
{
    assert(!data.empty());
    Buffer buffer(m_Ctx, {.size = data.size(), .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          .memory = MemoryUsage::GpuOnly, .debugName = debugName});
    const VkBuffer     dst  = buffer.Handle();
    const VkDeviceSize size = data.size();
    const UploadTicket t    = Record(data, {.buffer = dst}, [&](VkCommandBuffer cmd, VkBuffer src, VkDeviceSize offset) {
        const VkBufferCopy region{offset, 0, size};
        vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    });
    ticket = std::max(ticket, t);
    return buffer;
}

void UploadQueue::WriteBuffer(VkBuffer dst, VkDeviceSize dstOffset, std::span<const std::byte> data,
                              UploadTicket& ticket)
{
    const VkDeviceSize size = data.size();
    const UploadTicket t    = Record(data, {}, [&](VkCommandBuffer cmd, VkBuffer src, VkDeviceSize offset) {
        const VkBufferCopy region{offset, dstOffset, size};
        vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    });
    ticket = std::max(ticket, t);
}

Image UploadQueue::CreateTexture2D(const TextureDesc& desc, UploadTicket& ticket)
{
    assert(desc.pixels && desc.width > 0 && desc.height > 0);
    const VkDeviceSize byteSize = VkDeviceSize{desc.width} * desc.height * 4;

    std::uint32_t mipLevels = 1;
    if (desc.generateMips) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_Ctx.PhysicalDevice(), desc.format, &props);
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((props.optimalTilingFeatures & required) == required)
            mipLevels = std::bit_width(std::max(desc.width, desc.height)); // floor(log2(n)) + 1
        else
            ENGINE_WARN("Format {} can't be linearly blitted; skipping mip generation", static_cast<int>(desc.format));
    }

    Image image(m_Ctx, {.extent    = {desc.width, desc.height, 1},
                        .format    = desc.format,
                        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        .mipLevels = mipLevels,
                        .debugName = desc.debugName});

    // Transfer queue: mip 0 only. Blits need a graphics queue -> the mip chain is built in RecordAcquires.
    const VkImage      dst = image.Handle();
    const UploadTicket t   = Record({static_cast<const std::byte*>(desc.pixels), byteSize},
                                    {.image = dst, .extent = {desc.width, desc.height}, .mipLevels = mipLevels},
                                    [&](VkCommandBuffer cmd, VkBuffer src, VkDeviceSize offset) {
        CmdImageBarrier(cmd, {.image     = dst,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .dstStage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                              .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .baseMip   = 0,
                              .mipCount  = 1});
        VkBufferImageCopy copy{};
        copy.bufferOffset     = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {desc.width, desc.height, 1};
        vkCmdCopyBufferToImage(cmd, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    });
    ticket = std::max(ticket, t);
    return image;
}

Image UploadQueue::CreateTexture(VkFormat format, std::uint32_t width, std::uint32_t height,
                                 std::span<const std::byte> data, std::span<const UploadMip> mips, UploadTicket& ticket,
                                 const char* debugName)
{
    assert(!mips.empty() && !data.empty());
    const auto levels = static_cast<std::uint32_t>(mips.size());
    Image      image(m_Ctx, {.extent    = {width, height, 1},
                             .format    = format,
                             .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .mipLevels = levels,
                             .debugName = debugName});
    const VkImage      dst = image.Handle();
    const UploadTicket t   = Record(data, {.image = dst, .extent = {width, height}, .mipLevels = levels, .uploadedMips = levels},
                                    [&](VkCommandBuffer cmd, VkBuffer src, VkDeviceSize offset) {
        CmdImageBarrier(cmd, {.image     = dst,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .dstStage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                              .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .baseMip   = 0,
                              .mipCount  = levels});
        std::vector<VkBufferImageCopy> regions(levels);
        for (std::uint32_t i = 0; i < levels; ++i) {
            regions[i].bufferOffset     = offset + mips[i].offset;
            regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            regions[i].imageExtent      = {mips[i].width, mips[i].height, 1};
        }
        vkCmdCopyBufferToImage(cmd, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions.data());
    });
    ticket = std::max(ticket, t);
    return image;
}

void UploadQueue::Submit()
{
    SubmitBatches(false);
}

void UploadQueue::SubmitBatches(bool all)
{
    std::vector<std::unique_ptr<Batch>> batches;
    VkDeviceSize                        sent = 0;
    {
        std::unique_lock lock{m_Mutex};
        if (all) // Flush: everything, so wait for copies still in progress
            m_WritesDone.wait(lock, [&] {
                return (!m_Open || m_Open->pendingWrites == 0) &&
                       std::ranges::all_of(m_Closed, [](const auto& b) { return b->pendingWrites == 0; });
            });
        // Oldest first; always at least one batch so large uploads make progress. A batch whose
        // staging memory is still being written stops the submission (order is kept).
        const auto fits = [&](const Batch& b) {
            return b.pendingWrites == 0 && (all || sent == 0 || sent + b.bytes <= m_Desc.frameBudget);
        };
        while (!m_Closed.empty() && fits(*m_Closed.front())) {
            sent += m_Closed.front()->bytes;
            batches.push_back(std::move(m_Closed.front()));
            m_Closed.pop_front();
        }
        if (m_Closed.empty() && m_Open && fits(*m_Open)) { // workers open a fresh batch from here on
            sent += m_Open->bytes;
            batches.push_back(std::move(m_Open));
        }
        m_Stats.submittedLastFrame = sent;
    }
    for (std::unique_ptr<Batch>& batch : batches)
        SubmitBatch(std::move(batch));
}

void UploadQueue::SubmitBatch(std::unique_ptr<Batch> batch)
{
    RecordBatchEndBarriers(*batch);
    VK_CHECK(vkEndCommandBuffer(batch->cmd));

    VkSemaphoreSubmitInfo signal{};
    signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = m_Timeline;
    signal.value     = batch->value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = batch->cmd;

    VkSubmitInfo2 submit{};
    submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos    = &signal;
    VK_CHECK(vkQueueSubmit2(m_Ctx.TransferQueue().handle, 1, &submit, VK_NULL_HANDLE));

    m_InFlight.push_back(std::move(batch));
}

void UploadQueue::RecycleBatch(std::unique_ptr<Batch> batch)
{
    // The transfer queue is done with it: staging memory and the command pool are reused.
    batch->staging.clear();
    batch->acquires.clear();
    batch->value = 0;
    batch->bytes = 0;
    VK_CHECK(vkResetCommandPool(m_Ctx.Device(), batch->pool, 0));
    std::scoped_lock lock{m_Mutex};
    if (batch->ringBytes > 0) { // batches complete in ring order
        m_RingUsed -= batch->ringBytes;
        m_RingTail = batch->ringEnd;
    }
    batch->ringBytes = 0;
    batch->ringEnd   = 0;
    m_FreeBatches.push_back(std::move(batch));
}

std::uint64_t UploadQueue::RecordAcquires(VkCommandBuffer graphicsCmd)
{
    if (m_InFlight.empty())
        return 0;

    std::uint64_t completed = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(m_Ctx.Device(), m_Timeline, &completed));

    std::uint64_t acquired = 0;
    while (!m_InFlight.empty() && m_InFlight.front()->value <= completed) {
        std::unique_ptr<Batch> batch = std::move(m_InFlight.front());
        m_InFlight.pop_front();

        RecordAcquireBarriers(graphicsCmd, *batch);
        for (const PendingAcquire& a : batch->acquires)
            if (a.image && a.uploadedMips < a.mipLevels)
                RecordMipChain(graphicsCmd, a);
        acquired = batch->value;
        RecycleBatch(std::move(batch));
    }

    if (acquired == 0)
        return 0;
    m_AcquiredValue.store(acquired, std::memory_order_release);
    return acquired; // already signaled: the frame's wait on it never stalls
}

void UploadQueue::Flush()
{
    SubmitBatches(true);
    if (m_InFlight.empty())
        return;

    const std::uint64_t last = m_InFlight.back()->value;
    VkSemaphoreWaitInfo wait{};
    wait.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores    = &m_Timeline;
    wait.pValues        = &last;
    VK_CHECK(vkWaitSemaphores(m_Ctx.Device(), &wait, UINT64_MAX));

    ImmediateSubmit([this](VkCommandBuffer cmd) { (void)RecordAcquires(cmd); }, last);
}

UploadStats UploadQueue::Stats() const
{
    std::scoped_lock lock{m_Mutex};
    UploadStats stats     = m_Stats;
    stats.ringUsed        = m_RingUsed;
    stats.queuedBatches   = static_cast<std::uint32_t>(m_Closed.size()) + (m_Open ? 1u : 0u);
    stats.inFlightBatches = static_cast<std::uint32_t>(m_InFlight.size()); // main thread only (Stats caller)
    return stats;
}

void UploadQueue::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record, std::uint64_t waitValue)
{
    const VkDevice dev = m_Ctx.Device();
    VK_CHECK(vkResetCommandPool(dev, m_ImmediatePool, 0));
    BeginOneTimeCommands(m_ImmediateCmd);
    record(m_ImmediateCmd);
    VK_CHECK(vkEndCommandBuffer(m_ImmediateCmd));

    VkSemaphoreSubmitInfo wait{};
    wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = m_Timeline;
    wait.value     = waitValue;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = m_ImmediateCmd;

    VkSubmitInfo2 submit{};
    submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = waitValue > 0 ? 1u : 0u;
    submit.pWaitSemaphoreInfos    = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos    = &cmdInfo;
    VK_CHECK(vkQueueSubmit2(m_Ctx.GraphicsQueue().handle, 1, &submit, m_ImmediateFence));

    VK_CHECK(vkWaitForFences(dev, 1, &m_ImmediateFence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(dev, 1, &m_ImmediateFence));
}

void UploadQueue::RecordBatchEndBarriers(const Batch& batch) const
{
    if (!m_OwnershipTransfer) {
        // Shared queue family: make the copies visible to everything later on the queue right
        // here, so every path into a later frame (submission order, present -> acquire
        // semaphore) sees them synchronized. The graphics-side barriers then only move layouts.
        VkMemoryBarrier2 barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dep{};
        dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(batch.cmd, &dep);
        return;
    }

    // Queue family release (transfer side). Must mirror RecordAcquireBarriers exactly
    // (same range, same old/new layout); the layout transition executes only once.
    const std::uint32_t transfer = m_Ctx.TransferQueue().family;
    const std::uint32_t graphics = m_Ctx.GraphicsQueue().family;

    std::vector<VkBufferMemoryBarrier2> buffers;
    std::vector<VkImageMemoryBarrier2>  images;
    for (const PendingAcquire& a : batch.acquires) {
        if (!a.buffer && !a.image)
            continue; // concurrent buffer write: the timeline semaphore is enough
        if (a.buffer) {
            VkBufferMemoryBarrier2& b = buffers.emplace_back();
            b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            b.srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT;
            b.srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; // chains to the timeline signal
            b.srcQueueFamilyIndex = transfer;
            b.dstQueueFamilyIndex = graphics;
            b.buffer              = a.buffer;
            b.size                = VK_WHOLE_SIZE;
        } else {
            images.push_back(MakeImageBarrier({.image     = a.image,
                                               .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                               .newLayout = UploadedLayout(a),
                                               .srcStage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                                               .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                               .dstStage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                               .baseMip   = 0,
                                               .mipCount  = a.uploadedMips,
                                               .srcFamily = transfer,
                                               .dstFamily = graphics}));
        }
    }
    PipelineBarriers(batch.cmd, buffers, images);
}

void UploadQueue::RecordAcquireBarriers(VkCommandBuffer cmd, const Batch& batch) const
{
    // srcStage ALL_TRANSFER chains with the frame's timeline wait (ALL_COMMANDS). With an
    // ownership transfer this is the acquire half (srcAccess is ignored -> 0); on a shared
    // queue family it is an ordinary barrier that also performs the layout transition.
    const std::uint32_t  srcFamily = m_OwnershipTransfer ? m_Ctx.TransferQueue().family : VK_QUEUE_FAMILY_IGNORED;
    const std::uint32_t  dstFamily = m_OwnershipTransfer ? m_Ctx.GraphicsQueue().family : VK_QUEUE_FAMILY_IGNORED;
    const VkAccessFlags2 srcAccess = m_OwnershipTransfer ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT;

    std::vector<VkBufferMemoryBarrier2> buffers;
    std::vector<VkImageMemoryBarrier2>  images;
    for (const PendingAcquire& a : batch.acquires) {
        if (!a.buffer && !a.image)
            continue;
        if (a.buffer) {
            VkBufferMemoryBarrier2& b = buffers.emplace_back();
            b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            b.srcStageMask        = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask       = srcAccess;
            b.dstStageMask        = kBufferReadStages;
            b.dstAccessMask       = kBufferReadAccess;
            b.srcQueueFamilyIndex = srcFamily;
            b.dstQueueFamilyIndex = dstFamily;
            b.buffer              = a.buffer;
            b.size                = VK_WHOLE_SIZE;
            continue;
        }

        const bool mips = a.uploadedMips < a.mipLevels; // the rest is blitted below
        images.push_back(MakeImageBarrier({.image     = a.image,
                                           .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .newLayout = UploadedLayout(a),
                                           .srcStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                           .srcAccess = srcAccess,
                                           .dstStage  = mips ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT : kShaderStages,
                                           .dstAccess = mips ? VK_ACCESS_2_TRANSFER_READ_BIT
                                                             : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                           .baseMip   = 0,
                                           .mipCount  = a.uploadedMips,
                                           .srcFamily = srcFamily,
                                           .dstFamily = dstFamily}));
        if (mips) {
            // Mips 1..n were never written: no ownership transfer needed, contents are discarded.
            images.push_back(MakeImageBarrier({.image     = a.image,
                                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                               .dstStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                               .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                               .baseMip   = 1,
                                               .mipCount  = a.mipLevels - 1}));
        }
    }
    PipelineBarriers(cmd, buffers, images);
}

void UploadQueue::RecordMipChain(VkCommandBuffer cmd, const PendingAcquire& a)
{
    // Entry: mip 0 TRANSFER_SRC, mips 1..n TRANSFER_DST. Each level is blitted from the previous one.
    auto w = static_cast<std::int32_t>(a.extent.width);
    auto h = static_cast<std::int32_t>(a.extent.height);
    for (std::uint32_t i = 1; i < a.mipLevels; ++i) {
        if (i > 1) {
            CmdImageBarrier(cmd, {.image     = a.image,
                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
                                  .baseMip   = i - 1,
                                  .mipCount  = 1});
        }

        const std::int32_t nw = std::max(w / 2, 1);
        const std::int32_t nh = std::max(h / 2, 1);

        VkImageBlit2 blit{};
        blit.sType          = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[1]  = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[1]  = {nw, nh, 1};

        VkBlitImageInfo2 info{};
        info.sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
        info.srcImage       = a.image;
        info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        info.dstImage       = a.image;
        info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        info.regionCount    = 1;
        info.pRegions       = &blit;
        info.filter         = VK_FILTER_LINEAR; // sRGB formats are filtered in linear space
        vkCmdBlitImage2(cmd, &info);

        w = nw;
        h = nh;
    }

    // Exit: mips [0, n-1) are TRANSFER_SRC, the last one TRANSFER_DST -> all SHADER_READ_ONLY.
    const std::array<VkImageMemoryBarrier2, 2> toShaderRead{
        MakeImageBarrier({.image     = a.image,
                          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          .srcAccess = VK_ACCESS_2_NONE, // only reads happened: execution dependency suffices
                          .dstStage  = kShaderStages,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .baseMip   = 0,
                          .mipCount  = a.mipLevels - 1}),
        MakeImageBarrier({.image     = a.image,
                          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStage  = kShaderStages,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .baseMip   = a.mipLevels - 1,
                          .mipCount  = 1})};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(toShaderRead.size());
    dep.pImageMemoryBarriers    = toShaderRead.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace Engine
