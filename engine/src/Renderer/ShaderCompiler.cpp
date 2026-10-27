#include "Engine/Renderer/ShaderCompiler.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace Engine {

namespace {

std::once_flag g_GlslangInit;

void InitGlslang()
{
    std::call_once(g_GlslangInit, [] {
        glslang::InitializeProcess();
        std::atexit([] { glslang::FinalizeProcess(); });
    });
}

bool ReadTextFile(const fs::path& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream text;
    text << file.rdbuf();
    out = std::move(text).str();
    return true;
}

// Resolves #include "x": virtual files, then relative to the including file, then include/ and
// the root of the shader sources.
class Includer final : public glslang::TShader::Includer {
public:
    explicit Includer(const ShaderCompileOptions& options) : m_Options(options) {}

    IncludeResult* includeLocal(const char* header, const char* includer, size_t) override
    {
        for (const ShaderSource& file : m_Options.virtualFiles)
            if (file.name == header)
                return Result(file.name, file.text);
        const fs::path base = PathFromUtf8(includer).parent_path();
        if (!base.empty())
            if (IncludeResult* r = FromFile(base / PathFromUtf8(header)))
                return r;
        return includeSystem(header, includer, 0);
    }

    IncludeResult* includeSystem(const char* header, const char*, size_t) override
    {
        const fs::path& root = ShaderSourceDirectory();
        if (IncludeResult* r = FromFile(root / "include" / PathFromUtf8(header)))
            return r;
        return FromFile(root / PathFromUtf8(header));
    }

    void releaseInclude(IncludeResult* result) override
    {
        if (!result)
            return;
        delete static_cast<std::string*>(result->userData);
        delete result;
    }

private:
    IncludeResult* FromFile(const fs::path& path)
    {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return nullptr;
        std::string text;
        if (!ReadTextFile(path, text))
            return nullptr;
        return Result(PathToUtf8(path.lexically_normal()), std::move(text));
    }
    static IncludeResult* Result(const std::string& name, std::string text)
    {
        auto* owned = new std::string(std::move(text));
        return new IncludeResult(name, owned->data(), owned->size(), owned);
    }

    const ShaderCompileOptions& m_Options;
};

EShLanguage Language(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::Vertex: return EShLangVertex;
    case ShaderStage::Fragment: return EShLangFragment;
    case ShaderStage::Compute: return EShLangCompute;
    }
    return EShLangVertex;
}

std::atomic<std::uint64_t> g_SourceHash{0};
std::mutex                 g_SourceHashMutex;

} // namespace

ShaderCompileResult CompileGlsl(ShaderStage stage, const ShaderSource& source, const ShaderCompileOptions& options)
{
    InitGlslang();
    ShaderCompileResult result;
    const EShLanguage   language = Language(stage);
    glslang::TShader    shader(language);

    // Like glslc: #include support, defines after #version.
    std::string preamble = "#extension GL_GOOGLE_include_directive : require\n";
    for (const auto& [name, value] : options.defines)
        preamble += "#define " + name + (value.empty() ? std::string{} : " " + value) + "\n";
    shader.setPreamble(preamble.c_str());

    const char* text = source.text.c_str();
    const int   length = static_cast<int>(source.text.size());
    const char* name = source.name.c_str();
    shader.setStringsWithLengthsAndNames(&text, &length, &name, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    if (options.debugInfo)
        shader.setDebugInfo(true);

    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules |
                                                   (options.debugInfo ? static_cast<int>(EShMsgDebugInfo) : 0));
    Includer includer(options);
    if (!shader.parse(GetDefaultResources(), 460, ECoreProfile, false, false, messages, includer)) {
        result.log = shader.getInfoLog();
        if (result.log.empty())
            result.log = "GLSL parse failed";
        return result;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        result.log = program.getInfoLog();
        return result;
    }
    result.log = shader.getInfoLog();

    glslang::SpvOptions spv;
    spv.generateDebugInfo = options.debugInfo;
    spv.disableOptimizer  = true;
    spv.validate          = false;
    spv::SpvBuildLogger logger;
    std::vector<unsigned int> words;
    glslang::GlslangToSpv(*program.getIntermediate(language), words, &logger, &spv);
    const std::string spvLog = logger.getAllMessages();
    if (!spvLog.empty())
        result.log += spvLog;
    result.spirv.assign(words.begin(), words.end());
    if (result.spirv.empty() && result.log.empty())
        result.log = "SPIR-V generation failed";
    return result;
}

ShaderCompileResult CompileEngineShader(ShaderStage stage, std::string_view file, const ShaderCompileOptions& options)
{
    const fs::path path = ShaderSourceDirectory() / PathFromUtf8(std::string(file));
    ShaderSource   source{PathToUtf8(path), {}};
    if (!ReadTextFile(path, source.text)) {
        ShaderCompileResult result;
        result.log = "cannot read shader source '" + source.name + "'";
        return result;
    }
    return CompileGlsl(stage, source, options);
}

const fs::path& ShaderSourceDirectory()
{
    static const fs::path directory = [] {
        if (const char* env = std::getenv("UNGINE_SHADER_SOURCE_DIR"); env && *env)
            return fs::path(env);
        std::error_code ec;
        // Development build: the source tree (follows shader hot reload).
        if (ShaderDirectory() == fs::path(ENGINE_SHADER_DIR) && fs::is_directory(fs::path(ENGINE_SHADER_SOURCE_DIR), ec))
            return fs::path(ENGINE_SHADER_SOURCE_DIR);
        const fs::path installed = ShaderDirectory() / "source";
        if (fs::is_directory(installed, ec))
            return installed;
        return fs::path(ENGINE_SHADER_SOURCE_DIR);
    }();
    return directory;
}

std::uint64_t ShaderSourceHash()
{
    if (const std::uint64_t hash = g_SourceHash.load(std::memory_order_acquire))
        return hash;
    std::lock_guard lock(g_SourceHashMutex);
    if (const std::uint64_t hash = g_SourceHash.load(std::memory_order_relaxed))
        return hash;
    std::vector<fs::path> files;
    std::error_code       ec;
    for (fs::recursive_directory_iterator it(ShaderSourceDirectory(), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const fs::path& file : files) {
        std::string name = PathToUtf8(file.lexically_relative(ShaderSourceDirectory()));
        std::replace(name.begin(), name.end(), '\\', '/');
        hash = Fnv1a64(std::as_bytes(std::span(name.data(), name.size())), hash);
        std::string text;
        if (ReadTextFile(file, text)) {
            text.erase(std::remove(text.begin(), text.end(), '\r'), text.end()); // checkout line endings
            hash = Fnv1a64(std::as_bytes(std::span(text.data(), text.size())), hash);
        }
    }
    hash = hash ? hash : 1;
    g_SourceHash.store(hash, std::memory_order_release);
    return hash;
}

void ResetShaderSourceHash()
{
    g_SourceHash.store(0, std::memory_order_release);
}

} // namespace Engine
