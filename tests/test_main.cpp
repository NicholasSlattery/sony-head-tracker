// test_main.cpp
// Runs every registered test case and prints a summary. Exit code is non-zero if
// any assertion failed, so CI can gate on it.
#include "test_framework.hpp"

#include <cstdio>

int main() {
    int passed = 0;
    for (const auto& c : test::registry()) {
        const int before = test::failures();
        c.fn();
        if (test::failures() == before) { ++passed; std::printf("PASS  %s\n", c.name); }
        else                            {              std::printf("FAIL  %s\n", c.name); }
    }
    std::printf("\n%d/%zu test(s) passed, %d assertion failure(s)\n",
                passed, test::registry().size(), test::failures());
    return test::failures() ? 1 : 0;
}
