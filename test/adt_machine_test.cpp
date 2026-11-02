#include <state_machine/adt.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <variant>

namespace {

namespace adt = state_machine::adt;

// ============================================================================
// Test 1: Hierarchical Sum-Type Machine (Chassis-Hold & Controller Shaped)
// ============================================================================
enum class RejectCode : uint8_t {
    kNone = 0,
    kNotHeld,
    kRevisionConflict,
    kFaultActive,
    kSensorsUnhealthy,
};

struct ReleasedState {};

struct GatedStage {};
struct ZeroWrittenStage {
    double written_at_sec{0.0};
    std::optional<double> rest_since_sec{};
};
struct StoppedStage {
    double written_at_sec{0.0};
    double stopped_at_sec{0.0};
};
struct FaultedStage {
    std::string_view fault_reason{};
};

using HeldStageVariant =
    std::variant<GatedStage, ZeroWrittenStage, StoppedStage, FaultedStage>;

struct HeldState {
    uint64_t revision{0};
    double gated_at_sec{0.0};
    HeldStageVariant stage{GatedStage{}};
};

using HoldStateVariant = std::variant<ReleasedState, HeldState>;

// Strongly typed events (zero std::map / string allocation)
struct EvEngage {
    double at_sec{0.0};
};
struct EvRelease {
    uint64_t revision{0};
};
struct EvZeroWritten {
    uint64_t revision{0};
};
struct EvMotionSample {
    uint64_t revision{0};
    bool at_rest{false};
};
struct EvFault {
    std::string_view reason{};
};
struct EvClearFault {};

using HoldEventVariant =
    std::variant<EvEngage, EvRelease, EvZeroWritten, EvMotionSample, EvFault, EvClearFault>;

struct OutGateChanged {
    bool held{false};
    uint64_t revision{0};
};
struct OutCommandZero {
    uint64_t revision{0};
};

using HoldOutputVariant = std::variant<OutGateChanged, OutCommandZero>;

struct HoldContext {
    double rest_dwell_sec{0.5};
    uint64_t current_revision{0};
    int active_held_instances{0}; // Tracks RAII lifecycle of HeldState
};

struct HoldMachineSpec {
    using StateVariant = HoldStateVariant;
    using EventVariant = HoldEventVariant;
    using OutputVariant = HoldOutputVariant;
    using Context = HoldContext;
    using RejectReason = RejectCode;
    using Trans = adt::Transition<StateVariant, RejectReason>;
    using OutputBuf = adt::BoundedVector<OutputVariant, 16>;

    static constexpr std::string_view stateName(const ReleasedState&) noexcept { return "Released"; }
    static constexpr std::string_view stateName(const HeldState& h) noexcept {
        return std::visit(
            adt::Overloaded{
                [](const GatedStage&) -> std::string_view { return "Held::Gated"; },
                [](const ZeroWrittenStage&) -> std::string_view { return "Held::ZeroWritten"; },
                [](const StoppedStage&) -> std::string_view { return "Held::Stopped"; },
                [](const FaultedStage&) -> std::string_view { return "Held::Faulted"; },
            },
            h.stage);
    }

    static void onEnter(HeldState& s, Context& ctx, double /*now_sec*/, OutputBuf& out) {
        ++ctx.active_held_instances;
        out.push_back(OutGateChanged{true, s.revision});
    }
    static void onExit(HeldState& s, Context& ctx, double /*now_sec*/, OutputBuf& out) {
        --ctx.active_held_instances;
        out.push_back(OutGateChanged{false, s.revision});
    }

    static adt::TickPolicy tickPolicy(const ReleasedState&,
                                      const Context&,
                                      double /*now_sec*/,
                                      double /*entered_at*/) noexcept {
        return adt::Policy::idle(); // 0 Hz when Released!
    }

