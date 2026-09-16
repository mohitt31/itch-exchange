cmake_minimum_required(VERSION 3.24)
project(itch_exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 0 release, 1 cheap O(1) invariants, 2 also O(n) structure re-derivation.
set(ITCH_INVARIANT_LEVEL "2" CACHE STRING "Runtime invariant checking level (0/1/2)")
set_property(CACHE ITCH_INVARIANT_LEVEL PROPERTY STRINGS 0 1 2)

option(ITCH_NATIVE_ARCH "Tune for the build machine's CPU" ON)

include(CheckCXXCompilerFlag)

add_library(itch_warnings INTERFACE)
target_compile_options(itch_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wold-style-cast
  -Wnon-virtual-dtor
  -Wformat=2
  # There is no floating point below apps/. This is a tripwire, not a style rule.
  -Wdouble-promotion
)

add_library(itch_arch INTERFACE)
if(ITCH_NATIVE_ARCH)
  # -mcpu on ARM, -march elsewhere. GCC accepts -mcpu=native on x86 as a
  # deprecated alias, so probing -mcpu first would succeed and then warn on
  # every translation unit.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM64")
    set(ITCH_ARCH_FLAGS "-mcpu=native" "-march=native")
  else()
    set(ITCH_ARCH_FLAGS "-march=native" "-mcpu=native")
  endif()
  foreach(flag IN LISTS ITCH_ARCH_FLAGS)
    string(MAKE_C_IDENTIFIER "ITCH_HAVE${flag}" flag_var)
    check_cxx_compiler_flag("${flag}" ${flag_var})
    if(${flag_var})
      target_compile_options(itch_arch INTERFACE "${flag}")
      break()
    endif()
  endforeach()
endif()

add_library(itch
  src/assert.cpp
)
target_include_directories(itch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(itch PUBLIC ITCH_INVARIANT_LEVEL=${ITCH_INVARIANT_LEVEL})
target_link_libraries(itch PUBLIC itch_arch PRIVATE itch_warnings)

find_package(ZLIB REQUIRED)

add_library(itch_gzip INTERFACE)
target_link_libraries(itch_gzip INTERFACE itch ZLIB::ZLIB)

add_subdirectory(apps)
add_subdirectory(bench)

enable_testing()
add_subdirectory(tests)
