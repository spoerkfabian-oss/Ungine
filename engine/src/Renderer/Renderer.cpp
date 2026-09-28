#include "Engine/Renderer/Renderer.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/Events.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <cassert>
#include <cstddef>
#include <stdexcept>

namespace Engine {

Renderer::Renderer(VulkanContext& ctx, Window& window, EventBus& events, const RendererDesc& desc)
    : m_Ctx(ctx), m_Window(window)
{
    m_Swapchain = std::make_unique<Swapchain>(ctx, window.FramebufferExtent(), SwapchainDesc{.vsync = desc.vsync});
    m_Bindless  = std::make_unique<BindlessRegistry>(ctx);
    m_Upload    = std::make_unique<UploadQueue>(ctx);
    m_Profiler  = std::make_unique<GpuProfiler>(ctx, kFramesInFlight);
    CreateDefaultTextures();

    const VkDevice dev = ctx.Device();
    for (FrameData& f : m_Frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; // reset whole pool per frame
        poolInfo.queueFamilyIndex = ctx.GraphicsQueue().family;
        VK_CHECK(vkCreateCommandPool(dev, &poolInfo, nullptr, &f.pool));

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = f.pool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev, &alloc, &f.cmd));

        f.inFlight       = MakeFence(dev, true); // signaled: first wait passes immediately
        f.imageAvailable = MakeSemaphore(dev);
        f.transient      = Buffer(ctx, {.size      = kTransientBufferSize,
                                        .usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        .memory    = MemoryUsage::Upload,
                                        .debugName = "FrameTransient"});
    }
    GrowRenderFinishedSemaphores();

    m_ResizeSub = events.Subscribe<FramebufferResizeEvent>(
        [this](const FramebufferResizeEvent&) { m_ResizePending = true; });
}

Renderer::~Renderer()
{
    m_Ctx.WaitIdle();
    for (FrameData& f : m_Frames)
        CollectGarbage(f); // may reference the bindless registry -> run first
    m_Profiler.reset();
    m_Upload.reset();
    m_Bindless.reset();

    const VkDevice dev = m_Ctx.Device();
    for (FrameData& f : m_Frames) {
        vkDestroyFence(dev, f.inFlight, nullptr);
        vkDestroySemaphore(dev, f.imageAvailable, nullptr);
        vkDestroyCommandPool(dev, f.pool, nullptr); // frees its command buffer
    }
    for (VkSemaphore s : m_RenderFinished)
        vkDestroySemaphore(dev, s, nullptr);
    m_Swapchain.reset();
}

