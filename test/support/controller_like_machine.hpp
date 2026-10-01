#pragma once

// A state machine shaped like the multirotor controller's flight machine: three
// parallel top-level regions (health / flight / debug), a nested flight
// hierarchy, ~45 transitions with std::function guards, and a debug state that
// emits one output event on every tick. Shared by the tick benchmark and the
// equivalence test so both exercise the same realistic graph.

#include <state_machine/state_machine.hpp>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace controller_like {

namespace sm = state_machine;

// Identifiers copied from the controller's common/types.h.
namespace region_id {
constexpr sm::RegionId kHealth = 1;
constexpr sm::RegionId kControl = 2;
constexpr sm::RegionId kDebug = 3;
} // namespace region_id

namespace state_id {
constexpr sm::StateId kSelfCheck = 1;
constexpr sm::StateId kReady = 2;
constexpr sm::StateId kTakeoffInit = 3;
constexpr sm::StateId kTakeoffOffboard = 4;
constexpr sm::StateId kTakeoffArm = 5;
constexpr sm::StateId kTakeoffAscending = 6;
constexpr sm::StateId kHover = 7;
constexpr sm::StateId kNormal = 8;
constexpr sm::StateId kLanding = 9;
constexpr sm::StateId kTakeoff = 10;
constexpr sm::StateId kCustom1 = 11;
constexpr sm::StateId kHealthMonitor = 100;
constexpr sm::StateId kDebugMonitor = 101;
} // namespace state_id

namespace input_event {
constexpr sm::EventId kTakeoffRequested = 1;
constexpr sm::EventId kLandingRequested = 2;
constexpr sm::EventId kHoverRequested = 3;
constexpr sm::EventId kTrajectoryTrackingRequested = 6;
constexpr sm::EventId kAltitudeReached = 10;
constexpr sm::EventId kTouchdown = 13;
constexpr sm::EventId kTakeoffTimeout = 15;
constexpr sm::EventId kAltctlReady = 16;
constexpr sm::EventId kOffboardReady = 17;
constexpr sm::EventId kArmReady = 18;
constexpr sm::EventId kReferenceTrajectoryFinished = 19;
constexpr sm::EventId kLocalPosition = 50;
constexpr sm::EventId kLocalVelocity = 51;
constexpr sm::EventId kImu = 52;
constexpr sm::EventId kVrpnPose = 55;
constexpr sm::EventId kMpcTrajectory = 57;
constexpr sm::EventId kStateEstimate = 63;
constexpr sm::EventId kUnusedSafety = 30; // edge event that no transition consumes
} // namespace input_event

namespace output_event {
constexpr sm::EventId kPublishSetpoint = 10004;
constexpr sm::EventId kPublishControllerStatus = 10007;
constexpr sm::EventId kPublishSensorStats = 10008;
constexpr sm::EventId kPublishStateMachineEvents = 10009;
constexpr sm::EventId kPrintSensorDebug = 10010;
constexpr sm::EventId kPublishTrackingError = 10012;
} // namespace output_event

constexpr int kEmergency = 100;
constexpr int kCritical = 90;
constexpr int kUserCommand = 80;
constexpr int kCommand = 50;
constexpr int kAutomatic = 20;

// Safety events that move the flight region from Normal to Landing.
constexpr sm::EventId kSafetyToLanding[] = {2, 40, 41, 42, 43, 44, 47, 28, 27, 20, 21, 22, 23};

struct Flags {
    bool sensors_ready{true};
    bool airborne{false};
    bool distance_available{true};
    bool distance_exceeded{false};
    bool custom1_requested{false};
    bool custom1_reference_ready{true};
    bool take_off_abort{false};
    bool toggle{false}; // flips every tick in the "transitions" scenario
    int internal_every{0};
    sm::EventId safety_event{0}; // posted once by the health state, then cleared
    int setpoint_every{5};
    int tick{0};
    double now{0.0};
};

class NamedState : public sm::State {
  public:
    explicit NamedState(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }

  protected:
    std::string name_;
};

// Health region: cheap flag checks; optionally raises an edge safety event.
class HealthMonitor final : public NamedState {
  public:
    explicit HealthMonitor(Flags& flags) : NamedState("HealthMonitor"), flags_(flags) {}
    sm::ActionResult onTick(sm::StateContext& ctx) override {
        if (flags_.internal_every > 0 && flags_.tick % flags_.internal_every == 0) {
            sm::Event event(input_event::kUnusedSafety, sm::EventTimestamp{flags_.now});
            event.source = "health";
            ctx.postInternalEvent(std::move(event));
        }
        if (flags_.safety_event != 0) {
            sm::Event event(flags_.safety_event, sm::EventTimestamp{flags_.now});
            event.source = "health";
            flags_.safety_event = 0;
            ctx.postInternalEvent(std::move(event));
        }
        return {};
    }

