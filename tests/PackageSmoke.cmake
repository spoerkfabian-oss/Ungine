# cmake -DINSTALL=<extracted package root (has bin/)> -DWORK=<scratch dir> [-DRUN_PLAYER=ON] -P PackageSmoke.cmake
# Checks an installed / unpacked Ungine package the way a user meets it: a project made from the
# installed Basic template (level switch + textured model from the installed samples) is cooked with
# the installed UngineCook; the packaged game then runs from its Content.upak (RUN_PLAYER: needs a
# Vulkan driver and a display, e.g. lavapipe + Xvfb).
cmake_minimum_required(VERSION 3.25)
if(NOT INSTALL OR NOT WORK)
    message(FATAL_ERROR "INSTALL and WORK are required")
endif()
set(bin "${INSTALL}/bin")
set(exe "${CMAKE_EXECUTABLE_SUFFIX}")
if(CMAKE_HOST_WIN32)
    set(exe ".exe")
endif()
foreach(required UngineEditor${exe} UnginePlayer${exe} UngineCook${exe} templates/Basic/Content shaders/source samples/models/BoxTextured.glb)
    if(NOT EXISTS "${bin}/${required}")
        message(FATAL_ERROR "Package is missing bin/${required}")
    endif()
endforeach()
file(GLOB spv "${bin}/shaders/*.spv")
list(LENGTH spv spvCount)
if(spvCount LESS 10)
    message(FATAL_ERROR "Package has only ${spvCount} compiled shaders")
endif()

file(REMOVE_RECURSE "${WORK}")
execute_process(COMMAND "${CMAKE_COMMAND}" -DTEMPLATE=${bin}/templates/Basic -DDIR=${WORK}/project -DNAME=SmokeGame
                        -DLEVEL_SWITCH=ON -DMODEL=${bin}/samples/models/BoxTextured.glb -P "${CMAKE_CURRENT_LIST_DIR}/MakeProject.cmake"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Making the project failed (${result})")
endif()
execute_process(COMMAND "${bin}/UngineCook${exe}" "${WORK}/project/SmokeGame.ungineproj" "${WORK}/game"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "UngineCook failed (${result}):\n${out}\n${err}")
endif()
foreach(required SmokeGame${exe} Content.upak SmokeGame.ungineproj shaders/source BuildReport.txt)
    if(NOT EXISTS "${WORK}/game/${required}")
        message(FATAL_ERROR "The packaged game is missing ${required}")
    endif()
endforeach()
message(STATUS "Cooked: ${WORK}/game")

if(RUN_PLAYER)
    execute_process(COMMAND "${WORK}/game/SmokeGame${exe}" --frames 200 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(log "${out}\n${err}")
    if(NOT result EQUAL 0 OR NOT log MATCHES "Playing 'SmokeGame' \\([^)]*Annex\\.scene\\.json\\)" OR log MATCHES "\\[ERROR\\]|not cooked into the pak")
        message(FATAL_ERROR "The packaged game failed (${result}):\n${log}")
    endif()
    message(STATUS "The packaged game ran and switched levels")
endif()
