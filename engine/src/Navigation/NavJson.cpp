#include "Navigation/NavJson.h"

namespace Engine {

using json = nlohmann::json;

json NavSettingsToJson(const NavSettings& s)
{
    json profiles = json::array(), areas = json::array();
    for (const NavAgentProfile& p : s.profiles)
        profiles.push_back({{"name", p.name},
                            {"radius", p.radius},
                            {"height", p.height},
                            {"maxClimb", p.maxClimb},
                            {"maxSlope", p.maxSlope}});
    for (const NavAreaType& a : s.areas)
        areas.push_back({{"name", a.name}, {"cost", a.cost}, {"color", {a.color.r, a.color.g, a.color.b}}});
    return {{"cellSize", s.cellSize},
            {"cellHeight", s.cellHeight},
            {"tileSize", s.tileSize},
            {"edgeMaxLength", s.edgeMaxLength},
            {"edgeMaxError", s.edgeMaxError},
            {"regionMinSize", s.regionMinSize},
            {"regionMergeSize", s.regionMergeSize},
            {"maxTiles", s.maxTiles},
            {"maxObstacles", s.maxObstacles},
            {"maxAgents", s.maxAgents},
            {"profiles", std::move(profiles)},
            {"areas", std::move(areas)}};
}

void NavSettingsFromJson(const json& j, NavSettings& s)
{
    if (!j.is_object())
        return;
    s.cellSize        = j.value("cellSize", s.cellSize);
    s.cellHeight      = j.value("cellHeight", s.cellHeight);
    s.tileSize        = j.value("tileSize", s.tileSize);
    s.edgeMaxLength   = j.value("edgeMaxLength", s.edgeMaxLength);
    s.edgeMaxError    = j.value("edgeMaxError", s.edgeMaxError);
    s.regionMinSize   = j.value("regionMinSize", s.regionMinSize);
    s.regionMergeSize = j.value("regionMergeSize", s.regionMergeSize);
    s.maxTiles        = j.value("maxTiles", s.maxTiles);
    s.maxObstacles    = j.value("maxObstacles", s.maxObstacles);
    s.maxAgents       = j.value("maxAgents", s.maxAgents);
    if (const auto p = j.find("profiles"); p != j.end() && p->is_array()) {
        s.profiles.clear();
        for (const json& e : *p) {
            NavAgentProfile profile;
            profile.name     = e.value("name", profile.name);
            profile.radius   = e.value("radius", profile.radius);
            profile.height   = e.value("height", profile.height);
            profile.maxClimb = e.value("maxClimb", profile.maxClimb);
            profile.maxSlope = e.value("maxSlope", profile.maxSlope);
            s.profiles.push_back(std::move(profile));
        }
    }
    if (const auto a = j.find("areas"); a != j.end() && a->is_array()) {
        s.areas.clear();
        for (const json& e : *a) {
            NavAreaType area;
            area.name = e.value("name", area.name);
            area.cost = e.value("cost", area.cost);
            if (const auto c = e.find("color"); c != e.end() && c->is_array() && c->size() == 3)
                area.color = {(*c)[0].get<float>(), (*c)[1].get<float>(), (*c)[2].get<float>()};
            s.areas.push_back(std::move(area));
        }
    }
    SanitizeNavSettings(s);
}

} // namespace Engine
