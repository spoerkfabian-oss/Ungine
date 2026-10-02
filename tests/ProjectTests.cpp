#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Script/ScriptGraph.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <iterator>
#include <string>

using namespace Engine;
namespace fs = std::filesystem;

namespace {
fs::path TempDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}
} // namespace

TEST_CASE(Project_TemplatesCreateLoadSave)
{
    const std::vector<ProjectTemplate> templates = ProjectTemplates();
    CHECK(templates.size() >= 3);
    const auto basic = std::ranges::find_if(templates, [](const ProjectTemplate& t) { return t.id == "Basic"; });
    CHECK(basic != templates.end());
    if (basic == templates.end())
        return;
    CHECK(!basic->startScene.empty() && !basic->description.empty());

    const fs::path location = TempDir("create");
    CHECK(!IsValidProjectName("") && !IsValidProjectName("a/b") && !IsValidProjectName(" x") && IsValidProjectName("My Game_2"));
    std::string error;
    CHECK(!Project::Create(location, "bad/name", *basic, &error) && !error.empty());

    const std::optional<Project> project = Project::Create(location, "My Game", *basic, &error);
    CHECK(project.has_value());
    if (!project)
        return;
    CHECK(project->File() == location / "My Game" / "My Game.ungineproj");
    CHECK(fs::exists(project->StartScene()) && fs::is_directory(project->SavedDirectory()));
    CHECK(fs::exists(project->ContentDirectory() / "Scripts" / "Rotator.ugraph"));
    CHECK(!fs::exists(project->Root() / "template.json"));
    CHECK(project->Relative(project->StartScene()) == "Content/Scenes/Main.scene.json");
    CHECK(!Project::Create(location, "My Game", *basic, &error)); // exists, not empty

    // The template's scene references the script relative to itself; the graph is valid.
    std::ifstream              sceneFile(project->StartScene());
    const nlohmann::json       scene = nlohmann::json::parse(sceneFile);
    bool                       scripted = false;
    for (const auto& e : scene.at("entities"))
        if (e.contains("script")) {
            const fs::path graph = (project->StartScene().parent_path() / e["script"]["graph"].get<std::string>()).lexically_normal();
            CHECK(fs::exists(graph));
            const auto diagnostics = ValidateScriptGraph(LoadScriptGraph(graph));
            CHECK(std::ranges::none_of(diagnostics, [](const ScriptDiagnostic& d) { return d.error; }));
            scripted = true;
        }
    CHECK(scripted);

    // Settings round trip.
    Project edited = *project;
    edited.settings.windowWidth = 1280;
    edited.settings.fullscreen  = true;
    edited.settings.startScene  = "Content/Scenes/Other.scene.json";
    CHECK(edited.Save(&error));
    const std::optional<Project> loaded = Project::Load(project->File(), &error);
    CHECK(loaded && loaded->settings.windowWidth == 1280 && loaded->settings.fullscreen && loaded->settings.name == "My Game");
    CHECK(loaded && loaded->settings.startScene == "Content/Scenes/Other.scene.json");
    CHECK(!Project::Load(location / "missing.ungineproj", &error) && !error.empty());
}

TEST_CASE(Project_RecentListAndPackaging)
{
    const fs::path dir  = TempDir("recent");
    const fs::path list = dir / "recent.json";
    ProjectTemplate empty; // no files: just the project file
    const auto a = Project::Create(dir, "Alpha", empty);
    const auto b = Project::Create(dir, "Beta", empty);
    CHECK(a && b);
    if (!a || !b)
        return;
    AddRecentProject(*a, list);
    AddRecentProject(*b, list);
    AddRecentProject(*a, list); // moves to the front, no duplicate
    auto recent = LoadRecentProjects(list);
    CHECK(recent.size() == 2 && recent[0].name == "Alpha");
    std::error_code ec;
    fs::remove_all(b->Root(), ec); // gone: dropped when loading
    recent = LoadRecentProjects(list);
    CHECK(recent.size() == 1 && recent[0].file == a->File());
    RemoveRecentProject(a->File(), list);
    CHECK(LoadRecentProjects(list).empty());

    // Packaging: renamed player, shaders, project file, Content.
    fs::create_directories(a->ContentDirectory() / "Scenes", ec);
    std::ofstream(a->ContentDirectory() / "Scenes" / "Main.scene.json") << "{\"version\":1,\"entities\":[]}";
    const fs::path fakePlayer = dir / "UnginePlayer.bin";
    std::ofstream(fakePlayer) << "player";
    const fs::path out = dir / "Package";
    std::string    error;
    CHECK(!PackageProject(*a, dir / "missing", out, &error) && !error.empty());
    CHECK(PackageProject(*a, fakePlayer, out, &error));
    CHECK(fs::exists(out / "Alpha.bin") && fs::exists(out / "Alpha.ungineproj"));
    CHECK(fs::exists(out / "Content" / "Scenes" / "Main.scene.json") && fs::exists(out / "shaders" / "mesh.vert.spv"));
    CHECK(PackageProject(*a, fakePlayer, out, &error)); // again: replaces
    fs::remove_all(dir, ec);
}

TEST_CASE(Platform_PathsAndProcess)
{
    CHECK(fs::exists(ExecutablePath()) && fs::is_directory(ExecutableDirectory()));
    CHECK(fs::exists(ShaderDirectory() / "mesh.vert.spv"));
    CHECK(fs::is_directory(TemplateDirectory()));
    CHECK(!UserConfigDirectory().empty());
    const std::string utf8 = "Proj\xC3\xA9ts/\xE6\xB8\xB8\xE6\x88\x8F";
    CHECK(PathToUtf8(PathFromUtf8(utf8)) == utf8);
    const char* argv[] = {"prog", "arg one"};
    const auto  args   = CommandLineUtf8(2, const_cast<char**>(argv));
    CHECK(args.size() >= 1);
#ifndef _WIN32
    CHECK(args.size() == 2 && args[1] == "arg one");
    const fs::path marker = TempDir("launch") / "started";
    CHECK(LaunchProcess("/bin/sh", {"-c", "touch \"$0\"", marker.string()}));
    for (int i = 0; i < 100 && !fs::exists(marker); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(fs::exists(marker));
    CHECK(!LaunchProcess("/nonexistent/program", {}));
#endif
}
