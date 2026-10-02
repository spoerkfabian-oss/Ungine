# Developer-only utilities. Nothing here is built by default.
add_executable(MakeSounds EXCLUDE_FROM_ALL MakeSounds.cpp)
target_link_libraries(MakeSounds PRIVATE Engine)
engine_set_warnings(MakeSounds)
set_target_properties(MakeSounds PROPERTIES FOLDER "Tools")