    static adt::TickPolicy tickPolicy(const HeldState& h,
                                      const Context&,
                                      double /*now_sec*/,
                                      double /*entered_at*/) noexcept {
        return std::visit(
            adt::Overloaded{
                [](const GatedStage&) -> adt::TickPolicy { return adt::Policy::periodic(0.02); },
                [](const ZeroWrittenStage&) -> adt::TickPolicy { return adt::Policy::periodic(0.02); },
                [](const StoppedStage&) -> adt::TickPolicy { return adt::Policy::periodic(0.10); },
                [](const FaultedStage&) -> adt::TickPolicy { return adt::Policy::periodic(0.05); },
            },
            h.stage);
    }

    // Event transitions from ReleasedState
    static Trans onEvent(ReleasedState&,
                         Context& ctx,
                         const EvEngage& ev,
                         double /*now_sec*/,
                         OutputBuf&) {
        ++ctx.current_revision;
        return Trans::to(HeldState{ctx.current_revision, ev.at_sec, GatedStage{}});
    }

    static Trans onEvent(ReleasedState&,
                         Context&,
                         const EvRelease&,
                         double /*now_sec*/,
                         OutputBuf&) {
        return Trans::reject(RejectCode::kNotHeld);
    }

    template <typename OtherEvent>
    static Trans onEvent(ReleasedState&,
                         Context&,
                         const OtherEvent&,
                         double /*now_sec*/,
                         OutputBuf&) {
        return Trans::stay();
    }

    // Event transitions from HeldState
    static Trans onEvent(HeldState&,
                         Context&,
                         const EvEngage&,
                         double /*now_sec*/,
                         OutputBuf&) {
        return Trans::stay(); // Already held
    }

    static Trans onEvent(HeldState& s,
                         Context& ctx,
                         const EvRelease& ev,
                         double /*now_sec*/,
                         OutputBuf&) {
        if (std::holds_alternative<FaultedStage>(s.stage)) {
            return Trans::reject(RejectCode::kFaultActive);
        }
        if (ev.revision != s.revision) {
            return Trans::reject(RejectCode::kRevisionConflict);
        }
        ++ctx.current_revision;
        return Trans::to(ReleasedState{});
    }

    static Trans onEvent(HeldState& s,
                         Context&,
                         const EvZeroWritten& ev,
                         double now_sec,
                         OutputBuf&) {
        if (ev.revision != s.revision) {
            return Trans::stay();
        }
        if (std::holds_alternative<GatedStage>(s.stage)) {
            s.stage = ZeroWrittenStage{now_sec, std::nullopt};
        }
        return Trans::stay();
    }

    static Trans onEvent(HeldState& s,
                         Context& ctx,
                         const EvMotionSample& ev,
                         double now_sec,
                         OutputBuf&) {
        if (ev.revision != s.revision) {
            return Trans::stay();
        }
        if (auto* zw = std::get_if<ZeroWrittenStage>(&s.stage)) {
            if (ev.at_rest) {
                if (!zw->rest_since_sec.has_value()) {
                    zw->rest_since_sec = now_sec;
                }
                if (now_sec - *zw->rest_since_sec >= ctx.rest_dwell_sec) {
                    s.stage = StoppedStage{zw->written_at_sec, now_sec};
                }
            } else {
                zw->rest_since_sec.reset();
            }
        } else if (auto* st = std::get_if<StoppedStage>(&s.stage)) {
            if (!ev.at_rest) {
                s.stage = ZeroWrittenStage{st->written_at_sec, std::nullopt};
            }
        }
        return Trans::stay();
    }

    static Trans onEvent(HeldState& s,
                         Context&,
                         const EvFault& ev,
                         double /*now_sec*/,
                         OutputBuf&) {
        s.stage = FaultedStage{ev.reason};
        return Trans::stay();
    }

    static Trans onEvent(HeldState& s,
                         Context&,
                         const EvClearFault&,
                         double /*now_sec*/,
                         OutputBuf&) {
        if (std::holds_alternative<FaultedStage>(s.stage)) {
            s.stage = GatedStage{};
        }
        return Trans::stay();
    }