  private:
    Flags& flags_;
};

// Flight states that stream a setpoint output event at a fixed tick period.
class Streaming final : public NamedState {
  public:
    Streaming(std::string name, Flags& flags) : NamedState(std::move(name)), flags_(flags) {}
    sm::ActionResult onTick(sm::StateContext& ctx) override {
        if (flags_.setpoint_every > 0 && flags_.tick % flags_.setpoint_every == 0) {
            ctx.emitOutput(sm::Event(output_event::kPublishSetpoint, sm::EventTimestamp{flags_.now}));
        }
        return {};
    }

  private:
    Flags& flags_;
};

// Debug region: one output event every tick plus the periodic status bundle.
class DebugMonitor final : public NamedState {
  public:
    explicit DebugMonitor(Flags& flags) : NamedState("DebugMonitor"), flags_(flags) {}
    sm::ActionResult onTick(sm::StateContext& ctx) override {
        emit(ctx, output_event::kPublishStateMachineEvents);
        if (flags_.tick % 200 == 0) {
            emit(ctx, output_event::kPublishControllerStatus);
            emit(ctx, output_event::kPublishSensorStats);
            emit(ctx, output_event::kPublishTrackingError);
        }
        if (flags_.tick % 1000 == 0) {
            emit(ctx, output_event::kPrintSensorDebug);
        }
        return {};
    }

  private:
    void emit(sm::StateContext& ctx, sm::EventId id) const {
        sm::Event event(id, sm::EventTimestamp{flags_.now});
        event.source = "debug";
        ctx.emitOutput(std::move(event));
    }

    Flags& flags_;
};

