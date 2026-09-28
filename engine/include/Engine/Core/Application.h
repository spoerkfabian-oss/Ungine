#pragma once
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Input.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <memory>

namespace Engine {

#ifdef ENGINE_DEBUG
inline constexpr bool kDebugBuild = true;
#else
inline constexpr bool kDebugBuild = false;
#endif

struct ApplicationDesc {
    WindowDesc    window{};
    RendererDesc  renderer{};
    bool          enableValidation = kDebugBuild;
    double        fixedTimestep    = 1.0 / 60.0; // physics tick (Jolt, Phase 3)
    double        maxFrameTime     = 0.25;       // clamp to avoid "spiral of death"
    std::uint32_t workerThreads    = 0;          // 0 = ThreadPool::DefaultThreadCount()
};

class Application {
public:
    explicit Application(const ApplicationDesc& desc);
    virtual ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    void Run();

protected:
    virtual void OnInit() {}
    virtual void OnFixedUpdate(double /*dt*/) {}
    virtual void OnUpdate(double /*dt*/) {}
    virtual void OnRender(const FrameContext& /*frame*/, double /*interpolationAlpha*/) {}
    virtual void OnShutdown() {}

    [[nodiscard]] EventBus&      GetEvents()   { return m_Events; }
    [[nodiscard]] Input&         GetInput()    { return m_Input; }
    [[nodiscard]] Window&        GetWindow()   { return *m_Window; }
    [[nodiscard]] VulkanContext& GetContext()  { return *m_Context; }
    [[nodiscard]] Renderer&      GetRenderer() { return *m_Renderer; }
    [[nodiscard]] ThreadPool&    GetJobs()     { return *m_Jobs; }
    [[nodiscard]] AssetManager&  GetAssets()   { return *m_Assets; }

private:
    ApplicationDesc m_Desc;
    // Declaration order = reverse destruction order:
    // assets -> jobs -> renderer -> context -> window -> input -> event bus.
    EventBus                       m_Events;
    Input                          m_Input{m_Events};
    std::unique_ptr<Window>        m_Window;
    std::unique_ptr<VulkanContext> m_Context;
    std::unique_ptr<Renderer>      m_Renderer;
    std::unique_ptr<ThreadPool>    m_Jobs;
    std::unique_ptr<AssetManager>  m_Assets; // waits for its jobs, defers GPU frees to the renderer
};

} // namespace Engine
