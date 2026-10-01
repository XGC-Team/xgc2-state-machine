// A worker thread delivers task results while the owner thread runs update() and another
// thread posts input events, as a solver thread and a ROS thread would. Every result must
// be accepted exactly once and reach the state that started the task.
//
// Build with -fsanitize=thread to check the locking, e.g.
//   cmake -DCMAKE_CXX_FLAGS=-fsanitize=thread ...

#include <state_machine/state_machine.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace sm = state_machine;

namespace {

constexpr sm::RegionId kRegion = 1;
constexpr sm::StateId kWorking = 10;
constexpr sm::EventId kSensor = 100;
constexpr int kResults = 5000;

// Hands each new task handle from the owner thread to the worker thread.
struct Mailbox {
    std::mutex mutex;
    std::optional<sm::TaskHandle> handle;
};

class Working final : public sm::State {
  public:
    explicit Working(Mailbox& mailbox) : mailbox_(mailbox) {}

    std::string name() const override { return "Working"; }

    sm::ActionResult onEnter(sm::StateContext& ctx) override { return startNext(ctx); }

    sm::ActionResult onEvent(sm::StateContext& ctx, const sm::Event& event) override {
        if (event.id != sm::kTaskResultEvent) {
            return sm::Status{};
        }
        ++results;
        return results < kResults ? startNext(ctx) : sm::Status{};
    }

    int results{0};
    int start_failures{0};

  private:
    sm::ActionResult startNext(sm::StateContext& ctx) {
        auto started =
            ctx.startTask(sm::TaskCancelPolicy::kCancelOnStateExit, static_cast<sm::CorrelationId>(results) + 1);
        if (!started.ok()) {
            ++start_failures;
            return started.status;
        }
        std::lock_guard<std::mutex> lock(mailbox_.mutex);
        mailbox_.handle = started.value;
        return sm::Status{};
    }

    Mailbox& mailbox_;
};

} // namespace

int main() {
    Mailbox mailbox;
    auto working = std::make_unique<Working>(mailbox);
    Working* state = working.get();
    auto builder = sm::StateMachine::builder("task-results");
    builder.region(kRegion).initial(kWorking).state(kWorking).impl(std::move(working)).endRegion();
    auto built = builder.build();
    if (!built.ok()) {
        std::fprintf(stderr, "build failed: %s\n", built.status.message.c_str());
        return 2;
    }
    std::unique_ptr<sm::StateMachine> machine = std::move(built.value);
    if (!machine->start().ok()) {
        std::fprintf(stderr, "start failed\n");
        return 2;
    }

    std::atomic<bool> running{true};
    std::atomic<int> rejected{0};
    std::thread worker([&machine, &mailbox, &running, &rejected] {
        while (running.load()) {
            std::optional<sm::TaskHandle> handle;
            {
                std::lock_guard<std::mutex> lock(mailbox.mutex);
                handle.swap(mailbox.handle);
            }
            if (!handle) {
                std::this_thread::yield();
                continue;
            }
            if (!machine->postTaskResult(*handle, sm::TaskStatus::kCompleted).ok()) {
                ++rejected;
            }
        }
    });

    std::atomic<int> sensor_errors{0};
    std::thread sensors([&machine, &running, &sensor_errors] {
        while (running.load()) {
            sm::Event event(kSensor);
            event.source = "sensor";
            const sm::Status status = machine->postEvent(std::move(event));
            if (!status.ok() && status.code != sm::ErrorCode::kLimitReached) {
                ++sensor_errors;
            }
            std::this_thread::yield();
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int updates = 0;
    bool update_failed = false;
    while (state->results < kResults && std::chrono::steady_clock::now() < deadline) {
        if (!machine->update().ok()) {
            update_failed = true;
            break;
        }
        ++updates;
    }
    running = false;
    worker.join();
    sensors.join();

    const bool ok = !update_failed && state->results == kResults && state->start_failures == 0 &&
                    rejected.load() == 0 && sensor_errors.load() == 0;
    std::printf(
        "task results: %d of %d delivered in %d updates; start_failures=%d rejected=%d sensor_errors=%d -> %s\n",
        state->results, kResults, updates, state->start_failures, rejected.load(), sensor_errors.load(),
        ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
