#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Boost::coroutine" for configuration "Debug"
set_property(TARGET Boost::coroutine APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Boost::coroutine PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libboost_coroutine-vc144-mt-gd-x64-1_84d.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS Boost::coroutine )
list(APPEND _IMPORT_CHECK_FILES_FOR_Boost::coroutine "${_IMPORT_PREFIX}/lib/libboost_coroutine-vc144-mt-gd-x64-1_84d.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
