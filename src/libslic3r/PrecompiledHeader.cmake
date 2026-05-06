#
# Minimal PCH helper for this project.
# The upstream project expects an `add_precompiled_header()` function.
#

include_guard(GLOBAL)

function(add_precompiled_header target header)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "add_precompiled_header(): target '${target}' not found")
    endif()

    # Only enable when requested by the project.
    if (NOT DEFINED SLIC3R_PCH OR NOT SLIC3R_PCH)
        return()
    endif()

    # target_precompile_headers was added in CMake 3.16.
    if (CMAKE_VERSION VERSION_LESS 3.16)
        message(STATUS "PCH requested but CMake < 3.16; skipping PCH for ${target}")
        return()
    endif()

    # FORCEINCLUDE behavior is handled by the compiler when using CMake's PCH.
    # Keep this lightweight and cross-platform.
    target_precompile_headers(${target} PRIVATE "${header}")
endfunction()

