cmake_minimum_required(VERSION 3.25)

project(TrafficMonitorCPA
    VERSION 0.1.0
    DESCRIPTION "Lightweight TrafficMonitor plugin"
    LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "This plugin must be built with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT: the DLL has no dependency on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(TrafficMonitorCPA SHARED
    src/plugin.cpp
    src/plugin.h
    src/version.rc
)

target_include_directories(TrafficMonitorCPA SYSTEM PRIVATE
    third_party/TrafficMonitor
)
target_include_directories(TrafficMonitorCPA PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(TrafficMonitorCPA PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
)

target_compile_options(TrafficMonitorCPA PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8 /W4 /permissive- /Zc:__cplusplus>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:/O1 /Gy /GL>
)

target_link_options(TrafficMonitorCPA PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:/LTCG /OPT:REF /OPT:ICF>
)

configure_file(src/version.h.in version.h @ONLY)
