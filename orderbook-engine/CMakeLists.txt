cmake_minimum_required(VERSION 3.20)
project(orderbook-engine)

set(CMAKE_CXX_STANDARD 20)

# ThreadSanitizer build: instruments the lock-free SPSC queue and matching
# pipeline for data races. Mutually exclusive with the HFT release flags
# below (TSan wants -O1 and no LTO for accurate, symbolized stack traces).
option(ENABLE_TSAN "Build with ThreadSanitizer instrumentation" OFF)

# perf/flame-graph build: keeps the release optimization level (-O3, LTO,
# etc.) so the profile reflects real hot-path behavior, but adds debug
# symbols and frame pointers so `perf record -g` can symbolize and unwind
# stacks. Linux-only in practice (perf); see scripts/cloud_init.sh.
option(ENABLE_PROFILING "Add -g -fno-omit-frame-pointer to the release build for perf" OFF)

if(ENABLE_TSAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -g -O1 -fno-omit-frame-pointer -fno-exceptions -fno-rtti -pthread")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
else()
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
  if(ENABLE_PROFILING)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer")
  endif()
endif()

include_directories(include)

set(LIB_SOURCES src/orderbook.cpp src/order.cpp)

add_executable(orderbook src/main.cpp ${LIB_SOURCES})
add_executable(latency_benchmark benchmarks/latency_test.cpp ${LIB_SOURCES})

# Tests build without -fno-exceptions (GTest requires them).
add_subdirectory(tests)
