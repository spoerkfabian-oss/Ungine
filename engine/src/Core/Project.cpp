#include "Engine/Core/Project.h"
#include "Engine/Assets/ContentCooker.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {
constexpr int kProjectVersion = 1;

void SetError(std::string* error, std::string message)
{
    if (error)
        *error = std::move(message);
}

std::optional<json> ReadJson(const fs::path& file, std::string* error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        SetError(error, "cannot read '" + PathToUtf8(file) + "'");
        return std::nullopt;
    }
    try {
        return json::parse(in);
    } catch (const json::exception& e) {
        SetError(error, "'" + PathToUtf8(file) + "': " + e.what());
        return std::nullopt;
    }
}

bool WriteText(const fs::path& file, const std::string& text, std::string* error)
{
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text << '\n';
        if (!out) {
            SetError(error, "cannot write '" + PathToUtf8(temp) + "'");
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) {
        SetError(error, "cannot replace '" + PathToUtf8(file) + "': " + ec.message());
        return false;
    }
    return true;
}

fs::path DefaultRecentList() { return UserConfigDirectory() / "recent_projects.json"; }

std::int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void SaveRecent(const std::vector<RecentProject>& projects, const fs::path& list)
{
    json entries = json::array();
    for (const RecentProject& p : projects)
        entries.push_back({{"file", PathToUtf8(p.file)}, {"name", p.name}, {"lastOpened", p.lastOpened}});
    std::string error;
    if (!WriteText(list, json{{"projects", entries}}.dump(2), &error))
        ENGINE_WARN("Recent projects: {}", error);
}

bool SameFile(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
}
} // namespace

// --- Project ------------------------------------------------------------------------------------

std::optional<Project> Project::Load(const fs::path& file, std::string* error)
{
    const std::optional<json> root = ReadJson(file, error);
    if (!root)
        return std::nullopt;
    try {
        const int version = root->value("version", 0);
        if (version < 1 || version > kProjectVersion) {
            SetError(error, "unsupported project version " + std::to_string(version));
            return std::nullopt;
        }
        Project project;
        std::error_code ec;
        project.m_File                 = fs::absolute(file, ec).lexically_normal();
        ProjectSettings& s             = project.settings;
        s.name                         = root->value("name", project.m_File.stem().string());
        s.startScene                   = root->value("startScene", std::string());
        s.loadingScreen                = root->value("loadingScreen", std::string());
        s.saveVersion                  = std::max(root->value("saveVersion", 1u), 1u);
        if (const auto w = root->find("window"); w != root->end()) {
            s.windowWidth  = std::clamp(w->value("width", s.windowWidth), 320u, 16384u);
            s.windowHeight = std::clamp(w->value("height", s.windowHeight), 200u, 16384u);
            s.fullscreen   = w->value("fullscreen", s.fullscreen);
            s.vsync        = w->value("vsync", s.vsync);
        }
        if (const auto a = root->find("audio"); a != root->end() && a->is_object()) {
            if (const auto buses = a->find("buses"); buses != a->end() && buses->is_object())
                for (std::size_t i = 0; i < kAudioBusCount; ++i)
                    if (const auto b = buses->find(ToString(static_cast<AudioBus>(i))); b != buses->end()) {
                        s.audio.volume[i] = std::clamp(b->value("volume", s.audio.volume[i]), 0.0f, 4.0f);
                        s.audio.muted[i]  = b->value("muted", s.audio.muted[i]);
                    }
            s.audio.occlusion         = a->value("occlusion", s.audio.occlusion);
            s.audio.occlusionStrength = std::clamp(a->value("occlusionStrength", s.audio.occlusionStrength), 0.0f, 1.0f);
            s.audio.occlusionRays     = std::min(a->value("occlusionRays", s.audio.occlusionRays), 4096u);
        }
        if (const auto in = root->find("input"); in != root->end() && in->is_object()) {
            for (const json& a : in->value("actions", json::array()))
                s.input.actions.push_back({a.at("name").get<std::string>(), a.value("keys", std::vector<std::string>{})});
            for (const json& a : in->value("axes", json::array())) {
                InputAxisBinding axis{a.at("name").get<std::string>(), {}};
                for (const json& k : a.value("keys", json::array()))
                    axis.keys.push_back({k.at("key").get<std::string>(), k.value("scale", 1.0f)});
                s.input.axes.push_back(std::move(axis));
            }
        }
        return project;
    } catch (const json::exception& e) {
        SetError(error, "'" + PathToUtf8(file) + "': " + e.what());
        return std::nullopt;
    }
}

