cmake_minimum_required(VERSION 3.20)

# Find Google Test
find_package(GTest REQUIRED)

# Include directories
include_directories(../include)

# Test sources
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

# Create test executable
add_executable(orderbook_tests ${TEST_SOURCES})

# Link with Google Test and main library
target_link_libraries(orderbook_tests 
    GTest::GTest 
    GTest::Main
)

# Add main library sources (exclude the main.cpp entry point).
file(GLOB MAIN_SOURCES "../src/*.cpp")
list(REMOVE_ITEM MAIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../src/main.cpp")
target_sources(orderbook_tests PRIVATE ${MAIN_SOURCES})

# Enable testing
enable_testing()
add_test(NAME OrderBookTests COMMAND orderbook_tests)