inline std::unique_ptr<sm::StateMachine>
buildControllerLikeMachine(Flags& f, std::shared_ptr<sm::Clock> clock = std::make_shared<sm::SteadyClock>()) {
    using namespace state_id;
    auto builder = sm::StateMachine::builder("FlightStateMachine", sm::RuntimeOptions{}, std::move(clock));
    builder.region(region_id::kHealth)
        .name("health")
        .order(0)
        .initial(kHealthMonitor)
        .state(kHealthMonitor)
        .name("HealthMonitor")
        .impl(std::make_unique<HealthMonitor>(f))
        .endRegion()
        .region(region_id::kControl)
        .name("flight")
        .order(10)
        .initial(kSelfCheck)
        .state(kSelfCheck)
        .name("SelfCheck")
        .impl(std::make_unique<NamedState>("SelfCheck"))
        .state(kNormal)
        .name("Normal")
        .initial(kReady)
        .state(kReady)
        .name("Ready")
        .impl(std::make_unique<NamedState>("Ready"))
        .state(kTakeoff)
        .name("Takeoff")
        .initial(kTakeoffInit)
        .state(kTakeoffInit)
        .name("TakeoffInit")
        .impl(std::make_unique<Streaming>("TakeoffInit", f))
        .state(kTakeoffOffboard)
        .name("TakeoffOffboardRequest")
        .impl(std::make_unique<Streaming>("TakeoffOffboardRequest", f))
        .state(kTakeoffArm)
        .name("TakeoffArmRequest")
        .impl(std::make_unique<Streaming>("TakeoffArmRequest", f))
        .state(kTakeoffAscending)
        .name("TakeoffAscending")
        .impl(std::make_unique<Streaming>("TakeoffAscending", f))
        .endState()
        .state(kHover)
        .name("Hover")
        .impl(std::make_unique<Streaming>("Hover", f))
        .state(kCustom1)
        .name("Custom1")
        .impl(std::make_unique<Streaming>("Custom1", f))
        .endState()
        .state(kLanding)
        .name("Landing")
        .impl(std::make_unique<Streaming>("Landing", f))
        .endRegion()
        .region(region_id::kDebug)
        .name("debug")
        .order(20)
        .initial(kDebugMonitor)
        .state(kDebugMonitor)
        .name("DebugMonitor")
        .impl(std::make_unique<DebugMonitor>(f))
        .endRegion();

    const auto guard_only = [&](sm::StateId from, sm::StateId to, int priority, std::function<bool()> test) {
        builder.transition().from(from).to(to).priority(priority).when(
            [test = std::move(test)](const sm::GuardContext&) {
                return test();
            });
    };
    const auto event_only = [&](sm::StateId from, sm::StateId to, sm::EventId event, int priority) {
        builder.transition().from(from).to(to).on(event).priority(priority);
    };
    const auto event_guard = [&](sm::StateId from, sm::StateId to, sm::EventId event, int priority,
                                 std::function<bool()> test) {
        builder.transition().from(from).to(to).on(event).priority(priority).when(
            [test = std::move(test)](const sm::GuardContext&) {
                return test();
            });
    };

    Flags* flags = &f;
    guard_only(kSelfCheck, kNormal, kAutomatic, [flags] {
        return flags->sensors_ready;
    });
    guard_only(kReady, kSelfCheck, kAutomatic, [flags] {
        return !flags->sensors_ready;
    });
    guard_only(kReady, kLanding, kCritical, [flags] {
        return flags->airborne;
    });
    event_guard(kReady, kTakeoff, input_event::kTakeoffRequested, kCommand, [flags] {
        return flags->distance_available && !flags->distance_exceeded;
    });
    guard_only(kTakeoff, kLanding, kEmergency, [flags] {
        return flags->distance_exceeded;
    });
    guard_only(kHover, kLanding, kEmergency, [flags] {
        return flags->distance_exceeded;
    });
    guard_only(kCustom1, kLanding, kEmergency, [flags] {
        return flags->distance_exceeded;
    });
    guard_only(kTakeoffInit, kReady, kCritical, [flags] {
        return !flags->sensors_ready;
    });
    guard_only(kTakeoffOffboard, kReady, kCritical, [flags] {
        return !flags->sensors_ready;
    });
    event_guard(kTakeoffInit, kTakeoffOffboard, input_event::kAltctlReady, kAutomatic, [flags] {
        return flags->sensors_ready;
    });
    event_only(kTakeoffOffboard, kTakeoffArm, input_event::kOffboardReady, kAutomatic);
    event_only(kTakeoffArm, kTakeoffAscending, input_event::kArmReady, kAutomatic);
    event_only(kTakeoffAscending, kHover, input_event::kAltitudeReached, kAutomatic);
    event_only(kTakeoffAscending, kHover, input_event::kHoverRequested, kCommand);
    guard_only(kTakeoffInit, kLanding, kAutomatic, [flags] {
        return flags->take_off_abort;
    });
    guard_only(kTakeoffOffboard, kLanding, kAutomatic, [flags] {
        return flags->take_off_abort;
    });
    guard_only(kTakeoffArm, kLanding, kAutomatic, [flags] {
        return flags->take_off_abort;
    });
    event_only(kTakeoffAscending, kLanding, input_event::kTakeoffTimeout, kAutomatic);
    event_guard(kTakeoffAscending, kCustom1, input_event::kTrajectoryTrackingRequested, kCommand, [flags] {
        return flags->custom1_reference_ready;
    });
    guard_only(kTakeoffAscending, kCustom1, kAutomatic, [flags] {
        return flags->custom1_requested && flags->custom1_reference_ready;
    });
    event_guard(kHover, kCustom1, input_event::kTrajectoryTrackingRequested, kCommand, [flags] {
        return flags->custom1_reference_ready;
    });
    guard_only(kHover, kCustom1, kAutomatic, [flags] {
        return (flags->custom1_requested && flags->custom1_reference_ready) || flags->toggle;
    });
    event_only(kCustom1, kHover, input_event::kHoverRequested, kCommand);
    event_only(kCustom1, kHover, input_event::kReferenceTrajectoryFinished, kAutomatic);
    guard_only(kCustom1, kHover, kAutomatic, [flags] {
        return !flags->toggle;
    });
    event_only(kNormal, kLanding, input_event::kLandingRequested, kUserCommand);
    for (const sm::EventId safety : kSafetyToLanding) {
        if (safety != input_event::kLandingRequested) {
            event_only(kNormal, kLanding, safety, kEmergency);
        }
    }
    event_only(kLanding, kSelfCheck, input_event::kTouchdown, kAutomatic);

    auto result = builder.build();
    if (!result.ok()) {
        std::fprintf(stderr, "build failed: %s\n", result.status.message.c_str());
        std::abort();
    }
    return std::move(result.value);
}

} // namespace controller_like
