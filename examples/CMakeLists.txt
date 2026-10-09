# ============================================================================
# ntplite - examples/CMakeLists.txt
# ============================================================================

# C++ consumer: header-only, no extra translation unit, no library to link
# beyond the platform sockets dependency.
add_executable(ntplite_example_cpp cpp_quickstart.cpp)
target_link_libraries(ntplite_example_cpp PRIVATE ntplite::ntplite)
ntplite_apply_cxx_warnings(ntplite_example_cpp)

# C consumer: links the prebuilt C ABI library.  Note that this target never
# sees `cxx_std_11` because ntplite::c keeps its C++ dependency PRIVATE.
add_executable(ntplite_example_c c_quickstart.c)
target_link_libraries(ntplite_example_c PRIVATE ntplite::c)
ntplite_apply_c_warnings(ntplite_example_c)
