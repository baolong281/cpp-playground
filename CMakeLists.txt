cmake_minimum_required(VERSION 3.20)
project(cpp_playground CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

add_executable(main src/main.cpp src/greet.cpp)
target_include_directories(main PRIVATE include)

find_package(Threads REQUIRED)
enable_testing()

add_executable(hashmap_tests tests/test_hashmap.cpp)
target_include_directories(hashmap_tests PRIVATE include)
target_link_libraries(hashmap_tests PRIVATE Threads::Threads)
add_test(NAME hashmap_tests COMMAND hashmap_tests)

option(ENABLE_TSAN "Build hashmap_tests_tsan with ThreadSanitizer" ON)
if(ENABLE_TSAN)
  add_executable(hashmap_tests_tsan tests/test_hashmap.cpp)
  target_include_directories(hashmap_tests_tsan PRIVATE include)
  target_link_libraries(hashmap_tests_tsan PRIVATE Threads::Threads)
  target_compile_options(hashmap_tests_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(hashmap_tests_tsan PRIVATE -fsanitize=thread)
  add_test(NAME hashmap_tests_tsan COMMAND hashmap_tests_tsan)
endif()
