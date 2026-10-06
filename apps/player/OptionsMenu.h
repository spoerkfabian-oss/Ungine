#pragma once
// The player's Options page (pause menu): Graphics / Audio / Controls tabs on the runtime UI.
// Changes apply at once (GameOptions::Changed); Back saves them for the next start.
#include "Engine/Core/GameOptions.h"
#include "Engine/Core/Input.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/UI/UiLayout.h"

#include <array>
#include <format>
#include <string>
#include <vector>

namespace Engine {

class OptionsMenu {
public:
    static constexpr std::size_t kRowsPerPage = 5;

    // Builds the hidden page below `canvas` (design size 1280 x 720).
    void Create(Scene& scene, Entity canvas)
    {
        m_Panel = Add(scene, "Options Panel", canvas,
                      {.type = UiWidgetType::Panel, .anchorMin = {0.5f, 0.5f}, .anchorMax = {0.5f, 0.5f},
                       .offsetMax = {640.0f, 540.0f}, .pivot = {0.5f, 0.5f}, .background = {0.035f, 0.045f, 0.07f, 0.97f},
                       .visible = false});
        Add(scene, "Options", m_Panel,
            {.type = UiWidgetType::Text, .offsetMin = {20.0f, 16.0f}, .offsetMax = {620.0f, 60.0f},
             .color = {1.0f, 0.84f, 0.36f, 1.0f}, .text = "OPTIONS", .fontSize = 32.0f});
        const char* tabNames[] = {"Graphics", "Audio", "Controls"};
        for (std::size_t i = 0; i < 3; ++i) {
            const float x = 20.0f + 205.0f * static_cast<float>(i);
            m_Tabs[i]  = Button(scene, m_Panel, tabNames[i], {x, 70.0f}, {x + 190.0f, 112.0f});
            m_Pages[i] = Add(scene, std::string(tabNames[i]) + " Page", m_Panel,
                             {.type = UiWidgetType::Panel, .offsetMin = {20.0f, 128.0f}, .offsetMax = {620.0f, 460.0f},
                              .background = {0.0f, 0.0f, 0.0f, 0.0f}, .visible = i == 0});
        }
        // Graphics.
        m_Fullscreen = Check(scene, m_Pages[0], "Fullscreen", 0.0f);
        m_VSync      = Check(scene, m_Pages[0], "VSync", 54.0f);
        m_Shadows    = Button(scene, m_Pages[0], "Shadows", {0.0f, 108.0f}, {600.0f, 152.0f});
        m_Ao         = Check(scene, m_Pages[0], "Ambient occlusion", 162.0f);
        m_Bloom      = Check(scene, m_Pages[0], "Bloom", 216.0f);
        // Audio: one slider per mixer bus.
        for (std::size_t i = 0; i < kAudioBusCount; ++i) {
            const float y = 60.0f * static_cast<float>(i);
            m_Volumes[i]  = Add(scene, std::string("Volume ") + ToString(static_cast<AudioBus>(i)), m_Pages[1],
                                {.type = UiWidgetType::Slider, .offsetMin = {0.0f, y}, .offsetMax = {600.0f, y + 50.0f},
                                 .color = {0.25f, 0.75f, 1.0f, 1.0f}, .text = BusLabel(static_cast<AudioBus>(i)), .fontSize = 20.0f});
        }
        // Controls: a page of rows (label + key button), a hint line, paging.
        for (std::size_t i = 0; i < kRowsPerPage; ++i) {
            const float y = 44.0f * static_cast<float>(i);
            m_RowLabels[i] = Add(scene, std::format("Binding {}", i), m_Pages[2],
                                 {.type = UiWidgetType::Text, .offsetMin = {0.0f, y + 6.0f}, .offsetMax = {290.0f, y + 40.0f},
                                  .color = {0.9f, 0.92f, 0.96f, 1.0f}, .fontSize = 20.0f});
            m_RowKeys[i] = Button(scene, m_Pages[2], "Key", {300.0f, y}, {600.0f, y + 38.0f});
        }
        m_Hint = Add(scene, "Controls Hint", m_Pages[2],
                     {.type = UiWidgetType::Text, .offsetMin = {0.0f, 226.0f}, .offsetMax = {600.0f, 256.0f},
                      .color = {1.0f, 0.75f, 0.3f, 1.0f}, .fontSize = 18.0f});
        m_Prev     = Button(scene, m_Pages[2], "<", {0.0f, 270.0f}, {90.0f, 312.0f});
        m_PageText = Add(scene, "Controls Page", m_Pages[2],
                         {.type = UiWidgetType::Text, .offsetMin = {100.0f, 276.0f}, .offsetMax = {300.0f, 312.0f},
                          .color = {0.8f, 0.82f, 0.86f, 1.0f}, .fontSize = 18.0f});
        m_Next     = Button(scene, m_Pages[2], ">", {310.0f, 270.0f}, {400.0f, 312.0f});
        m_ResetKeys = Button(scene, m_Pages[2], "Reset keys", {410.0f, 270.0f}, {600.0f, 312.0f});
        // Bottom.
        m_Reset = Button(scene, m_Panel, "Reset all", {20.0f, 478.0f}, {215.0f, 524.0f});
        m_Back  = Button(scene, m_Panel, "Back", {430.0f, 478.0f}, {620.0f, 524.0f});
    }

