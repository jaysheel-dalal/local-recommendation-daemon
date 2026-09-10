# Each test file becomes its own executable and its own CTest entry, so a crash
# or a hang in one area cannot take the whole suite's reporting down with it -
# which matters here because we will be testing threads and blocking sockets.
function(lrd_add_test name)
    add_executable(${name} ${name}.cpp test_main.cpp)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE lrd_core lrd_options)
    add_test(NAME ${name} COMMAND ${name})
    # A deadlocked thread-pool test would otherwise hang a CI run forever.
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

lrd_add_test(test_version)
