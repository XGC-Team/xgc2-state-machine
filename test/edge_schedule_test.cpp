#include <gtest/gtest.h>
#include <state_machine/sync_runtime_tick_adapter.hpp>
#include <vector>

TEST(SyncRuntimeTickAdapter, ReplacesCancelsAndRearmsConsumedOneShot) {
    state_machine::SyncRuntimeTickAdapter schedule;
    std::vector<int64_t> calls;
    const auto wake_at = [&](int64_t at) {
        calls.push_back(at);
    };
    schedule.arm(std::nullopt, wake_at);
    schedule.arm(100, wake_at);
    schedule.arm(100, wake_at); // Same owner demand does not restart a phase.
    schedule.arm(50, wake_at);
    schedule.arm(200, wake_at); // Later replaces earlier; no stale minimum remains.
    schedule.arm(std::nullopt, wake_at);
    schedule.arm(std::nullopt, wake_at);
    schedule.arm(200, wake_at);
    schedule.consumed();
    schedule.arm(200, wake_at); // Delivered host one-shot must be armed again.
    schedule.arm(std::nullopt, wake_at);
    EXPECT_EQ(calls, (std::vector<int64_t>{100, 50, 200, -1, 200, 200, -1}));
}
