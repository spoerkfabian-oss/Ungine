#include "Engine/Core/Platform.h"
#include "Engine/Core/Log.h"

#include <cstdlib>
#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Engine {

namespace fs = std::filesystem;

bool Log::OpenFile(const char* utf8Path)
{
    CloseFile();
#ifdef _WIN32
    g_File = _wfopen(PathFromUtf8(utf8Path).c_str(), L"w");
#else
    g_File = std::fopen(utf8Path, "w");
#endif
    return g_File != nullptr;
}

void Log::CloseFile()
{
    if (g_File)
        std::fclose(std::exchange(g_File, nullptr));
}

std::string PathToUtf8(const fs::path& path)
{
    const std::u8string s = path.u8string();
    return {s.begin(), s.end()};
}

fs::path PathFromUtf8(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::vector<std::string> CommandLineUtf8(int argc, char** argv)
{
    std::vector<std::string> args;
#ifdef _WIN32
    int       count = 0;
    wchar_t** wide  = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide) {
        for (int i = 0; i < count; ++i)
            args.push_back(PathToUtf8(fs::path(wide[i])));
        LocalFree(wide);
        return args;
    }
#endif
    for (int i = 0; i < argc; ++i)
        args.emplace_back(argv[i]);
    return args;
}

fs::path ExecutablePath()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0)
            return {};
        if (n < buffer.size()) {
            buffer.resize(n);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code ec;
    const fs::path  exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : exe;
#endif
}

fs::path ExecutableDirectory() { return ExecutablePath().parent_path(); }

fs::path HomeDirectory()
{
#ifdef _WIN32
    const wchar_t* home = _wgetenv(L"USERPROFILE"); // wide: non-ASCII user names
#else
    const char* home = std::getenv("HOME");
#endif
    return home && *home ? fs::path(home) : fs::path();
}

fs::path UserConfigDirectory()
{
    fs::path dir;
#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"APPDATA"); appData && *appData)
        dir = fs::path(appData) / "Ungine";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        dir = fs::path(xdg) / "ungine";
    else if (const char* home = std::getenv("HOME"); home && *home)
        dir = fs::path(home) / ".config" / "ungine";
#endif
    if (dir.empty())
        dir = fs::temp_directory_path() / "ungine";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

bool LaunchProcess(const fs::path& executable, const std::vector<std::string>& arguments, const fs::path& workingDirectory)
{
#ifdef _WIN32
    // Command line with quoted arguments (backslashes before quotes doubled, per the MSVC CRT rules).
    const auto quote = [](const std::wstring& arg) {
        std::wstring out = L"\"";
        std::size_t  slashes = 0;
        for (wchar_t c : arg) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, L'\\');
        return out + L"\"";
    };
    std::wstring commandLine = quote(executable.wstring());
    for (const std::string& arg : arguments)
        commandLine += L" " + quote(PathFromUtf8(arg).wstring());
    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    const std::wstring dir = workingDirectory.wstring();
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &startup, &process)) {
        ENGINE_ERROR("Cannot start '{}' (error {})", PathToUtf8(executable), GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    // Children that exited since the last launch are reaped (only ours, no zombies pile up).
    static std::vector<pid_t> children;
    std::erase_if(children, [](pid_t child) { return waitpid(child, nullptr, WNOHANG) == child; });

    std::vector<std::string> args{executable.string()};
    args.insert(args.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    if (!workingDirectory.empty())
        posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.c_str());
#endif
    pid_t     pid    = 0;
    const int result = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        ENGINE_ERROR("Cannot start '{}' (error {})", executable.string(), result);
        return false;
    }
    children.push_back(pid);
    return true; // not waited for: it may outlive us (reaped by init if we exit first)
#endif
}

bool OpenInFileBrowser(const fs::path& directory)
{
#ifdef _WIN32
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    return LaunchProcess("/usr/bin/xdg-open", {directory.string()});
#endif
}

fs::path SiblingExecutable(const std::string& name)
{
#ifdef _WIN32
    return ExecutableDirectory() / (name + ".exe");
#else
    return ExecutableDirectory() / name;
#endif
}

const fs::path& ShaderDirectory()
{
    static const fs::path directory = [] {
        std::error_code ec;
        if (const char* env = std::getenv("UNGINE_SHADER_DIR"); env && *env)
            return fs::path(env);
        const fs::path installed = ExecutableDirectory() / "shaders";
        if (fs::exists(installed / "mesh.vert.spv", ec))
            return installed;
        return fs::path(ENGINE_SHADER_DIR);
    }();
    static const bool logged = [] {
        ENGINE_INFO("Shaders: {}", PathToUtf8(directory));
        return true;
    }();
    (void)logged;
    return directory;
}

fs::path TemplateDirectory()
{
    std::error_code ec;
    const fs::path  installed = ExecutableDirectory() / "templates";
    if (fs::is_directory(installed, ec))
        return installed;
    return fs::path(ENGINE_TEMPLATE_DIR);
}

} // namespace Engine
