#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "EXPAT::expat" for configuration "Release"
set_property(TARGET EXPAT::expat APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(EXPAT::expat PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/expat.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS EXPAT::expat )
list(APPEND _IMPORT_CHECK_FILES_FOR_EXPAT::expat "${_IMPORT_PREFIX}/lib/expat.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
