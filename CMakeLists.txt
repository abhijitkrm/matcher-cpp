cmake_minimum_required(VERSION 3.16)
project(matcher-cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)

# Header-only library target.
add_library(matcher INTERFACE)
target_include_directories(matcher INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(matcher INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(matcher INTERFACE -Wall -Wextra)
endif()

enable_testing()

add_executable(golden_test tests/golden.cpp)
target_link_libraries(golden_test PRIVATE matcher)
add_test(NAME golden COMMAND golden_test ${CMAKE_CURRENT_SOURCE_DIR}/vectors)

add_executable(snapshot_test tests/snapshot.cpp)
target_link_libraries(snapshot_test PRIVATE matcher)
add_test(NAME snapshot COMMAND snapshot_test ${CMAKE_CURRENT_SOURCE_DIR}/vectors)

add_executable(matcherbench bench/matcherbench.cpp)
target_link_libraries(matcherbench PRIVATE matcher)

add_executable(matcherfuzz bench/matcherfuzz.cpp)
target_link_libraries(matcherfuzz PRIVATE matcher)

add_executable(matchersnap bench/matchersnap.cpp)
target_link_libraries(matchersnap PRIVATE matcher)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(matcherbench PRIVATE -O3 -march=native)
endif()

# Install: header-only lib — consumers get #include <matcher/matcher.hpp>.
install(DIRECTORY include/matcher DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS matcher EXPORT matcher-targets)
install(EXPORT matcher-targets NAMESPACE matcher:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/matcher)
