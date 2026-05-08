# Debug uses the same static import as Release (single shipped import library).

set(CMAKE_IMPORT_FILE_VERSION 1)

set_property(TARGET png_static APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(png_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libpng16_static.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS png_static )
list(APPEND _IMPORT_CHECK_FILES_FOR_png_static "${_IMPORT_PREFIX}/lib/libpng16_static.lib" )

set(CMAKE_IMPORT_FILE_VERSION)
