# lrd_bench - the load-testing client. Empty harness in step 0; the real
# concurrent driver and latency histogram land in step 6.
add_executable(lrd_bench main.cpp)
target_link_libraries(lrd_bench PRIVATE lrd_core lrd_options)
