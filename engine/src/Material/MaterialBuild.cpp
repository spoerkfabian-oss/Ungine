#include "MaterialBuild.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Renderer/ShaderCompiler.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace Engine {

namespace {
constexpr std::string_view kShaderCacheVersion = "umat-shaders-2";
constexpr int              kMaxChainDepth      = 16;

std::uint64_t Hash(std::string_view text, std::uint64_t hash)
{
    return Fnv1a64(std::as_bytes(std::span(text.data(), text.size())), hash);
}

fs::path CacheFile(const fs::path& directory, std::uint64_t key, MaterialShader shader)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx.%s.spv", static_cast<unsigned long long>(key), ToString(shader));
    return directory / name;
}

bool ReadSpirv(const fs::path& file, std::vector<std::uint32_t>& out)
{
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const std::streamsize size = in.tellg();
    if (size <= 0 || size % 4 != 0)
        return false;
    out.resize(static_cast<std::size_t>(size) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in) && out.front() == 0x07230203u; // SPIR-V magic
}

void WriteSpirv(const fs::path& file, const std::vector<std::uint32_t>& spirv)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
        if (!out)
            return;
    }
    fs::rename(temp, file, ec);
    if (ec)
        fs::remove(temp, ec);
}
} // namespace

std::uint32_t MaterialAsset::ParameterSlot(std::string_view name) const
{
    for (std::uint32_t i = 0; i < slots.size(); ++i)
        if (slots[i].kind == MaterialSlotKind::Parameter && slots[i].name == name &&
            slots[i].type != MaterialParameterType::Texture)
            return i;
    return ~0u;
}

std::shared_ptr<const MaterialShaders> MaterialShaderCache::Find(std::uint64_t key)
{
    std::scoped_lock lock{m_Mutex};
    const auto it = m_Shaders.find(key);
    if (it == m_Shaders.end())
        return nullptr;
    std::shared_ptr<const MaterialShaders> shaders = it->second.lock();
    if (!shaders)
        m_Shaders.erase(it);
    return shaders;
}

void MaterialShaderCache::Store(const std::shared_ptr<const MaterialShaders>& shaders)
{
    std::scoped_lock lock{m_Mutex};
    m_Shaders[shaders->key] = shaders;
}

fs::path NormalizeMaterialPath(const fs::path& path)
{
    std::error_code ec;
    const fs::path  absolute = fs::absolute(path, ec);
    const fs::path  base     = ec ? path : absolute;
    fs::path        result   = fs::weakly_canonical(base, ec);
    return ec ? base.lexically_normal() : result;
}

