# ============================================================================
# ntplite - tools/CMakeLists.txt
# ============================================================================

add_executable(ntplite_cli ntplite_cli.cpp)

# ntplite::c, not ntplite::ntplite: the tool is written against the C API on
# purpose, so building it is itself a check that the C ABI is complete enough to
# write a program with.  Linking the header-only C++ target instead would make
# that check impossible to fail.
target_link_libraries(ntplite_cli PRIVATE ntplite::c)
ntplite_apply_cxx_warnings(ntplite_cli)

# The binary is called `ntplite`, matching the project.  On Windows this is
# ntplite.exe; on Unix it is a plain executable, so it does not clash with the
# static library `libntplite.a`.
set_target_properties(ntplite_cli PROPERTIES OUTPUT_NAME ntplite)

if(NTP_LITE_INSTALL)
  install(TARGETS ntplite_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
