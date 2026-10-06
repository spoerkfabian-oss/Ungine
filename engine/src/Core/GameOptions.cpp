#include "Engine/Core/GameOptions.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

UserSettings DefaultsFrom(const ProjectSettings& project)
{
    UserSettings u;
    u.fullscreen = project.fullscreen;
    u.vsync      = project.vsync;
    u.audio      = project.audio;
    return u;
}

template <class T>
T* FindByName(std::vector<T>& list, std::string_view name)
{
    const auto it = std::ranges::find(list, name, &T::name);
    return it != list.end() ? &*it : nullptr;
}

} // namespace

GameOptions::GameOptions(const ProjectSettings& project, fs::path file)
    : m_File(std::move(file)), m_ProjectInput(project.input), m_Defaults(DefaultsFrom(project)), m_Values(m_Defaults)
{
    std::ifstream in(m_File, std::ios::binary);
    if (!in)
        return; // first start: the defaults
    try {
        const json root = json::parse(in);
        if (const auto g = root.find("graphics"); g != root.end() && g->is_object()) {
            m_Values.fullscreen       = g->value("fullscreen", m_Values.fullscreen);
            m_Values.vsync            = g->value("vsync", m_Values.vsync);
            m_Values.windowWidth      = std::min(g->value("windowWidth", 0u), 16384u);
            m_Values.windowHeight     = std::min(g->value("windowHeight", 0u), 16384u);
            m_Values.shadowQuality    = std::min(g->value("shadowQuality", m_Values.shadowQuality), 3u);
            m_Values.ambientOcclusion = g->value("ambientOcclusion", m_Values.ambientOcclusion);
            m_Values.bloom            = g->value("bloom", m_Values.bloom);
        }
        if (const auto a = root.find("audio"); a != root.end() && a->is_object())
            for (std::size_t i = 0; i < kAudioBusCount; ++i)
                if (const auto b = a->find(ToString(static_cast<AudioBus>(i))); b != a->end() && b->is_object()) {
                    m_Values.audio.volume[i] = std::clamp(b->value("volume", m_Values.audio.volume[i]), 0.0f, 4.0f);
                    m_Values.audio.muted[i]  = b->value("muted", m_Values.audio.muted[i]);
                }
        if (const auto input = root.find("input"); input != root.end() && input->is_object()) {
            for (const json& a : input->value("actions", json::array()))
                if (a.is_object() && m_ProjectInput.FindAction(a.value("name", std::string())))
                    m_Values.actions.push_back({a.at("name").get<std::string>(), a.value("keys", std::vector<std::string>{})});
            for (const json& a : input->value("axes", json::array()))
                if (a.is_object() && m_ProjectInput.FindAxis(a.value("name", std::string()))) {
                    InputAxisBinding axis{a.at("name").get<std::string>(), {}};
                    for (const json& k : a.value("keys", json::array()))
                        if (k.is_object())
                            axis.keys.push_back({k.value("key", std::string()), k.value("scale", 1.0f)});
                    m_Values.axes.push_back(std::move(axis));
                }
        }
    } catch (const std::exception& e) {
        ENGINE_WARN("Options '{}' ignored: {}", PathToUtf8(m_File), e.what());
        m_Values = m_Defaults;
    }
}

void GameOptions::Changed()
{
    if (m_Apply)
        m_Apply(*this);
}

void GameOptions::SetApply(MoveOnlyFunction<void(const GameOptions&)> apply) { m_Apply = std::move(apply); }

