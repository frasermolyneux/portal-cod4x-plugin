cmake_minimum_required(VERSION 3.24)

project(portal_cod4x_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PORTAL_COD4X_BUILD_PLUGIN_BINARY "Build the loadable CoD4x plugin module." OFF)
option(PORTAL_COD4X_USE_EXTERNAL_SDK "Use CoD4x pinc.h from an external SDK include path." OFF)
set(PORTAL_COD4X_PLUGIN_VERSION "" CACHE STRING "Semantic version string stamped into the plugin (for example 0.1.0).")
set(PORTAL_COD4X_PLUGIN_ARCH "x86" CACHE STRING "Target architecture for the loadable plugin module (x86 or x64).")
set_property(CACHE PORTAL_COD4X_PLUGIN_ARCH PROPERTY STRINGS x86 x64)
set(COD4X_PLUGIN_SDK_INCLUDE_DIR "" CACHE PATH "Path to the CoD4x plugin SDK include folder containing pinc.h")
set(COD4X_PLUGIN_HOST_IMPORT_LIB "" CACHE FILEPATH "Path to CoD4x host import library (required for Windows plugin module linking).")

string(TOLOWER "${PORTAL_COD4X_PLUGIN_ARCH}" PORTAL_COD4X_PLUGIN_ARCH_NORMALIZED)
if(NOT PORTAL_COD4X_PLUGIN_ARCH_NORMALIZED STREQUAL "x86" AND NOT PORTAL_COD4X_PLUGIN_ARCH_NORMALIZED STREQUAL "x64")
  message(FATAL_ERROR "PORTAL_COD4X_PLUGIN_ARCH must be either x86 or x64.")
endif()

set(PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS "")
set(PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS "")

if(PORTAL_COD4X_BUILD_PLUGIN_BINARY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(PORTAL_COD4X_PLUGIN_ARCH_NORMALIZED STREQUAL "x86")
    set(PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS -m32)
    set(PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS -m32)
  elseif(PORTAL_COD4X_PLUGIN_ARCH_NORMALIZED STREQUAL "x64")
    set(PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS -m64)
    set(PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS -m64)
  endif()
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.json" VERSION_JSON_CONTENT)
string(REGEX MATCH "\"version\"[ \t]*:[ \t]*\"([0-9]+)\\.([0-9]+)" _VERSION_MATCH "${VERSION_JSON_CONTENT}")
if(NOT _VERSION_MATCH)
  message(FATAL_ERROR "Unable to parse major.minor from version.json")
endif()

set(PORTAL_COD4X_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PORTAL_COD4X_VERSION_MINOR "${CMAKE_MATCH_2}")

if(NOT PORTAL_COD4X_PLUGIN_VERSION)
  set(PORTAL_COD4X_PLUGIN_VERSION "${PORTAL_COD4X_VERSION_MAJOR}.${PORTAL_COD4X_VERSION_MINOR}.0")
endif()

add_library(portal_cod4x_core STATIC
  src/plugin_runtime.cpp
)

target_include_directories(portal_cod4x_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(portal_cod4x_core
  PUBLIC
    PORTAL_COD4X_PLUGIN_VERSION=\"${PORTAL_COD4X_PLUGIN_VERSION}\"
    PORTAL_COD4X_PLUGIN_VERSION_MAJOR=${PORTAL_COD4X_VERSION_MAJOR}
    PORTAL_COD4X_PLUGIN_VERSION_MINOR=${PORTAL_COD4X_VERSION_MINOR}
)

if(MSVC)
  target_compile_options(portal_cod4x_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(portal_cod4x_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS)
  target_compile_options(portal_cod4x_core PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS})
endif()

add_library(portal_cod4x_plugin_exports OBJECT
  src/plugin_exports.cpp
)

target_link_libraries(portal_cod4x_plugin_exports PRIVATE portal_cod4x_core)

target_include_directories(portal_cod4x_plugin_exports
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(PORTAL_COD4X_USE_EXTERNAL_SDK)
  target_compile_definitions(portal_cod4x_plugin_exports PRIVATE PORTAL_COD4X_USE_EXTERNAL_SDK=1)
  if(COD4X_PLUGIN_SDK_INCLUDE_DIR)
    target_include_directories(portal_cod4x_plugin_exports PRIVATE ${COD4X_PLUGIN_SDK_INCLUDE_DIR})
  else()
    message(WARNING "PORTAL_COD4X_USE_EXTERNAL_SDK is ON but COD4X_PLUGIN_SDK_INCLUDE_DIR is empty.")
  endif()
endif()

if(PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS)
  target_compile_options(portal_cod4x_plugin_exports PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS})
endif()

if(PORTAL_COD4X_BUILD_PLUGIN_BINARY)
  add_library(portal_cod4x_plugin MODULE
    $<TARGET_OBJECTS:portal_cod4x_plugin_exports>
  )

  target_link_libraries(portal_cod4x_plugin PRIVATE portal_cod4x_core)

  set_target_properties(portal_cod4x_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "portal-cod4x-plugin"
  )

  if(MSVC)
    if(COD4X_PLUGIN_HOST_IMPORT_LIB)
      target_link_libraries(portal_cod4x_plugin PRIVATE ${COD4X_PLUGIN_HOST_IMPORT_LIB})
    else()
      message(FATAL_ERROR "Windows plugin output requires COD4X_PLUGIN_HOST_IMPORT_LIB (for example com_plugin.lib from a CoD4x SDK/build output).")
    endif()
  endif()

  if(PORTAL_COD4X_PLUGIN_ARCH_NORMALIZED STREQUAL "x86")
    if(MSVC)
      if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "x86 plugin output requires a Win32 generator platform. Reconfigure with -A Win32.")
      endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(portal_cod4x_plugin PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS})
    else()
      message(WARNING "x86 plugin architecture is selected, but no explicit 32-bit flags are configured for this platform.")
    endif()
  elseif(PORTAL_COD4X_PLUGIN_ARCH_NORMALIZED STREQUAL "x64")
    if(MSVC)
      if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "x64 plugin output requires a 64-bit generator platform.")
      endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(portal_cod4x_plugin PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS})
    endif()
  endif()
endif()

enable_testing()

add_executable(portal_cod4x_plugin_tests
  tests/plugin_runtime_tests.cpp
)

target_link_libraries(portal_cod4x_plugin_tests PRIVATE portal_cod4x_core)

if(PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS)
  target_compile_options(portal_cod4x_plugin_tests PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS})
endif()

if(PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS)
  target_link_options(portal_cod4x_plugin_tests PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS})
endif()

add_executable(portal_cod4x_plugin_exports_tests
  tests/plugin_exports_tests.cpp
  $<TARGET_OBJECTS:portal_cod4x_plugin_exports>
)

target_link_libraries(portal_cod4x_plugin_exports_tests PRIVATE portal_cod4x_core)

if(PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS)
  target_compile_options(portal_cod4x_plugin_exports_tests PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_COMPILE_OPTIONS})
endif()

if(PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS)
  target_link_options(portal_cod4x_plugin_exports_tests PRIVATE ${PORTAL_COD4X_PLUGIN_ARCH_LINK_OPTIONS})
endif()

add_test(
  NAME portal_cod4x_plugin_tests
  COMMAND portal_cod4x_plugin_tests
)

add_test(
  NAME portal_cod4x_plugin_exports_tests
  COMMAND portal_cod4x_plugin_exports_tests
)
