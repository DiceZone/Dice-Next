#pragma once
// ─── Dice!Next — Minimal Test Framework ──────────────────────
// A lightweight header-only test framework (no external deps).
// Usage:
//   TEST(suite_name, test_name) {
//       ASSERT_EQ(a, b);
//       EXPECT_TRUE(cond);
//   }
//   int main() { return dice::test::runAll(); }

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>
#include <cmath>

namespace dice::test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& totalTests()  { static int n = 0; return n; }
inline int& passedTests() { static int n = 0; return n; }
inline int& failedTests() { static int n = 0; return n; }

class TestRegistrar {
public:
    TestRegistrar(const std::string& suite, const std::string& name, std::function<void()> fn) {
        registry().push_back({suite, name, fn});
    }
};

#define TEST(suite, name) \
    static void suite##_##name##_fn(); \
    static ::dice::test::TestRegistrar reg_##suite##_##name(#suite, #name, suite##_##name##_fn); \
    static void suite##_##name##_fn()

#define ASSERT_TRUE(cond) \
    do { \
        ::dice::test::totalTests()++; \
        if (!(cond)) { \
            ::dice::test::failedTests()++; \
            std::cerr << "  FAIL: " << #cond << " (line " << __LINE__ << ")\n"; \
            return; \
        } \
        ::dice::test::passedTests()++; \
    } while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) \
    do { \
        ::dice::test::totalTests()++; \
        if (!((a) == (b))) { \
            ::dice::test::failedTests()++; \
            std::cerr << "  FAIL: " << #a << " == " << #b \
                      << " (got " << (a) << " vs " << (b) << ", line " << __LINE__ << ")\n"; \
            return; \
        } \
        ::dice::test::passedTests()++; \
    } while(0)

#define ASSERT_NE(a, b) \
    do { \
        ::dice::test::totalTests()++; \
        if (((a) == (b))) { \
            ::dice::test::failedTests()++; \
            std::cerr << "  FAIL: " << #a << " != " << #b \
                      << " (both " << (a) << ", line " << __LINE__ << ")\n"; \
            return; \
        } \
        ::dice::test::passedTests()++; \
    } while(0)

#define EXPECT_TRUE(cond) \
    do { \
        ::dice::test::totalTests()++; \
        if (!(cond)) { \
            ::dice::test::failedTests()++; \
            std::cerr << "  FAIL: " << #cond << " (line " << __LINE__ << ")\n"; \
        } else { \
            ::dice::test::passedTests()++; \
        } \
    } while(0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b) \
    do { \
        ::dice::test::totalTests()++; \
        if (!((a) == (b))) { \
            ::dice::test::failedTests()++; \
            std::cerr << "  FAIL: " << #a << " == " << #b \
                      << " (got " << (a) << " vs " << (b) << ", line " << __LINE__ << ")\n"; \
        } else { \
            ::dice::test::passedTests()++; \
        } \
    } while(0)

#define EXPECT_NE(a, b) \
    do { \
        ::dice::test::totalTests()++; \
        if (((a) == (b))) { \
            ::dice::test::failedTests()++; \
            std::cerr << "  FAIL: " << #a << " != " << #b \
                      << " (both " << (a) << ", line " << __LINE__ << ")\n"; \
        } else { \
            ::dice::test::passedTests()++; \
        } \
    } while(0)

inline int runAll() {
    std::cout.sync_with_stdio(true);
    std::cout << "=== Dice!Next Test Suite ===" << std::endl;
    std::cout << "Running " << registry().size() << " test cases..." << std::endl << std::endl;

    std::string lastSuite;
    for (auto& tc : registry()) {
        if (tc.suite != lastSuite) {
            std::cout << "[" << tc.suite << "]" << std::endl;
            lastSuite = tc.suite;
        }
        std::cout << "  " << tc.name << " ... " << std::flush;
        try {
            tc.fn();
            std::cout << "OK" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "ERROR (exception: " << e.what() << ")" << std::endl;
            failedTests()++;
            totalTests()++;
        }
    }

    std::cout << std::endl << "=== Results ===" << std::endl;
    std::cout << "Total:  " << totalTests() << std::endl;
    std::cout << "Passed: " << passedTests() << std::endl;
    std::cout << "Failed: " << failedTests() << std::endl;

    return failedTests() > 0 ? 1 : 0;
}

}  // namespace dice::test
