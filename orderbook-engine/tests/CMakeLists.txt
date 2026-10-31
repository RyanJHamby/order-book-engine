cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

include_directories(../include)

set(TEST_SOURCES
    test_order.cpp
    test_orderbook.cpp
    test_order_queue.cpp
    test_memory_pool.cpp
    test_integration.cpp
    test_performance.cpp
    test_edge_cases.cpp
    test_concurrency.cpp
    test_differential.cpp
)

add_executable(orderbook_tests ${TEST_SOURCES})

# GTest requires exceptions and RTTI — override parent flags for test target.
target_compile_options(orderbook_tests PRIVATE -fexceptions -frtti)

target_link_libraries(orderbook_tests
    GTest::GTest
    GTest::Main
)

# Library sources (explicit list, no glob).
target_sources(orderbook_tests PRIVATE
    ../src/orderbook.cpp
    ../src/order.cpp
)

enable_testing()
add_test(NAME OrderBookTests COMMAND orderbook_tests)

# Spec reject-duplicate-order-id: separate target so the main suite stays green.
# WILL_FAIL makes ctest pass while the feature is missing; the implementing
# change deletes the set_tests_properties(... WILL_FAIL ...) line.
add_executable(orderbook_duplicate_id_tests
    test_duplicate_order_id.cpp
    ../src/orderbook.cpp
    ../src/order.cpp
)
target_compile_options(orderbook_duplicate_id_tests PRIVATE -fexceptions -frtti)
target_link_libraries(orderbook_duplicate_id_tests GTest::GTest GTest::Main)
add_test(NAME DuplicateOrderIdTests COMMAND orderbook_duplicate_id_tests)
set_tests_properties(DuplicateOrderIdTests PROPERTIES WILL_FAIL TRUE)