    [[nodiscard]] Entity Panel() const { return m_Panel; }
    [[nodiscard]] bool   Capturing() const { return m_Capture != kNone; }

    void Show(Scene& scene, const GameOptions& options, bool visible)
    {
        if (Registry& r = scene.GetRegistry(); r.Valid(m_Panel))
            r.Get<UiWidget>(m_Panel).visible = visible;
        m_Capture = kNone;
        m_HintText.clear();
        if (visible)
            Refresh(scene, options);
    }

    // A UI event of the menu: true when it was one of this page's widgets. `closed`: Back pressed
    // (the options are saved then).
    bool Handle(Scene& scene, GameOptions& options, const UiEvent& event, bool& closed)
    {
        closed = false;
        UserSettings& u       = options.Edit();
        bool          changed = true;
        if (const auto tab = std::ranges::find(m_Tabs, event.entity); tab != m_Tabs.end()) {
            m_Tab     = static_cast<std::size_t>(tab - m_Tabs.begin());
            m_Capture = kNone;
            changed   = false;
        } else if (event.entity == m_Fullscreen && event.type == UiEventType::CheckedChanged) {
            u.fullscreen = event.checked;
        } else if (event.entity == m_VSync && event.type == UiEventType::CheckedChanged) {
            u.vsync = event.checked;
        } else if (event.entity == m_Shadows && event.type == UiEventType::Clicked) {
            u.shadowQuality = (u.shadowQuality + 1) % 4;
        } else if (event.entity == m_Ao && event.type == UiEventType::CheckedChanged) {
            u.ambientOcclusion = event.checked;
        } else if (event.entity == m_Bloom && event.type == UiEventType::CheckedChanged) {
            u.bloom = event.checked;
        } else if (const auto slider = std::ranges::find(m_Volumes, event.entity);
                   slider != m_Volumes.end() && event.type == UiEventType::ValueChanged) {
            u.audio.volume[static_cast<std::size_t>(slider - m_Volumes.begin())] = event.value;
        } else if (const auto key = std::ranges::find(m_RowKeys, event.entity); key != m_RowKeys.end()) {
            const std::size_t row = m_Page * kRowsPerPage + static_cast<std::size_t>(key - m_RowKeys.begin());
            m_Capture  = row < Rows(options).size() ? row : kNone;
            m_HintText = m_Capture != kNone ? "Press a key (Escape: cancel)" : std::string();
            changed    = false;
        } else if (event.entity == m_Prev || event.entity == m_Next) {
            const std::size_t pages = std::max<std::size_t>(1, (Rows(options).size() + kRowsPerPage - 1) / kRowsPerPage);
            m_Page    = event.entity == m_Next ? (m_Page + 1) % pages : (m_Page + pages - 1) % pages;
            m_Capture = kNone;
            changed   = false;
        } else if (event.entity == m_ResetKeys) {
            options.ResetInput();
            m_HintText = "Key bindings reset";
        } else if (event.entity == m_Reset) {
            options.ResetToDefaults();
            m_HintText.clear();
        } else if (event.entity == m_Back) {
            (void)options.Save();
            closed  = true;
            changed = false;
        } else {
            return false;
        }
        if (changed)
            options.Changed();
        Refresh(scene, options);
        return true;
    }

