// MeetMind — 轻量测试框架（零外部依赖）
// 设计目标：不引入 gtest/catch2，避免额外的构建与分发成本，同时提供足够表达力。
//
// 用法：
//   MM_TEST(suite_name, case_name) {
//       MM_EXPECT_EQ(1 + 1, 2);
//       MM_REQUIRE_TRUE(ptr != nullptr);   // 失败则中止本用例
//   }
#pragma once

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mmtest {

/// 致命断言失败时抛出，用于中止当前用例。
class TestFailure : public std::exception {
public:
    explicit TestFailure(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

class Registry {
public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    void add(const std::string& suite, const std::string& name, std::function<void()> fn) {
        cases_.push_back(TestCase{suite, name, std::move(fn)});
    }

    const std::vector<TestCase>& cases() const { return cases_; }

    /// @return 失败的用例数（0 表示全部通过）
    int run(bool verbose, const std::string& filter) {
        int passed = 0;
        int failed = 0;
        int skipped = 0;
        std::vector<std::string> failures;

        std::cout << "=============== MeetMind 测试开始 ===============\n";
        for (const TestCase& tc : cases_) {
            const std::string full = tc.suite + "." + tc.name;
            if (!filter.empty() && full.find(filter) == std::string::npos) {
                ++skipped;
                continue;
            }
            currentTest_ = full;
            failureCount_ = 0;
            const auto t0 = std::chrono::steady_clock::now();
            std::string verdict = "PASS";
            std::string detail;
            try {
                tc.fn();
            } catch (const TestFailure& e) {
                verdict = "FAIL";
                detail = e.what();
            } catch (const std::exception& e) {
                verdict = "FAIL";
                detail = std::string("未捕获异常: ") + e.what();
            } catch (...) {
                verdict = "FAIL";
                detail = "未捕获的未知异常";
            }
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

            if (failureCount_ > 0 && verdict == "PASS") verdict = "FAIL";
            if (verdict == "PASS") {
                ++passed;
                if (verbose) {
                    std::printf("  [PASS] %-56s %7.2f ms\n", full.c_str(), ms);
                }
            } else {
                ++failed;
                std::printf("  [FAIL] %-56s %7.2f ms\n", full.c_str(), ms);
                if (!detail.empty()) std::printf("         %s\n", detail.c_str());
                failures.push_back(full + (detail.empty() ? "" : " -> " + detail));
            }
        }
        currentTest_.clear();

        std::cout << "-----------------------------------------------\n";
        std::printf("合计 %zu 个用例：通过 %d，失败 %d，跳过 %d\n",
                    cases_.size(), passed, failed, skipped);
        if (!failures.empty()) {
            std::cout << "失败清单:\n";
            for (const std::string& f : failures) std::cout << "  - " << f << "\n";
        }
        std::cout << "=============== 测试结束 ===============\n";
        return failed;
    }

    void recordFailure(const char* file, int line, const std::string& message) {
        ++failureCount_;
        std::printf("         %s:%d 断言失败: %s\n", baseName(file), line, message.c_str());
    }

    void recordSkipNote(const std::string& note) { (void)note; }

    static const char* baseName(const char* path) {
        const char* p = std::strrchr(path, '/');
        const char* q = std::strrchr(path, '\\');
        const char* best = path;
        if (p && p > best) best = p;
        if (q && q > best) best = q;
        return best == path ? path : best + 1;
    }

private:
    std::vector<TestCase> cases_;
    std::string currentTest_;
    int failureCount_ = 0;
};

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        Registry::instance().add(suite, name, std::move(fn));
    }
};

// ---- 内部辅助 ----
template <typename T>
std::string describe(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}
inline std::string describe(const std::string& value) { return "\"" + value + "\""; }
inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(const std::vector<std::string>& value) {
    std::string s = "[";
    for (size_t i = 0; i < value.size(); ++i) {
        if (i) s += ", ";
        s += "\"" + value[i] + "\"";
    }
    return s + "]";
}

}  // namespace mmtest

// ---------------------------------------------------------------- 宏

