#pragma once
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/InputMap.h"
#include "Engine/Core/MoveOnlyFunction.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

struct ProjectSettings;

// Options a player changes for themselves (Options menu, Blueprint "Options" nodes). Defaults come
// from the project settings; only the user's choices are stored.
struct UserSettings {
    bool          fullscreen       = false;
    bool          vsync            = true;
    std::uint32_t windowWidth      = 0; // windowed size; 0: the project's
    std::uint32_t windowHeight     = 0;
    std::uint32_t shadowQuality    = 3; // 0 off, 1 low, 2 medium, 3 high (see GameOptions::ShadowResolution)
    bool          ambientOcclusion = true;
    bool          bloom            = true;
    AudioSettings audio; // bus volumes / mutes (occlusion stays as the project sets it)
    // Remapped bindings, whole lists by name; names the project does not have are ignored.
    std::vector<InputActionBinding> actions;
    std::vector<InputAxisBinding>   axes;

    bool operator==(const UserSettings&) const = default;
};

// The user's options of one game: stored as JSON (the player: Settings.json in its save directory),
// applied by the application through the apply callback (window, renderer, audio, input map).
// Main thread only.
class GameOptions {
public:
    // Reads `file` when it exists (unknown or broken entries keep their defaults).
    GameOptions(const ProjectSettings& project, std::filesystem::path file);

    [[nodiscard]] const UserSettings& Values() const { return m_Values; }
    [[nodiscard]] UserSettings&       Edit() { return m_Values; } // then Changed()
    void Changed();                                                 // runs the apply callback
    void SetApply(MoveOnlyFunction<void(const GameOptions&)> apply);
    bool Save() const; // false: cannot write the file
    void ResetToDefaults();
    void ResetInput(); // only the bindings

    // The project's bindings with the user's remappings.
    [[nodiscard]] InputMap        Input() const;
    [[nodiscard]] const InputMap& ProjectInput() const { return m_ProjectInput; }
    // Replaces key `index` of an action or axis (index == key count: appends; the axis key keeps
    // its scale, a new one gets 1). An empty key removes it. False: no such action / axis.
    bool RemapAction(std::string_view action, std::size_t index, const std::string& key);
    bool RemapAxis(std::string_view axis, std::size_t index, const std::string& key, float scale = 1.0f);
    // Actions / axes (except `except`) whose bindings contain `key`: a remap conflict hint.
    [[nodiscard]] std::vector<std::string> Conflicts(std::string_view key, std::string_view except = {}) const;

    [[nodiscard]] const std::filesystem::path& File() const { return m_File; }
    [[nodiscard]] const UserSettings&          Defaults() const { return m_Defaults; }
    // Shadow map resolution cap of a quality level (0: shadows off).
    [[nodiscard]] static std::uint32_t ShadowResolution(std::uint32_t quality);

private:
    std::filesystem::path                        m_File;
    InputMap                                     m_ProjectInput;
    UserSettings                                 m_Defaults;
    UserSettings                                 m_Values;
    MoveOnlyFunction<void(const GameOptions&)>   m_Apply;
};

} // namespace Engine
