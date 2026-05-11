cmake_minimum_required(VERSION 3.13)
project(libslicgui)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)

#Boost
set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_LIST_DIR}/../third_party/local")
set(Boost_NO_SYSTEM_PATHS TRUE)
set(Boost_DEBUG ON)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem system log)
 
#OPENSSL
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OPENSSL_DEBUG ON)
find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)

include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party)
#EIGEN
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/eigen)

file(GLOB_RECURSE libslicgui_sources "*.cpp")
file(GLOB_RECURSE libslicgui_headers "*.hpp")
 
 
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Widgets Core)

add_library(libslicgui STATIC ${libslicgui_sources} ${libslicgui_headers})
# Align with libslic3r: Qt pulls windows.h; NOMINMAX avoids min/max macro breakage in Boost/templates (MSVC).
target_compile_definitions(libslicgui PRIVATE EIGEN_HAS_STD_RESULT_OF=0 _USE_MATH_DEFINES UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(libslicgui PRIVATE libslic3r Qt6::Widgets Qt6::Core Qt6::Quick Qt6::QuickControls2 Qt6::OpenGL)

target_link_libraries(libslicgui PRIVATE Boost::filesystem Boost::system Boost::log)
target_link_libraries(libslicgui PRIVATE OpenSSL::Crypto OpenSSL::SSL)
target_include_directories(libslicgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

