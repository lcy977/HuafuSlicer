# Debug uses the same Expat static import as Release (single shipped .lib).

set(CMAKE_IMPORT_FILE_VERSION 1)

set_property(TARGET EXPAT::expat APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(EXPAT::expat PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/expat.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS EXPAT::expat )
list(APPEND _IMPORT_CHECK_FILES_FOR_EXPAT::expat "${_IMPORT_PREFIX}/lib/expat.lib" )

set(CMAKE_IMPORT_FILE_VERSION)
