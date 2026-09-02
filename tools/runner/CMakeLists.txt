add_executable(rewind_run main.cpp)
target_link_libraries(rewind_run PRIVATE rewind::rewind)
rewind_apply_flags(rewind_run)
