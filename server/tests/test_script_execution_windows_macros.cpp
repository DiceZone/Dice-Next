#include "test_framework.h"

// Preload the STL, then reproduce Windows min/max macro pollution on every
// platform. Keep this translation unit independent of incidental include order
// or NOMINMAX definitions in other tests.
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#include "common/script_execution.h"
#undef min
#undef max

using namespace dice;

TEST(ScriptExecutionWindowsMacros, NestedExecutionCannotRenewExpiredDeadline) {
    ASSERT_FALSE(ScriptExecution::active());
    {
        ScriptExecution outer(std::chrono::milliseconds(-1));
        ASSERT_TRUE(ScriptExecution::expired());
        {
            ScriptExecution nested(std::chrono::minutes(1));
            ASSERT_TRUE(ScriptExecution::expired());
        }
        ASSERT_TRUE(ScriptExecution::expired());
    }
    ASSERT_FALSE(ScriptExecution::active());
}

TEST(ScriptExecutionWindowsMacros, ShorterNestedBudgetRestoresOuterExecution) {
    ASSERT_FALSE(ScriptExecution::active());
    {
        ScriptExecution outer(std::chrono::minutes(1));
        ASSERT_FALSE(ScriptExecution::expired());
        {
            ScriptExecution nested(std::chrono::milliseconds(-1));
            ASSERT_TRUE(ScriptExecution::expired());
        }
        ASSERT_FALSE(ScriptExecution::expired());
    }
    ASSERT_FALSE(ScriptExecution::active());
}
