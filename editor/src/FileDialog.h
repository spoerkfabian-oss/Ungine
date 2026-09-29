#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

// Modal ImGui file browser (no native dialogs, same on every platform). Open() once, then call
// Draw() every frame: it returns the chosen path exactly once. Save asks before overwriting an
// existing file; Folder returns the current directory. Places: home, working directory, drives
// (Windows).
class FileDialog {
public:
    enum class Mode { Open, Save, Folder };

    // extensions: lower case with dot (".glb"); empty = all files. Save mode appends the first one
    // when the typed name has none of them.
    void Open(std::string title, Mode mode, const std::filesystem::path& directory,
              std::vector<std::string> extensions, std::string fileName = {});
    [[nodiscard]] std::optional<std::filesystem::path> Draw();
    [[nodiscard]] bool IsOpen() const { return m_Open; }

    // Existing roots to jump to (drive letters on Windows, "/" elsewhere) plus home / working dir.
    [[nodiscard]] static std::vector<std::filesystem::path> Places();

private:
    struct Item {
        std::filesystem::path path;
        std::string           label;
        bool                  directory = false;
    };

    void Refresh();
    void Navigate(const std::filesystem::path& directory);
    [[nodiscard]] bool Matches(const std::filesystem::path& file) const;
    [[nodiscard]] std::optional<std::filesystem::path> Confirm();

    std::string              m_Title;
    Mode                     m_Mode = Mode::Open;
    bool                     m_Open = false;
    bool                     m_OpenRequested = false;
    std::filesystem::path    m_Directory;
    std::vector<std::string> m_Extensions;
    std::string              m_FileName;
    std::string              m_Error;
    std::vector<Item>        m_Items;
    std::filesystem::path    m_Overwrite; // Save: existing file waiting for confirmation
    std::vector<std::filesystem::path> m_Places;
};

} // namespace Engine
