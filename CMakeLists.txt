cmake_minimum_required(VERSION 3.20)

project(lrd
    VERSION 0.1.0
    DESCRIPTION "Local Recommendation Daemon - on-device caching/ranking daemon with UDS IPC"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language level
#
# C++20, not 23: we want jthread / concepts / <span> / <bit>, all of which are
# C++20. CXX_EXTENSIONS OFF means we compile as -std=c++20 rather than
# -std=gnu++20, so a GNU-only extension fails at compile time here instead of
# silently becoming a portability problem on a different toolchain.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json for clangd / clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Put all binaries in one place so the bench script and the daemon can find
# each other without preset-specific paths.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LRD_WERROR "Treat compiler warnings as errors" ON)
option(LRD_BUILD_TESTS "Build unit tests" ON)
option(LRD_BUILD_BENCH "Build the load-testing client" ON)
set(LRD_SANITIZER "none" CACHE STRING "Sanitizer: none | thread | address | undefined")
set_property(CACHE LRD_SANITIZER PROPERTY STRINGS none thread address undefined)

# ---------------------------------------------------------------------------
# lrd_options: an INTERFACE target carrying our warning flags and sanitizer
# settings. Every target links it, so flags are declared once instead of being
# smeared across add_compile_options() calls that leak into subdirectories.
# ---------------------------------------------------------------------------
add_library(lrd_options INTERFACE)

target_compile_options(lrd_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow             # a shadowed variable in a lock scope is a real bug source
    -Wconversion         # silent narrowing on the wire is how framing bugs start
    -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Wcast-align
    $<$<BOOL:${LRD_WERROR}>:-Werror>)

if(NOT LRD_SANITIZER STREQUAL "none")
    # Sanitizers need the same flags at compile AND link time; an INTERFACE
    # target that sets both is the least error-prone way to guarantee that.
    target_compile_options(lrd_options INTERFACE
        -fsanitize=${LRD_SANITIZER}
        -fno-omit-frame-pointer
        -g)
    target_link_options(lrd_options INTERFACE -fsanitize=${LRD_SANITIZER})
    message(STATUS "lrd: ${LRD_SANITIZER} sanitizer ENABLED")
endif()

# ---------------------------------------------------------------------------
# Protocol Buffers (optional).
#
# MODULE, not CONFIG, and not by accident: protobuf 3.21 as packaged here ships
# no CMake config that CMake 4.2 will accept, so find_package(Protobuf CONFIG)
# fails outright. CMake's own FindProtobuf module works and provides both the
# protobuf::libprotobuf imported target and protobuf_generate_cpp. Verified with
# scripts/probe-protobuf-cmake.sh.
#
# Auto-detected rather than required: the whole of Phase 1 builds without
# protobuf, and someone cloning this repo should not need it to run the tests.
# ---------------------------------------------------------------------------
option(LRD_WITH_PROTOBUF "Build the protobuf codec if protobuf is available" ON)

if(LRD_WITH_PROTOBUF)
    find_package(Protobuf QUIET MODULE)
    if(Protobuf_FOUND)
        message(STATUS "lrd: protobuf ${Protobuf_VERSION} found; ProtobufCodec enabled")
    else()
        message(STATUS "lrd: protobuf not found; ProtobufCodec disabled")
        set(LRD_WITH_PROTOBUF OFF)
    endif()
endif()

# Threads::Threads resolves to -pthread. Using the imported target rather than
# hardcoding -lpthread keeps this correct on toolchains where the flag differs.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_subdirectory(src)

if(LRD_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(LRD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(LRD_BUILD_EXAMPLES "Build the SDK example application" ON)
if(LRD_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Install and export.
#
# What makes `find_package(lrd)` work for a consumer outside this tree, and what
# turns "the SDK is self-contained" from a claim into something checkable -
# scripts/verify-sdk-install.sh installs to a throwaway prefix and builds the
# example against it with no access to these sources.
#
# Only the SDK and the libraries it is built from are installed. lrdd, lrd_cli
# and the benchmarks are ours, not a consumer's.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(LRD_INSTALL_TARGETS lrd_sdk lrd_client lrd_core lrd_options)
if(LRD_WITH_PROTOBUF)
    list(APPEND LRD_INSTALL_TARGETS lrd_proto_gen)
endif()

install(TARGETS ${LRD_INSTALL_TARGETS}
    EXPORT lrdTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The public headers. Deliberately a curated list rather than the whole include
# tree: installing everything would ship the transport and protocol headers and
# quietly make them part of the supported surface.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/lrd/sdk"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lrd)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/lrd/rank"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lrd
        FILES_MATCHING PATTERN "item.hpp" PATTERN "signal.hpp")

install(EXPORT lrdTargets
    FILE lrdTargets.cmake
    NAMESPACE lrd::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lrd)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/lrdConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/lrdConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lrdConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lrd)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/lrdConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/lrdConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lrd)

message(STATUS "lrd ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
               "| C++${CMAKE_CXX_STANDARD} | ${CMAKE_BUILD_TYPE} | sanitizer=${LRD_SANITIZER}")
