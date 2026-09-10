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
    proto/framing.cpp)

# PUBLIC on the include dir: anything linking lrd_core also gets include/ on its
# header search path. That is why tests and bench can #include <lrd/...> without
# repeating this line.
target_include_directories(lrd_core PUBLIC
    "${CMAKE_SOURCE_DIR}/include")

# LRD_VERSION comes from project(... VERSION) so the version lives in exactly
# one place. PRIVATE: only lrd_core's own sources need it.
target_compile_definitions(lrd_core PRIVATE
    LRD_VERSION="${PROJECT_VERSION}")

target_link_libraries(lrd_core
    PUBLIC Threads::Threads
    PRIVATE lrd_options)

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
    daemon/handler.cpp)

target_link_libraries(lrd_daemon
    PUBLIC lrd_core
    PRIVATE lrd_options)

# ---------------------------------------------------------------------------
# Binaries.
# ---------------------------------------------------------------------------
add_executable(lrdd daemon/main.cpp)
target_link_libraries(lrdd PRIVATE lrd_daemon lrd_core lrd_options)

add_executable(lrd_cli client/main.cpp)
target_link_libraries(lrd_cli PRIVATE lrd_client lrd_core lrd_options)
