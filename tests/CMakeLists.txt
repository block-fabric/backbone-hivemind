add_executable(bt_tests test_hivemind_types.cpp test_hivemind_oracle.cpp test_hivemind_mempool.cpp test_hivemind_replay.cpp)
target_link_libraries(bt_tests PRIVATE bt_test_harness bt_core_test_cases hivemind_core bt_core)
target_compile_definitions(bt_tests PRIVATE BT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME bt_tests COMMAND bt_tests)
