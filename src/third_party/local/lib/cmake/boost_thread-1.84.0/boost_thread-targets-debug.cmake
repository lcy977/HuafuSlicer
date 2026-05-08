#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Boost::thread" for configuration "Debug"
set_property(TARGET Boost::thread APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Boost::thread PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libboost_thread-vc144-mt-gd-x64-1_84d.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS Boost::thread )
list(APPEND _IMPORT_CHECK_FILES_FOR_Boost::thread "${_IMPORT_PREFIX}/lib/libboost_thread-vc144-mt-gd-x64-1_84d.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
