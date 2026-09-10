# ---------------------------------------------------------------------------
# lrd_core - everything reusable: transport, framing, codec, cache, pool.
# Built as a static library so the daemon, the client library, the bench tool
# and the unit tests all compile it exactly once and share one definition.
# ---------------------------------------------------------------------------
add_library(lrd_core STATIC
    common/version.cpp)

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
# lrdd - the daemon binary.
# ---------------------------------------------------------------------------
add_executable(lrdd
    daemon/main.cpp)

target_link_libraries(lrdd PRIVATE lrd_core lrd_options)
