#----------------------------------------------------------------
# Debug uses the same Draco static import as Release (single shipped .lib).
#----------------------------------------------------------------

set(CMAKE_IMPORT_FILE_VERSION 1)

set_property(TARGET draco::draco APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(draco::draco PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/draco.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS draco::draco )
list(APPEND _IMPORT_CHECK_FILES_FOR_draco::draco "${_IMPORT_PREFIX}/lib/draco.lib" )

set(CMAKE_IMPORT_FILE_VERSION)
