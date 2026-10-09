// ============================================================================
// ntplite - tests/test_main.cpp
// ----------------------------------------------------------------------------
// The one translation unit that owns main().  Every other test file just
// registers cases at static-initialisation time.
// ============================================================================

#include "ntplite_test.hpp"

int main(int argc, char** argv) {
  return ntplite_test::run_all(argc, argv);
}
