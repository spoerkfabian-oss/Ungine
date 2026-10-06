#pragma once
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/InputMap.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

// A game project: <Root>/<Name>.ungineproj (JSON) next to Content/ (scenes, scripts, models,
// textures) and Saved/ (caches, editor layout; not part of the game). The editor opens it
// (double-click on the file), the player runs its start scene.
struct ProjectSettings {
    std::string   name = "Untitled";
    std::string   startScene; // relative to the project root, '/' separated
    // Shown while Open Level loads the next level (a scene with UI canvases; a progress bar
    // widget tagged "LoadingProgress" follows the progress). Empty: a built-in screen.
    std::string   loadingScreen;
    std::uint32_t windowWidth  = 1600;
    std::uint32_t windowHeight = 900;
    bool          fullscreen   = false;
    bool          vsync        = true;
    AudioSettings audio;        // mixer (bus volumes / mutes), occlusion
    InputMap      input;        // input actions / axes for blueprints
};

struct ProjectTemplate {
    std::string           id;          // directory name
    std::string           name;
    std::string           description;
    std::filesystem::path directory;   // copied into new projects (except template.json)
    std::string           startScene;  // relative path inside the template
};

class Project {
public:
    static constexpr std::string_view kExtension = ".ungineproj";

    // Returns nullopt with a message in `error` on failure.
    static std::optional<Project> Load(const std::filesystem::path& file, std::string* error = nullptr);
    // New project <location>/<name>/<name>.ungineproj with the template's files. The directory
    // must not exist or be empty.
    static std::optional<Project> Create(const std::filesystem::path& location, const std::string& name,
                                         const ProjectTemplate& projectTemplate, std::string* error = nullptr);
    bool Save(std::string* error = nullptr) const;

    [[nodiscard]] const std::filesystem::path& File() const { return m_File; }
    [[nodiscard]] std::filesystem::path Root() const { return m_File.parent_path(); }
    [[nodiscard]] std::filesystem::path ContentDirectory() const { return Root() / "Content"; }
    [[nodiscard]] std::filesystem::path SavedDirectory() const { return Root() / "Saved"; }
    // Absolute start scene, empty if none is set.
    [[nodiscard]] std::filesystem::path StartScene() const;
    // Path relative to the root ('/' separated) if inside the project, else absolute (UTF-8).
    [[nodiscard]] std::string Relative(const std::filesystem::path& file) const;

    ProjectSettings settings;

private:
    std::filesystem::path m_File; // absolute
};

[[nodiscard]] bool IsValidProjectName(std::string_view name); // letters, digits, '_', '-', ' ' (1..64)

// Templates in TemplateDirectory()/<id>/template.json, sorted by name.
[[nodiscard]] std::vector<ProjectTemplate> ProjectTemplates();

// Recently opened projects, newest first (UserConfigDirectory()/recent_projects.json). Entries
// whose file is gone are dropped when loading.
struct RecentProject {
    std::filesystem::path file;
    std::string           name;
    std::int64_t          lastOpened = 0; // seconds since the epoch
};
[[nodiscard]] std::vector<RecentProject> LoadRecentProjects(const std::filesystem::path& list = {});
void AddRecentProject(const Project& project, const std::filesystem::path& list = {}); // front, max 20
void RemoveRecentProject(const std::filesystem::path& file, const std::filesystem::path& list = {});

// Standalone game folder: the player executable renamed after the project, the shaders, the
// project file and Content/. Returns false with a message in `error`.
bool PackageProject(const Project& project, const std::filesystem::path& playerExecutable,
                    const std::filesystem::path& outputDirectory, std::string* error = nullptr);

} // namespace Engine
