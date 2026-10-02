# Packages: `cpack -C Release` in the build directory after building.
# Windows: NSIS installer (Start menu + desktop shortcut, .ungineproj file association) and ZIP;
# Linux: TGZ (bin/install-desktop-integration.sh registers the menu entry and the file type).
set(CPACK_PACKAGE_NAME "Ungine")
set(CPACK_PACKAGE_VENDOR "Ungine")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Ungine game engine: editor, visual scripting and player")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Ungine")
set(CPACK_PACKAGE_EXECUTABLES "UngineEditor" "Ungine Editor") # Start menu shortcut
set(CPACK_STRIP_FILES ON)

if(WIN32)
    find_program(ENGINE_MAKENSIS makensis)
    set(CPACK_GENERATOR ZIP)
    if(ENGINE_MAKENSIS)
        list(PREPEND CPACK_GENERATOR NSIS)
    endif()
    set(CPACK_CREATE_DESKTOP_LINKS "UngineEditor")
    set(CPACK_NSIS_DISPLAY_NAME "Ungine")
    set(CPACK_NSIS_PACKAGE_NAME "Ungine Engine")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/resources/ungine.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/resources/ungine.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\UngineEditor.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    # Double-click on a .ungineproj opens the editor (per machine; the installer runs elevated).
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [=[
  WriteRegStr HKCR ".ungineproj" "" "Ungine.Project"
  WriteRegStr HKCR "Ungine.Project" "" "Ungine Project"
  WriteRegStr HKCR "Ungine.Project\DefaultIcon" "" "$INSTDIR\bin\UngineEditor.exe,0"
  WriteRegStr HKCR "Ungine.Project\shell\open\command" "" '"$INSTDIR\bin\UngineEditor.exe" "%1"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
]=])
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [=[
  DeleteRegKey HKCR ".ungineproj"
  DeleteRegKey HKCR "Ungine.Project"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
]=])
else()
    set(CPACK_GENERATOR TGZ)
endif()

include(CPack)
