#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

// GLSL -> SPIR-V at runtime (glslang, in process): generated material shaders are compiled from
// the engine's mesh / shadow shader sources with defines and virtual include files. Thread-safe.

enum class ShaderStage : std::uint8_t { Vertex, Fragment, Compute };

struct ShaderSource {
    std::string name; // file name for messages and relative includes ("mesh.vert")
    std::string text;
};

struct ShaderCompileOptions {
    std::vector<std::pair<std::string, std::string>> defines; // name, value ("" = defined)
    std::vector<ShaderSource> virtualFiles; // #include "name" resolves here first
    bool debugInfo = false;                 // -g
};

struct ShaderCompileResult {
    std::vector<std::uint32_t> spirv; // empty on failure
    std::string                log;   // errors / warnings
    [[nodiscard]] bool Ok() const { return !spirv.empty(); }
};

[[nodiscard]] ShaderCompileResult CompileGlsl(ShaderStage stage, const ShaderSource& source,
                                              const ShaderCompileOptions& options = {});
// Compiles a file of the engine's shader sources (ShaderSourceDirectory()).
[[nodiscard]] ShaderCompileResult CompileEngineShader(ShaderStage stage, std::string_view file,
                                                      const ShaderCompileOptions& options = {});

// The engine's GLSL sources: env UNGINE_SHADER_SOURCE_DIR, the source tree for development builds,
// else <ShaderDirectory()>/source (installed / packaged).
[[nodiscard]] const std::filesystem::path& ShaderSourceDirectory();
// FNV-1a over every file of ShaderSourceDirectory() (cache keys of compiled material shaders).
// Computed once; ResetShaderSourceHash() after shader hot reload.
[[nodiscard]] std::uint64_t ShaderSourceHash();
void                        ResetShaderSourceHash();

} // namespace Engine
