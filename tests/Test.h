#pragma once
// Minimal self-registering test harness (no third-party dependency).
#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

namespace Test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Cases()
{
    static std::vector<Case> cases;
    return cases;
}

inline int& Failures()
{
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }
};

// Runs every case (or only those containing `filter`); returns the process exit code.
inline int RunAll(std::string_view filter = {})
{
    int failedCases = 0;
    for (const Case& c : Cases()) {
        if (!filter.empty() && std::string_view{c.name}.find(filter) == std::string_view::npos)
            continue;
        const int before = Failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  unexpected exception: %s\n", e.what());
            ++Failures();
        }
        const bool ok = Failures() == before;
        failedCases += ok ? 0 : 1;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    }
    std::printf("%d case(s) failed\n", failedCases);
    std::fflush(stdout); // sanitizers may _exit() before stdio is flushed
    return failedCases == 0 ? 0 : 1;
}

} // namespace Test

#define TEST_CASE(name)                                            \
    static void name();                                            \
    static const ::Test::Registrar name##_registrar{#name, name}; \
    static void name()

#define CHECK(expr)                                                                           \
    do {                                                                                      \
        if (!(expr)) {                                                                        \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
            ++::Test::Failures();                                                             \
        }                                                                                     \
    } while (0)
