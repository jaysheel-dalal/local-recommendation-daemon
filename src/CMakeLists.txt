# ---------------------------------------------------------------------------
# lrd_proto_gen - protoc output.
#
# A separate target on purpose: generated code is deliberately NOT compiled with
# lrd_options. Our warning set (-Wconversion, -Wold-style-cast, -Werror and the
# rest) is a style contract for code we write and review; holding a code
# generator to it means either a wall of suppressions or patching generated
# files, and it would break on the next protoc release. The include directory is
# marked SYSTEM so that warnings from lrd.pb.h do not fire in our translation
# units either.
# ---------------------------------------------------------------------------
if(LRD_WITH_PROTOBUF)
    protobuf_generate_cpp(LRD_PROTO_SRCS LRD_PROTO_HDRS
        "${CMAKE_SOURCE_DIR}/proto/lrd.proto")

    add_library(lrd_proto_gen STATIC ${LRD_PROTO_SRCS} ${LRD_PROTO_HDRS})

    # SYSTEM: suppresses generated-header warnings for every consumer.
    #
    # BUILD_INTERFACE only, with no INSTALL_INTERFACE: the generated headers are
    # an implementation detail and are not installed, so an installed consumer
    # must never be told to look for them. An exported target carrying a raw
    # build-tree path is an error CMake catches at generate time.
    target_include_directories(lrd_proto_gen SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
    target_link_libraries(lrd_proto_gen PUBLIC protobuf::libprotobuf)
endif()

# ---------------------------------------------------------------------------
# lrd_core - everything reusable: transport, framing, codec, cache, pool.
# Built as a static library so the daemon, the client library, the bench tool
# and the unit tests all compile it exactly once and share one definition.
# ---------------------------------------------------------------------------
add_library(lrd_core STATIC
    common/version.cpp
    common/fd.cpp
    common/errors.cpp
    common/log.cpp
    net/io.cpp
    net/unix_socket.cpp
    proto/wire.cpp
    proto/message.cpp
    proto/codec.cpp
    proto/framing.cpp
    concurrency/thread_pool.cpp
    rank/scorer.cpp
    policy/exposure_store.cpp)

# PUBLIC on the include dir: anything linking lrd_core also gets include/ on its
# header search path. That is why tests and bench can #include <lrd/...> without
# repeating this line.
# BUILD_INTERFACE / INSTALL_INTERFACE rather than a bare path: the two differ
# once the target is exported, and CMake refuses to export a target that names a
# source-tree directory a consumer will not have.
target_include_directories(lrd_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# LRD_VERSION comes from project(... VERSION) so the version lives in exactly
# one place. PRIVATE: only lrd_core's own sources need it.
target_compile_definitions(lrd_core PRIVATE
    LRD_VERSION="${PROJECT_VERSION}")

target_link_libraries(lrd_core
    PUBLIC Threads::Threads
    PRIVATE lrd_options)

if(LRD_WITH_PROTOBUF)
    target_sources(lrd_core PRIVATE proto/protobuf_codec.cpp)
    target_link_libraries(lrd_core PUBLIC lrd_proto_gen)
    # PUBLIC so that tests and benchmarks can #ifdef on the same symbol lrd_core
    # was compiled with, rather than guessing.
    target_compile_definitions(lrd_core PUBLIC LRD_WITH_PROTOBUF)
endif()

# ---------------------------------------------------------------------------
# lrd_client - the client library. Separate from the lrd_cli binary so that
# tests can link the protocol client without dragging in a main(). This is the
# target that becomes the Phase 2 SDK.
# ---------------------------------------------------------------------------
add_library(lrd_client STATIC
    client/connection.cpp)

target_link_libraries(lrd_client
    PUBLIC lrd_core
    PRIVATE lrd_options)

# ---------------------------------------------------------------------------
# lrd_daemon - the daemon's request handling, minus its main(). Split out for
# the same reason: the handler is pure logic (Request in, Response out) and is
# worth testing without a socket in the way.
# ---------------------------------------------------------------------------
add_library(lrd_daemon STATIC
    daemon/handler.cpp
    daemon/connection_registry.cpp
    daemon/server.cpp)

target_link_libraries(lrd_daemon
    PUBLIC lrd_core
    PRIVATE lrd_options)

# ---------------------------------------------------------------------------
# lrd_sdk - the public library other applications link against.
#
# Linked PRIVATE against lrd_client and lrd_core: a consumer gets the SDK's
# headers and nothing else. That is the whole point - if this were PUBLIC, every
# consumer would inherit the protocol and transport headers on their include
# path and the pimpl would be decorative.
# ---------------------------------------------------------------------------
add_library(lrd_sdk STATIC
    sdk/client.cpp)

target_include_directories(lrd_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(lrd_sdk
    PRIVATE lrd_client lrd_core lrd_options)

# Consumers still need a threading runtime at link time even though they never
# see a thread in the API.
target_link_libraries(lrd_sdk PUBLIC Threads::Threads)

# Two different things, both needed:
#   ALIAS gives `lrd::sdk` inside this build tree (used by examples/).
#   EXPORT_NAME gives `lrd::sdk` to an *installed* consumer - without it the
#   exported target is `lrd::lrd_sdk`, because the export namespace is prefixed
#   onto the raw target name. An ALIAS is not exported, so the two are unrelated
#   and it is entirely possible to have one and not the other.
add_library(lrd::sdk ALIAS lrd_sdk)
set_target_properties(lrd_sdk PROPERTIES EXPORT_NAME sdk)

# ---------------------------------------------------------------------------
# Binaries.
# ---------------------------------------------------------------------------
add_executable(lrdd daemon/main.cpp)
target_link_libraries(lrdd PRIVATE lrd_daemon lrd_core lrd_options)

add_executable(lrd_cli client/main.cpp)
target_link_libraries(lrd_cli PRIVATE lrd_client lrd_core lrd_options)
