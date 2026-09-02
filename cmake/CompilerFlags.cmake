# Shared warning and floating-point settings for engine and game targets.
#
# Determinism matters here: the same input log must produce the same state on
# every run, so the compiler must not reorder or contract floating-point math.
function(rewind_apply_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /MP
            /fp:strict)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -ffp-contract=off -fno-fast-math)
    endif()
endfunction()
