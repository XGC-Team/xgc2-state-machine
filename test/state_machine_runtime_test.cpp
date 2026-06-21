#include <state_machine/runtime/async_task_executor.hpp>
#include <state_machine/runtime/steady_timer.hpp>
#include <state_machine/state_machine.hpp>

#include <gtest/gtest.h>

#ifndef TEST
#define TEST(test_suite_name, test_name) static void test_suite_name##_##test_name()
#endif

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sm = state_machine;
namespace rt = state_machine::runtime;

namespace {

constexpr sm::RegionId kHealthRegion = 1;
constexpr sm::RegionId kFlightRegion = 2;
constexpr sm::RegionId kExtraRegion = 3;

constexpr sm::StateId kHealth = 10;
constexpr sm::StateId kSelfCheck = 20;
constexpr sm::StateId kNormal = 21;
constexpr sm::StateId kReady = 22;
constexpr sm::StateId kTakeoff = 23;
constexpr sm::StateId kTakeoffInit = 24;
constexpr sm::StateId kTakeoffOffboard = 25;
constexpr sm::StateId kTakeoffArm = 26;
constexpr sm::StateId kTakeoffAscending = 27;
constexpr sm::StateId kHover = 28;
constexpr sm::StateId kCustom = 29;
constexpr sm::StateId kLanding = 30;
constexpr sm::StateId kA = 40;
constexpr sm::StateId kB = 41;
constexpr sm::StateId kC = 42;
constexpr sm::StateId kD = 43;

constexpr sm::EventId kSelfCheckPassed = 100;
constexpr sm::EventId kTakeoffRequested = 101;
constexpr sm::EventId kDirectOffboard = 102;
constexpr sm::EventId kAscend = 103;
constexpr sm::EventId kLand = 104;
constexpr sm::EventId kSafety = 105;
constexpr sm::EventId kOutputOnly = 106;
constexpr sm::EventId kArbitrate = 107;

class RecordingState final : public sm::State {
  public:
    explicit RecordingState(std::string name, std::vector<std::string>* log = nullptr)
        : name_(std::move(name)), log_(log) {}

    std::string name() const override { return name_; }

    sm::ActionResult onEnter(sm::StateContext&) override {
        if (log_ != nullptr) {
            log_->push_back("enter:" + name_);
        }
        return sm::Status{};
    }

    sm::ActionResult onExit(sm::StateContext&) override {
        if (log_ != nullptr) {
            log_->push_back("exit:" + name_);
        }
        return sm::Status{};
    }

    sm::ActionResult onTick(sm::StateContext& ctx) override {
        ++tick_count;
        if (log_ != nullptr) {
            log_->push_back("tick:" + name_);
        }
        if (internal_on_tick != 0) {
            ctx.postInternalEvent(sm::Event(internal_on_tick));
        }
        if (output_on_tick != 0) {
            ctx.emitOutput(sm::Event(output_on_tick));
        }
        return sm::Status{};
    }

    sm::ActionResult onEvent(sm::StateContext&, const sm::Event& event) override {
        if (log_ != nullptr) {
            log_->push_back("event:" + name_ + ":" + std::to_string(event.id));
        }
        return sm::Status{};
    }

    sm::EventId internal_on_tick{0};
    sm::EventId output_on_tick{0};
    int tick_count{0};