bool GameOptions::Save() const
{
    json buses = json::object();
    for (std::size_t i = 0; i < kAudioBusCount; ++i)
        buses[ToString(static_cast<AudioBus>(i))] = {{"volume", m_Values.audio.volume[i]}, {"muted", m_Values.audio.muted[i]}};
    json actions = json::array(), axes = json::array();
    for (const InputActionBinding& a : m_Values.actions)
        actions.push_back({{"name", a.name}, {"keys", a.keys}});
    for (const InputAxisBinding& a : m_Values.axes) {
        json keys = json::array();
        for (const InputAxisKey& k : a.keys)
            keys.push_back({{"key", k.key}, {"scale", k.scale}});
        axes.push_back({{"name", a.name}, {"keys", std::move(keys)}});
    }
    const json root{{"version", 1},
                    {"graphics",
                     {{"fullscreen", m_Values.fullscreen},
                      {"vsync", m_Values.vsync},
                      {"windowWidth", m_Values.windowWidth},
                      {"windowHeight", m_Values.windowHeight},
                      {"shadowQuality", m_Values.shadowQuality},
                      {"ambientOcclusion", m_Values.ambientOcclusion},
                      {"bloom", m_Values.bloom}}},
                    {"audio", std::move(buses)},
                    {"input", {{"actions", std::move(actions)}, {"axes", std::move(axes)}}}};
    std::error_code ec;
    if (m_File.has_parent_path())
        fs::create_directories(m_File.parent_path(), ec);
    fs::path temp = m_File;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << root.dump(2) << '\n';
        if (!out)
            return false;
    }
    fs::rename(temp, m_File, ec);
    return !ec;
}

void GameOptions::ResetToDefaults() { m_Values = m_Defaults; }

void GameOptions::ResetInput()
{
    m_Values.actions.clear();
    m_Values.axes.clear();
}

InputMap GameOptions::Input() const
{
    InputMap map = m_ProjectInput;
    for (InputActionBinding& a : map.actions)
        if (const auto it = std::ranges::find(m_Values.actions, a.name, &InputActionBinding::name); it != m_Values.actions.end())
            a.keys = it->keys;
    for (InputAxisBinding& a : map.axes)
        if (const auto it = std::ranges::find(m_Values.axes, a.name, &InputAxisBinding::name); it != m_Values.axes.end())
            a.keys = it->keys;
    return map;
}

bool GameOptions::RemapAction(std::string_view action, std::size_t index, const std::string& key)
{
    const InputActionBinding* project = m_ProjectInput.FindAction(action);
    if (!project)
        return false;
    InputActionBinding* user = FindByName(m_Values.actions, action);
    if (!user)
        user = &m_Values.actions.emplace_back(*project);
    if (key.empty()) {
        if (index < user->keys.size())
            user->keys.erase(user->keys.begin() + static_cast<std::ptrdiff_t>(index));
    } else if (index < user->keys.size()) {
        user->keys[index] = key;
    } else {
        user->keys.push_back(key);
    }
    if (user->keys == project->keys) // back to the project's: nothing to store
        std::erase_if(m_Values.actions, [&](const InputActionBinding& a) { return a.name == action; });
    return true;
}

bool GameOptions::RemapAxis(std::string_view axis, std::size_t index, const std::string& key, float scale)
{
    const InputAxisBinding* project = m_ProjectInput.FindAxis(axis);
    if (!project)
        return false;
    InputAxisBinding* user = FindByName(m_Values.axes, axis);
    if (!user)
        user = &m_Values.axes.emplace_back(*project);
    if (key.empty()) {
        if (index < user->keys.size())
            user->keys.erase(user->keys.begin() + static_cast<std::ptrdiff_t>(index));
    } else if (index < user->keys.size()) {
        user->keys[index].key = key;
    } else {
        user->keys.push_back({key, scale});
    }
    if (user->keys == project->keys)
        std::erase_if(m_Values.axes, [&](const InputAxisBinding& a) { return a.name == axis; });
    return true;
}

std::vector<std::string> GameOptions::Conflicts(std::string_view key, std::string_view except) const
{
    std::vector<std::string> names;
    if (key.empty())
        return names;
    const InputMap map = Input();
    for (const InputActionBinding& a : map.actions)
        if (a.name != except && std::ranges::find(a.keys, key) != a.keys.end())
            names.push_back(a.name);
    for (const InputAxisBinding& a : map.axes)
        if (a.name != except && std::ranges::find(a.keys, key, &InputAxisKey::key) != a.keys.end())
            names.push_back(a.name);
    return names;
}

std::uint32_t GameOptions::ShadowResolution(std::uint32_t quality)
{
    static constexpr std::uint32_t kResolution[] = {0, 1024, 2048, 4096};
    return kResolution[std::min<std::uint32_t>(quality, 3)];
}

} // namespace Engine