bool Project::Save(std::string* error) const
{
    json buses = json::object();
    for (std::size_t i = 0; i < kAudioBusCount; ++i)
        buses[ToString(static_cast<AudioBus>(i))] = {{"volume", settings.audio.volume[i]}, {"muted", settings.audio.muted[i]}};
    json actions = json::array(), axes = json::array();
    for (const InputActionBinding& a : settings.input.actions)
        actions.push_back({{"name", a.name}, {"keys", a.keys}});
    for (const InputAxisBinding& a : settings.input.axes) {
        json keys = json::array();
        for (const InputAxisKey& k : a.keys)
            keys.push_back({{"key", k.key}, {"scale", k.scale}});
        axes.push_back({{"name", a.name}, {"keys", std::move(keys)}});
    }
    const json root{{"version", kProjectVersion},
                    {"engine", "Ungine"},
                    {"name", settings.name},
                    {"startScene", settings.startScene},
                    {"loadingScreen", settings.loadingScreen},
                    {"saveVersion", settings.saveVersion},
                    {"window",
                     {{"width", settings.windowWidth},
                      {"height", settings.windowHeight},
                      {"fullscreen", settings.fullscreen},
                      {"vsync", settings.vsync}}},
                    {"audio",
                     {{"buses", std::move(buses)},
                      {"occlusion", settings.audio.occlusion},
                      {"occlusionStrength", settings.audio.occlusionStrength},
                      {"occlusionRays", settings.audio.occlusionRays}}},
                    {"input", {{"actions", std::move(actions)}, {"axes", std::move(axes)}}}};
    return WriteText(m_File, root.dump(2), error);
}

std::optional<Project> Project::Create(const fs::path& location, const std::string& name,
                                       const ProjectTemplate& projectTemplate, std::string* error)
{
    if (!IsValidProjectName(name)) {
        SetError(error, "invalid project name (letters, digits, '_', '-', spaces)");
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path  root = fs::absolute(location, ec).lexically_normal() / PathFromUtf8(name);
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) {
        SetError(error, "'" + PathToUtf8(root) + "' already exists and is not empty");
        return std::nullopt;
    }
    fs::create_directories(root / "Content", ec);
    fs::create_directories(root / "Saved", ec);
    if (ec) {
        SetError(error, "cannot create '" + PathToUtf8(root) + "': " + ec.message());
        return std::nullopt;
    }
    if (!projectTemplate.directory.empty()) {
        for (fs::recursive_directory_iterator it(projectTemplate.directory, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path relative = fs::relative(it->path(), projectTemplate.directory, ec);
            if (relative == "template.json")
                continue;
            if (it->is_directory(ec))
                fs::create_directories(root / relative, ec);
            else
                fs::copy_file(it->path(), root / relative, fs::copy_options::overwrite_existing, ec);
            if (ec)
                break;
        }
        if (ec) {
            SetError(error, "copying the template failed: " + ec.message());
            return std::nullopt;
        }
    }
    Project project;
    project.m_File              = root / PathFromUtf8(name + std::string(kExtension));
    project.settings.name       = name;
    project.settings.startScene = projectTemplate.startScene;
    if (!project.Save(error))
        return std::nullopt;
    return project;
}

fs::path Project::StartScene() const
{
    return settings.startScene.empty() ? fs::path() : (Root() / PathFromUtf8(settings.startScene)).lexically_normal();
}

std::string Project::Relative(const fs::path& file) const
{
    std::error_code ec;
    const fs::path  absolute = fs::absolute(file, ec).lexically_normal();
    const fs::path  relative = absolute.lexically_relative(Root());
    const std::u8string generic =
        relative.empty() || *relative.begin() == ".." ? absolute.generic_u8string() : relative.generic_u8string();
    return {generic.begin(), generic.end()};
}

bool IsValidProjectName(std::string_view name)
{
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ')
        return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ';
    });
}

std::vector<ProjectTemplate> ProjectTemplates()
{
    std::vector<ProjectTemplate> templates;
    std::error_code              ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(TemplateDirectory(), ec)) {
        const std::optional<json> info = entry.is_directory(ec) ? ReadJson(entry.path() / "template.json", nullptr) : std::nullopt;
        if (!info)
            continue;
        templates.push_back({.id          = PathToUtf8(entry.path().filename()),
                             .name        = info->value("name", PathToUtf8(entry.path().filename())),
                             .description = info->value("description", std::string()),
                             .directory   = entry.path(),
                             .startScene  = info->value("startScene", std::string())});
    }
    std::ranges::sort(templates, {}, &ProjectTemplate::name);
    return templates;
}

// --- Recent projects ----------------------------------------------------------------------------

