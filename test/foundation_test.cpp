#include <gtest/gtest.h>
#include <state_machine/next_wake.hpp>
#include <state_machine/ordered_worker.hpp>
#include <state_machine/signal_slot.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;

TEST(SignalSlot, ReplacesOnlyTheReplaceableFactAndTransfersOwnership) {
    state_machine::SignalSlot<std::unique_ptr<int>> slot;
    EXPECT_EQ(slot.value(), nullptr);
    slot.replace(std::make_unique<int>(3));
    slot.replace(std::make_unique<int>(7));
    ASSERT_NE(slot.value(), nullptr);
    EXPECT_EQ(**slot.value(), 7);
    auto taken = slot.take();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(**taken, 7);
    EXPECT_EQ(slot.value(), nullptr);
    EXPECT_FALSE(slot.take().has_value());
}

TEST(NextWake, EarliestWithinOneTurnAndReplacementAcrossTurns) {
    state_machine::NextWake<int> domain;
    EXPECT_FALSE(domain.deadline().has_value());
    domain.at(12);
    domain.at(20);
    domain.at(9);
    state_machine::NextWake<int> child;
    child.at(10);
    domain.include(child);
    EXPECT_EQ(domain.deadline(), 9);
    // A new owner result replaces the old demand, including a later or absent one.
    domain = state_machine::NextWake<int>{};
    domain.at(30);
    EXPECT_EQ(domain.deadline(), 30);
    domain = state_machine::NextWake<int>{};
    EXPECT_FALSE(domain.deadline().has_value());
}

TEST(OrderedWorker, FifoExceptionsAndRestartPreserveItems) {
    std::mutex mutex;
    std::condition_variable done;
    std::vector<int> seen;
    auto execute = [&](const int& item) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            seen.push_back(item);
        }
        done.notify_all();
        if (item == 2)
            throw std::runtime_error("original failed item");
    };
    state_machine::OrderedWorker<int, decltype(execute)> worker(execute);
    worker.post(0);
    worker.start();
    worker.start();
    for (int i = 1; i <= 3; ++i)
        worker.post(i);
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(done.wait_for(lock, 2s, [&] {
            return seen.size() == 3;
        }));
    }
    worker.stop();
    worker.post(4);
    worker.start();
    worker.post(5);
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(done.wait_for(lock, 2s, [&] {
            return seen.size() == 4;
        }));
    }
    worker.stop();
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3, 5}));
}

TEST(OrderedWorker, PostDoesNotWaitForBlockedWorkAndStopDiscardsOnlyPending) {
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> executed{0};
    auto execute = [&](const int& item) {
        ++executed;
        if (item == 1) {
            entered.set_value();
            released.wait();
        }
    };
    state_machine::OrderedWorker<int, decltype(execute)> worker(execute);
    worker.start();
    worker.post(1);
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    worker.post(2);
    auto stopping = std::async(std::launch::async, [&] {
        worker.stop();
    });
    EXPECT_EQ(stopping.wait_for(20ms), std::future_status::timeout);
    release.set_value();
    ASSERT_EQ(stopping.wait_for(2s), std::future_status::ready);
    stopping.get();
    EXPECT_EQ(executed.load(), 1);
    worker.post(3);
    EXPECT_EQ(executed.load(), 1);
}

TEST(OrderedWorker, DoesNotRequireADefaultConstructiblePayload) {
    struct Item {
        explicit Item(int input) : value(input) {}
        int value;
    };
    std::promise<int> result;
    auto execute = [&](const Item& item) {
        result.set_value(item.value);
    };
    state_machine::OrderedWorker<Item, decltype(execute)> worker(execute);
    worker.start();
    worker.post(Item{17});
    auto future = result.get_future();
    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(future.get(), 17);
    worker.stop();
}
