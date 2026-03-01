cmake_minimum_required(VERSION 3.20)
project(orderbook-engine)

set(CMAKE_CXX_STANDARD 20)

# HFT-oriented compiler flags:
#   -O3            Aggressive optimization
#   -march=native  Target host CPU features (AVX2, BMI2, etc.)
#   -flto          Link-time optimization for cross-TU inlining
#   -fno-exceptions  Avoid exception overhead in hot path
#   -fno-rtti      No RTTI needed; saves vtable cost
#   -pthread       Threading support
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto -fno-exceptions -fno-rtti -pthread")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
endif()

include_directories(include)

set(LIB_SOURCES src/orderbook.cpp src/order.cpp)

add_executable(orderbook src/main.cpp ${LIB_SOURCES})
add_executable(latency_benchmark benchmarks/latency_test.cpp ${LIB_SOURCES})

# Tests build without -fno-exceptions (GTest requires them).
add_subdirectory(tests)