#define MM_TEST(suite, name)                                                              \
    static void mm_test_##suite##_##name();                                               \
    static ::mmtest::Registrar mm_reg_##suite##_##name(#suite, #name,                     \
                                                       mm_test_##suite##_##name);         \
    static void mm_test_##suite##_##name()

#define MM_EXPECT_TRUE(expr)                                                              \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            ::mmtest::Registry::instance().recordFailure(__FILE__, __LINE__,              \
                                                         "期望为真: " #expr);              \
        }                                                                                 \
    } while (false)

#define MM_EXPECT_FALSE(expr)                                                             \
    do {                                                                                  \
        if ((expr)) {                                                                     \
            ::mmtest::Registry::instance().recordFailure(__FILE__, __LINE__,              \
                                                         "期望为假: " #expr);              \
        }                                                                                 \
    } while (false)

#define MM_EXPECT_EQ(a, b)                                                                \
    do {                                                                                  \
        const auto& mm_a = (a);                                                           \
        const auto& mm_b = (b);                                                           \
        if (!(mm_a == mm_b)) {                                                            \
            ::mmtest::Registry::instance().recordFailure(                                 \
                __FILE__, __LINE__,                                                       \
                std::string("期望 " #a " == " #b "，实际: ") + ::mmtest::describe(mm_a) +  \
                    " vs " + ::mmtest::describe(mm_b));                                   \
        }                                                                                 \
    } while (false)

#define MM_EXPECT_NE(a, b)                                                                \
    do {                                                                                  \
        const auto& mm_a = (a);                                                           \
        const auto& mm_b = (b);                                                           \
        if ((mm_a == mm_b)) {                                                             \
            ::mmtest::Registry::instance().recordFailure(                                 \
                __FILE__, __LINE__,                                                       \
                std::string("期望 " #a " != " #b "，两者均为 ") + ::mmtest::describe(mm_a)); \
        }                                                                                 \
    } while (false)

#define MM_EXPECT_NEAR(a, b, eps)                                                         \
    do {                                                                                  \
        const double mm_a = static_cast<double>(a);                                       \
        const double mm_b = static_cast<double>(b);                                       \
        const double mm_e = static_cast<double>(eps);                                     \
        if (std::fabs(mm_a - mm_b) > mm_e) {                                              \
            ::mmtest::Registry::instance().recordFailure(                                 \
                __FILE__, __LINE__,                                                       \
                std::string("期望 " #a " ≈ " #b " (容差 ") + std::to_string(mm_e) +       \
                    ")，实际 " + std::to_string(mm_a) + " vs " + std::to_string(mm_b));    \
        }                                                                                 \
    } while (false)

#define MM_EXPECT_GT(a, b) MM_EXPECT_TRUE((a) > (b))
#define MM_EXPECT_LT(a, b) MM_EXPECT_TRUE((a) < (b))
#define MM_EXPECT_GE(a, b) MM_EXPECT_TRUE((a) >= (b))
#define MM_EXPECT_LE(a, b) MM_EXPECT_TRUE((a) <= (b))

#define MM_EXPECT_CONTAINS(haystack, needle)                                              \
    do {                                                                                  \
        const std::string mm_h = (haystack);                                              \
        const std::string mm_n = (needle);                                                \
        if (mm_h.find(mm_n) == std::string::npos) {                                       \
            ::mmtest::Registry::instance().recordFailure(                                 \
                __FILE__, __LINE__,                                                       \
                std::string("期望包含 \"") + mm_n + "\"，实际内容: " + mm_h);             \
        }                                                                                 \
    } while (false)

#define MM_REQUIRE_TRUE(expr)                                                             \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            ::mmtest::Registry::instance().recordFailure(__FILE__, __LINE__,              \
                                                         "必需条件不满足: " #expr);        \
            throw ::mmtest::TestFailure("必需条件不满足: " #expr);                        \
        }                                                                                 \
    } while (false)

#define MM_FAIL(msg)                                                                      \
    do {                                                                                  \
        ::mmtest::Registry::instance().recordFailure(__FILE__, __LINE__, (msg));          \
        throw ::mmtest::TestFailure(msg);                                                 \
    } while (false)
