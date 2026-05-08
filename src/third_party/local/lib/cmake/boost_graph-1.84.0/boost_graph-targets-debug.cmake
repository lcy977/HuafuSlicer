#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Boost::graph" for configuration "Debug"
set_property(TARGET Boost::graph APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Boost::graph PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libboost_graph-vc144-mt-gd-x64-1_84d.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS Boost::graph )
list(APPEND _IMPORT_CHECK_FILES_FOR_Boost::graph "${_IMPORT_PREFIX}/lib/libboost_graph-vc144-mt-gd-x64-1_84d.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
