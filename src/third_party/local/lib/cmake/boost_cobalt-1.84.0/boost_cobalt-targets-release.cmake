#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Boost::cobalt" for configuration "Release"
set_property(TARGET Boost::cobalt APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Boost::cobalt PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libboost_cobalt-vc144-mt-x64-1_84.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS Boost::cobalt )
list(APPEND _IMPORT_CHECK_FILES_FOR_Boost::cobalt "${_IMPORT_PREFIX}/lib/libboost_cobalt-vc144-mt-x64-1_84.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
