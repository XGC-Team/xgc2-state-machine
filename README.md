# XGC2 State Machine

`libxgc2-state-machine-dev` provides a small deterministic, event-driven C++17 state machine runtime. It is intentionally packaged as a system development library so ROS1 packages, non-ROS CMake projects, MATLAB bindings, and Python extensions can all consume the same installed headers and shared library.

The runtime is intentionally simple:

- the state graph is configured before `start()`;
- events enter the machine through `postEvent()` or callback-side `StateContext::postEvent()`;
- all state transitions, callbacks, task bookkeeping, and fault handling run inside the owner thread's `update()`;
- cross-thread inputs are accepted only as queued events;
- public APIs return `Status` or `Result<T>` and must be checked by callers.

For deeper runtime semantics, see [`docs/runtime_design.md`](docs/runtime_design.md).

## Install

```bash
sudo apt update
sudo apt install libxgc2-state-machine-dev
```

The package installs:

```text
/usr/include/state_machine/state_machine.hpp
/usr/lib/<multiarch>/libxgc2_state_machine.so
/usr/lib/<multiarch>/cmake/xgc2_state_machine/
```

## CMake Usage

```cmake
find_package(xgc2_state_machine REQUIRED CONFIG)

target_link_libraries(your_target
  PRIVATE
    xgc2_state_machine::state_machine
)
```

## Source Layout

```text
include/state_machine/state_machine.hpp   Public API
src/state_machine.cpp                     Runtime implementation
test/state_machine_runtime_test.cpp       Unit, stress, and sanitizer-oriented tests
docs/runtime_design.md                    Design and reliability notes
.xgc2/scripts/build_deb.sh                Debian package builder
```

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

    status = machine.setInitialState({sm::kDefaultRegion, kIdle});
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
- `setInitialState({RegionId, StateId})`
- `start()`
- `postEvent(Event)`
- `update(UpdateOptions)`
- `stop()`
- `snapshot()`, `eventLog()`, `faultLog()`, `currentEvents()`

## Threading Rules

`postEvent()` is thread-safe and may be called from ROS callbacks or worker threads.

`update()` is owner-thread only. The owner is bound by `bindOwnerThread()` or by the first successful `start()`. Calling `update()` from another thread returns `kWrongOwnerThread`. Recursive `update()` returns `kUpdateAlreadyInProgress`, records a fault, and schedules a fault event.

State callbacks should not directly mutate the graph or call `update()`. They should return `Status` and use `StateContext::postEvent()` when they need follow-up behavior.

## Build And Test

```bash
cmake -S . -B .ci/build -DCMAKE_BUILD_TYPE=Release
cmake --build .ci/build -- -j"$(nproc)"
(cd .ci/build && ctest --output-on-failure)
```

Build the Debian package:

```bash
.xgc2/scripts/build_deb.sh
```

The CI builds and smoke-tests `libxgc2-state-machine-dev` for Ubuntu 20.04, 22.04, and 24.04 on both `amd64` and `arm64`.