std::optional<FrameContext> Renderer::BeginFrame()
{
    assert(!m_FrameActive && "BeginFrame called twice without EndFrame");
    const VkDevice dev = m_Ctx.Device();
    FrameData&     f   = m_Frames[m_FrameIndex];

    // 1) CPU waits until the GPU finished the frame that last used this slot.
    VK_CHECK(vkWaitForFences(dev, 1, &f.inFlight, VK_TRUE, UINT64_MAX));
    CollectGarbage(f); // everything this slot's last submission could touch is now free
    f.transientOffset = 0;
    m_Profiler->BeginFrame(m_FrameIndex); // this slot's timestamps are complete now

    // Hand everything workers recorded since the last frame to the transfer queue.
    m_Upload->Submit();

    if (m_ResizePending)
        RecreateSwapchain();

    // 2) Acquire. On OUT_OF_DATE the semaphore is NOT signaled and the fence is still
    //    signaled (not reset yet), so skipping the frame cannot deadlock.
    std::uint32_t imageIndex = 0;
    const VkResult acquire =
        vkAcquireNextImageKHR(dev, m_Swapchain->Handle(), UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        m_ResizePending = true;
        return std::nullopt;
    }
    if (acquire == VK_SUBOPTIMAL_KHR)
        m_ResizePending = true; // semaphore IS signaled: finish this frame, recreate next one
    else
        VK_CHECK(acquire);

    // 3) Only reset the fence once we are certain to submit work that signals it.
    VK_CHECK(vkResetFences(dev, 1, &f.inFlight));
    VK_CHECK(vkResetCommandPool(dev, f.pool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &begin));
    f.frameScope = m_Profiler->Begin(f.cmd, "Frame");

    // Finished uploads: queue-family acquire + mip generation, before any pass can use them.
    f.uploadWait = m_Upload->RecordAcquires(f.cmd);

    const VkImage image = m_Swapchain->Image(imageIndex);
    // srcStage = COLOR_ATTACHMENT_OUTPUT chains with the acquire-semaphore wait stage.
    CmdImageBarrier(f.cmd, {.image     = image,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .srcAccess = VK_ACCESS_2_NONE,
                            .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    m_FrameActive = true;
    return FrameContext{.cmd        = f.cmd,
                        .image      = image,
                        .view       = m_Swapchain->View(imageIndex),
                        .format     = m_Swapchain->Format(),
                        .extent     = m_Swapchain->Extent(),
                        .frameIndex = m_FrameIndex,
                        .imageIndex = imageIndex};
}

void Renderer::EndFrame(const FrameContext& frame)
{
    assert(m_FrameActive && "EndFrame without BeginFrame");
    FrameData& f = m_Frames[frame.frameIndex];

    CmdImageBarrier(f.cmd, {.image     = frame.image,
                            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            // Must be inside the signal's stage mask: chains the layout transition
                            // to the render-finished semaphore (NONE would leave it unordered).
                            .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .dstAccess = VK_ACCESS_2_NONE});
    m_Profiler->End(f.cmd, f.frameScope);
    VK_CHECK(vkEndCommandBuffer(f.cmd));

    if (f.transientOffset > 0) // no-op on HOST_COHERENT memory
        f.transient.Flush(0, f.transientOffset);

    VkSemaphore renderFinished = m_RenderFinished[frame.imageIndex];

    std::array<VkSemaphoreSubmitInfo, 2> waits{};
    waits[0].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[0].semaphore = f.imageAvailable;
    waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    // Upload timeline: already reached (host-checked), orders the acquire barriers after the
    // transfer queue's release. ALL_COMMANDS chains with their ALL_TRANSFER source stage.
    waits[1].sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[1].semaphore = m_Upload->Timeline();
    waits[1].value     = f.uploadWait;
    waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    const std::uint32_t waitCount = f.uploadWait > 0 ? 2u : 1u;

    VkSemaphoreSubmitInfo signal{};
    signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = renderFinished;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = f.cmd;

    VkSubmitInfo2 submit{};
    submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount   = waitCount;
    submit.pWaitSemaphoreInfos      = waits.data();
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos    = &signal;
    VK_CHECK(vkQueueSubmit2(m_Ctx.GraphicsQueue().handle, 1, &submit, f.inFlight));

    const VkSwapchainKHR swapchain = m_Swapchain->Handle();
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &renderFinished;
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchain;
    present.pImageIndices      = &frame.imageIndex;

    const VkResult result = vkQueuePresentKHR(m_Ctx.PresentQueue().handle, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        m_ResizePending = true;
    else
        VK_CHECK(result);

    m_FrameIndex  = (m_FrameIndex + 1) % kFramesInFlight;
    m_FrameActive = false;
}

TransientAllocation Renderer::AllocateTransient(VkDeviceSize size, VkDeviceSize alignment)
{
    assert(m_FrameActive && "Transient memory is only valid inside a frame");
    assert(alignment > 0 && (alignment & (alignment - 1)) == 0);
    FrameData&         f      = m_Frames[m_FrameIndex];
    const VkDeviceSize offset = (f.transientOffset + alignment - 1) & ~(alignment - 1);
    if (offset + size > f.transient.Size())
        throw std::runtime_error("Transient frame buffer exhausted - raise kTransientBufferSize");
    f.transientOffset = offset + size;
    return {static_cast<std::byte*>(f.transient.Mapped()) + offset, f.transient.Address() + offset};
}

void Renderer::CreateDefaultTextures()
{
    // RGBA8 little-endian (0xAABBGGRR). Flat normal = (0.5, 0.5, 1.0).
    constexpr std::array<std::uint32_t, kDefaultTextureCount> texels{0xFFFFFFFFu, 0xFF000000u, 0xFFFF8080u};
    constexpr std::array<const char*, kDefaultTextureCount>   names{"DefaultWhite", "DefaultBlack", "DefaultNormal"};
    UploadTicket ticket = 0;
    for (std::size_t i = 0; i < kDefaultTextureCount; ++i) {
        m_DefaultTextures[i] = m_Upload->CreateTexture2D({.pixels       = &texels[i],
                                                          .width        = 1,
                                                          .height       = 1,
                                                          .format       = VK_FORMAT_R8G8B8A8_UNORM,
                                                          .generateMips = false,
                                                          .debugName    = names[i]},
                                                         ticket);
        m_DefaultTextureSlots[i] = m_Bindless->AddSampledImage(m_DefaultTextures[i].View());
    }
    m_Upload->Flush(); // resident before the first frame
}

Renderer::FrameData& Renderer::GarbageSlot()
{
    // While recording: the current slot (freed when its fence is next waited on).
    // Between frames: the slot of the most recent submission, which may still be in flight.
    return m_FrameActive ? m_Frames[m_FrameIndex]
                         : m_Frames[(m_FrameIndex + kFramesInFlight - 1) % kFramesInFlight];
}

void Renderer::CollectGarbage(FrameData& frame)
{
    for (auto& fn : frame.deferred)
        fn();
    frame.deferred.clear();
    frame.garbage.clear();
}

void Renderer::RecreateSwapchain()
{
    const VkExtent2D extent = m_Window.FramebufferExtent();
    if (extent.width == 0 || extent.height == 0)
        return; // minimized: stay pending

    m_Ctx.WaitIdle(); // simple + safe; VK_EXT_swapchain_maintenance1 could avoid the stall
    m_Swapchain->Recreate(extent);
    GrowRenderFinishedSemaphores();
    m_ResizePending = false;
}

void Renderer::GrowRenderFinishedSemaphores()
{
    // Never destroy these mid-run: vkDeviceWaitIdle does not guarantee that
    // presentation has released them. Only grow if the image count increases.
    while (m_RenderFinished.size() < m_Swapchain->ImageCount())
        m_RenderFinished.push_back(MakeSemaphore(m_Ctx.Device()));
}

} // namespace Engine
