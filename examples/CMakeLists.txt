# The SDK example.
#
# Links `lrd_sdk` and nothing else - deliberately NOT lrd_core. If the example
# ever needs an internal target to build, the SDK has stopped being
# self-contained and this build fails, which is the point.
add_executable(lrd_example_app recommender_app.cpp)
target_link_libraries(lrd_example_app PRIVATE lrd_sdk)

# lrd_options is applied here too: the example is our code and is held to the
# same warning standard as the rest of the project.
target_link_libraries(lrd_example_app PRIVATE lrd_options)
