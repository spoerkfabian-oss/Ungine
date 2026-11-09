# Patch smoke: changes the packaged project's content, then writes a patch into the package with
# UngineCook --patch (the package was just cooked from scratch, so this is Patch_1.upak).
#   cmake -DCOOK=<UngineCook> -DPROJECT=<project file> -DPACKAGE=<package dir> -P PatchSmoke.cmake
get_filename_component(content "${PROJECT}" DIRECTORY)
string(TIMESTAMP now "%Y-%m-%dT%H:%M:%S.%f" UTC) # always a change
file(WRITE "${content}/Content/Patched.txt" "${now}")
execute_process(COMMAND "${COOK}" "${PROJECT}" "${PACKAGE}" --patch RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "UngineCook --patch failed (${result})")
endif()
if(NOT EXISTS "${PACKAGE}/Patch_1.upak")
    message(FATAL_ERROR "no Patch_1.upak in ${PACKAGE}")
endif()