    static void onTick(HeldState& s,
                       Context&,
                       double /*now_sec*/,
                       double /*elapsed_sec*/,
                       OutputBuf& out) {
        out.push_back(OutCommandZero{s.revision});
    }
};

TEST(AdtMachineTest, NestedSumTypeLifecycleAndSynchronousAdmission) {
    HoldContext ctx{};
    adt::AdtMachine<HoldMachineSpec> machine(ReleasedState{}, 1.0);
    machine.start(ctx, 1.0);

    // 1. Released state is TickIdle (0 Hz timer demand)
    EXPECT_TRUE(std::holds_alternative<adt::TickIdle>(machine.currentTickPolicy(ctx, 1.0)));
    EXPECT_EQ(machine.currentStateName(), "Released");

    // 2. Releasing when not held synchronously returns RejectCode::kNotHeld
    auto bad_release = machine.dispatch(ctx, EvRelease{1}, 1.1);
    ASSERT_FALSE(bad_release.ok());
    EXPECT_EQ(bad_release.error(), RejectCode::kNotHeld);

    // 3. Engage transitions to Held::Gated and requests 50 Hz (0.02s) periodic ticks
    auto engaged = machine.dispatch(ctx, EvEngage{1.2}, 1.2);
    ASSERT_TRUE(engaged.ok());
    EXPECT_TRUE(engaged->transitioned);
    EXPECT_EQ(ctx.active_held_instances, 1);
    EXPECT_EQ(machine.currentStateName(), "Held::Gated");
    ASSERT_TRUE(std::holds_alternative<adt::TickPeriodic>(engaged->tick_policy));
    EXPECT_DOUBLE_EQ(std::get<adt::TickPeriodic>(engaged->tick_policy).period_sec, 0.02);

    // 4. Progress through ZeroWritten -> Stopped
    ASSERT_TRUE(machine.dispatch(ctx, EvZeroWritten{1}, 1.3).ok());
    EXPECT_EQ(machine.currentStateName(), "Held::ZeroWritten");

    ASSERT_TRUE(machine.dispatch(ctx, EvMotionSample{1, true}, 1.4).ok());
    EXPECT_EQ(machine.currentStateName(), "Held::ZeroWritten");
    ASSERT_TRUE(machine.dispatch(ctx, EvMotionSample{1, true}, 2.0).ok());
    EXPECT_EQ(machine.currentStateName(), "Held::Stopped");

    // In Stopped stage, tick cadence automatically relaxes to 10 Hz (0.10s)
    const auto stopped_policy = machine.currentTickPolicy(ctx, 2.0);
    ASSERT_TRUE(std::holds_alternative<adt::TickPeriodic>(stopped_policy));
    EXPECT_DOUBLE_EQ(std::get<adt::TickPeriodic>(stopped_policy).period_sec, 0.10);

    // 5. Wrong revision release is synchronously rejected with kRevisionConflict
    auto wrong_rev = machine.dispatch(ctx, EvRelease{999}, 2.1);
    ASSERT_FALSE(wrong_rev.ok());
    EXPECT_EQ(wrong_rev.error(), RejectCode::kRevisionConflict);

    // 6. Valid release destroys HeldState (and all its nested stage data) in place
    auto released = machine.dispatch(ctx, EvRelease{1}, 2.2);
    ASSERT_TRUE(released.ok());
    EXPECT_TRUE(released->transitioned);
    EXPECT_EQ(ctx.active_held_instances, 0);
    EXPECT_EQ(machine.currentStateName(), "Released");
    EXPECT_TRUE(std::holds_alternative<adt::TickIdle>(released->tick_policy));
}

// ============================================================================
// Test 2: Forward-Event-On-Transition (Estimator Initializing -> Running)
// ============================================================================
struct InitState {};
struct RunState {
    int samples_integrated{0};
    double last_value{0.0};
};
using EstStateVariant = std::variant<InitState, RunState>;

struct EvSample {
    double value{0.0};
};
using EstEventVariant = std::variant<EvSample>;
using EstOutputVariant = std::variant<double>;

struct EstContext {
    bool sensors_ready{false};
};

struct EstSpec {
    using StateVariant = EstStateVariant;
    using EventVariant = EstEventVariant;
    using OutputVariant = EstOutputVariant;
    using Context = EstContext;
    using RejectReason = std::string_view;
    using Trans = adt::Transition<StateVariant, RejectReason>;
    using OutputBuf = adt::BoundedVector<OutputVariant, 16>;

