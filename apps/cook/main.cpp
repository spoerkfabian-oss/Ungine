// UngineCook: packages a project from the command line (CI, build scripts, smoke tests).
//   UngineCook <project.ungineproj> <outputDirectory>             player + shaders + Content.upak
//   UngineCook <project.ungineproj> <outputDirectory> --pak-only  only Content.upak + BuildReport.txt
//   UngineCook <project.ungineproj> <outputDirectory> --patch     Patch_<n>.upak against the game there
//   --key <file>  the pak signing key (projects with a pakPublicKey; default: the user's key store)
// Exit code 0: packaged; 1: bad arguments or project; 2: cooking / packaging failed.
#include "Engine/Assets/ContentCooker.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    const std::vector<std::string> args = CommandLineUtf8(argc, argv);
    std::vector<std::string>       positional;
    bool                           pakOnly = false, patch = false;
    PackageOptions                 packaging;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--pak-only")
            pakOnly = true;
        else if (args[i] == "--patch")
            patch = true;
        else if (args[i] == "--key" && i + 1 < args.size())
            packaging.keyFile = PathFromUtf8(args[++i]);
        else
            positional.push_back(args[i]);
    }
    if (positional.size() != 2) {
        std::fprintf(stderr, "usage: UngineCook <project.ungineproj> <outputDirectory> [--pak-only | --patch] [--key <file>]\n");
        return 1;
    }
    std::string                  error;
    const std::optional<Project> project = Project::Load(PathFromUtf8(positional[0]), &error);
    if (!project) {
        ENGINE_ERROR("Cannot load the project: {}", error);
        return 1;
    }
    const fs::path out = PathFromUtf8(positional[1]);
    CookReport     report;
    if (patch) {
        fs::path file;
        if (!PackagePatch(*project, out, &error, &report, &packaging, &file)) {
            ENGINE_ERROR("Patching '{}' failed: {}", project->settings.name, error);
            return 2;
        }
        if (file.empty())
            ENGINE_INFO("'{}': no changes since the last build, no patch written", project->settings.name);
        return 0;
    }
    if (pakOnly) {
        CookOptions options;
        options.textures.cacheDirectory = project->SavedDirectory() / "Cache" / "Textures";
        if (!project->settings.pakPublicKey.empty()) {
            options.signingKey = LoadPakSigningKey(project->settings, packaging.keyFile, &error);
            if (!options.signingKey) {
                ENGINE_ERROR("Packaging '{}' failed: {}", project->settings.name, error);
                return 2;
            }
        }
        report = CookProjectContent(*project, out / "Content.upak", options);
        std::ofstream(out / "BuildReport.txt", std::ios::binary | std::ios::trunc) << report.Text();
        if (!report.Ok())
            error = report.errors.front();
    } else if (!PackageProject(*project, SiblingExecutable("UnginePlayer"), out, &error, &report, &packaging)) {
        report.errors.push_back(error);
    }
    for (const std::string& w : report.warnings)
        ENGINE_WARN("Cook: {}", w);
    if (!report.Ok() || !error.empty()) {
        ENGINE_ERROR("Packaging '{}' failed: {}", project->settings.name, error);
        return 2;
    }
    ENGINE_INFO("Cooked '{}': {} raw files, {} models, {} textures, {:.2f} MB pak in {:.1f} s", project->settings.name,
                report.rawFiles, report.cookedModels, report.cookedTextures, report.pakBytes / 1048576.0, report.seconds);
    return 0;
}
