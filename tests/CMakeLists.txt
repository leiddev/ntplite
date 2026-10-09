# ============================================================================
# ntplite - tests/CMakeLists.txt
# ============================================================================

# ---------------------------------------------------------------------------
# The main test binary.
#
# The explicit source list is intentional: GLOB/CONFIGURE_DEPENDS behave
# inconsistently across generators, and adding a line here is cheap.
# ---------------------------------------------------------------------------
set(NTP_LITE_TEST_SOURCES
    test_main.cpp
    test_error.cpp
    test_ntp_time.cpp
    test_packet.cpp
    test_socket.cpp
    test_time.cpp
    test_version.cpp)

add_executable(ntplite_tests ${NTP_LITE_TEST_SOURCES})
# Both surfaces on purpose: linking ntplite::ntplite (inline, C++) together
# with ntplite::c (out-of-line, C ABI) is the documented combination, and the
# version-parity tests compare the two APIs against each other.  It also proves
# that the header-only path and the prebuilt library do not define overlapping
# symbols.
target_link_libraries(ntplite_tests PRIVATE ntplite::ntplite ntplite::c)
target_include_directories(ntplite_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
ntplite_apply_cxx_warnings(ntplite_tests)

add_test(NAME ntplite.unit COMMAND ntplite_tests)
set_tests_properties(ntplite.unit PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# Header hygiene: every public header must compile on its own, and every
# `.h` header must be includable from plain C.  This is checked by simply
# compiling a generated translation unit per header, so a missing include in
# a header fails the build instead of failing in a user's project.
#
# NOTE: the file list is resolved at configure time; re-run CMake after adding
# a header.
# ---------------------------------------------------------------------------
file(
  GLOB_RECURSE NTP_LITE_PUBLIC_HEADERS
  RELATIVE "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/include/ntplite/*.h"
  "${PROJECT_SOURCE_DIR}/include/ntplite/*.hpp")

set(hygiene_cpp_sources "")
set(hygiene_c_sources "")

foreach(header IN LISTS NTP_LITE_PUBLIC_HEADERS)
  string(MAKE_C_IDENTIFIER "${header}" header_id)
  if(header MATCHES "\\.h$")
    set(extension "c")
  else()
    set(extension "cpp")
  endif()

  set(generated "${CMAKE_CURRENT_BINARY_DIR}/hygiene/${header_id}.${extension}")
  file(WRITE "${generated}" "/* generated: standalone include check */\n#include <${header}>\n")

  if(extension STREQUAL "c")
    list(APPEND hygiene_c_sources "${generated}")
  else()
    list(APPEND hygiene_cpp_sources "${generated}")
  endif()
endforeach()

if(hygiene_cpp_sources)
  add_library(ntplite_header_hygiene_cpp OBJECT ${hygiene_cpp_sources})
  target_include_directories(ntplite_header_hygiene_cpp PRIVATE
                             "${PROJECT_SOURCE_DIR}/include")
  target_compile_features(ntplite_header_hygiene_cpp PRIVATE cxx_std_11)
  ntplite_apply_cxx_warnings(ntplite_header_hygiene_cpp)
endif()

if(hygiene_c_sources)
  add_library(ntplite_header_hygiene_c OBJECT ${hygiene_c_sources})
  target_include_directories(ntplite_header_hygiene_c PRIVATE
                             "${PROJECT_SOURCE_DIR}/include")
  ntplite_apply_c_warnings(ntplite_header_hygiene_c)
endif()

# ---------------------------------------------------------------------------
# ODR check: two translation units that both include the header-only API and
# are linked into a single binary.  If any inline/static decision in the
# library is wrong, this fails to link.
# ---------------------------------------------------------------------------
add_executable(ntplite_odr_test odr/odr_unit_a.cpp odr/odr_unit_b.cpp
                                 odr/odr_main.cpp)
target_link_libraries(ntplite_odr_test PRIVATE ntplite::ntplite)
ntplite_apply_cxx_warnings(ntplite_odr_test)

add_test(NAME ntplite.odr COMMAND ntplite_odr_test)
set_tests_properties(ntplite.odr PROPERTIES TIMEOUT 60)

# ---------------------------------------------------------------------------
# C interoperability: a real C translation unit including the C header and
# linking the prebuilt ntplite::c library.
# ---------------------------------------------------------------------------
add_executable(ntplite_c_consumer c_consumer/main.c)
target_link_libraries(ntplite_c_consumer PRIVATE ntplite::c)
ntplite_apply_c_warnings(ntplite_c_consumer)

add_test(NAME ntplite.c_consumer COMMAND ntplite_c_consumer)
set_tests_properties(ntplite.c_consumer PROPERTIES TIMEOUT 60)
