add_executable(rewind_cli main.cpp)
target_link_libraries(rewind_cli PRIVATE rewind::rewind)
set_target_properties(rewind_cli PROPERTIES OUTPUT_NAME rewind)
rewind_apply_flags(rewind_cli)
