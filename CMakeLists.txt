cmake_minimum_required(VERSION 3.16)
project(webserver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -DNDEBUG")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)

add_library(ws_core STATIC
  src/buffer.cpp
  src/logger.cpp
  src/memory_pool.cpp
  src/timer.cpp
  src/epoller.cpp
  src/event_loop.cpp
  src/http.cpp
  src/http_parser.cpp
  src/connection.cpp
  src/server.cpp
)
target_include_directories(ws_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ws_core PUBLIC Threads::Threads)

add_executable(ws_server src/main.cpp)
target_link_libraries(ws_server PRIVATE ws_core)

# ---------------------------------------------------------------- tests
option(WS_BUILD_TESTS "Build unit tests" ON)
if(WS_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND AND EXISTS /usr/src/googletest)
    # libgtest-dev on Debian/Ubuntu ships sources, not a prebuilt library.
    add_subdirectory(/usr/src/googletest ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
    set(GTest_FOUND TRUE)
  endif()

  if(GTest_FOUND)
    add_executable(ws_tests
      tests/test_buffer.cpp
      tests/test_http_parser.cpp
      tests/test_timer.cpp
      tests/test_memory_pool.cpp
      tests/test_logger.cpp
    )
    target_link_libraries(ws_tests PRIVATE ws_core GTest::gtest GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(ws_tests)
  else()
    message(STATUS "GoogleTest not found - skipping ws_tests")
  endif()
endif()
