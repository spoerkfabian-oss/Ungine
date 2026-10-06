// Levels panel: streaming volumes of the scene and the sub-levels of the level streamer.
#include "Editor/Editor.h"
#include "Editor/FileDialog.h"

#include "Engine/Core/Platform.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/LevelStreaming.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine {

std::string Editor::StreamedLevelOf(Entity entity) const
{
    const Registry& registry = m_Ctx.scene.GetRegistry();
    for (Entity e = entity; registry.Valid(e); e = registry.Get<Hierarchy>(e).parent)
        if (const StreamedLevel* level = registry.TryGet<StreamedLevel>(e))
            return level->level;
    return {};
}

bool Editor::IsStreamed(Entity entity) const
{
    const Registry& registry = m_Ctx.scene.GetRegistry();
    for (Entity e = entity; registry.Valid(e); e = registry.Get<Hierarchy>(e).parent)
        if (registry.Has<StreamedLevel>(e))
            return true;
    return false;
}

void Editor::DrawLevels()
{
    if (!ImGui::Begin("Levels", &m_ShowLevels)) {
        ImGui::End();
        return;
    }
    LevelStreamer& streamer = *m_Ctx.streaming;
    Registry&      registry = m_Ctx.scene.GetRegistry();
    const bool     editing  = m_PlayState == PlayState::Edit;

    if (editing)
        ImGui::TextDisabled("Previews: read-only, not saved. Edit a level by opening its file.");
    else
        ImGui::TextDisabled("Playing: volumes stream around the StreamingSource (else the camera).");
    if (ImGui::Button("Load level...")) {
        m_DialogPurpose = DialogPurpose::PreviewLevel;
        m_FileDialog->Open("Load sub-level", FileDialog::Mode::Open,
                           m_ScenePath.empty() ? ContentRoot() : m_ScenePath.parent_path(), {".json"});
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load a scene file additively (preview in edit mode)");
    ImGui::SameLine();
    if (ImGui::Button("Unload all"))
        streamer.UnloadAll(m_Ctx.scene);

    // Sub-levels known to the streamer.
    const std::vector<StreamedLevelInfo> levels = streamer.Levels();
    ImGui::SeparatorText("Sub-levels");
    if (levels.empty())
        ImGui::TextDisabled("none");
    else if (ImGui::BeginTable("levels", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Entities", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();
        for (const StreamedLevelInfo& level : levels) {
            ImGui::PushID(level.key.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(level.level.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s%s", level.key.c_str(), level.requested ? "requested " : "",
                                  level.byVolume ? "wanted by a volume" : "");
            ImGui::TableNextColumn();
            if (level.state == LevelState::Preparing || level.state == LevelState::Loading) {
                ImGui::ProgressBar(level.progress, ImVec2(-FLT_MIN, 0.0f), LevelStateName(level.state));
            } else if (level.state == LevelState::Failed) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "failed");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", level.error.c_str());
            } else {
                ImGui::TextUnformatted(LevelStateName(level.state));
            }
            ImGui::TableNextColumn();
            ImGui::Text("%zu", level.roots);
            ImGui::TableNextColumn();
            if (level.requested ? ImGui::SmallButton("Unload") : ImGui::SmallButton("Load")) {
                if (level.requested)
                    streamer.Unload(PathFromUtf8(level.key));
                else
                    streamer.Load(PathFromUtf8(level.key));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Open")) {
                const std::filesystem::path file = PathFromUtf8(level.key);
                RequestSceneChange([this, file] { OpenScene(file); });
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Streaming volumes of this scene (click selects).
    std::vector<Entity> volumes;
    registry.ViewOf<LevelStreamingVolume>().Each([&](Entity e, LevelStreamingVolume&) { volumes.push_back(e); });
    std::ranges::sort(volumes, {}, [](Entity e) { return EntityIndex(e); });
    ImGui::SeparatorText("Streaming volumes");
    if (volumes.empty())
        ImGui::TextDisabled("none (Hierarchy: + Add > Streaming Volume)");
    for (Entity e : volumes) {
        const LevelStreamingVolume& volume = registry.Get<LevelStreamingVolume>(e);
        const std::string&          name   = registry.Get<Name>(e).value;
        ImGui::PushID(static_cast<int>(EntityIndex(e)));
        const std::string label = (name.empty() ? std::string("(unnamed)") : name) + "  ->  " +
                                  (volume.level.empty() ? std::string("(no level)") : LevelStreamer::DisplayPath(PathFromUtf8(volume.level)));
        if (ImGui::Selectable(label.c_str(), IsSelected(e)))
            SelectFromClick(e, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
        if (!volume.level.empty()) {
            const LevelState state = streamer.State(PathFromUtf8(volume.level));
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", LevelStateName(state));
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace Engine