std::string MaterialPathKey(const fs::path& normalized)
{
    const std::u8string generic = normalized.generic_u8string();
    std::string         key(generic.begin(), generic.end());
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

MaterialBuildOutput BuildMaterial(const MaterialBuildInput& input, MaterialShaderCache& cache)
{
    MaterialBuildOutput out;
    // Instance chain: leaf first; overrides apply from the graph outwards (the leaf wins).
    std::vector<std::vector<MaterialParameterValue>> overrides;
    std::optional<MaterialGraph> graph;
    fs::path                     file = input.file;
    for (int depth = 0;; ++depth) {
        if (depth >= kMaxChainDepth) {
            out.error = "material instance chain is too deep (cycle?)";
            return out;
        }
        if (std::find(out.dependencies.begin(), out.dependencies.end(), file) != out.dependencies.end()) {
            out.error = "material instance cycle at '" + PathToUtf8(file) + "'";
            return out;
        }
        out.dependencies.push_back(file);
        const std::string key = MaterialPathKey(file);
        std::string       error;
        if (IsMaterialInstanceFile(file)) {
            std::optional<MaterialInstanceFile> instance;
            if (const auto it = input.providedInstances.find(key); it != input.providedInstances.end())
                instance = it->second;
            else
                instance = LoadMaterialInstance(file, &error);
            if (!instance) {
                out.error = error;
                return out;
            }
            if (instance->parent.empty()) {
                out.error = "'" + PathToUtf8(file) + "' has no parent material";
                return out;
            }
            overrides.push_back(std::move(instance->overrides));
            file = NormalizeMaterialPath(PathFromUtf8(instance->parent));
            continue;
        }
        if (const auto it = input.providedGraphs.find(key); it != input.providedGraphs.end())
            graph = it->second;
        else
            graph = LoadMaterialGraph(file, &error);
        if (!graph) {
            out.error = error.empty() ? "cannot read '" + PathToUtf8(file) + "'" : error;
            return out;
        }
        break;
    }

    const MaterialCode code = GenerateMaterialCode(*graph);
    if (!code.Ok()) {
        out.error = "'" + PathToUtf8(file) + "':";
        for (const MaterialProblem& p : code.problems)
            if (p.error)
                out.error += (p.node ? " node " + std::to_string(p.node) + " " : std::string(" ")) + p.message + ";";
        return out;
    }

    auto material       = std::make_unique<MaterialAsset>();
    material->graphFile = PathToUtf8(file);
    material->settings  = graph->settings;
    material->slots     = code.slots;
    std::vector<MaterialParameterValue> combined;
    for (auto it = overrides.rbegin(); it != overrides.rend(); ++it)
        combined.insert(combined.end(), it->begin(), it->end());
    material->parameters = ResolveMaterialParameters(*graph, combined);
    for (const MaterialProblem& p : code.problems)
        if (!p.error)
            material->warnings.push_back(p);
    // Slots: parameter textures follow the resolved parameters.
    for (MaterialSlot& slot : material->slots)
        if (slot.kind == MaterialSlotKind::Parameter)
            for (const MaterialParameter& p : material->parameters)
                if (p.name == slot.name) {
                    slot.texture     = p.texture;
                    slot.textureKind = p.textureKind;
                }
    material->block.assign(material->slots.size(), glm::vec4(0.0f));
    for (std::size_t i = 0; i < material->slots.size(); ++i) {
        const MaterialSlot& slot = material->slots[i];
        if (slot.kind == MaterialSlotKind::AlphaCutoff)
            material->block[i] = glm::vec4(graph->settings.alphaCutoff);
        else if (slot.kind == MaterialSlotKind::Parameter && slot.type != MaterialParameterType::Texture)
            for (const MaterialParameter& p : material->parameters)
                if (p.name == slot.name)
                    material->block[i] = slot.type == MaterialParameterType::Scalar ? glm::vec4(p.value.x, 0.0f, 0.0f, 0.0f)
                                                                                    : p.value;
    }

    // Shaders: memory cache, disk cache, compile.
    const std::vector<MaterialShader> variants = MaterialShaderVariants(graph->settings);
    const auto                        defines  = MaterialShaderDefines(graph->settings, code);
    std::uint64_t key = Hash(kShaderCacheVersion, 0xcbf29ce484222325ull);
    key               = Hash(code.glsl, key);
    for (const auto& [name, value] : defines)
        key = Hash(name + "=" + value + ";", key);
    for (const MaterialShader v : variants)
        key = Hash(ToString(v), key);
    const std::uint64_t sources = ShaderSourceHash();
    key = Fnv1a64(std::as_bytes(std::span(&sources, 1)), key);

    if (std::shared_ptr<const MaterialShaders> shared = cache.Find(key)) {
        material->shaders        = std::move(shared);
        material->shaderCacheHit = true;
        out.material             = std::move(material);
        return out;
    }
    auto shaders = std::make_shared<MaterialShaders>();
    shaders->key = key;
    bool fromDisk = !input.shaderCacheDirectory.empty();
    for (const MaterialShader v : variants) {
        std::vector<std::uint32_t>& spirv = shaders->spirv[static_cast<std::size_t>(v)];
        if (fromDisk && ReadSpirv(CacheFile(input.shaderCacheDirectory, key, v), spirv))
            continue;
        fromDisk = false;
        ShaderCompileOptions options;
        options.defines = defines;
        options.virtualFiles.push_back({"material.generated.glsl", code.glsl});
        ShaderCompileResult result = CompileEngineShader(
            MaterialShaderIsVertex(v) ? ShaderStage::Vertex : ShaderStage::Fragment, MaterialShaderTemplate(v), options);
        if (!result.Ok()) {
            out.error = "'" + PathToUtf8(file) + "': " + ToString(v) + " shader: " + result.log;
            return out;
        }
        spirv = std::move(result.spirv);
        if (!input.shaderCacheDirectory.empty())
            WriteSpirv(CacheFile(input.shaderCacheDirectory, key, v), spirv);
    }
    material->shaderCacheHit = fromDisk;
    cache.Store(shaders);
    material->shaders = std::move(shaders);
    out.material      = std::move(material);
    return out;
}

} // namespace Engine
