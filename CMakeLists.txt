# ============================================================================
# ntplite - top level CMakeLists.txt
# ----------------------------------------------------------------------------
# Lightweight, header-only NTP client library for C++11 and C.
#
# Requirements: CMake >= 3.16 (verified against 3.22 on Linux and 4.x on
# Windows).  The C++ standard is pinned to C++11: the library must build with
# -std=c++11 and must not use any C++14/17 facility.
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(
  ntplite
  VERSION 0.1.0
  DESCRIPTION "Lightweight header-only NTP client library for C++11 and C"
  HOMEPAGE_URL "https://github.com/leiddev/ntplite"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NTP_LITE_BUILD_TOOLS "Build the ntplite command line tool" ON)
option(NTP_LITE_BUILD_EXAMPLES "Build the example programs" ON)
option(NTP_LITE_BUILD_TESTS "Build the unit and integration tests" ON)
option(NTP_LITE_INSTALL "Generate installation / packaging rules" ON)

option(NTP_LITE_ONLINE_TESTS
       "Register tests that need real internet access (off by default so CI stays deterministic)"
       OFF)

option(NTP_LITE_WERROR "Treat compiler warnings as errors" OFF)
option(NTP_LITE_SANITIZE
       "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang only)"
       OFF)

# ---------------------------------------------------------------------------
# Defaults for single-configuration generators
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      "RelWithDebInfo"
      CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release"
                                               "RelWithDebInfo" "MinSizeRel")
endif()

# ---------------------------------------------------------------------------
# Pin the language standards.  ntplite is C++11 *strictly*: if a future change
# needs C++14/17 the build should break here, loudly, and get a deliberate bump.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Warning helpers
# ---------------------------------------------------------------------------
function(ntplite_apply_cxx_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8
                                             /Zc:__cplusplus /Zc:inline)
    if(NTP_LITE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(
      ${target}
      PRIVATE -Wall
              -Wextra
              -Wpedantic
              -Wshadow
              -Wundef
              -Wcast-qual
              -Wcast-align
              -Wdouble-promotion
              -Wformat=2
              -Wnon-virtual-dtor
              -Wold-style-cast
              -Woverloaded-virtual
              -Wpointer-arith
              -Wundef
              -Wno-unused-parameter)
    if(NTP_LITE_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Targets that also contain C sources cannot take C++-only flags: the MSVC C
# front end would reject /permissive- with warning D9002, which /WX turns into
# an error.
function(ntplite_apply_c_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
    if(NTP_LITE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(NTP_LITE_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers (test / CI builds only)
# ---------------------------------------------------------------------------
if(NTP_LITE_SANITIZE)
  if(MSVC)
    message(WARNING "NTP_LITE_SANITIZE is not supported with MSVC; ignoring.")
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library targets
#
#   ntplite::ntplite   header-only INTERFACE library  -> C++ consumers
#   ntplite::c         one compiled TU, stable C ABI  -> C / FFI consumers
# ---------------------------------------------------------------------------
add_library(ntplite INTERFACE)
add_library(ntplite::ntplite ALIAS ntplite)
set_target_properties(ntplite PROPERTIES EXPORT_NAME ntplite)

target_include_directories(
  ntplite INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(ntplite INTERFACE cxx_std_11)

if(WIN32)
  # Winsock2: gethostbyname / getaddrinfo / select / sendto / recvfrom.
  target_link_libraries(ntplite INTERFACE ws2_32)
endif()

add_library(ntplite_c STATIC src/ntplite_c.cpp)
add_library(ntplite::c ALIAS ntplite_c)
set_target_properties(
  ntplite_c
  PROPERTIES EXPORT_NAME c
             OUTPUT_NAME ntplite
             POSITION_INDEPENDENT_CODE ON)

# PRIVATE on purpose: a pure C consumer target must not inherit `cxx_std_11`,
# or CMake will refuse to generate because the C language has no C++ dialect.
target_link_libraries(ntplite_c PRIVATE ntplite)

target_include_directories(
  ntplite_c PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                   $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(WIN32)
  target_link_libraries(ntplite_c PUBLIC ws2_32)
endif()

ntplite_apply_cxx_warnings(ntplite_c)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(NTP_LITE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(NTP_LITE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(NTP_LITE_BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
if(NTP_LITE_INSTALL)
  install(
    TARGETS ntplite ntplite_c
    EXPORT ntpliteTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  set(NTP_LITE_CMAKE_INSTALL_DIR
      ${CMAKE_INSTALL_LIBDIR}/cmake/ntplite
      CACHE STRING "Install destination for the CMake package files")

  configure_package_config_file(
    cmake/ntpliteConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ntpliteConfig.cmake"
    INSTALL_DESTINATION ${NTP_LITE_CMAKE_INSTALL_DIR})

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ntpliteConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(EXPORT ntpliteTargets
          FILE ntpliteTargets.cmake
          NAMESPACE ntplite::
          DESTINATION ${NTP_LITE_CMAKE_INSTALL_DIR})

  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ntpliteConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/ntpliteConfigVersion.cmake"
          DESTINATION ${NTP_LITE_CMAKE_INSTALL_DIR})

  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/ntplite)

  configure_file(cmake/ntplite.pc.in "${CMAKE_CURRENT_BINARY_DIR}/ntplite.pc"
                 @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ntplite.pc"
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ntplite ${PROJECT_VERSION}")
message(STATUS "  system ............. ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  C compiler ......... ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler ....... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard ....... c++${CMAKE_CXX_STANDARD}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  tools/examples/tests ${NTP_LITE_BUILD_TOOLS}/${NTP_LITE_BUILD_EXAMPLES}/${NTP_LITE_BUILD_TESTS}")
message(STATUS "  online tests ....... ${NTP_LITE_ONLINE_TESTS}")
message(STATUS "  warnings as errors . ${NTP_LITE_WERROR}")
message(STATUS "  sanitizers ......... ${NTP_LITE_SANITIZE}")
message(STATUS "")
