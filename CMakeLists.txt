cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PODLOGS_VERSION)
  set(PODLOGS_VERSION "dev")
endif()

project(podlogs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PODLOGS_STATIC "Link fully static (musl/Alpine builds)" OFF)
option(PODLOGS_STATIC_STDLIB "Link libstdc++/libgcc statically (glibc builds such as RHEL 8)" OFF)
option(PODLOGS_BUILD_TESTS "Build the unit/e2e tests" ON)

find_package(Threads REQUIRED)

configure_file(src/version.hpp.in ${CMAKE_BINARY_DIR}/generated/version.hpp @ONLY)

add_library(podlogs_core STATIC
  src/checkpoint.cpp
  src/config.cpp
  src/demux.cpp
  src/http.cpp
  src/log.cpp
  src/loki.cpp
  src/metrics.cpp
  src/parse.cpp
  src/podman.cpp
  src/tailer.cpp
  src/util.cpp
)
target_include_directories(podlogs_core PUBLIC src third_party ${CMAKE_BINARY_DIR}/generated)
target_compile_options(podlogs_core PUBLIC -Wall -Wextra -Wpedantic)
target_link_libraries(podlogs_core PUBLIC Threads::Threads)

add_executable(podlogs src/main.cpp)
target_link_libraries(podlogs PRIVATE podlogs_core)

if(PODLOGS_STATIC)
  target_link_options(podlogs PRIVATE -static)
elseif(PODLOGS_STATIC_STDLIB)
  target_link_options(podlogs PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS podlogs RUNTIME DESTINATION bin)

if(PODLOGS_BUILD_TESTS)
  enable_testing()
  add_executable(podlogs_tests
    tests/test_main.cpp
    tests/test_util.cpp
    tests/test_http.cpp
    tests/test_demux.cpp
    tests/test_parse.cpp
    tests/test_loki.cpp
    tests/test_checkpoint.cpp
    tests/test_config.cpp
    tests/test_e2e.cpp
  )
  target_include_directories(podlogs_tests PRIVATE tests)
  target_link_libraries(podlogs_tests PRIVATE podlogs_core)
  if(PODLOGS_STATIC)
    target_link_options(podlogs_tests PRIVATE -static)
  endif()
  add_test(NAME podlogs_tests COMMAND podlogs_tests)
endif()
