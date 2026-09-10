# ---------------------------------------------------------------------------
# lrd_bench        - end-to-end: N client threads over real sockets. Measures
#                    what a client experiences, syscalls and all.
# lrd_cache_bench  - in-process: the same LockedCache with no sockets in the
#                    way. Measures the lock, which the end-to-end number cannot
#                    see because IPC is ~300x larger. See the comment at the top
#                    of cache_bench.cpp.
#
# Quote numbers from the release preset only.
# ---------------------------------------------------------------------------
add_executable(lrd_bench main.cpp)
target_link_libraries(lrd_bench PRIVATE lrd_client lrd_core lrd_options)

add_executable(lrd_cache_bench cache_bench.cpp)
target_link_libraries(lrd_cache_bench PRIVATE lrd_core lrd_options)

# ---------------------------------------------------------------------------
# Baseline probes.
#
# These deliberately do NOT link lrd_core. They are bare two-process ping-pongs
# that establish what an IPC round trip costs on the machine under test, so that
# the daemon's own numbers can be read as "X above the floor" rather than as an
# absolute that says nothing on its own. WSL2 in particular has syscall overhead
# several times a native kernel's, and without a floor to compare against every
# measurement of the daemon looks alarming.
#
#   lrd_baseline_pingpong        one write + one read per side
#   lrd_baseline_framing_shape   compares one read per message against the
#                                two (length prefix, then body) that read_frame
#                                actually issues
#   lrd_baseline_timer           how late sleep_until actually wakes. The
#                                open-loop benchmark charges any oversleep to
#                                the server, so this says how much of an
#                                open-loop latency figure is really the
#                                scheduler.
#
# See the "Measured cost" section of docs/protocol.md for the numbers.
# ---------------------------------------------------------------------------
add_executable(lrd_baseline_pingpong baseline_pingpong.cpp)
target_link_libraries(lrd_baseline_pingpong PRIVATE lrd_options)

add_executable(lrd_baseline_framing_shape baseline_framing_shape.cpp)
target_link_libraries(lrd_baseline_framing_shape PRIVATE lrd_options)

add_executable(lrd_baseline_timer baseline_timer.cpp)
target_link_libraries(lrd_baseline_timer PRIVATE Threads::Threads lrd_options)