std::vector<RecentProject> LoadRecentProjects(const fs::path& list)
{
    std::vector<RecentProject> projects;
    const std::optional<json>  root = ReadJson(list.empty() ? DefaultRecentList() : list, nullptr);
    if (!root)
        return projects;
    try {
        std::error_code ec;
        for (const json& p : root->value("projects", json::array())) {
            RecentProject r{.file       = PathFromUtf8(p.at("file").get<std::string>()),
                            .name       = p.value("name", std::string()),
                            .lastOpened = p.value("lastOpened", std::int64_t{0})};
            if (fs::exists(r.file, ec))
                projects.push_back(std::move(r));
        }
    } catch (const json::exception&) { // broken list: start over
        projects.clear();
    }
    std::ranges::sort(projects, std::greater{}, &RecentProject::lastOpened);
    return projects;
}

void AddRecentProject(const Project& project, const fs::path& list)
{
    std::vector<RecentProject> projects = LoadRecentProjects(list);
    std::erase_if(projects, [&](const RecentProject& p) { return SameFile(p.file, project.File()); });
    projects.insert(projects.begin(), {project.File(), project.settings.name, Now()});
    if (projects.size() > 20)
        projects.resize(20);
    SaveRecent(projects, list.empty() ? DefaultRecentList() : list);
}

void RemoveRecentProject(const fs::path& file, const fs::path& list)
{
    std::vector<RecentProject> projects = LoadRecentProjects(list);
    std::erase_if(projects, [&](const RecentProject& p) { return SameFile(p.file, file); });
    SaveRecent(projects, list.empty() ? DefaultRecentList() : list);
}

// --- Packaging ----------------------------------------------------------------------------------

bool PackageProject(const Project& project, const fs::path& playerExecutable, const fs::path& outputDirectory,
                    std::string* error, CookReport* reportOut)
{
    std::error_code ec;
    if (!fs::exists(playerExecutable, ec)) {
        SetError(error, "player executable not found: '" + PathToUtf8(playerExecutable) + "'");
        return false;
    }
    // The output's Content/ is removed below: never the project's own folder (or one inside Content/).
    {
        const fs::path out     = fs::weakly_canonical(fs::absolute(outputDirectory), ec);
        const fs::path root    = fs::weakly_canonical(fs::absolute(project.Root()), ec);
        const fs::path content = fs::weakly_canonical(fs::absolute(project.ContentDirectory()), ec);
        const fs::path inside  = out.lexically_relative(content);
        if (out == root || (!inside.empty() && *inside.begin() != "..")) {
            SetError(error, "the output directory '" + PathToUtf8(outputDirectory) +
                                "' is the project folder or inside Content/; choose another folder");
            return false;
        }
        ec.clear();
    }
    fs::create_directories(outputDirectory / "shaders", ec);
    if (ec) {
        SetError(error, "cannot create '" + PathToUtf8(outputDirectory) + "': " + ec.message());
        return false;
    }
    const auto fail = [&](const std::string& what) {
        SetError(error, what + ": " + ec.message());
        return false;
    };
    // The game: player renamed after the project, finds the project file next to it.
    const fs::path game = outputDirectory / (PathFromUtf8(project.settings.name).string() + playerExecutable.extension().string());
    fs::copy_file(playerExecutable, game, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return fail("copying the player");
    for (const fs::directory_entry& e : fs::directory_iterator(ShaderDirectory(), ec))
        if (e.path().extension() == ".spv")
            fs::copy_file(e.path(), outputDirectory / "shaders" / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    if (ec)
        return fail("copying the shaders");
    fs::copy_file(project.File(), outputDirectory / project.File().filename(), fs::copy_options::overwrite_existing, ec);
    if (ec)
        return fail("copying the project file");
    fs::remove_all(outputDirectory / "Content", ec); // loose content of older packages
    ec.clear();

    // The content: cooked into one pak (+ a build report next to it).
    CookOptions options;
    options.textures.cacheDirectory = project.SavedDirectory() / "Cache" / "Textures";
    const CookReport report = CookProjectContent(project, outputDirectory / "Content.upak", options);
    {
        std::ofstream text(outputDirectory / "BuildReport.txt", std::ios::binary | std::ios::trunc);
        text << report.Text();
    }
    if (reportOut)
        *reportOut = report;
    if (!report.Ok()) {
        SetError(error, std::format("cooking failed with {} error(s), first: {} (see BuildReport.txt)", report.errors.size(),
                                    report.errors.front()));
        return false;
    }
    for (const std::string& file : {project.settings.startScene, project.settings.loadingScreen})
        if (!file.empty() && !std::ranges::any_of(report.entries, [&](const CookReport::Entry& e) { return e.path == file; })) {
            SetError(error, "'" + file + "' (project settings) is not in Content/ and was not packaged");
            return false;
        }
    ENGINE_INFO("Packaged '{}' into '{}': {} raw files, {} cooked models, {} cooked textures, {:.1f} MB pak",
                project.settings.name, PathToUtf8(outputDirectory), report.rawFiles, report.cookedModels,
                report.cookedTextures, report.pakBytes / 1048576.0);
    return true;
}

} // namespace Engine
