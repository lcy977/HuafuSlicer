# Vendored Qhull: reentrant C library (libqhull_r) + C++ wrapper (libqhullcpp).
# TriangleMesh::its_convex_hull uses orgQhull::Qhull from libqhullcpp.

cmake_minimum_required(VERSION 3.16)

# Parent HuafuSlicer project is CXX-only; libqhull_r needs C.
enable_language(C)

set(_qhull_inc "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(_qhull_r_c
  ${_qhull_inc}/libqhull_r/global_r.c
  ${_qhull_inc}/libqhull_r/stat_r.c
  ${_qhull_inc}/libqhull_r/geom2_r.c
  ${_qhull_inc}/libqhull_r/poly2_r.c
  ${_qhull_inc}/libqhull_r/merge_r.c
  ${_qhull_inc}/libqhull_r/libqhull_r.c
  ${_qhull_inc}/libqhull_r/geom_r.c
  ${_qhull_inc}/libqhull_r/poly_r.c
  ${_qhull_inc}/libqhull_r/qset_r.c
  ${_qhull_inc}/libqhull_r/mem_r.c
  ${_qhull_inc}/libqhull_r/random_r.c
  ${_qhull_inc}/libqhull_r/usermem_r.c
  ${_qhull_inc}/libqhull_r/userprintf_r.c
  ${_qhull_inc}/libqhull_r/io_r.c
  ${_qhull_inc}/libqhull_r/user_r.c
  ${_qhull_inc}/libqhull_r/rboxlib_r.c
  ${_qhull_inc}/libqhull_r/userprintf_rbox_r.c
)

add_library(qhullstatic_r STATIC ${_qhull_r_c})
target_include_directories(qhullstatic_r PUBLIC ${_qhull_inc})
set_target_properties(qhullstatic_r PROPERTIES C_STANDARD 99)

if(UNIX AND NOT APPLE AND NOT ANDROID)
  target_link_libraries(qhullstatic_r PRIVATE m)
endif()

set(_qhull_cpp
  ${_qhull_inc}/libqhullcpp/Coordinates.cpp
  ${_qhull_inc}/libqhullcpp/PointCoordinates.cpp
  ${_qhull_inc}/libqhullcpp/Qhull.cpp
  ${_qhull_inc}/libqhullcpp/QhullFacet.cpp
  ${_qhull_inc}/libqhullcpp/QhullFacetList.cpp
  ${_qhull_inc}/libqhullcpp/QhullFacetSet.cpp
  ${_qhull_inc}/libqhullcpp/QhullHyperplane.cpp
  ${_qhull_inc}/libqhullcpp/QhullPoint.cpp
  ${_qhull_inc}/libqhullcpp/QhullPoints.cpp
  ${_qhull_inc}/libqhullcpp/QhullPointSet.cpp
  ${_qhull_inc}/libqhullcpp/QhullQh.cpp
  ${_qhull_inc}/libqhullcpp/QhullRidge.cpp
  ${_qhull_inc}/libqhullcpp/QhullSet.cpp
  ${_qhull_inc}/libqhullcpp/QhullStat.cpp
  ${_qhull_inc}/libqhullcpp/QhullVertex.cpp
  ${_qhull_inc}/libqhullcpp/QhullVertexSet.cpp
  ${_qhull_inc}/libqhullcpp/RboxPoints.cpp
  ${_qhull_inc}/libqhullcpp/RoadError.cpp
  ${_qhull_inc}/libqhullcpp/RoadLogEvent.cpp
)

add_library(qhullcpp STATIC ${_qhull_cpp})
target_include_directories(qhullcpp PUBLIC ${_qhull_inc})
target_link_libraries(qhullcpp PUBLIC qhullstatic_r)
