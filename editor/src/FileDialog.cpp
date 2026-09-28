#include "FileDialog.h"

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
    m_Open          = true;
    m_OpenRequested = true;
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
    if (m_FileName.empty())
        return std::nullopt;
    std::filesystem::path path = FromUtf8(m_FileName);
    if (path.is_relative())
        path = m_Directory / path;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) { // typed a directory: go there
        m_Directory = std::filesystem::absolute(path, ec).lexically_normal();
        m_FileName.clear();
        Refresh();
        return std::nullopt;
    }
    if (m_Mode == Mode::Open && !std::filesystem::exists(path, ec)) {
        m_Error = "File not found";
        return std::nullopt;
    }
    if (m_Mode == Mode::Save && !m_Extensions.empty() && !Matches(path))
        path += m_Extensions.front();
    return path.lexically_normal();
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

    if (ImGui::Button("Up") && m_Directory.has_parent_path() && m_Directory.parent_path() != m_Directory) {
        m_Directory = m_Directory.parent_path();
        Refresh();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(Utf8(m_Directory).c_str());

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("items", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
        for (const Item& item : m_Items) {
            const std::string label    = item.directory ? "[" + item.label + "]" : item.label;
            const bool        selected = !item.directory && item.label == m_FileName;
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (item.directory) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        m_Directory = item.path;
                        Refresh();
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

    ImGui::SetNextItemWidth(-160.0f);
    const bool enter = ImGui::InputText("##name", &m_FileName, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(m_Mode == Mode::Open ? "Open" : "Save", ImVec2(70.0f, 0.0f)) || enter)
        result = Confirm();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(70.0f, 0.0f)))
        m_Open = false;
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
