#include "Editor/ProjectLauncher.h"
#include "FileDialog.h"
#include "ImGuiLayer.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Window.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <ctime>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;

namespace {
fs::path DefaultLocation()
{
    std::error_code ec;
    const fs::path  home = HomeDirectory();
    const fs::path  base = !home.empty() ? home : fs::current_path(ec);
    const fs::path  documents = base / "Documents";
    return (fs::is_directory(documents, ec) ? documents : base) / "Ungine Projects";
}

std::string FormatTime(std::int64_t seconds)
{
    if (seconds <= 0)
        return {};
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm           tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
    return buffer;
}
} // namespace

ProjectLauncher::ProjectLauncher(Window& window, Renderer& renderer)
    : m_Window(window),
      m_ImGui(std::make_unique<ImGuiLayer>(window, renderer, std::string())), // no layout file
      m_Dialog(std::make_unique<FileDialog>()),
      m_Recent(LoadRecentProjects()),
      m_Templates(ProjectTemplates()),
      m_Location(PathToUtf8(DefaultLocation()))
{
    // The basic template is the friendliest start.
    for (std::size_t i = 0; i < m_Templates.size(); ++i)
        if (m_Templates[i].id == "Basic")
            m_Template = i;
}

ProjectLauncher::~ProjectLauncher() = default;

void ProjectLauncher::Choose(const fs::path& file)
{
    std::string error;
    const auto  project = Project::Load(file, &error);
    if (!project) {
        m_Error = error;
        return;
    }
    AddRecentProject(*project);
    m_Chosen = project->File();
}

bool ProjectLauncher::CreateProject(const std::string& name, const fs::path& location, std::size_t templateIndex)
{
    if (templateIndex >= m_Templates.size()) {
        m_Error = "No project templates found in '" + PathToUtf8(TemplateDirectory()) + "'";
        return false;
    }
    std::string error;
    const auto  project = Project::Create(location, name, m_Templates[templateIndex], &error);
    if (!project) {
        m_Error = error;
        return false;
    }
    ENGINE_INFO("Created project '{}' ({})", name, PathToUtf8(project->File()));
    AddRecentProject(*project);
    m_Chosen = project->File();
    return true;
}

void ProjectLauncher::Update()
{
    m_ImGui->NewFrame();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Ungine Project Browser", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextUnformatted("Ungine");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("  Project Browser");
    ImGui::Separator();

    const float column = ImGui::GetContentRegionAvail().x * 0.5f - 8.0f;
    // Recent projects.
    if (ImGui::BeginChild("recent", ImVec2(column, -ImGui::GetFrameHeightWithSpacing() * 2.0f), ImGuiChildFlags_Borders)) {
        ImGui::SeparatorText("Recent projects");
        if (m_Recent.empty())
            ImGui::TextDisabled("No projects yet - create one on the right, or open an existing .ungineproj.");
        std::optional<fs::path> remove;
        for (std::size_t i = 0; i < m_Recent.size(); ++i) {
            const RecentProject& r = m_Recent[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string label = r.name + "##project";
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 36.0f)) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                Choose(r.file);
            if (ImGui::BeginPopupContextItem("recent item")) {
                if (ImGui::MenuItem("Open"))
                    Choose(r.file);
                if (ImGui::MenuItem("Show in folder"))
                    (void)OpenInFileBrowser(r.file.parent_path());
                if (ImGui::MenuItem("Remove from list"))
                    remove = r.file;
                ImGui::EndPopup();
            }
            // Second line: path and date, drawn over the selectable.
            const ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 4.0f, min.y + 18.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                (PathToUtf8(r.file) + "   " + FormatTime(r.lastOpened)).c_str());
            ImGui::PopID();
        }
        if (remove) {
            RemoveRecentProject(*remove);
            m_Recent = LoadRecentProjects();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // New project.
    if (ImGui::BeginChild("new", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f), ImGuiChildFlags_Borders)) {
        ImGui::SeparatorText("New project");
        if (m_Templates.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "No templates found in %s", PathToUtf8(TemplateDirectory()).c_str());
        for (std::size_t i = 0; i < m_Templates.size(); ++i) {
            const ProjectTemplate& t = m_Templates[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(("##template" + t.id).c_str(), m_Template == i, 0, ImVec2(0.0f, 40.0f)))
                m_Template = i;
            const ImVec2 min = ImGui::GetItemRectMin();
            ImDrawList*  list = ImGui::GetWindowDrawList();
            list->AddText(ImVec2(min.x + 6.0f, min.y + 3.0f), ImGui::GetColorU32(ImGuiCol_Text), t.name.c_str());
            list->AddText(ImVec2(min.x + 6.0f, min.y + 21.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), t.description.c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::InputText("Name", &m_Name);
        ImGui::InputText("Location", &m_Location);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            m_DialogPurpose = DialogPurpose::Location;
            std::error_code ec;
            fs::path        start = PathFromUtf8(m_Location);
            while (!start.empty() && !fs::is_directory(start, ec) && start.has_parent_path() && start.parent_path() != start)
                start = start.parent_path();
            m_Dialog->Open("Project location", FileDialog::Mode::Folder, start, {});
        }
        const bool validName = IsValidProjectName(m_Name);
        if (!validName)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Name: letters, digits, '_', '-' and spaces");
        else
            ImGui::TextDisabled("Creates %s", PathToUtf8(PathFromUtf8(m_Location) / PathFromUtf8(m_Name)).c_str());
        ImGui::BeginDisabled(!validName || m_Templates.empty());
        if (ImGui::Button("Create project", ImVec2(160.0f, 32.0f)))
            (void)CreateProject(m_Name, PathFromUtf8(m_Location), m_Template);
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    if (ImGui::Button("Open project...", ImVec2(160.0f, 0.0f))) {
        m_DialogPurpose = DialogPurpose::Open;
        m_Dialog->Open("Open project", FileDialog::Mode::Open, PathFromUtf8(m_Location), {std::string(Project::kExtension)});
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(100.0f, 0.0f)))
        m_Window.RequestClose();
    if (!m_Error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_Error.c_str());
    }
    ImGui::End();

    if (const std::optional<fs::path> path = m_Dialog->Draw()) {
        if (m_DialogPurpose == DialogPurpose::Open)
            Choose(*path);
        else if (m_DialogPurpose == DialogPurpose::Location)
            m_Location = PathToUtf8(*path);
        m_DialogPurpose = DialogPurpose::None;
    }
}

void ProjectLauncher::Render(const FrameContext& frame) { m_ImGui->Render(frame.cmd, frame.view, frame.extent); }

} // namespace Engine
