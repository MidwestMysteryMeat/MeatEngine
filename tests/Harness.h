#pragma once
// Minimal shared test harness so multiple test files link into one MeatTests
// binary (CMake globs tests/*.cpp) without each carrying its own main(). Each
// test file exposes one `void run<Suite>()` in namespace meattest; TestMain.cpp
// owns main() and calls them. check() feeds shared counters.
#include <cstdio>

namespace meattest {

inline int g_checks = 0;
inline int g_failures = 0;

inline void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("  [pass] %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

} // namespace meattest
