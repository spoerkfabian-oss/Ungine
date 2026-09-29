#include "FileDialog.h"

#include "Engine/Core/Platform.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace Engine {

namespace {
std::string Utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}
std::filesystem::path FromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

void FileDialog::Open(std::string title, Mode mode, const std::filesystem::path& directory,
                      std::vector<std::string> extensions, std::string fileName)
{
    std::error_code ec;
    m_Title         = std::move(title);
    m_Mode          = mode;
    m_Directory     = std::filesystem::is_directory(directory, ec) ? std::filesystem::absolute(directory, ec)
                                                                   : std::filesystem::current_path(ec);
    m_Extensions    = std::move(extensions);
    m_FileName      = std::move(fileName);
    m_Error.clear();
    m_Overwrite.clear();
    m_Places        = Places();
    m_Open          = true;
    m_OpenRequested = true;
    Refresh();
}

std::vector<std::filesystem::path> FileDialog::Places()
{
    std::vector<std::filesystem::path> places;
    std::error_code                    ec;
    if (const std::filesystem::path home = HomeDirectory(); !home.empty() && std::filesystem::is_directory(home, ec))
        places.push_back(home);
    if (const auto cwd = std::filesystem::current_path(ec); !ec)
        places.push_back(cwd);
#ifdef _WIN32
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        const std::filesystem::path drive = std::string(1, letter) + ":\\";
        if (std::filesystem::exists(drive, ec))
            places.push_back(drive);
    }
#else
    places.emplace_back("/");
#endif
    return places;
}

void FileDialog::Navigate(const std::filesystem::path& directory)
{
    std::error_code ec;
    m_Directory = std::filesystem::absolute(directory, ec).lexically_normal();
    m_Error.clear();
    m_Overwrite.clear();
    Refresh();
}

bool FileDialog::Matches(const std::filesystem::path& file) const
{
    if (m_Extensions.empty())
        return true;
    const std::string name = Lower(Utf8(file.filename()));
    return std::ranges::any_of(m_Extensions, [&](const std::string& ext) {
        return name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
    });
}

void FileDialog::Refresh()
{
    m_Items.clear();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_Directory, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code typeError;
        const bool      directory = it->is_directory(typeError);
        if (typeError || (!directory && !Matches(it->path())))
            continue;
        m_Items.push_back({it->path(), Utf8(it->path().filename()), directory});
    }
    if (ec)
        m_Error = ec.message();
    std::ranges::sort(m_Items, [](const Item& a, const Item& b) {
        return a.directory != b.directory ? a.directory : Lower(a.label) < Lower(b.label);
    });
}

std::optional<std::filesystem::path> FileDialog::Confirm()
{
    if (m_Mode == Mode::Folder) {
        if (!m_FileName.empty()) { // a typed or selected sub directory
            std::filesystem::path path = FromUtf8(m_FileName);
            std::error_code       ec;
            if (path.is_relative())
                path = m_Directory / path;
            if (!std::filesystem::is_directory(path, ec)) {
                m_Error = "Not a directory";
                return std::nullopt;
            }
            return std::filesystem::absolute(path, ec).lexically_normal();
        }
        return m_Directory.lexically_normal();
    }
    if (m_FileName.empty())
        return std::nullopt;
    std::filesystem::path path = FromUtf8(m_FileName);
    if (path.is_relative())
        path = m_Directory / path;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) { // typed a directory: go there
        Navigate(path);
        m_FileName.clear();
        return std::nullopt;
    }
    if (m_Mode == Mode::Open && !std::filesystem::exists(path, ec)) {
        m_Error = "File not found";
        return std::nullopt;
    }
    if (m_Mode == Mode::Save && !m_Extensions.empty() && !Matches(path))
        path += m_Extensions.front();
    path = path.lexically_normal();
    if (m_Mode == Mode::Save && std::filesystem::exists(path, ec) && path != m_Overwrite) {
        m_Overwrite = path; // ask first; confirming the same path again saves
        return std::nullopt;
    }
    return path;
}

std::optional<std::filesystem::path> FileDialog::Draw()
{
    if (!m_Open)
        return std::nullopt;
    const std::string id = m_Title + "##FileDialog";
    if (std::exchange(m_OpenRequested, false))
        ImGui::OpenPopup(id.c_str());

    std::optional<std::filesystem::path> result;
    ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(id.c_str(), &m_Open)) {
        m_Open = ImGui::IsPopupOpen(id.c_str());
        return std::nullopt;
    }

    if (ImGui::Button("Up") && m_Directory.has_parent_path() && m_Directory.parent_path() != m_Directory)
        Navigate(m_Directory.parent_path());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("##places", "Places")) {
        for (const std::filesystem::path& place : m_Places)
            if (ImGui::Selectable(Utf8(place).c_str()))
                Navigate(place);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(Utf8(m_Directory).c_str());

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("items", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
        for (const Item& item : m_Items) {
            const std::string label    = item.directory ? "[" + item.label + "]" : item.label;
            const bool        selected = !item.directory && item.label == m_FileName;
            if (m_Mode == Mode::Folder && !item.directory) {
                ImGui::TextDisabled("%s", label.c_str()); // files for orientation only
                continue;
            }
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                m_Overwrite.clear();
                if (item.directory) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        const std::filesystem::path next = item.path;
                        m_FileName.clear();
                        Navigate(next);
                        break; // m_Items changed
                    }
                } else {
                    m_FileName = item.label;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        result = Confirm();
                }
            }
        }
    }
    ImGui::EndChild();

    if (!m_Overwrite.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "'%s' exists. Overwrite?", Utf8(m_Overwrite.filename()).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Overwrite"))
            result = m_Overwrite;
        ImGui::SameLine();
        if (ImGui::Button("Keep"))
            m_Overwrite.clear();
    } else {
        const char* action = m_Mode == Mode::Open ? "Open" : m_Mode == Mode::Save ? "Save" : "Select";
        ImGui::SetNextItemWidth(m_Mode == Mode::Folder ? -250.0f : -160.0f);
        const bool enter = ImGui::InputText("##name", &m_FileName, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited())
            m_Error.clear();
        ImGui::SameLine();
        if (ImGui::Button(action, ImVec2(70.0f, 0.0f)) || enter)
            result = Confirm();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(70.0f, 0.0f)))
            m_Open = false;
        if (m_Mode == Mode::Folder) {
            ImGui::SameLine();
            if (ImGui::Button("New folder", ImVec2(90.0f, 0.0f)) && !m_FileName.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(m_Directory / FromUtf8(m_FileName), ec);
                if (ec)
                    m_Error = ec.message();
                else
                    Refresh();
            }
        }
    }
    if (!m_Error.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, 1.0f), "%s", m_Error.c_str());
    else if (!m_Extensions.empty()) {
        std::string filter;
        for (const std::string& ext : m_Extensions)
            filter += (filter.empty() ? "" : " ") + ext;
        ImGui::TextDisabled("Files: %s", filter.c_str());
    }

    if (result)
        m_Open = false;
    if (!m_Open)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return result;
}

} // namespace Engine
