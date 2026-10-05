# ImGui editor on top of the engine. The engine itself stays free of ImGui.
add_library(Editor STATIC
    src/ContentBrowser.cpp
    src/Editor.cpp
    src/EditorCommands.cpp
    src/FileDialog.cpp
    src/History.cpp
    src/ImGuiLayer.cpp
    src/Panels.cpp
    src/ProjectLauncher.cpp
    src/ScriptGraphEditor.cpp)
target_include_directories(Editor PUBLIC include PRIVATE src)
target_link_libraries(Editor PUBLIC Engine PRIVATE imgui imguizmo nlohmann_json::nlohmann_json)
engine_set_warnings(Editor)
set_target_properties(Editor PROPERTIES FOLDER "Editor")
