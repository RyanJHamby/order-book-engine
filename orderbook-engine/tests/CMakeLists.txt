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
