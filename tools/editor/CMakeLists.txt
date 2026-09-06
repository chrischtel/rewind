add_executable(rewind_editor editor.cpp)
target_link_libraries(rewind_editor PRIVATE rewind::rewind)
rewind_apply_flags(rewind_editor)
