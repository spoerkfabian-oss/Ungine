#pragma once
#include "Engine/Core/Project.h"
#include "Engine/Renderer/Renderer.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

class FileDialog;
class ImGuiLayer;
class Window;

// Project browser shown by UngineEditor when it is started without a project: recent projects,
// a new project from a template (name + location) and "Open..." for any .ungineproj. Has its own
// ImGui context and draws over the whole window.
class ProjectLauncher {
public:
    ProjectLauncher(Window& window, Renderer& renderer);
    ~ProjectLauncher();

    ProjectLauncher(const ProjectLauncher&)            = delete;
    ProjectLauncher& operator=(const ProjectLauncher&) = delete;

    void Update();                          // UI, once per frame
    void Render(const FrameContext& frame); // into the swapchain image

    // The project file to open once chosen / created (the launcher is done then).
    [[nodiscard]] const std::optional<std::filesystem::path>& Chosen() const { return m_Chosen; }

    // Also used by tests: creates the project (and remembers it as recent) and chooses it.
    bool CreateProject(const std::string& name, const std::filesystem::path& location, std::size_t templateIndex);
    [[nodiscard]] const std::vector<ProjectTemplate>& Templates() const { return m_Templates; }
    [[nodiscard]] const std::string&                  Error() const { return m_Error; }

private:
    void Choose(const std::filesystem::path& file); // validates, remembers as recent

    Window&                     m_Window;
    std::unique_ptr<ImGuiLayer> m_ImGui;
    std::unique_ptr<FileDialog> m_Dialog;
    enum class DialogPurpose { None, Open, Location } m_DialogPurpose = DialogPurpose::None;

    std::vector<RecentProject>   m_Recent;
    std::vector<ProjectTemplate> m_Templates;
    std::size_t                  m_Template = 0;
    std::string                  m_Name     = "MyGame";
    std::string                  m_Location;
    std::string                  m_Error;
    std::optional<std::filesystem::path> m_Chosen;
};

} // namespace Engine