  private:
    std::string name_;
    std::vector<std::string>* log_{nullptr};
};

std::unique_ptr<RecordingState> state(std::string name, std::vector<std::string>* log = nullptr) {
    return std::make_unique<RecordingState>(std::move(name), log);
}

std::unique_ptr<RecordingState> tickPostingState(std::string name, sm::EventId event_id) {
    auto recording_state = state(std::move(name));
    recording_state->internal_on_tick = event_id;
    return recording_state;
}

std::unique_ptr<sm::StateMachine> requireMachine(sm::Result<std::unique_ptr<sm::StateMachine>> result) {
    EXPECT_TRUE(result.ok()) << result.status.message;
    return std::move(result.value);
}

std::unique_ptr<sm::StateMachine> buildFlightLikeMachine() {
    auto builder = sm::StateMachine::builder("flight");
    builder.region(kHealthRegion)
        .name("health")
        .order(0)
        .initial(kHealth)
        .state(kHealth)
        .impl(state("Health"))
        .endRegion()
        .region(kFlightRegion)
        .name("flight")
        .order(10)
        .initial(kSelfCheck)
        .state(kSelfCheck)
        .impl(state("SelfCheck"))
        .state(kNormal)
        .name("Normal")
        .initial(kReady)
        .state(kReady)
        .impl(state("Ready"))
        .state(kTakeoff)
        .name("Takeoff")
        .initial(kTakeoffInit)
        .state(kTakeoffInit)
        .impl(state("TakeoffInit"))
        .state(kTakeoffOffboard)
        .impl(state("TakeoffOffboard"))
        .state(kTakeoffArm)
        .impl(state("TakeoffArm"))
        .state(kTakeoffAscending)
        .impl(state("TakeoffAscending"))
        .endState()
        .state(kHover)
        .impl(state("Hover"))
        .state(kCustom)
        .impl(state("Custom"))
        .endState()
        .state(kLanding)
        .impl(state("Landing"))
        .endRegion()
        .transition()
        .from(kSelfCheck)
        .to(kNormal)
        .on(kSelfCheckPassed)
        .transition()
        .from(kSelfCheck)
        .to(kTakeoffOffboard)
        .on(kDirectOffboard)
        .transition()
        .from(kReady)
        .to(kTakeoff)
        .on(kTakeoffRequested)
        .transition()
        .from(kTakeoffInit)
        .to(kTakeoffAscending)
        .on(kAscend)
        .transition()
        .from(kNormal)
        .to(kLanding)
        .on(kLand)
        .priority(1000);
    return requireMachine(builder.build());
}

TEST(StateMachineBuilder, BuildsNestedExclusiveAndParallelRegions) {
    auto builder = sm::StateMachine::builder("nested");
    builder.region(kFlightRegion)
        .name("root")
        .initial(kA)
        .state(kA)
        .name("Parent")
        .region(kHealthRegion)
        .name("child_a")
        .order(0)
        .initial(kB)
        .state(kB)
        .impl(state("B"))
        .endRegion()
        .region(kExtraRegion)
        .name("child_b")
        .order(10)
        .initial(kC)
        .state(kC)
        .impl(state("C"))
        .endRegion()
        .endState()
        .endRegion();
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    EXPECT_TRUE(machine->isActiveInPath({kFlightRegion, kA}));
    EXPECT_TRUE(machine->isActiveInPath({kHealthRegion, kB}));
    EXPECT_TRUE(machine->isActiveInPath({kExtraRegion, kC}));
}

TEST(StateMachineBuilder, MissingDefaultStateIsRejected) {
    auto builder = sm::StateMachine::builder("bad");
    builder.region(kFlightRegion).state(kA).impl(state("A")).endRegion();
    auto result = builder.build();
    EXPECT_FALSE(result.ok());
}

TEST(StateMachineRuntime, DefaultExpansionOnParentAndDeepTargets) {
    auto machine = buildFlightLikeMachine();
    ASSERT_TRUE(machine->start().ok());
    EXPECT_EQ(machine->currentStatePath(kFlightRegion), std::vector<sm::StateId>{kSelfCheck});

    ASSERT_TRUE(machine->postEvent(sm::Event(kSelfCheckPassed)).ok());
    auto first = machine->update({64, 64, false});
    ASSERT_TRUE(first.ok()) << first.status.message;
    EXPECT_EQ(machine->currentStatePath(kFlightRegion), (std::vector<sm::StateId>{kNormal, kReady}));

    ASSERT_TRUE(machine->postEvent(sm::Event(kTakeoffRequested)).ok());
    auto second = machine->update({64, 64, false});
    ASSERT_TRUE(second.ok()) << second.status.message;
    EXPECT_EQ(machine->currentStatePath(kFlightRegion), (std::vector<sm::StateId>{kNormal, kTakeoff, kTakeoffInit}));

    auto direct = buildFlightLikeMachine();
    ASSERT_TRUE(direct->start().ok());
    ASSERT_TRUE(direct->postEvent(sm::Event(kDirectOffboard)).ok());
    auto direct_result = direct->update({64, 64, false});
    ASSERT_TRUE(direct_result.ok()) << direct_result.status.message;
    EXPECT_EQ(direct->currentStatePath(kFlightRegion), (std::vector<sm::StateId>{kNormal, kTakeoff, kTakeoffOffboard}));
}

TEST(StateMachineRuntime, CurrentStateNameUsesRegisteredStateName) {
    auto builder = sm::StateMachine::builder("named");
    builder.region(kFlightRegion)
        .initial(kA)
        .state(kA)
        .name("RegisteredA")
        .impl(state("ImplA"))
        .state(kB)
        .impl(state("ImplB"))
        .transition()
        .from(kA)
        .to(kB)
        .on(kArbitrate);

    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    EXPECT_EQ(machine->currentStateName(kFlightRegion), "RegisteredA");

    ASSERT_TRUE(machine->postEvent(sm::Event(kArbitrate)).ok());
    ASSERT_TRUE(machine->update({64, 64, false}).ok());
    EXPECT_EQ(machine->currentStateName(kFlightRegion), "ImplB");
}

TEST(StateMachineRuntime, ParentTransitionCoversActiveSubtree) {
    auto machine = buildFlightLikeMachine();
    ASSERT_TRUE(machine->start().ok());
    ASSERT_TRUE(machine->postEvent(sm::Event(kSelfCheckPassed)).ok());
    ASSERT_TRUE(machine->update({64, 64, false}).ok());
    ASSERT_TRUE(machine->postEvent(sm::Event(kTakeoffRequested)).ok());
    ASSERT_TRUE(machine->update({64, 64, false}).ok());
    ASSERT_TRUE(machine->postEvent(sm::Event(kAscend)).ok());
    ASSERT_TRUE(machine->update({64, 64, false}).ok());
    EXPECT_EQ(machine->currentStatePath(kFlightRegion),
              (std::vector<sm::StateId>{kNormal, kTakeoff, kTakeoffAscending}));

    ASSERT_TRUE(machine->postEvent(sm::Event(kLand)).ok());
    auto result = machine->update({64, 64, false});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(machine->currentStatePath(kFlightRegion), std::vector<sm::StateId>{kLanding});
}

TEST(StateMachineRuntime, EarlierRegionInternalEventIsVisibleInSameTick) {
    auto healthState = [] {
        auto health = state("Health");
        health->internal_on_tick = kSafety;
        return health;
    };
    auto builder = sm::StateMachine::builder("ordered");
    builder.region(kHealthRegion)
        .order(0)
        .initial(kHealth)
        .state(kHealth)
        .impl(healthState())
        .endRegion()
        .region(kFlightRegion)
        .order(10)
        .initial(kReady)
        .state(kReady)
        .impl(state("Ready"))
        .state(kLanding)
        .impl(state("Landing"))
        .endRegion()
        .transition()
        .from(kReady)
        .to(kLanding)
        .on(kSafety);
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    auto result = machine->update({64, 64, true});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(machine->currentState(kFlightRegion), kLanding);
}

TEST(StateMachineRuntime, SameRegionInternalEventFromTickCanTriggerTransition) {
    auto builder = sm::StateMachine::builder("same_region_tick_event");
    builder.region(kFlightRegion)
        .initial(kTakeoffInit)
        .state(kTakeoffInit)
        .impl(tickPostingState("TakeoffInit", kArbitrate))
        .state(kTakeoffOffboard)
        .impl(state("TakeoffOffboard"))
        .endRegion()
        .transition()
        .from(kTakeoffInit)
        .to(kTakeoffOffboard)
        .on(kArbitrate);

    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    auto result = machine->update({64, 64, true});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(machine->currentState(kFlightRegion), kTakeoffOffboard);
}

TEST(StateMachineRuntime, LaterRegionInternalEventWaitsUntilNextTick) {
    auto healthState = [] {
        auto health = state("Health");
        health->internal_on_tick = kSafety;
        return health;
    };
    auto builder = sm::StateMachine::builder("reverse");
    builder.region(kFlightRegion)
        .order(0)
        .initial(kReady)
        .state(kReady)
        .impl(state("Ready"))
        .state(kLanding)
        .impl(state("Landing"))
        .endRegion()
        .region(kHealthRegion)
        .order(10)
        .initial(kHealth)
        .state(kHealth)
        .impl(healthState())
        .endRegion()
        .transition()
        .from(kReady)
        .to(kLanding)
        .on(kSafety);
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    ASSERT_TRUE(machine->update({64, 64, true}).ok());
    EXPECT_EQ(machine->currentState(kFlightRegion), kReady);
    ASSERT_TRUE(machine->update({64, 64, true}).ok());
    EXPECT_EQ(machine->currentState(kFlightRegion), kLanding);
}

TEST(StateMachineRuntime, SameStateTransitionsShortCircuitByPriorityThenOrder) {
    int high_guard = 0;
    int middle_guard = 0;
    int low_guard = 0;
    auto builder = sm::StateMachine::builder("arbitration");
    builder.region(kFlightRegion)
        .initial(kA)
        .state(kA)
        .impl(state("A"))
        .state(kB)
        .impl(state("B"))
        .state(kC)
        .impl(state("C"))
        .state(kD)
        .impl(state("D"))
        .endRegion()
        .transition()
        .from(kA)
        .to(kB)
        .when([&](const sm::GuardContext&) {
            ++low_guard;
            return true;
        })
        .priority(10)
        .transition()
        .from(kA)
        .to(kC)
        .on(kArbitrate)
        .when([&](const sm::GuardContext&) {
            ++middle_guard;
            return true;
        })
        .priority(50)
        .transition()
        .from(kA)
        .to(kD)
        .on(kArbitrate)
        .when([&](const sm::GuardContext&) {
            ++high_guard;
            return true;
        })
        .priority(100);
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    ASSERT_TRUE(machine->postEvent(sm::Event(kArbitrate)).ok());
    auto result = machine->update({64, 64, false});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(machine->currentState(kFlightRegion), kD);
    EXPECT_EQ(high_guard, 1);
    EXPECT_EQ(middle_guard, 0);
    EXPECT_EQ(low_guard, 0);
}

TEST(StateMachineRuntime, ParentStateIsEvaluatedBeforeChildState) {
    int parent_guard = 0;
    int child_guard = 0;
    auto builder = sm::StateMachine::builder("top_down");
    builder.region(kFlightRegion)
        .initial(kNormal)
        .state(kNormal)
        .initial(kReady)
        .state(kReady)
        .impl(state("Ready"))
        .state(kB)
        .impl(state("B"))
        .endState()
        .state(kLanding)
        .impl(state("Landing"))
        .endRegion()
        .transition()
        .from(kNormal)
        .to(kLanding)
        .when([&](const sm::GuardContext&) {
            ++parent_guard;
            return true;
        })
        .priority(1)
        .transition()
        .from(kReady)
        .to(kB)
        .when([&](const sm::GuardContext&) {
            ++child_guard;
            return true;
        })
        .priority(1000);
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    auto result = machine->update({64, 64, false});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(machine->currentState(kFlightRegion), kLanding);
    EXPECT_EQ(parent_guard, 1);
    EXPECT_EQ(child_guard, 0);
}

TEST(StateMachineRuntime, ConditionOnlyTransitionRunsOncePerTick) {
    bool ready = false;
    int guard_calls = 0;
    auto builder = sm::StateMachine::builder("condition");
    builder.region(kFlightRegion)
        .initial(kA)
        .state(kA)
        .impl(state("A"))
        .state(kB)
        .impl(state("B"))
        .endRegion()
        .transition()
        .from(kA)
        .to(kB)
        .when([&](const sm::GuardContext&) {
            ++guard_calls;
            return ready;
        });
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    ASSERT_TRUE(machine->update({64, 64, false}).ok());
    EXPECT_EQ(machine->currentState(kFlightRegion), kA);
    EXPECT_EQ(guard_calls, 1);

    ready = true;
    ASSERT_TRUE(machine->update({64, 64, false}).ok());
    EXPECT_EQ(machine->currentState(kFlightRegion), kB);
    EXPECT_EQ(guard_calls, 2);

    ASSERT_TRUE(machine->update({64, 64, false}).ok());
    EXPECT_EQ(guard_calls, 2);
}

TEST(StateMachineRuntime, OutputEventsDoNotTriggerTransitions) {
    auto activeState = [] {
        auto active = state("A");
        active->output_on_tick = kOutputOnly;
        return active;
    };
    auto builder = sm::StateMachine::builder("output");
    builder.region(kFlightRegion)
        .initial(kA)
        .state(kA)
        .impl(activeState())
        .state(kB)
        .impl(state("B"))
        .endRegion()
        .transition()
        .from(kA)
        .to(kB)
        .on(kOutputOnly);
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());
    auto result = machine->update({64, 64, true});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(machine->currentState(kFlightRegion), kA);
    ASSERT_EQ(machine->currentOutputEvents().size(), 1U);
    EXPECT_EQ(machine->currentOutputEvents().front().category, sm::EventCategory::kOutput);
}

