#include "Engine/Renderer/Renderer.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/Events.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <stdexcept>

namespace Engine {

Renderer::Renderer(VulkanContext& ctx, Window& window, EventBus& events, const RendererDesc& desc)
    : m_Ctx(ctx), m_Window(window), m_PresentFences(ctx.SupportsSwapchainMaintenance())
{
    // Started minimized: a swapchain needs a non-empty surface, so wait until the window shows.
    while (window.IsMinimized())
        window.WaitEvents();
    m_Swapchain = std::make_unique<Swapchain>(ctx, window.FramebufferExtent(), SwapchainDesc{.vsync = desc.vsync});
    m_Bindless  = std::make_unique<BindlessRegistry>(ctx);
    m_Upload    = std::make_unique<UploadQueue>(ctx, desc.upload);
    m_Profiler  = std::make_unique<GpuProfiler>(ctx, kFramesInFlight);
    m_Geometry  = std::make_unique<GeometryPool>(ctx, *m_Upload, desc.geometry);
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
    if (!m_PresentFences)
        GrowRenderFinishedSemaphores();

    m_ResizeSub = events.Subscribe<FramebufferResizeEvent>(
        [this](const FramebufferResizeEvent&) { m_ResizePending = true; });
}

Renderer::~Renderer()
{
    m_Ctx.WaitIdle();
    for (FrameData& f : m_Frames)
        CollectGarbage(f); // may reference the bindless registry / geometry pool -> run first
    RetirePresents(true);
    m_Geometry.reset();
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
    for (VkSemaphore s : m_FreeRenderSemaphores)
        vkDestroySemaphore(dev, s, nullptr);
    for (VkFence f : m_FreePresentFences)
        vkDestroyFence(dev, f, nullptr);
    m_RetiredSwapchains.clear();
    m_Swapchain.reset();
}

std::optional<FrameContext> Renderer::BeginFrame()
{
    assert(!m_FrameActive && "BeginFrame called twice without EndFrame");
    if (m_ShaderHotReload)
        (void)m_ShaderReload->Poll();
    if (m_ShaderReload && m_ShaderReload->Generation() != m_ReloadGeneration) { // polled or recompiled directly
        m_ReloadGeneration = m_ShaderReload->Generation();
        ++m_ShaderGeneration;
    }
    const VkDevice dev = m_Ctx.Device();
    FrameData&     f   = m_Frames[m_FrameIndex];

    // 1) CPU waits until the GPU finished the frame that last used this slot.
    VK_CHECK(vkWaitForFences(dev, 1, &f.inFlight, VK_TRUE, UINT64_MAX));
    CollectGarbage(f); // everything this slot's last submission could touch is now free
    ++m_FrameCounter;
    RetirePresents(false);
    f.transientOffset = 0;
    m_Profiler->BeginFrame(m_FrameIndex); // this slot's timestamps are complete now
    UpdateTextureTable(f);

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
    // srcStage = COLOR_ATTACHMENT_OUTPUT chains with the acquire-semaphore wait stage. READ too: a
    // pass may load the (undefined) contents, e.g. an overlay drawn alone over a loading screen.
    CmdImageBarrier(f.cmd, {.image     = image,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .srcAccess = VK_ACCESS_2_NONE,
                            .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

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

    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence     presentFence   = VK_NULL_HANDLE;
    if (m_PresentFences) { // recycled once this present is done (see RetirePresents)
        if (m_FreeRenderSemaphores.empty()) {
            renderFinished = MakeSemaphore(m_Ctx.Device());
        } else {
            renderFinished = m_FreeRenderSemaphores.back();
            m_FreeRenderSemaphores.pop_back();
        }
        if (m_FreePresentFences.empty()) {
            presentFence = MakeFence(m_Ctx.Device(), false);
        } else {
            presentFence = m_FreePresentFences.back();
            m_FreePresentFences.pop_back();
        }
    } else {
        renderFinished = m_RenderFinished[frame.imageIndex];
    }

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
    VkSwapchainPresentFenceInfoEXT fenceInfo{};
    if (m_PresentFences) {
        fenceInfo.sType          = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
        fenceInfo.swapchainCount = 1;
        fenceInfo.pFences        = &presentFence;
        present.pNext            = &fenceInfo;
    }

    const VkResult result = vkQueuePresentKHR(m_Ctx.PresentQueue().handle, &present);
    if (m_PresentFences) // signaled even for OUT_OF_DATE / SUBOPTIMAL (the semaphore wait still runs)
        m_PendingPresents.push_back({.fence = presentFence, .semaphore = renderFinished, .swapchain = m_SwapchainId});
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
    // RGBA8 little-endian (0xAABBGGRR). Flat normal = (0.5, 0.5, 1.0). Error: magenta/black checker.
    constexpr std::uint32_t kErrorSize = 8;
    std::array<std::uint32_t, kErrorSize * kErrorSize> checker{};
    for (std::uint32_t y = 0; y < kErrorSize; ++y)
        for (std::uint32_t x = 0; x < kErrorSize; ++x)
            checker[y * kErrorSize + x] = ((x / 2 + y / 2) & 1) != 0 ? 0xFF000000u : 0xFFFF00FFu;
    constexpr std::array<std::uint32_t, 3>                  texels{0xFFFFFFFFu, 0xFF000000u, 0xFFFF8080u};
    constexpr std::array<const char*, kDefaultTextureCount> names{"DefaultWhite", "DefaultBlack", "DefaultNormal",
                                                                  "DefaultError"};
    UploadTicket ticket = 0;
    for (std::size_t i = 0; i < kDefaultTextureCount; ++i) {
        const bool error = i == static_cast<std::size_t>(DefaultTexture::Error);
        m_DefaultTextures[i] = m_Upload->CreateTexture2D({.pixels       = error ? checker.data() : &texels[i],
                                                          .width        = error ? kErrorSize : 1,
                                                          .height       = error ? kErrorSize : 1,
                                                          .format       = VK_FORMAT_R8G8B8A8_UNORM,
                                                          .generateMips = error,
                                                          .debugName    = names[i]},
                                                         ticket);
        m_DefaultTextureSlots[i] = m_Bindless->AddSampledImage(m_DefaultTextures[i].View());
        m_TextureTable.push_back(m_DefaultTextureSlots[i]); // entry i
    }
    m_Upload->Flush(); // resident before the first frame
}

void Renderer::SetShaderHotReload(bool enabled)
{
    if (enabled && !m_ShaderReload)
        m_ShaderReload = std::make_unique<ShaderHotReload>();
    m_ShaderHotReload = enabled && m_ShaderReload->Available();
    if (enabled && !m_ShaderHotReload)
        ENGINE_WARN("Shader hot reload unavailable (no shader compiler)");
}

std::uint32_t Renderer::AllocateTextureEntry(std::uint32_t bindlessSlot)
{
    std::uint32_t entry = 0;
    if (!m_FreeTextureEntries.empty()) {
        entry = m_FreeTextureEntries.back();
        m_FreeTextureEntries.pop_back();
        m_TextureTable[entry] = bindlessSlot;
    } else {
        entry = static_cast<std::uint32_t>(m_TextureTable.size());
        m_TextureTable.push_back(bindlessSlot);
    }
    ++m_TextureTableVersion;
    return entry;
}

void Renderer::SetTextureEntry(std::uint32_t entry, std::uint32_t bindlessSlot)
{
    assert(entry < m_TextureTable.size());
    if (m_TextureTable[entry] == bindlessSlot)
        return;
    m_TextureTable[entry] = bindlessSlot;
    ++m_TextureTableVersion;
}

void Renderer::FreeTextureEntry(std::uint32_t entry)
{
    assert(entry >= kDefaultTextureCount && entry < m_TextureTable.size());
    DeferCall([this, entry] {
        m_TextureTable[entry] = m_DefaultTextureSlots[static_cast<std::size_t>(DefaultTexture::Error)];
        m_FreeTextureEntries.push_back(entry);
        ++m_TextureTableVersion;
    });
}

VkDeviceAddress Renderer::TextureTableAddress() const
{
    assert(m_FrameActive && "The texture table is per frame");
    return m_Frames[m_FrameIndex].textureTable.Address();
}

void Renderer::UpdateTextureTable(FrameData& frame)
{
    if (frame.textureTableVersion == m_TextureTableVersion)
        return;
    const VkDeviceSize bytes = m_TextureTable.size() * sizeof(std::uint32_t);
    if (!frame.textureTable || frame.textureTable.Size() < bytes) // this slot's GPU work is done
        frame.textureTable = Buffer(m_Ctx, {.size      = std::max<VkDeviceSize>(bytes * 2, 4096),
                                            .usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            .memory    = MemoryUsage::Upload,
                                            .debugName = "TextureTable"});
    frame.textureTable.Write(m_TextureTable.data(), bytes);
    frame.textureTableVersion = m_TextureTableVersion;
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

    if (m_PresentFences) {
        // No stall: the driver retires the old swapchain; it is destroyed once its presents are
        // done and no frame in flight can still reference its images (RetirePresents).
        auto fresh = std::make_unique<Swapchain>(m_Ctx, extent, m_Swapchain->Desc(), m_Swapchain->Handle());
        m_RetiredSwapchains.push_back({.swapchain = std::move(m_Swapchain), .id = m_SwapchainId, .retiredAt = m_FrameCounter});
        m_Swapchain = std::move(fresh);
        ++m_SwapchainId;
    } else {
        m_Ctx.WaitIdle();
        m_Swapchain->Recreate(extent);
        GrowRenderFinishedSemaphores();
    }
    m_ResizePending = false;
}

void Renderer::RetirePresents(bool wait)
{
    const VkDevice dev = m_Ctx.Device();
    std::erase_if(m_PendingPresents, [&](const PendingPresent& p) {
        const VkResult status = wait ? vkWaitForFences(dev, 1, &p.fence, VK_TRUE, 1'000'000'000ull)
                                     : vkGetFenceStatus(dev, p.fence);
        if (status != VK_SUCCESS)
            return false;
        VK_CHECK(vkResetFences(dev, 1, &p.fence));
        m_FreePresentFences.push_back(p.fence);
        m_FreeRenderSemaphores.push_back(p.semaphore);
        return true;
    });
    std::erase_if(m_RetiredSwapchains, [&](const RetiredSwapchain& r) {
        const bool presenting = std::ranges::any_of(m_PendingPresents, [&](const PendingPresent& p) { return p.swapchain == r.id; });
        return wait || (!presenting && m_FrameCounter - r.retiredAt >= kFramesInFlight);
    });
    if (wait) // shutdown: whatever did not signal in time is dropped with the device
        for (const PendingPresent& p : m_PendingPresents) {
            vkDestroyFence(dev, p.fence, nullptr);
            vkDestroySemaphore(dev, p.semaphore, nullptr);
        }
    if (wait)
        m_PendingPresents.clear();
}

void Renderer::GrowRenderFinishedSemaphores()
{
    // Never destroy these mid-run: vkDeviceWaitIdle does not guarantee that
    // presentation has released them. Only grow if the image count increases.
    while (m_RenderFinished.size() < m_Swapchain->ImageCount())
        m_RenderFinished.push_back(MakeSemaphore(m_Ctx.Device()));
}

} // namespace Engine
