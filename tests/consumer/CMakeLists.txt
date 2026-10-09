# ============================================================================
# ntplite - tests/consumer/CMakeLists.txt
# ----------------------------------------------------------------------------
# A deliberately separate CMake project that consumes an *installed* ntplite
# through find_package().  CI builds it in .github/workflows/ci.yml (job
# "package") to prove that the exported targets are actually usable.
#
#   cmake -S tests/consumer -B consumer-build -DCMAKE_PREFIX_PATH=<install prefix>
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(ntplite_consumer LANGUAGES C CXX)

find_package(ntplite CONFIG REQUIRED)

message(STATUS "found ntplite ${ntplite_VERSION}")

# C++ consumer: header-only target.
add_executable(consumer_cpp main.cpp)
target_link_libraries(consumer_cpp PRIVATE ntplite::ntplite)

# C consumer: prebuilt C ABI target.
add_executable(consumer_c main.c)
target_link_libraries(consumer_c PRIVATE ntplite::c)
