// test_framework.hpp
// A tiny dependency-free test harness (no third-party framework, in keeping with
// the project's zero-dependency ethos). Each TEST() self-registers; test_main.cpp
// runs the registry and reports pass/fail counts.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case { const char* name; std::function<void()> fn; };

inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
    std::printf("    FAIL %s:%d  %s\n", file, line, message.c_str());
    ++failures();
}

} // namespace test

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::test::Register test_register_##name(#name, name);            \
    static void name()

#define CHECK(cond)                                                       \
    do { if (!(cond)) ::test::reportFailure(__FILE__, __LINE__, "CHECK failed: " #cond); } while (0)

#define CHECK_NEAR(a, b, eps)                                             \
    do {                                                                  \
        const double _a = (a), _b = (b);                                  \
        if (std::fabs(_a - _b) > (eps))                                   \
            ::test::reportFailure(__FILE__, __LINE__,                     \
                std::string("CHECK_NEAR failed: " #a " (") + std::to_string(_a) + ") vs " #b " (" + std::to_string(_b) + ")"); \
    } while (0)