TEST(RuntimeUtilities, SteadyTimerMeasuresElapsedAndIntervals) {
    rt::Timer<> timer;
    EXPECT_FALSE(timer.started());
    EXPECT_EQ(timer.elapsed().count(), 0.0);

    timer.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(timer.started());
    EXPECT_GT(timer.elapsed().count(), 0.0);

    timer.reset();
    EXPECT_TRUE(timer.started());

    rt::IntervalTimer<> interval_timer;
    EXPECT_EQ(interval_timer.interval().count(), 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_GT(interval_timer.interval().count(), 0.0);
}

TEST(RuntimeUtilities, AsyncTaskExecutorRunsTasksAndCountsFailures) {
    int context = 0;
    rt::AsyncTaskExecutor<int> executor(context);
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    executor.start();
    executor.pushTask(std::make_unique<rt::LambdaTask<int>>("set_context", [&](int& ctx) {
        std::lock_guard<std::mutex> lock(mutex);
        ctx = 42;
        done = true;
        cv.notify_one();
    }));

    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return done;
        }));
        EXPECT_EQ(context, 42);
    }

    executor.pushTask(std::make_unique<rt::LambdaTask<int>>("throwing_task", [](int&) {
        throw std::runtime_error("boom");
    }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (executor.failedCount() == 0u && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    executor.stop();
    EXPECT_EQ(executor.executedCount(), 1u);
    EXPECT_EQ(executor.failedCount(), 1u);
}

TEST(StateMachineRuntime, PureLogicPerformanceSmoke) {
    int active_guard_calls = 0;
    int inactive_guard_calls = 0;
    auto builder = sm::StateMachine::builder("perf");
    builder.region(kHealthRegion)
        .order(0)
        .initial(kHealth)
        .state(kHealth)
        .impl(state("Health"))
        .endRegion()
        .region(kFlightRegion)
        .order(10)
        .initial(kNormal)
        .state(kNormal)
        .initial(kReady)
        .state(kReady)
        .impl(state("Ready"))
        .state(kTakeoff)
        .initial(kTakeoffInit)
        .state(kTakeoffInit)
        .impl(state("TakeoffInit"))
        .state(kTakeoffOffboard)
        .impl(state("TakeoffOffboard"))
        .state(kTakeoffArm)
        .impl(state("TakeoffArm"))
        .state(kTakeoffAscending)
        .impl(state("TakeoffAscending"))
        .endState()
        .state(kHover)
        .impl(state("Hover"))
        .state(kCustom)
        .impl(state("Custom"))
        .endState()
        .state(kLanding)
        .impl(state("Landing"))
        .endRegion()
        .transition()
        .from(kReady)
        .to(kTakeoff)
        .when([&](const sm::GuardContext&) {
            ++active_guard_calls;
            return false;
        })
        .transition()
        .from(kTakeoffAscending)
        .to(kLanding)
        .when([&](const sm::GuardContext&) {
            ++inactive_guard_calls;
            return true;
        });
    auto machine = requireMachine(builder.build());
    ASSERT_TRUE(machine->start().ok());

    const auto begin = std::chrono::steady_clock::now();
    constexpr int kTicks = 5000;
    for (int i = 0; i < kTicks; ++i) {
        auto result = machine->update({64, 64, false});
        ASSERT_TRUE(result.ok()) << result.status.message;
        ASSERT_EQ(machine->currentState(kFlightRegion), kReady);
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto average_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / kTicks;
    EXPECT_LT(average_ns, 1000000);
    EXPECT_EQ(active_guard_calls, kTicks);
    EXPECT_EQ(inactive_guard_calls, 0);
}

} // namespace
