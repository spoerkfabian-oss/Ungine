#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine {

struct ShaderCompilerDesc {
    std::filesystem::path compiler;     // glslc
    std::filesystem::path includeDir;   // -I
    std::filesystem::path spvDirectory; // where the engine loads SPIR-V from (ShaderPath)
    bool                  optimize = true; // -O, else -g -O0 (matches the build configuration)
};

// Defaults from the build (glslc, include and output directories); empty paths when unknown.
[[nodiscard]] ShaderCompilerDesc DefaultShaderCompiler();

// Development hot reload: watches the GLSL sources behind the engine's SPIR-V (dependencies from
// glslc's depfiles, <name>.spv.d next to each .spv), recompiles changed ones with the build's
// flags (to a temporary file, then renamed over the .spv) and counts successful rounds in
// Generation(). Owners of pipelines rebuild them when the generation changes. A shader that
// fails to compile keeps its previous SPIR-V (LastError() holds glslc's output). Main thread.
class ShaderHotReload {
public:
    explicit ShaderHotReload(ShaderCompilerDesc desc = DefaultShaderCompiler());

    // False without a compiler or depfiles (installed builds): Poll() does nothing.
    [[nodiscard]] bool Available() const { return m_Available; }
    // Watches source -> spv explicitly (tools, tests); compiles it now if the SPIR-V is missing.
    bool Watch(const std::filesystem::path& source, const std::filesystem::path& spv);
    // Checks the sources (at most every `interval`); a changed file must be unchanged since the
    // previous check (editors write in steps). True when shaders were recompiled.
    bool Poll(std::chrono::duration<double> interval = std::chrono::milliseconds(500));
    // Recompiles `spv`'s shader now, whatever its timestamps. True on success.
    bool Recompile(const std::filesystem::path& spv);

    [[nodiscard]] std::uint64_t      Generation() const { return m_Generation; }
    [[nodiscard]] const std::string& LastError() const { return m_LastError; } // empty after a clean round
    [[nodiscard]] std::size_t        WatchedShaders() const { return m_Shaders.size(); }
    [[nodiscard]] std::uint32_t      Recompiled() const { return m_Recompiled; } // shaders, all rounds
    [[nodiscard]] std::uint32_t      Failures() const { return m_Failures; }

private:
    struct Shader {
        std::filesystem::path                        spv;
        std::vector<std::filesystem::path>           files; // source first, then its includes
        std::vector<std::filesystem::file_time_type> loaded, seen;
    };
    void Track(Shader& shader); // (re)reads the depfile and the current timestamps
    [[nodiscard]] bool Compile(const Shader& shader);
    [[nodiscard]] bool CompileOrThrow(const Shader& shader);

    ShaderCompilerDesc                    m_Desc;
    std::vector<Shader>                   m_Shaders;
    std::chrono::steady_clock::time_point m_LastPoll{};
    std::uint64_t                         m_Generation = 0;
    std::uint32_t                         m_Recompiled = 0, m_Failures = 0;
    std::string                           m_LastError;
    bool                                  m_Available = false;
};

} // namespace Engine
