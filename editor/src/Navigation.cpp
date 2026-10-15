// Navigation window: navmesh overlay options, bake, auto rebuild, test path, statistics.
#include "Editor/Editor.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Navigation/NavigationWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace Engine {

namespace {

bool HasNavMeshBounds(const Scene& scene)
{
    bool found = false;
    scene.GetRegistry().ViewOf<NavMeshBounds>().Each([&](Entity, const NavMeshBounds&) { found = true; });
    return found;
}

} // namespace

void Editor::BakeNavigation()
{
    if (!m_Ctx.navigation)
        return;
    m_Ctx.navigation->Build(m_Ctx.scene);
    if (!m_ScenePath.empty()) {
        SaveNavigation(m_ScenePath);
    } else {
        m_Status = "Navigation baked (save the scene to keep it)";
    }
    UpdateNavigationTestPath();
}

void Editor::SaveNavigation(const std::filesystem::path& scene)
{
    if (!m_Ctx.navigation || scene.empty())
        return;
    const std::filesystem::path file = NavMeshFileFor(scene);
    std::error_code             ec;
    if (!HasNavMeshBounds(m_Ctx.scene)) { // no navmesh: no stale file either
        if (std::filesystem::exists(file, ec))
            std::filesystem::remove(file, ec);
        return;
    }
    m_Ctx.navigation->Flush(m_Ctx.scene); // the file matches the saved scene
    std::string error;
    if (m_Ctx.navigation->SaveBaked(file, &error))
        m_Status = "Navigation baked: " + PathToUtf8(file.filename());
    else
        ENGINE_WARN("Navigation: cannot save '{}': {}", PathToUtf8(file), error);
}

void Editor::LoadNavigation(const std::filesystem::path& scene)
{
    if (!m_Ctx.navigation)
        return;
    m_Ctx.navigation->Reset();
    m_NavStart.reset(), m_NavEnd.reset();
    m_NavPath = {};
    m_NavPick = NavPick::None;
    if (scene.empty())
        return;
    const std::filesystem::path file = NavMeshFileFor(scene);
    std::error_code             ec;
    if (!std::filesystem::exists(file, ec))
        return; // built from the scene in the background
    std::string error;
    if (!m_Ctx.navigation->LoadBaked(file, &error))
        ENGINE_WARN("Navigation: {} (rebuilding)", error);
}

void Editor::SetNavigationTestPath(const glm::vec3& start, const glm::vec3& end)
{
    m_NavStart = start;
    m_NavEnd   = end;
    UpdateNavigationTestPath();
}

void Editor::UpdateNavigationTestPath()
{
    m_NavPath = m_Ctx.navigation && m_NavStart && m_NavEnd
                    ? m_Ctx.navigation->FindPath(*m_NavStart, *m_NavEnd, m_NavProfile)
                    : NavPath{};
}

void Editor::DrawNavigation()
{
    if (!ImGui::Begin("Navigation", &m_ShowNavigation)) {
        ImGui::End();
        return;
    }
    NavigationWorld&   nav      = *m_Ctx.navigation;
    const NavSettings& settings = nav.Settings();
    m_NavProfile                = std::min<std::uint32_t>(m_NavProfile, static_cast<std::uint32_t>(settings.profiles.size() - 1));

    ImGui::Checkbox("Show navmesh", &m_ShowNavMesh);
    ImGui::SameLine();
    ImGui::Checkbox("Tiles", &m_NavShowTiles);
    ImGui::SameLine();
    ImGui::Checkbox("Auto rebuild", &nav.autoRebuild);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rebuild the tiles touched by every edit in the background (else only Bake)");
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("Profile", settings.profiles[m_NavProfile].name.c_str())) {
        for (std::uint32_t i = 0; i < settings.profiles.size(); ++i)
            if (ImGui::Selectable(settings.profiles[i].name.c_str(), i == m_NavProfile))
                m_NavProfile = i;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Agent profile shown in the viewport and used by the test path (Project Settings > Navigation)");

    if (ImGui::Button("Bake"))
        BakeNavigation();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Build every tile now and save the navmesh next to the scene (also done on Save)");
    ImGui::SameLine();
    const NavStats& stats = nav.Stats();
    if (stats.pendingTiles > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Building... %u tile(s) left", stats.pendingTiles);
    else if (stats.tiles == 0)
        ImGui::TextDisabled("No navmesh: add a Nav Mesh Bounds volume around static colliders");
    else
        ImGui::TextDisabled("Up to date");

    ImGui::SeparatorText("Test path");
    const auto point = [&](const char* label, std::optional<glm::vec3>& p, NavPick pick) {
        const bool picking = m_NavPick == pick;
        if (picking)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        if (ImGui::Button(label))
            m_NavPick = picking ? NavPick::None : pick;
        if (picking)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Then click in the viewport");
        ImGui::SameLine();
        if (p)
            ImGui::Text("(%.2f, %.2f, %.2f)", p->x, p->y, p->z);
        else
            ImGui::TextDisabled("not set");
    };
    point("Pick start", m_NavStart, NavPick::Start);
    point("Pick end", m_NavEnd, NavPick::End);
    UpdateNavigationTestPath();
    if (m_NavStart && m_NavEnd) {
        const char* status = m_NavPath.status == NavPathStatus::Complete  ? "complete"
                             : m_NavPath.status == NavPathStatus::Partial ? "partial (end not reachable)"
                                                                          : "not found";
        ImGui::Text("Path %s: %.2f m, %zu corner(s)", status, m_NavPath.Length(), m_NavPath.points.size());
    }
    if (ImGui::Button("Clear path")) {
        m_NavStart.reset(), m_NavEnd.reset();
        m_NavPath = {};
        m_NavPick = NavPick::None;
    }

    ImGui::SeparatorText("Areas");
    for (const NavAreaType& area : settings.areas) {
        ImGui::ColorButton(area.name.c_str(), ImVec4(area.color.r, area.color.g, area.color.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoInputs, ImVec2(12.0f, 12.0f));
        ImGui::SameLine();
        ImGui::Text("%s (cost %.1f)", area.name.c_str(), area.cost);
    }
    ImGui::TextDisabled("Profiles, areas and voxel settings: Project Settings > Navigation");

    ImGui::SeparatorText("Statistics");
    ImGui::Text("Tiles %u, polygons %u", stats.tiles, stats.polygons);
    ImGui::Text("Sources %u, areas %u, links %u, obstacles %u", stats.sources, stats.areas, stats.links, stats.obstacles);
    ImGui::Text("Agents %u, update %.2f ms, tiles built %llu", stats.agents, stats.updateMs,
                static_cast<unsigned long long>(stats.tilesBuilt));
    ImGui::End();
}

} // namespace Engine
