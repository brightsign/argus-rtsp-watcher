/**
 * @file test_stall_watchdog.cpp
 * @brief Unit tests for stall_watchdog::StallWatchdog, the thread that notices
 * a recovery step which never returns (see orchestration/stall_watchdog.h).
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "orchestration/stall_watchdog.h"

using namespace std::chrono_literals;
using stall_watchdog::ScopedStallPhase;
using stall_watchdog::StallWatchdog;

namespace {

// Records stall reports and lets a test wait for one without sleeping blindly.
class Recorder {
 public:
    StallWatchdog::StallHandler handler() {
        return [this](const std::string& phase, std::chrono::milliseconds limit) {
            std::lock_guard<std::mutex> lock(mutex_);
            phases_.push_back(phase);
            limits_.push_back(limit);
            reported_.notify_all();
        };
    }

    bool wait_for_reports(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return reported_.wait_for(lock, timeout, [&] { return phases_.size() >= count; });
    }

    std::vector<std::string> phases() {
        std::lock_guard<std::mutex> lock(mutex_);
        return phases_;
    }

    std::vector<std::chrono::milliseconds> limits() {
        std::lock_guard<std::mutex> lock(mutex_);
        return limits_;
    }

 private:
    std::mutex mutex_;
    std::condition_variable reported_;
    std::vector<std::string> phases_;
    std::vector<std::chrono::milliseconds> limits_;
};

constexpr auto kShortLimit = 60ms;
constexpr auto kGenerousWait = 5s;  // an upper bound for waiting on a report that must come
constexpr auto kQuietPeriod = 400ms;  // long enough that a wrongly-armed 60 ms phase would have fired

}  // namespace

TEST(StallWatchdog, ReportsAPhaseThatOverrunsItsLimit) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    watchdog.arm("open input", kShortLimit);

    ASSERT_TRUE(recorder.wait_for_reports(1, kGenerousWait));
    EXPECT_EQ(recorder.phases().front(), "open input");
    EXPECT_EQ(recorder.limits().front(), kShortLimit);
}

TEST(StallWatchdog, DoesNotReportAPhaseDisarmedInTime) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    watchdog.arm("stop workers", 200ms);
    watchdog.disarm();

    EXPECT_FALSE(recorder.wait_for_reports(1, kQuietPeriod + 200ms));
}

TEST(StallWatchdog, ReportsEachStallOnlyOnce) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    watchdog.arm("release old pipeline", kShortLimit);
    ASSERT_TRUE(recorder.wait_for_reports(1, kGenerousWait));
    // Nothing disarmed it, yet it must not keep firing every wake-up.
    EXPECT_FALSE(recorder.wait_for_reports(2, kQuietPeriod));
    EXPECT_EQ(recorder.phases().size(), 1u);
}

TEST(StallWatchdog, ReArmingReplacesThePreviousPhase) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    watchdog.arm("first", kShortLimit);
    watchdog.arm("second", 10s);  // replaces "first" before it can fire

    EXPECT_FALSE(recorder.wait_for_reports(1, kQuietPeriod));
    EXPECT_TRUE(recorder.phases().empty());
}

TEST(StallWatchdog, ReArmingStartsTheNewLimitFromNow) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    watchdog.arm("first", 10s);
    watchdog.arm("second", kShortLimit);

    ASSERT_TRUE(recorder.wait_for_reports(1, kGenerousWait));
    EXPECT_EQ(recorder.phases().front(), "second");
}

TEST(StallWatchdog, CanBeArmedAgainAfterAStall) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    watchdog.arm("one", kShortLimit);
    ASSERT_TRUE(recorder.wait_for_reports(1, kGenerousWait));
    watchdog.arm("two", kShortLimit);

    ASSERT_TRUE(recorder.wait_for_reports(2, kGenerousWait));
    EXPECT_EQ(recorder.phases()[1], "two");
}

TEST(StallWatchdog, ScopedPhaseDisarmsWhenTheScopeEnds) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    {
        ScopedStallPhase phase(watchdog, "load models", 200ms);
    }

    EXPECT_FALSE(recorder.wait_for_reports(1, kQuietPeriod + 200ms));
}

TEST(StallWatchdog, ScopedPhaseDisarmsOnAnEarlyReturn) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    auto step_that_fails_early = [&] {
        ScopedStallPhase phase(watchdog, "open input", 200ms);
        return false;  // recover_pipeline returns false from inside such scopes
    };
    EXPECT_FALSE(step_that_fails_early());

    EXPECT_FALSE(recorder.wait_for_reports(1, kQuietPeriod + 200ms));
}

TEST(StallWatchdog, ScopedPhaseStillArmedWhenTheScopeOverruns) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    ScopedStallPhase phase(watchdog, "stop workers", kShortLimit);

    ASSERT_TRUE(recorder.wait_for_reports(1, kGenerousWait));
    EXPECT_EQ(recorder.phases().front(), "stop workers");
}

TEST(StallWatchdog, DestroyingWhileArmedStopsPromptlyAndDoesNotReport) {
    Recorder recorder;
    const auto start = std::chrono::steady_clock::now();
    {
        StallWatchdog watchdog(recorder.handler());
        watchdog.arm("never finishes", 60s);
    }  // destructor must not wait out the 60 s limit
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 5s);
    EXPECT_TRUE(recorder.phases().empty());
}

TEST(StallWatchdog, NeverArmedNeverReports) {
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    EXPECT_FALSE(recorder.wait_for_reports(1, kQuietPeriod));
}

TEST(StallWatchdog, ManyArmDisarmCyclesNeverFalselyReport) {
    // The recovery path arms and disarms around every phase; a race between a
    // disarm and the deadline must never turn into a spurious kill.
    Recorder recorder;
    StallWatchdog watchdog(recorder.handler());

    for (int i = 0; i < 2000; ++i) {
        ScopedStallPhase phase(watchdog, "cycle", 500ms);
    }

    EXPECT_FALSE(recorder.wait_for_reports(1, 700ms));
}