    static Trans onEvent(InitState&,
                         Context& ctx,
                         const EvSample&,
                         double /*now_sec*/,
                         OutputBuf&) {
        if (ctx.sensors_ready) {
            // Transition to RunState AND forward the triggering EvSample to RunState!
            return Trans::to(RunState{}, /*forward_event=*/true);
        }
        return Trans::stay();
    }

    static Trans onEvent(RunState& s,
                         Context&,
                         const EvSample& ev,
                         double /*now_sec*/,
                         OutputBuf& out) {
        ++s.samples_integrated;
        s.last_value = ev.value;
        out.push_back(ev.value);
        return Trans::stay();
    }
};

TEST(AdtMachineTest, ForwardEventOnTransitionIntegratesFirstSampleImmediately) {
    EstContext ctx{true};
    adt::AdtMachine<EstSpec> machine(InitState{}, 0.0);

    auto receipt = machine.dispatch(ctx, EvSample{42.5}, 1.0);
    ASSERT_TRUE(receipt.ok());
    EXPECT_TRUE(receipt->transitioned);
    ASSERT_TRUE(machine.isIn<RunState>());
    EXPECT_EQ(machine.getIf<RunState>()->samples_integrated, 1);
    EXPECT_DOUBLE_EQ(machine.getIf<RunState>()->last_value, 42.5);
    ASSERT_EQ(machine.outputs().size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(machine.outputs()[0]), 42.5);
}

// ============================================================================
// Test 3: TickPolicy Monoid, SignalSlot, and AsyncLatestWorker Epoch Fencing
// ============================================================================
TEST(AdtMachineTest, TickPolicyMonoidAndSignalSlotAndAsyncWorker) {
    // 1. TickPolicy Monoid properties
    const auto idle = adt::Policy::idle();
    const auto p10 = adt::Policy::rateHz(10.0);
    const auto p100 = adt::Policy::rateHz(100.0);

    EXPECT_EQ(adt::mergeTickPolicy(idle, p10), p10);
    EXPECT_EQ(adt::mergeTickPolicy(p10, idle), p10);
    const auto merged = adt::mergeTickPolicy(p10, p100);
    ASSERT_TRUE(std::holds_alternative<adt::TickPeriodic>(merged));
    EXPECT_DOUBLE_EQ(std::get<adt::TickPeriodic>(merged).period_sec, 0.01);

    // 2. SignalSlot sliding rate & freshness
    adt::SignalSlot<double, 16> imu_slot;
    for (int i = 0; i <= 20; ++i) {
        imu_slot.publish(9.81, 1.0 + i * 0.005, 1.0 + i * 0.005, 0.1);
    }
    EXPECT_TRUE(imu_slot.isHealthy(1.10, 0.05, 180.0));
    EXPECT_NEAR(imu_slot.rateWindow().rateHz(), 200.0, 1e-6);
    EXPECT_NEAR(imu_slot.rateWindow().periodStddevSec(), 0.0, 1e-9);
    EXPECT_FALSE(imu_slot.isFresh(1.20, 0.05));

    // 3. AsyncLatestWorker epoch fencing
    std::atomic<int> wake_calls{0};
    adt::AsyncLatestWorker<int, int> worker(
        [](const int& x) { return x * 2; },
        [&] { wake_calls.fetch_add(1, std::memory_order_relaxed); });
    worker.start();

    const uint64_t epoch_custom1 = 5;
    worker.submit(epoch_custom1, 21);
    while (wake_calls.load(std::memory_order_relaxed) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // If state machine moved to epoch 6 (e.g. Hover), stale result from epoch 5 is discarded!
    auto stale = worker.takeResultForEpoch(6);
    EXPECT_FALSE(stale.has_value());
    EXPECT_EQ(worker.staleDiscardedCount(), 1u);

    worker.stop();
}

} // namespace
