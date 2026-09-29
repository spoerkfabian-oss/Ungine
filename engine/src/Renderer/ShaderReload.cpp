#include "Engine/Renderer/ShaderReload.h"
#include "Engine/Core/Log.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;

namespace {
fs::file_time_type Time(const fs::path& path)
{
    std::error_code ec;
    const auto      time = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : time;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string{};
}

std::string Quote(const fs::path& path)
{
    return "\"" + path.string() + "\"";
}

// Make syntax: "target: dep dep \<newline> dep", spaces in paths escaped as "\ ".
// Returns the dependencies, the source first; the target is skipped.
std::vector<fs::path> ParseDepfile(const std::string& text)
{
    std::vector<fs::path> deps;
    std::size_t start = text.find(": "); // "C:\..." targets contain ':' but never ": "
    if (start == std::string::npos)
        return deps;
    std::string token;
    const auto  flush = [&] {
        if (!token.empty())
            deps.emplace_back(token);
        token.clear();
    };
    for (std::size_t i = start + 2; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '\n' || text[i + 1] == '\r')) {
            if (text[i + 1] == ' ')
                token += ' ';
            ++i; // escaped space, or line continuation
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            token += c;
        }
    }
    flush();
    return deps;
}

std::string EscapeMake(const std::string& path)
{
    std::string out;
    for (char c : path) {
        if (c == ' ')
            out += '\\';
        out += c;
    }
    return out;
}
} // namespace

ShaderCompilerDesc DefaultShaderCompiler()
{
    ShaderCompilerDesc desc;
    desc.spvDirectory = ENGINE_SHADER_DIR;
#if defined(ENGINE_GLSLC) && defined(ENGINE_SHADER_INCLUDE_DIR)
    desc.compiler   = ENGINE_GLSLC;
    desc.includeDir = ENGINE_SHADER_INCLUDE_DIR;
    desc.optimize   = ENGINE_SHADER_OPTIMIZE != 0;
#endif
    return desc;
}

ShaderHotReload::ShaderHotReload(ShaderCompilerDesc desc) : m_Desc(std::move(desc))
{
    std::error_code ec;
    if (m_Desc.compiler.empty() || !fs::exists(m_Desc.compiler, ec))
        return;
    m_Available = true;
    for (const fs::directory_entry& entry : fs::directory_iterator(m_Desc.spvDirectory, ec)) {
        const fs::path& path = entry.path();
        if (path.extension() != ".d" || path.stem().extension() != ".spv")
            continue;
        Shader shader{.spv = path.parent_path() / path.stem(), .files = {}, .loaded = {}, .seen = {}};
        Track(shader);
        if (!shader.files.empty())
            m_Shaders.push_back(std::move(shader));
    }
    ENGINE_INFO("Shader hot reload: watching {} shaders", m_Shaders.size());
}

void ShaderHotReload::Track(Shader& shader)
{
    std::vector<fs::path> files = ParseDepfile(ReadText(fs::path(shader.spv) += ".d"));
    if (!files.empty())
        shader.files = std::move(files);
    shader.loaded.clear();
    for (const fs::path& file : shader.files)
        shader.loaded.push_back(Time(file));
    shader.seen = shader.loaded;
}

bool ShaderHotReload::Watch(const fs::path& source, const fs::path& spv)
{
    if (!m_Available)
        return false;
    Shader shader{.spv = spv, .files = {source}, .loaded = {}, .seen = {}};
    std::error_code ec;
    if (!fs::exists(spv, ec) && !Compile(shader))
        return false;
    Track(shader);
    m_Shaders.push_back(std::move(shader));
    return true;
}

bool ShaderHotReload::Compile(const Shader& shader)
{
    try {
        return CompileOrThrow(shader);
    } catch (const std::exception& e) { // e.g. a path the narrow encoding cannot represent
        m_LastError = e.what();
        ++m_Failures;
        ENGINE_ERROR("Shader hot reload: {}", m_LastError);
        return false;
    }
}

bool ShaderHotReload::CompileOrThrow(const Shader& shader)
{
    const fs::path tmp = fs::path(shader.spv) += ".tmp";
    const fs::path dep = fs::path(shader.spv) += ".d.tmp";
    const fs::path log = fs::path(shader.spv) += ".log";
    std::string command = Quote(m_Desc.compiler) + " --target-env=vulkan1.3 -Werror " +
                          (m_Desc.optimize ? "-O" : "-g -O0") + " -I " + Quote(m_Desc.includeDir) + " -MD -MF " +
                          Quote(dep) + " -o " + Quote(tmp) + " " + Quote(shader.files.front()) + " > " + Quote(log) +
                          " 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd.exe strips the outer quotes
#endif
    const int         result = std::system(command.c_str());
    const std::string output = ReadText(log);
    std::error_code   ec;
    fs::remove(log, ec);
    if (result != 0) {
        fs::remove(tmp, ec);
        fs::remove(dep, ec);
        m_LastError = shader.spv.filename().string() + ":\n" + output;
        ++m_Failures;
        ENGINE_ERROR("Shader hot reload: {} failed to compile:\n{}", shader.files.front().filename().string(), output);
        return false;
    }

    // The depfile names the temporary output: point it at the real one (ninja checks it).
    const std::string depText = ReadText(dep);
    const std::size_t colon   = depText.find(": ");
    {
        std::ofstream out(fs::path(shader.spv) += ".d", std::ios::binary | std::ios::trunc);
        out << EscapeMake(shader.spv.string()) << (colon != std::string::npos ? depText.substr(colon) : ": \n");
    }
    fs::remove(dep, ec);
    fs::rename(tmp, shader.spv, ec);
    if (ec) {
        m_LastError = "cannot replace " + shader.spv.string() + ": " + ec.message();
        ++m_Failures;
        ENGINE_ERROR("Shader hot reload: {}", m_LastError);
        return false;
    }
    ENGINE_INFO("Shader hot reload: recompiled {}", shader.files.front().filename().string());
    return true;
}

bool ShaderHotReload::Poll(std::chrono::duration<double> interval)
{
    const auto now = std::chrono::steady_clock::now();
    if (!m_Available || now - m_LastPoll < interval)
        return false;
    m_LastPoll = now;

    bool compiled = false, failed = false;
    for (Shader& shader : m_Shaders) {
        bool changed = false, stable = true;
        for (std::size_t i = 0; i < shader.files.size(); ++i) {
            const auto time = Time(shader.files[i]);
            if (time != shader.loaded[i]) {
                changed = true;
                stable  = stable && time == shader.seen[i];
            }
            shader.seen[i] = time;
        }
        if (!changed || !stable)
            continue;
        if (Compile(shader)) {
            Track(shader); // includes may have changed
            ++m_Recompiled;
            compiled = true;
        } else {
            shader.loaded = shader.seen; // retried once the source changes again
            failed        = true;
        }
    }
    if (compiled) {
        ++m_Generation;
        if (!failed)
            m_LastError.clear();
    }
    return compiled;
}

bool ShaderHotReload::Recompile(const fs::path& spv)
{
    for (Shader& shader : m_Shaders) {
        if (shader.spv.lexically_normal() != spv.lexically_normal())
            continue;
        if (!Compile(shader))
            return false;
        Track(shader);
        ++m_Recompiled;
        ++m_Generation;
        m_LastError.clear();
        return true;
    }
    return false;
}

} // namespace Engine
