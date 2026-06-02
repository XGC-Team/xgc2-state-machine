# state_machine

`state_machine` is a small deterministic, event-driven C++17 state machine runtime packaged as a ROS1/catkin library. It is used by controllers, planners, and workflow code that need explicit lifecycle transitions without depending on an external FSM framework.

The runtime is intentionally simple:

- the state graph is configured before `start()`;
- events enter the machine through `postEvent()` or callback-side `StateContext::postEvent()`;
- all state transitions, callbacks, task bookkeeping, and fault handling run inside the owner thread's `update()`;
- cross-thread inputs are accepted only as queued events;
- public APIs return `Status` or `Result<T>` and must be checked by callers.

For deeper runtime semantics, see [`docs/runtime_design.md`](docs/runtime_design.md).

## Package Layout

```text
state_machine/
  include/state_machine/state_machine.hpp   Public API
  src/state_machine.cpp                     Runtime implementation
  test/state_machine_runtime_test.cpp       Unit, stress, and sanitizer-oriented tests
  docs/runtime_design.md                    Design and reliability notes
```

## Build

From `source/ros1_ws`:

```bash
catkin_make --pkg state_machine
```

Downstream packages should add `state_machine` to `find_package(catkin REQUIRED COMPONENTS ...)`, `catkin_package(CATKIN_DEPENDS ...)`, and `package.xml` dependencies, then link their targets against `${catkin_LIBRARIES}`.

## Minimal Usage

```cpp
#include <memory>
#include <stdexcept>

#include "state_machine/state_machine.hpp"

namespace sm = state_machine;

enum StateId : sm::StateId {
    kIdle = 1,
    kActive = 2,
};

enum EventId : sm::EventId {
    kStart = 1,
    kStop = 2,
};

class IdleState final : public sm::State {
public:
    std::string name() const override { return "Idle"; }
};

class ActiveState final : public sm::State {
public:
    std::string name() const override { return "Active"; }
    sm::ActionResult onTick(sm::StateContext& ctx) override {
        // Callback-generated events are processed by a later update() snapshot.
        return sm::Status{};
    }
};

void setup() {
    sm::StateMachine machine("example");

    auto status = machine.addState({kIdle}, std::make_unique<IdleState>());
    if (!status.ok()) {
        throw std::runtime_error(status.message);
    }

    status = machine.addState({kActive}, std::make_unique<ActiveState>());
    if (!status.ok()) {
        throw std::runtime_error(status.message);
    }

    sm::TransitionRule start_rule;
    start_rule.from = kIdle;
    start_rule.target = kActive;
    start_rule.event = kStart;
    status = machine.addTransition(std::move(start_rule));
    if (!status.ok()) {
        throw std::runtime_error(status.message);
    }

    sm::TransitionRule stop_rule;
    stop_rule.from = kActive;
    stop_rule.target = kIdle;
    stop_rule.event = kStop;
    status = machine.addTransition(std::move(stop_rule));
    if (!status.ok()) {
        throw std::runtime_error(status.message);
    }

    status = machine.setInitialState(sm::kDefaultRegion, kIdle);
    if (!status.ok()) {
        throw std::runtime_error(status.message);
    }

    status = machine.start();
    if (!status.ok()) {
        throw std::runtime_error(status.message);
    }

    status = machine.postEvent(sm::Event(kStart));
    if (!status.ok()) {
        throw std::runtime_error(status.message);
    }

    auto result = machine.update();
    if (!result.ok()) {
        throw std::runtime_error(result.status.message);
    }
}
```

## Runtime API

The public API is declared in `state_machine/state_machine.hpp`.

Core types:

- `StateMachine`: owns graph configuration, event queue, active states, task records, and logs.
- `State`: base class for user-defined state behavior.
- `TransitionRule`: event-triggered transition declaration with optional guard and action.
- `StateContext`: callback context that can post events, start/cancel tasks, and query snapshots.
- `GuardContext`: read-only context for transition guards.
- `Status` and `Result<T>`: explicit success/failure return values.

Common machine calls:

- `addState(StateConfig, std::unique_ptr<State>)`
- `addTransition(TransitionRule)`
- `setInitialState(RegionId, StateId)`
- `start()`
- `postEvent(Event)`
- `update(UpdateOptions)`
- `stop()`
- `snapshot()`, `eventLog()`, `faultLog()`, `currentEvents()`

Important runtime options:

- `RuntimeOptions::max_pending_events`: bounds queued external events. `0` disables the bound.
- `RuntimeOptions::event_log_capacity`: bounds retained event log records.
- `RuntimeOptions::fault_log_capacity`: bounds retained fault records.
- `RuntimeOptions::max_fault_depth`: prevents recursive fault handling loops.
- `UpdateOptions::max_events_per_update`: bounds one update snapshot.
- `UpdateOptions::max_transitions_per_update`: bounds transition work in one update.
- `UpdateOptions::run_tick`: controls whether active states receive `onTick()`.

## Threading Rules

`postEvent()` is thread-safe and may be called from ROS callbacks or worker threads.

`update()` is owner-thread only. The owner is bound by `bindOwnerThread()` or by the first successful `start()`. Calling `update()` from another thread returns `kWrongOwnerThread`. Recursive `update()` returns `kUpdateAlreadyInProgress`, records a fault, and schedules a fault event.

State callbacks should not directly mutate the graph or call `update()`. They should return `Status` and use `StateContext::postEvent()` when they need to drive follow-up behavior.

## Fault Handling

Callback exceptions and failed callback statuses are converted into fault records and fault events. Runtime-generated fault events bypass normal pending-event capacity so that normal queue backpressure cannot hide internal runtime faults.

The library records faults but does not define a domain-specific fail-safe action. Production users must explicitly define fault transitions and fail-safe states in their own graph.

## Current Users

Current in-tree users include:

- `src/controller/multirotor_controller`: UAV and UGV controller lifecycle/state logic.
- `src/planner/formation_generator`: DMPC scheduler lifecycle management.
- `src/manager/workflow`: workflow runner lifecycle transitions.

These packages configure their domain states locally and use this package only as the shared deterministic runtime.

## Tests

Run the catkin tests from `source/ros1_ws`:

```bash
catkin_make run_tests_state_machine
catkin_test_results build/test_results/state_machine
```

Run the package build only:

```bash
catkin_make --pkg state_machine
```

The test suite covers event ordering, bounded update snapshots, pending-event capacity, concurrent `postEvent()`, owner-thread enforcement, transition variants, parallel regions, task result filtering, cancellation policies, fault fuse behavior, stop semantics, and log/snapshot copying.

The repository CI also runs direct ASAN/UBSAN and TSAN builds for the same runtime tests.

## Production Notes

Before using this runtime in safety-critical behavior, each integration should:

- check every `Status` and `Result<T>`;
- define explicit fault transitions and fail-safe states;
- set a finite `max_pending_events` suited to the node's callback rate;
- keep long-running work outside state callbacks and report completion through events or task results;
- run the catkin tests and sanitizer tests in CI.
