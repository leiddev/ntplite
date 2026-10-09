# ============================================================================
# ntplite - tools/CMakeLists.txt
# ============================================================================

add_executable(ntplite_cli ntplite_cli.cpp)
target_link_libraries(ntplite_cli PRIVATE ntplite::ntplite)
ntplite_apply_cxx_warnings(ntplite_cli)

# The binary is called `ntplite`, matching the project.  On Windows this is
# ntplite.exe; on Unix it is a plain executable, so it does not clash with the
# static library `libntplite.a`.
set_target_properties(ntplite_cli PROPERTIES OUTPUT_NAME ntplite)