    // While remapping: the next key (or mouse button) pressed is bound; Escape cancels.
    void Update(Scene& scene, GameOptions& options, const Input& input)
    {
        if (m_Capture == kNone)
            return;
        if (QueryKey(input, "Escape", KeyQuery::Pressed)) {
            m_Capture  = kNone;
            m_HintText.clear();
            Refresh(scene, options);
            return;
        }
        for (const std::string& key : KeyNames()) {
            if (key == "Escape" || !QueryKey(input, key, KeyQuery::Pressed))
                continue;
            const std::vector<Row> rows = Rows(options);
            if (m_Capture < rows.size()) {
                const Row& row = rows[m_Capture];
                const bool ok  = row.axis ? options.RemapAxis(row.name, row.index, key, row.scale)
                                          : options.RemapAction(row.name, row.index, key);
                if (ok) {
                    options.Changed();
                    const std::vector<std::string> conflicts = options.Conflicts(key, row.name);
                    m_HintText = conflicts.empty() ? std::string() : "'" + key + "' is also used by: " + Join(conflicts);
                }
            }
            m_Capture = kNone;
            Refresh(scene, options);
            return;
        }
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    struct Row {
        std::string label, name, key;
        std::size_t index = 0;
        float       scale = 1.0f;
        bool        axis  = false;
    };

    static std::vector<Row> Rows(const GameOptions& options)
    {
        std::vector<Row> rows;
        const InputMap   map = options.Input();
        for (const InputActionBinding& a : map.actions)
            rows.push_back({a.name, a.name, a.keys.empty() ? std::string() : a.keys.front(), 0, 1.0f, false});
        for (const InputAxisBinding& a : map.axes)
            for (std::size_t i = 0; i < a.keys.size(); ++i)
                rows.push_back({std::format("{} ({:+g})", a.name, a.keys[i].scale), a.name, a.keys[i].key, i, a.keys[i].scale, true});
        return rows;
    }

    static std::string Join(const std::vector<std::string>& names)
    {
        std::string out;
        for (const std::string& n : names)
            out += (out.empty() ? "" : ", ") + n;
        return out;
    }

    static const char* BusLabel(AudioBus bus)
    {
        switch (bus) {
        case AudioBus::Master: return "Master volume";
        case AudioBus::World: return "Effects volume";
        case AudioBus::Music: return "Music volume";
        case AudioBus::Ui: return "Interface volume";
        case AudioBus::Ambient: return "Ambience volume";
        default: return "Volume";
        }
    }

    void Refresh(Scene& scene, const GameOptions& options)
    {
        Registry& r = scene.GetRegistry();
        if (!r.Valid(m_Panel))
            return;
        const UserSettings& u = options.Values();
        for (std::size_t i = 0; i < 3; ++i) {
            r.Get<UiWidget>(m_Pages[i]).visible   = i == m_Tab;
            r.Get<UiWidget>(m_Tabs[i]).background = i == m_Tab ? glm::vec4(0.25f, 0.38f, 0.58f, 1.0f) : glm::vec4(0.14f, 0.2f, 0.3f, 1.0f);
        }
        r.Get<UiWidget>(m_Fullscreen).checked = u.fullscreen;
        r.Get<UiWidget>(m_VSync).checked      = u.vsync;
        static constexpr const char* kQuality[] = {"Off", "Low", "Medium", "High"};
        r.Get<UiWidget>(m_Shadows).text       = std::string("Shadows: ") + kQuality[std::min<std::uint32_t>(u.shadowQuality, 3)];
        r.Get<UiWidget>(m_Ao).checked         = u.ambientOcclusion;
        r.Get<UiWidget>(m_Bloom).checked      = u.bloom;
        for (std::size_t i = 0; i < kAudioBusCount; ++i)
            r.Get<UiWidget>(m_Volumes[i]).value = u.audio.volume[i];

        const std::vector<Row> rows  = Rows(options);
        const std::size_t      pages = std::max<std::size_t>(1, (rows.size() + kRowsPerPage - 1) / kRowsPerPage);
        m_Page                       = std::min(m_Page, pages - 1);
        for (std::size_t i = 0; i < kRowsPerPage; ++i) {
            const std::size_t row   = m_Page * kRowsPerPage + i;
            const bool        shown = row < rows.size();
            r.Get<UiWidget>(m_RowLabels[i]).visible = shown;
            r.Get<UiWidget>(m_RowKeys[i]).visible   = shown;
            if (!shown)
                continue;
            r.Get<UiWidget>(m_RowLabels[i]).text = rows[row].label;
            r.Get<UiWidget>(m_RowKeys[i]).text   = row == m_Capture ? std::string("...")
                                                   : rows[row].key.empty() ? std::string("(none)")
                                                                           : rows[row].key;
        }
        r.Get<UiWidget>(m_PageText).text = rows.empty() ? std::string("No input actions") : std::format("Page {} / {}", m_Page + 1, pages);
        r.Get<UiWidget>(m_Prev).visible  = pages > 1;
        r.Get<UiWidget>(m_Next).visible  = pages > 1;
        r.Get<UiWidget>(m_Hint).text     = m_HintText;
    }

    static Entity Add(Scene& scene, std::string name, Entity parent, const UiWidget& widget)
    {
        const Entity e = scene.CreateEntity(std::move(name), parent);
        scene.GetRegistry().Emplace<UiWidget>(e, widget);
        return e;
    }
    static Entity Button(Scene& scene, Entity parent, const char* text, glm::vec2 min, glm::vec2 max)
    {
        return Add(scene, std::string("Options ") + text, parent,
                   {.type = UiWidgetType::Button, .offsetMin = min, .offsetMax = max, .color = {1.0f, 1.0f, 1.0f, 1.0f},
                    .background = {0.14f, 0.2f, 0.3f, 1.0f}, .text = text, .fontSize = 20.0f});
    }
    static Entity Check(Scene& scene, Entity parent, const char* text, float top)
    {
        return Add(scene, std::string("Options ") + text, parent,
                   {.type = UiWidgetType::Checkbox, .offsetMin = {0.0f, top}, .offsetMax = {600.0f, top + 44.0f},
                    .color = {1.0f, 1.0f, 1.0f, 1.0f}, .text = text, .fontSize = 20.0f});
    }

    Entity                                 m_Panel = NullEntity;
    std::array<Entity, 3>                  m_Tabs{}, m_Pages{};
    Entity                                 m_Fullscreen = NullEntity, m_VSync = NullEntity, m_Shadows = NullEntity,
                                           m_Ao = NullEntity, m_Bloom = NullEntity;
    std::array<Entity, kAudioBusCount>     m_Volumes{};
    std::array<Entity, kRowsPerPage>       m_RowLabels{}, m_RowKeys{};
    Entity                                 m_Hint = NullEntity, m_Prev = NullEntity, m_Next = NullEntity,
                                           m_PageText = NullEntity, m_ResetKeys = NullEntity, m_Reset = NullEntity,
                                           m_Back = NullEntity;
    std::size_t                            m_Tab = 0, m_Page = 0, m_Capture = kNone;
    std::string                            m_HintText;
};

} // namespace Engine
