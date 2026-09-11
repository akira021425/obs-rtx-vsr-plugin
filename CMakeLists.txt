cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs d3d11.lib)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src third-party/include)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE 
    src/plugin-main.cpp
    src/plugin-main.hpp
    src/rtx-vsr-filter.cpp
    src/rtx-vsr-filter.hpp
    src/d3d11-interop.cpp
    src/d3d11-interop.hpp
    src/nvidia-vsr.cpp
    src/nvidia-vsr.hpp
    src/nvCVImageProxy.cpp
    src/NVVideoEffectsProxy.cpp
    src/frame-interpolation.cpp
    src/frame-interpolation.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
