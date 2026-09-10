# Each test file becomes its own executable and its own CTest entry, so a crash
# or a hang in one area cannot take the whole suite's reporting down with it -
# which matters here because we will be testing threads and blocking sockets.
function(lrd_add_test name)
    cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})
    add_executable(${name} ${name}.cpp test_main.cpp)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} lrd_core lrd_options)
    add_test(NAME ${name} COMMAND ${name})
    # A deadlocked thread-pool test would otherwise hang a CI run forever.
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

lrd_add_test(test_version)
lrd_add_test(test_fd)
lrd_add_test(test_io)
lrd_add_test(test_unix_socket)
lrd_add_test(test_wire)
lrd_add_test(test_codec)
lrd_add_test(test_framing)
lrd_add_test(test_lru_cache)
lrd_add_test(test_thread_pool)
lrd_add_test(test_locked_cache)
lrd_add_test(test_handler LIBS lrd_daemon)
lrd_add_test(test_server LIBS lrd_daemon lrd_client)
