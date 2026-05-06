#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Boost::type_erasure" for configuration "Release"
set_property(TARGET Boost::type_erasure APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Boost::type_erasure PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libboost_type_erasure-vc144-mt-x64-1_84.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS Boost::type_erasure )
list(APPEND _IMPORT_CHECK_FILES_FOR_Boost::type_erasure "${_IMPORT_PREFIX}/lib/libboost_type_erasure-vc144-mt-x64-1_84.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
