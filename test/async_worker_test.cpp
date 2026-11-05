#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <state_machine/async_worker.hpp>
#include <vector>

using namespace std::chrono_literals;

TEST(AsyncWorker, BusyRefusalDoesNotReplaceTheClaimedRequest) {
    std::promise<void> entered, release, published;
    auto resume = release.get_future().share();
    std::vector<int> values;
    state_machine::AsyncWorker<int, int> worker(
        [&](const int& request) {
            entered.set_value();
            resume.wait();
            return request;
        },
        [&](const int& result) {
            values.push_back(result);
            published.set_value();
            return false;
        },
        [](const int&) {});
    EXPECT_TRUE(worker.trySubmit(0));   // Original pre-start pending value is cleared by start.
    EXPECT_FALSE(worker.trySubmit(99)); // Pending but not claimed still refuses.
    worker.start();
    const bool accepted = worker.trySubmit(1);
    ASSERT_TRUE(accepted);
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(worker.trySubmit(2));
    release.set_value();
    ASSERT_EQ(published.get_future().wait_for(2s), std::future_status::ready);
    worker.stop();
    EXPECT_EQ(values, (std::vector<int>{1}));
    EXPECT_FALSE(worker.trySubmit(3));
}

TEST(AsyncWorker, PublishesBeforeIdleAndNotifiesAfterIdleOutsideLock) {
    std::promise<void> done;
    std::atomic<int> publications{0}, notifications{0};
    state_machine::AsyncWorker<int, int>* address = nullptr;
    state_machine::AsyncWorker<int, int> worker(
        [](const int& request) {
            return request;
        },
        [&](const int&) {
            // A synchronous publication still sees the original busy state.
            EXPECT_FALSE(address->trySubmit(99));
            ++publications;
            return true;
        },
        [&](const int& result) {
            const int n = ++notifications;
            EXPECT_EQ(result, n);
            EXPECT_EQ(publications.load(), n);
            if (n == 1)
                EXPECT_TRUE(address->trySubmit(2));
            else
                done.set_value();
        });
    address = &worker;
    worker.start();
    const bool accepted = worker.trySubmit(1);
    ASSERT_TRUE(accepted);
    ASSERT_EQ(done.get_future().wait_for(2s), std::future_status::ready);
    worker.stop();
    EXPECT_EQ(publications.load(), 2);
    EXPECT_EQ(notifications.load(), 2);
}

TEST(AsyncWorker, CloseAllowsExistingRecipientInvalidationBeforeJoin) {
    std::promise<void> entered, release;
    auto resume = release.get_future().share();
    std::atomic<bool> recipient{true};
    std::atomic<int> notified{0}, stored{0};
    state_machine::AsyncWorker<int, int> worker(
        [&](const int& request) {
            entered.set_value();
            resume.wait();
            return request;
        },
        [&](const int&) {
            if (!recipient.load())
                return false;
            ++stored;
            return true;
        },
        [&](const int&) {
            ++notified;
        });
    worker.start();
    const bool accepted = worker.trySubmit(1);
    ASSERT_TRUE(accepted);
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    worker.close();
    recipient = false; // The product's existing recipient invalidation, not a library epoch.
    EXPECT_FALSE(worker.trySubmit(2));
    release.set_value();
    worker.join();
    EXPECT_EQ(stored.load(), 0);
    EXPECT_EQ(notified.load(), 0);
}

TEST(AsyncWorker, RestartReusesItsExistingThreadRole) {
    std::atomic<int> total{0};
    std::promise<void>* receipt = nullptr;
    state_machine::AsyncWorker<int, int> worker(
        [](const int& request) {
            return request;
        },
        [&](const int& result) {
            total += result;
            return true;
        },
        [&](const int&) {
            receipt->set_value();
        });
    for (int value : {1, 2}) {
        std::promise<void> done;
        receipt = &done;
        worker.start();
        const bool accepted = worker.trySubmit(value);
        ASSERT_TRUE(accepted);
        ASSERT_EQ(done.get_future().wait_for(2s), std::future_status::ready);
        worker.stop();
    }
    EXPECT_EQ(total.load(), 3);
}

