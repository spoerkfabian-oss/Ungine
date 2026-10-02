# cmake -DTEMPLATE=<templates/X> -DDIR=<out dir> -DNAME=<name> -P MakeProject.cmake
# A project from a template, like Project::Create (for the app smoke tests).
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}/Saved")
file(COPY "${TEMPLATE}/Content" DESTINATION "${DIR}")
file(WRITE "${DIR}/${NAME}.ungineproj" "{
  \"version\": 1,
  \"engine\": \"Ungine\",
  \"name\": \"${NAME}\",
  \"startScene\": \"Content/Scenes/Main.scene.json\",
  \"window\": { \"width\": 1280, \"height\": 720, \"fullscreen\": false, \"vsync\": true }
}
")
