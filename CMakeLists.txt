cmake_minimum_required(VERSION 3.16)

project(HuafuSlicer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set_property(GLOBAL PROPERTY VS_GLOBAL_BuildInParallel false)
  # Boost auto_link.hpp maps VS2022+ to vc143, but static libs from b2 are tagged vc144 — pragma pulls wrong .lib names.
  add_compile_definitions(BOOST_ALL_NO_LIB)
endif()

find_package(OpenGL REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 OpenGL Concurrent Svg Widgets)

qt_standard_project_setup(REQUIRES 6.5)

# Ensure local CMake helpers remain discoverable.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/src/libslic3r")

# IMPORTED targets (EXPAT::expat, draco::draco, JPEG::JPEG) must exist before libslicgui/Qt resolves
# libslic3r PUBLIC deps — MODULE-mode FindEXPAT does not define EXPAT::expat; require CONFIG packages.
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/src/third_party/local")
find_package(JPEG REQUIRED COMPONENTS JPEG)
find_package(draco CONFIG REQUIRED)
find_package(EXPAT REQUIRED CONFIG)

#add_subdirectory(src/third_party)
add_subdirectory(src/libslic3r)
include_directories(src/libslic3r)
add_subdirectory(src/libslicgui)
include_directories(src/libslicgui)

file(GLOB_RECURSE HuafuSlicer_sources "HuafuSlicer/*.cpp")
file(GLOB_RECURSE HuafuSlicer_headers "HuafuSlicer/*.h")
# Qt 6.7+：QML_FILES 不得为绝对路径；显式相对路径（相对本 CMakeLists 所在目录）
set(HuafuSlicer_qml
    HuafuSlicer/Main.qml
    HuafuSlicer/PreviewSimulation.qml
    HuafuSlicer/WorkspaceResponseHub.qml
)
# 源文件在子目录 HuafuSlicer/，但 QML 模块根下应为 Main.qml 等，否则会变成 HuafuSlicer/HuafuSlicer/Main.qml，
# 与 loadFromModule("HuafuSlicer", "Main")、import HuafuSlicer 不一致。
set_source_files_properties(
    HuafuSlicer/Main.qml
    PROPERTIES QT_RESOURCE_ALIAS "Main.qml"
)
set_source_files_properties(
    HuafuSlicer/PreviewSimulation.qml
    PROPERTIES QT_RESOURCE_ALIAS "PreviewSimulation.qml"
)
set_source_files_properties(
    HuafuSlicer/WorkspaceResponseHub.qml
    PROPERTIES QT_RESOURCE_ALIAS "WorkspaceResponseHub.qml"
)

# 图标：URL 为 qrc:/huafuslicer/resources/<文件名>。
# Qt 6.7：FILES 须为存在的绝对路径；勿用 BASE_PATH+裸文件名（会误解析成 ${CMAKE_SOURCE_DIR}/import_model.png 导致 Configure 失败）。
set(_hfs_res "${CMAKE_CURRENT_SOURCE_DIR}/resources")
set(HuafuSlicer_asset_files
    "${_hfs_res}/import_model.png"
    "${_hfs_res}/ic_import_gcode.svg"
    "${_hfs_res}/gcode.png"
    "${_hfs_res}/arrange_model.png"
    "${_hfs_res}/rotate_model.png"
    "${_hfs_res}/viewport_bg.png"
    "${_hfs_res}/ic_profile_save.png"
    "${_hfs_res}/ic_profile_edit.png"
    "${_hfs_res}/ic_profile_delete.png"
    "${_hfs_res}/ic_print_layer_height.png"
    "${_hfs_res}/ic_print_wall.png"
    "${_hfs_res}/ic_print_top_bottom.png"
    "${_hfs_res}/ic_print_infill.png"
    "${_hfs_res}/ic_print_other.png"
)
# Qt 6.7：qt_add_resources 里用绝对路径时必须设 QT_RESOURCE_ALIAS（相对 PREFIX 的路径，这里用文件名即可 → qrc:/huafuslicer/resources/<名>）
foreach(_abs IN LISTS HuafuSlicer_asset_files)
    get_filename_component(_base "${_abs}" NAME)
    set_source_files_properties("${_abs}" PROPERTIES QT_RESOURCE_ALIAS "${_base}")
endforeach()

qt_add_executable(appHuafuSlicer 
    ${HuafuSlicer_sources} 
    ${HuafuSlicer_headers} 
    ${HuafuSlicer_qml}
)

qt_add_resources(appHuafuSlicer "huafuslicer_assets"
    PREFIX "/huafuslicer/resources"
    FILES ${HuafuSlicer_asset_files}
)

qt_add_qml_module(appHuafuSlicer
    URI HuafuSlicer
    VERSION 1.0
    QML_FILES
        ${HuafuSlicer_qml}
)
source_group("QML" FILES
    ${HuafuSlicer_qml}
)
source_group("Headers" FILES
    ${HuafuSlicer_headers}
)
source_group("Sources" FILES
    ${HuafuSlicer_sources}
)
set_target_properties(appHuafuSlicer PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appHuafuSlicer
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::OpenGL Qt6::Concurrent Qt6::Svg Qt6::Widgets
    PRIVATE OpenGL::GL
    PRIVATE libslic3r
    PRIVATE libslicgui
)

include(GNUInstallDirs)
install(TARGETS appHuafuSlicer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
