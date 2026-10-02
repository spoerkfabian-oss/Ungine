#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace Engine {

// Operating system helpers (Windows / Linux).

// The running executable and its directory (installed builds find shaders / templates next to it).
[[nodiscard]] std::filesystem::path ExecutablePath();
[[nodiscard]] std::filesystem::path ExecutableDirectory();

// The user's home directory (Windows: %USERPROFILE%), empty if unknown.
[[nodiscard]] std::filesystem::path HomeDirectory();

// Per-user settings directory, created on demand: %APPDATA%/Ungine on Windows,
// $XDG_CONFIG_HOME/ungine (~/.config/ungine) elsewhere.
[[nodiscard]] std::filesystem::path UserConfigDirectory();

// Starts a program without waiting for it (arguments passed as they are, no shell).
// workingDirectory empty: inherited. Returns false if it could not be started.
bool LaunchProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                   const std::filesystem::path& workingDirectory = {});

// Shows a directory in the system file browser (Explorer / xdg-open).
bool OpenInFileBrowser(const std::filesystem::path& directory);

// A sibling executable of the running one ("UnginePlayer" -> UnginePlayer.exe on Windows).
[[nodiscard]] std::filesystem::path SiblingExecutable(const std::string& name);

// Directory with the compiled shaders: <exe dir>/shaders when installed / packaged, else the
// build tree (ENGINE_SHADER_DIR).
[[nodiscard]] const std::filesystem::path& ShaderDirectory();

// Project templates: <exe dir>/templates when installed, else the source tree.
[[nodiscard]] std::filesystem::path TemplateDirectory();

// Program arguments as UTF-8 (Windows: from the wide command line, so non-ASCII paths of a
// double-clicked file survive; elsewhere argv as it is). Element 0 is the program.
[[nodiscard]] std::vector<std::string> CommandLineUtf8(int argc, char** argv);

// UTF-8 <-> path (std::filesystem::path::string() is the ANSI code page on Windows).
[[nodiscard]] std::string           PathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path PathFromUtf8(const std::string& utf8);

} // namespace Engine
