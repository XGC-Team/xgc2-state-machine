# Deterministic State Runtime

`state_machine` is a small deterministic state runtime for shared robotics infrastructure. It intentionally does not depend on a third-party FSM framework.

## Design Principles

1. The state machine is not a concurrent container. It is a deterministic reducer owned by one update thread.
2. Cross-thread inputs, asynchronous task completion, communication changes, and faults must first become events.
3. State graph evaluation, state transitions, callbacks, and task lifecycle mutation only run inside owner-thread `StateMachine::update()`.

## Runtime Semantics

- The owner thread is bound by `bindOwnerThread()` or the first successful `start()`. `update()` from another thread is rejected.
- `update()` is non-reentrant. Recursive update attempts return `kUpdateAlreadyInProgress`, record a fault, and enqueue a fault event for a future update.
- `postEvent()` is thread-safe and accepts only `EventCategory::kInput`. FIFO order is defined by the runtime-assigned monotonic `Event::sequence`, but event order is not a business priority mechanism.
- `RuntimeOptions::max_pending_events` bounds the pending inbox. A value of `0` disables the bound.
- Runtime-generated fault events bypass the pending-inbox bound so callback faults cannot be silently hidden by normal event backpressure.
- Each `update()` is one logical tick. It takes a bounded snapshot of external input events at tick start. External events posted during callbacks or by other threads are processed by later updates.
- State callbacks may create `kInternal` events with `StateContext::postInternalEvent()`. Internal events produced by an earlier ordered region are visible to later regions in the same tick; events produced by later regions never affect already-executed regions until a later tick.
- State callbacks may create `kOutput` events with `StateContext::emitOutput()`. Output events never drive transitions or `onEvent()` and are exposed through `currentOutputEvents()` for external consumers.
- Guards receive `GuardContext`, which is read-only. Actions and state callbacks receive `StateContext`, which can post internal events, emit output events, and manage tasks.
- Hierarchical transitions use leaf-to-root selection and LCA-based exit/enter order.
- Transition types are `External`, `ExternalSelf`, `Internal`, and `Targetless`.
- Regions are evaluated by `RegionConfig::execution_order`, with registration order as a stable tie-breaker.
- Each region can commit at most one transition per update tick.
- Transition candidates are selected from visible input/internal events and the active state path. Events carry no priority. Candidate ordering is transition `priority` descending, `evaluation_order` ascending, deeper active state first, registration order, then event sequence as a final tie-breaker.
- Global transitions close other regions when committed. They are still evaluated from their source region according to the same ordered-region pass.
- Task results are accepted only if the task is active, correlation matches, and the owner state remains active in the region path. `KeepRunning` means the runtime does not request cancellation on state exit; it does not bypass stale-result filtering. Long-lived tasks should be owned by a parent state that remains active for the intended lifetime.
- Faults are logged and converted into fault events. Repeated fault handling is fused by `RuntimeOptions::max_fault_depth`.
- `stop()` is a thread-safe request. State exits happen deterministically in the owner update.

## Interface Boundary

The public API is `state_machine/state_machine.hpp`. `StateMachine` and `State` are the only public runtime types for machine and state objects.

- callback-side direct state mutation is not supported;
- external events posted during callbacks are processed in later update snapshots;
- internal events posted during callbacks follow ordered-region same-tick visibility;
- new code must use `StateMachine`, `State`, `TransitionRule`, `GuardContext`, and `StateContext`;
- projects that need a domain-specific state base should wrap `State` locally without reintroducing synchronous transition or recursive event semantics.

## Non-goals

- No third-party FSM, behavior tree DSL, or runtime graph mutation.
- No built-in executor or thread pool.
- No cross-process ordering guarantee beyond sequence order after an event enters this runtime inbox.
- No hidden priority queue for safety behavior. Safety, fallback, and fault handling must be explicit state graph behavior.
- No unbounded event or fault history.

## Verification

The package includes gtest coverage for FIFO sequencing, bounded snapshots, pending-event capacity, concurrent event posting, owner/reentrant update checks, lifecycle freezing, hierarchy, transition variants, parallel regions, global preemption, task stale filtering and cancellation policies, fault fuse behavior, stop semantics, and logs/snapshot copies.

Useful commands from `source/ros1_ws`:

```bash
catkin_make run_tests_state_machine
catkin_test_results build/test_results/state_machine
```

Direct sanitizer check:

```bash
g++ -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
  -Isrc/common/state_machine/include \
  src/common/state_machine/src/state_machine.cpp \
  src/common/state_machine/test/state_machine_runtime_test.cpp \
  -lgtest -o /tmp/state_machine_runtime_test_asan
/tmp/state_machine_runtime_test_asan
```

Coverage check:

```bash
rm -rf /tmp/state_machine_cov && mkdir -p /tmp/state_machine_cov
cd /tmp/state_machine_cov
ROS_WS=<ros1-workspace>
g++ -std=c++17 --coverage -O0 -g -pthread \
  -I"${ROS_WS}/src/common/state_machine/include" \
  "${ROS_WS}/src/common/state_machine/src/state_machine.cpp" \
  "${ROS_WS}/src/common/state_machine/test/state_machine_runtime_test.cpp" \
  -lgtest -o state_machine_runtime_test
./state_machine_runtime_test
lcov --capture --directory /tmp/state_machine_cov \
  --output-file /tmp/state_machine_cov/coverage.info \
  --include '*/src/common/state_machine/src/state_machine.cpp'
lcov --summary /tmp/state_machine_cov/coverage.info
```

The coverage target covers the `state_machine` production source through the public runtime API.
