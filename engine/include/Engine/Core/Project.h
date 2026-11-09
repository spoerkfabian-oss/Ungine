#pragma once
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/InputMap.h"
#include "Engine/Navigation/NavTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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
    // Written into every save slot; raise it when the saved data changes. Older slots fire
    // "On Migrate Save Game" when read, newer ones are refused (ScriptSystem::SetSaveVersion).
    std::uint32_t saveVersion = 1;
    // Ed25519 public key (64 hex digits) of the project's pak signing key; empty: unsigned paks.
    // When set, packaging signs the paks and the player refuses paks that are not signed by it.
    std::string   pakPublicKey;
    std::uint32_t windowWidth  = 1600;
    std::uint32_t windowHeight = 900;
    bool          fullscreen   = false;
    bool          vsync        = true;
    AudioSettings audio;        // mixer (bus volumes / mutes), occlusion
    InputMap      input;        // input actions / axes for blueprints
    NavSettings   navigation;   // navmesh voxels / tiles, agent profiles, area types

    bool operator==(const ProjectSettings&) const = default;
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

struct CookReport;

// Standalone game folder: the player executable renamed after the project, the shaders, the
// project file, Content.upak (everything below Content/, cooked: CookProjectContent) and
// BuildReport.txt. Texture encoding reuses the project's cache (Saved/Cache/Textures). Returns
// false with a message in `error` (also when cooking reported errors); `report` gets the details.
// `options` (optional): progress 0..1 with the current step and a cancel poll, both called on the
// packaging thread (a cancelled build fails with "packaging was cancelled" and leaves no pak), and
// the signing key file for projects with a pakPublicKey (empty: the user's key store).
struct PackageOptions {
    std::function<void(float progress, const std::string& step)> progress;
    std::function<bool()>                                        cancelled;
    std::filesystem::path                                        keyFile;
};
bool PackageProject(const Project& project, const std::filesystem::path& playerExecutable,
                    const std::filesystem::path& outputDirectory, std::string* error = nullptr,
                    CookReport* report = nullptr, const PackageOptions* options = nullptr);

// Patch for a game packaged into `outputDirectory` earlier: cooks the content like PackageProject,
// compares it with the paks there (Content.upak and the patches before), and writes only the
// changed and new entries plus deleted entries for removed ones as Patch_<n>.upak (n = the highest
// existing + 1; signed like the base), with the project file. `patchFile` gets its path, empty when
// nothing changed (no file is written then). The player and shaders are not patched.
bool PackagePatch(const Project& project, const std::filesystem::path& outputDirectory, std::string* error = nullptr,
                  CookReport* report = nullptr, const PackageOptions* options = nullptr,
                  std::filesystem::path* patchFile = nullptr);
// The patches in `directory` (Patch_<n>.upak) in mount order.
[[nodiscard]] std::vector<std::filesystem::path> PakPatches(const std::filesystem::path& directory);

// --- Pak signing (ProjectSettings::pakPublicKey) ----------------------------------------------------
// The private key never goes into the project: it is kept per user in
// UserConfigDirectory()/keys/<public key>.key (the seed as hex); copy that file to build machines
// or pass it with UngineCook --key.
[[nodiscard]] std::filesystem::path PakKeyFile(const PakPublicKey& publicKey);
// A new key pair: the seed goes to PakKeyFile, the public key into the settings (the project is not
// saved here). False when the key file cannot be written.
bool CreatePakSigningKey(Project& project, std::string* error = nullptr);
// The project's key pair from `keyFile` (empty: the key store); nullopt with an error when the
// project has no key, the file is missing or holds another key.
[[nodiscard]] std::optional<PakKeyPair> LoadPakSigningKey(const ProjectSettings& settings,
                                                          const std::filesystem::path& keyFile = {},
                                                          std::string* error = nullptr);
// Packaged game: mounts <root>/Content.upak and its patches <root>/Patch_<n>.upak (in increasing n)
// at the root. With a pakPublicKey every pak must be signed by it. True without paks (loose
// Content/); false with an error when a pak is missing its base, broken or refused.
bool MountPackagedContent(const Project& project, std::string* error = nullptr);

} // namespace Engine
