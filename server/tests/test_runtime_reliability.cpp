#include "test_framework.h"
#include "test_watchdog.h"
#include "common/bounded_executor.h"
#include "common/bounded_regex.h"
#include <future>

using namespace dice;

TEST(RuntimeReliability, NestedBudgetsCannotRenewDeadline) {
    ScriptExecution outer(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { ScriptExecution nested(std::chrono::seconds(30)); ASSERT_TRUE(ScriptExecution::expired()); }
    ASSERT_TRUE(ScriptExecution::expired());
}

TEST(RuntimeReliability, RegexBacktrackingCannotBlockAndNormalCapturesStillWork) {
    TestWatchdog watchdog;
    auto start = std::chrono::steady_clock::now();
    const auto bad = boundedRegex("^(a+)+$", std::string(100, 'a') + "!");
    ASSERT_FALSE(bad.matched);
    ASSERT_TRUE(!bad.error.empty());
    ASSERT_TRUE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    // The circuit breaker is separate from the next healthy expression.
    const auto good = boundedRegex("测试(.*)", "测试中文", RegexMode::Prefix);
    ASSERT_TRUE(good.matched);
    ASSERT_EQ(good.groups.at(1), std::string("中文"));
    ASSERT_FALSE(boundedRegex("hello", "xhello", RegexMode::Prefix).matched);
    ASSERT_TRUE(boundedRegex("hello", "xHELLO", RegexMode::Search).matched);
    ASSERT_FALSE(boundedRegex("hello", "hello!", RegexMode::Full).matched);
    ASSERT_TRUE(!boundedRegex("[", "", RegexMode::Search, true).error.empty());
}

TEST(RuntimeReliability, WorkerQueueIsBoundedAndExceptionsDoNotKillWorker) {
    TestWatchdog watchdog;
    BoundedExecutor worker(1);
    std::promise<void> entered, resume, done;
    auto resumeFuture = resume.get_future().share();
    ASSERT_TRUE(worker.submit([&] { entered.set_value(); resumeFuture.wait(); throw std::runtime_error("test callback"); }));
    entered.get_future().wait();
    const bool queued = worker.submit([&] { done.set_value(); });
    const bool overflow = worker.submit([] {});
    resume.set_value();
    done.get_future().wait();
    worker.stop();
    ASSERT_TRUE(queued);
    ASSERT_FALSE(overflow);
    ASSERT_FALSE(worker.submit([] {}));
}
