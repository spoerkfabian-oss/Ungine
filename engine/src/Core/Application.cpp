#include "Engine/Core/Application.h"

#include <algorithm>
#include <chrono>

namespace Engine {

Application::Application(const ApplicationDesc& desc)
    : m_Desc(desc)
    , m_Window(std::make_unique<Window>(desc.window, m_Events))
    , m_Context(std::make_unique<VulkanContext>(
          *m_Window, VulkanContextDesc{.appName = desc.window.title,
                                       .enableValidation = desc.enableValidation,
                                       .pipelineCache    = desc.pipelineCache}))
    , m_Renderer(std::make_unique<Renderer>(*m_Context, *m_Window, m_Events, desc.renderer))
    , m_Jobs(std::make_unique<ThreadPool>(desc.workerThreads))
    , m_Assets(std::make_unique<AssetManager>(*m_Renderer, *m_Jobs, m_Events, m_Desc.assets))
{
    ENGINE_INFO("Worker threads: {}", m_Jobs->ThreadCount());
}

Application::~Application() = default;

void Application::Run()
{
    using Clock = std::chrono::steady_clock;

    OnInit();

    auto   previous    = Clock::now();
    double accumulator = 0.0;

    while (!m_Window->ShouldClose()) {
        m_Input.NewFrame();     // clear last frame's deltas before new events arrive
        m_Window->PollEvents(); // immediate events (input, resize) dispatched here
        m_Events.Flush();       // deferred events (e.g. from loader threads)

        if (m_Window->IsMinimized()) {
            // No frames: sleep, but wake up regularly so background loads still finish (their
            // uploads are submitted and acquired directly instead of by BeginFrame).
            m_Window->WaitEvents(0.1);
            m_Renderer->GetUploader().Flush();
            m_Assets->Update();
            previous = Clock::now(); // don't count minimized time as a frame
            continue;
        }

        const auto now = Clock::now();
        const double frameTime =
            std::min(std::chrono::duration<double>(now - previous).count(), m_Desc.maxFrameTime);
        previous = now;

        accumulator += frameTime;
        while (accumulator >= m_Desc.fixedTimestep) {
            OnFixedUpdate(m_Desc.fixedTimestep);
            accumulator -= m_Desc.fixedTimestep;
        }

        m_Assets->Update(); // finished loads -> AssetLoadedEvent before game code runs
        OnUpdate(frameTime);

        if (auto frame = m_Renderer->BeginFrame()) {
            OnRender(*frame, accumulator / m_Desc.fixedTimestep);
            m_Renderer->EndFrame(*frame);
        }
    }

    m_Context->WaitIdle(); // GPU must be idle before any resource is destroyed
    OnShutdown();
}

} // namespace Engine
