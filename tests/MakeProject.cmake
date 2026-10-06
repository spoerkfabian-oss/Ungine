# cmake -DTEMPLATE=<templates/X> -DDIR=<out dir> -DNAME=<name> [-DLEVEL_SWITCH=ON] [-DMODEL=<file.glb>] -P MakeProject.cmake
# A project from a template, like Project::Create (for the app smoke tests).
# LEVEL_SWITCH (Basic template): starts in a small scene whose Blueprint opens the Annex level
# after a short delay, with a custom loading screen (progress bar tagged "LoadingProgress").
# MODEL (Basic template): copied to Content/Models and placed in the Annex level (textured content
# for the packaging smoke test).
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}/Saved")
file(COPY "${TEMPLATE}/Content" DESTINATION "${DIR}")
set(start "Content/Scenes/Main.scene.json")
set(loading "")
if(LEVEL_SWITCH)
    set(start "Content/Scenes/Start.scene.json")
    set(loading "Content/Scenes/Loading.scene.json")
    file(WRITE "${DIR}/Content/Scripts/OpenAnnex.ugraph" [=[{
  "version": 2,
  "nextId": 4,
  "variables": [],
  "nodes": [
    {"id": 1, "type": "Event.BeginPlay", "position": [0, 0]},
    {"id": 2, "type": "Flow.Delay", "position": [200, 0], "defaults": {"Duration": {"type": "float", "value": 0.3}}},
    {"id": 3, "type": "Game.OpenLevel", "position": [400, 0],
     "defaults": {"Scene": {"type": "string", "value": "Content/Scenes/Annex.scene.json"}}}
  ],
  "links": [
    {"from": [1, "Out"], "to": [2, "In"]},
    {"from": [2, "Completed"], "to": [3, "In"]}
  ],
  "comments": []
}
]=])
    file(WRITE "${DIR}/Content/Scenes/Start.scene.json" [=[{
  "version": 1,
  "entities": [
    {"uuid": 1, "parent": 0, "name": "Switcher",
     "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
     "script": {"graph": "../Scripts/OpenAnnex.ugraph"}}
  ]
}
]=])
    file(WRITE "${DIR}/Content/Scenes/Loading.scene.json" [=[{
  "version": 1,
  "entities": [
    {"uuid": 1, "parent": 0, "name": "Loading Canvas",
     "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
     "uiCanvas": {"designSize": [1280, 720]}},
    {"uuid": 2, "parent": 1, "name": "Loading Bar",
     "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
     "uiWidget": {"type": "progressBar", "anchorMin": [0.25, 0.8], "anchorMax": [0.75, 0.8],
                  "offsetMin": [0, -10], "offsetMax": [0, 10]},
     "tags": ["LoadingProgress"]}
  ]
}
]=])
endif()
if(MODEL)
    get_filename_component(model_name "${MODEL}" NAME)
    file(COPY "${MODEL}" DESTINATION "${DIR}/Content/Models")
    set(annex "${DIR}/Content/Scenes/Annex.scene.json")
    file(READ "${annex}" scene)
    string(JSON count LENGTH "${scene}" entities)
    string(JSON scene SET "${scene}" entities ${count} "{\"uuid\": 9001, \"parent\": 0, \"name\": \"Packaged Model\",
        \"transform\": {\"position\": [0, 1, -3], \"rotation\": [0, 0, 0, 1], \"scale\": [1, 1, 1]},
        \"mesh\": {\"model\": {\"file\": \"../Models/${model_name}\"}, \"index\": 0}}")
    file(WRITE "${annex}" "${scene}")
endif()
file(WRITE "${DIR}/${NAME}.ungineproj" "{
  \"version\": 1,
  \"engine\": \"Ungine\",
  \"name\": \"${NAME}\",
  \"startScene\": \"${start}\",
  \"loadingScreen\": \"${loading}\",
  \"window\": { \"width\": 1280, \"height\": 720, \"fullscreen\": false, \"vsync\": true }
}
")
