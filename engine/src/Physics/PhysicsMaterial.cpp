#include "Engine/Physics/PhysicsMaterial.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

std::optional<PhysicsMaterialData> LoadPhysicsMaterial(const fs::path& file, std::string* error)
{
    const auto fail = [&](const std::string& what) -> std::optional<PhysicsMaterialData> {
        if (error)
            *error = "'" + PathToUtf8(file) + "': " + what;
        return std::nullopt;
    };
    const std::optional<std::string> text = Vfs::ReadText(file);
    if (!text)
        return fail("cannot read");
    try {
        const json          root = json::parse(*text);
        PhysicsMaterialData m;
        m.friction    = root.value("friction", m.friction);
        m.restitution = root.value("restitution", m.restitution);
        m.surface     = root.value("surface", m.surface);
        if (!std::isfinite(m.friction) || !std::isfinite(m.restitution))
            return fail("friction / restitution are not finite");
        m.friction    = std::max(m.friction, 0.0f);
        m.restitution = std::clamp(m.restitution, 0.0f, 1.0f);
        return m;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

bool SavePhysicsMaterial(const fs::path& file, const PhysicsMaterialData& m, std::string* error)
{
    const json root{{"version", 1}, {"friction", m.friction}, {"restitution", m.restitution}, {"surface", m.surface}};
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << root.dump(2) << '\n';
        if (!out) {
            if (error)
                *error = "cannot write '" + PathToUtf8(temp) + "'";
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec && error)
        *error = "cannot write '" + PathToUtf8(file) + "': " + ec.message();
    return !ec;
}

} // namespace Engine
