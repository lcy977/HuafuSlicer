cmake_minimum_required(VERSION 3.16)

project(HuafuSlicer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set_property(GLOBAL PROPERTY VS_GLOBAL_BuildInParallel false)
endif()

find_package(OpenGL REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 OpenGL Concurrent  Svg)

qt_standard_project_setup(REQUIRES 6.5)

# Ensure local CMake helpers remain discoverable.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/src/libslic3r")

#add_subdirectory(src/third_party)
add_subdirectory(src/libslic3r)
include_directories(src/libslic3r)
add_subdirectory(src/libslicgui)
include_directories(src/libslicgui)


qt_add_executable(appHuafuSlicer
    HuafuSlicer/main.cpp
    HuafuSlicer/WindowHelper.cpp
    HuafuSlicer/MeshLoader.cpp
    HuafuSlicer/GCodeParser.cpp
    HuafuSlicer/OpenGLViewport.cpp
)

qt_add_qml_module(appHuafuSlicer
    URI Youtian3D
    VERSION 1.0
    QML_FILES
        HuafuSlicer/Main.qml
        HuafuSlicer/PreviewSimulation.qml
    RESOURCES
        icons/importModel.png
        icons/ic_import_gcode.png
)
source_group("QML" FILES
    HuafuSlicer/Main.qml
    HuafuSlicer/PreviewSimulation.qml
)
set_target_properties(appHuafuSlicer PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appHuafuSlicer
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::OpenGL Qt6::Concurrent Qt6::Svg
    PRIVATE OpenGL::GL
)

include(GNUInstallDirs)
install(TARGETS appHuafuSlicer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
