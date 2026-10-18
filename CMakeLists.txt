cmake_minimum_required(VERSION 3.25)

# Release builds pass -DCPA_VERSION=<tag without "v">.
set(CPA_VERSION "0.3.0" CACHE STRING "Plugin version (MAJOR.MINOR.PATCH)")
if(NOT CPA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "CPA_VERSION must be MAJOR.MINOR.PATCH, got '${CPA_VERSION}'")
endif()

project(CPAQuota
    VERSION ${CPA_VERSION}
    DESCRIPTION "TrafficMonitor plugin showing CLIProxyAPI account quotas"
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

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(
    "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive-;/Zc:__cplusplus>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:/O1;/Gy;/GL>"
)
add_link_options("$<$<NOT:$<CONFIG:Debug>>:/LTCG;/OPT:REF;/OPT:ICF>")

# Pure parsing logic, shared by the plugin and the unit tests.
add_library(cpa_core STATIC
    src/alerts.cpp
    src/alerts.h
    src/json.cpp
    src/json.h
    src/quota.cpp
    src/quota.h
)
target_include_directories(cpa_core PUBLIC src)

add_library(CPAQuota SHARED
    src/plugin.cpp
    src/plugin.h
    src/cpa_client.cpp
    src/cpa_client.h
    src/http_client.cpp
    src/http_client.h
    src/config.cpp
    src/config.h
    src/options_dialog.cpp
    src/options_dialog.h
    src/resource.h
    src/plugin.rc
)
target_include_directories(CPAQuota SYSTEM PRIVATE third_party/TrafficMonitor)
target_include_directories(CPAQuota PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(CPAQuota PRIVATE cpa_core winhttp crypt32 shell32)

configure_file(src/version.h.in version.h @ONLY)

include(CTest)
if(BUILD_TESTING)
    add_executable(cpa_tests tests/quota_tests.cpp)
    target_link_libraries(cpa_tests PRIVATE cpa_core)
    add_test(NAME cpa_tests COMMAND cpa_tests)
endif()
