# lrd_bench - the load-testing client. Empty harness in step 0; the real
# concurrent driver and latency histogram land in step 6.
add_executable(lrd_bench main.cpp)
target_link_libraries(lrd_bench PRIVATE lrd_core lrd_options)

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
#
# See the "Measured cost" section of docs/protocol.md for the numbers.
# ---------------------------------------------------------------------------
add_executable(lrd_baseline_pingpong baseline_pingpong.cpp)
target_link_libraries(lrd_baseline_pingpong PRIVATE lrd_options)

add_executable(lrd_baseline_framing_shape baseline_framing_shape.cpp)
target_link_libraries(lrd_baseline_framing_shape PRIVATE lrd_options)