TEST(AsyncWorker, NotificationCarriesTheExactPublishedRecipient) {
    struct Result {
        int generation;
    };
    std::atomic<int> active_generation{1};
    std::promise<int> notified_generation;
    std::atomic<int> accepted_wakes{0};
    state_machine::AsyncWorker<int, Result> worker(
        [](const int& generation) {
            return Result{generation};
        },
        [&](const Result& result) {
            EXPECT_EQ(result.generation, active_generation.load());
            active_generation = 2; // Existing owner invalidation after successful publication.
            return true;
        },
        [&](const Result& result) {
            if (active_generation.load() == result.generation)
                ++accepted_wakes;
            notified_generation.set_value(result.generation);
        });
    worker.start();
    const bool accepted = worker.trySubmit(1);
    ASSERT_TRUE(accepted);
    auto result = notified_generation.get_future();
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(result.get(), 1);
    worker.stop();
    EXPECT_EQ(accepted_wakes.load(), 0);
}

TEST(AsyncWorker, ComputeSessionBelongsToOneActualThreadRun) {
    std::promise<int>* receipt = nullptr;
    state_machine::AsyncWorker<int, int> worker(
        [entered = 0](const int&) mutable {
            return ++entered;
        },
        [](const int&) {
            return true;
        },
        [&](const int& result) {
            receipt->set_value(result);
        });
    for (int run = 0; run < 2; ++run) {
        worker.start();
        for (int expected : {1, 2}) {
            std::promise<int> done;
            receipt = &done;
            worker.start(); // Idempotent start must not recreate active session locals.
            const bool accepted = worker.trySubmit(0);
            ASSERT_TRUE(accepted);
            auto result = done.get_future();
            ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
            EXPECT_EQ(result.get(), expected);
        }
        worker.stop();
    }
}

TEST(AsyncWorker, MoveOnlyRequestsAndResultsAndPendingClose) {
    std::promise<int> receipt;
    state_machine::AsyncWorker<std::unique_ptr<int>, std::unique_ptr<int>> worker(
        [](const std::unique_ptr<int>& request) {
            return std::make_unique<int>(*request);
        },
        [](const std::unique_ptr<int>&) {
            return true;
        },
        [&](const std::unique_ptr<int>& result) {
            receipt.set_value(*result);
        });
    EXPECT_TRUE(worker.trySubmit(std::make_unique<int>(7)));
    worker.close();
    EXPECT_FALSE(worker.join()); // Discards the unclaimed request; no thread was created.
    worker.start();
    EXPECT_TRUE(worker.trySubmit(std::make_unique<int>(8)));
    auto result = receipt.get_future();
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(result.get(), 8);
    EXPECT_TRUE(worker.stop());
}

TEST(AsyncWorker, CallableCopyFailureStaysOnTheStartCaller) {
    struct Compute {
        explicit Compute(std::atomic<bool>& fail) : fail_(fail) {}
        Compute(const Compute& other) : fail_(other.fail_) {
            if (fail_.load())
                throw std::bad_alloc();
        }
        int operator()(const int& value) const { return value; }
        std::atomic<bool>& fail_;
    };
    std::atomic<bool> fail{false};
    std::promise<int> done;
    state_machine::AsyncWorker<int, int> worker(
        Compute(fail),
        [](const int&) {
            return true;
        },
        [&](const int& result) {
            done.set_value(result);
        });
    fail = true;
    EXPECT_THROW(worker.start(), std::bad_alloc);
    fail = false;
    worker.start();
    const bool accepted = worker.trySubmit(42);
    EXPECT_TRUE(accepted);
    auto result = done.get_future();
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(result.get(), 42);
    worker.stop();
}
