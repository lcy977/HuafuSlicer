cmake_minimum_required(VERSION 3.13)
project(clipper)

add_library(clipper STATIC
# We are using ClipperLib compiled as part of the libslic3r project using Slic3r::Point as its base type.
#    clipper.cpp
#    clipper.hpp
    clipper_z.cpp
    clipper_z.hpp
)

target_include_directories(clipper SYSTEM
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../eigen
        # clipper.hpp / clipper.cpp include <libslic3r/Int128.hpp> — search root is src/
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

# clipper.hpp uses oneapi/tbb/scalable_allocator.h; no imported target named "eigen" in this repo.
target_link_libraries(clipper
    PUBLIC TBB::tbb TBB::tbbmalloc
)
