# XGC2 State Machine

C++17 value-state kernel. A single owner dispatches typed input to the active
alternative, then explicitly applies `Stay` or `Enter<T>`. Every entry, including
same-kind reentry, destroys the previous state's resources. The owner supplies
time; the kernel owns no clocks, threads, queues, callbacks or runtime policies.

```cmake
find_package(xgc2_state_machine 0.2.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE xgc2_state_machine::state_machine)
```

Include `<state_machine/adt_machine.hpp>`. The exported INTERFACE target supplies
the include path and C++17 requirement. There is no state-machine shared library.
`Spec` supplies its `State` variant, `Time` value and typed `handle` overloads.
`dispatch` returns the handler's result; the owner calls `apply` at the domain's
commit point. States must have nonthrowing move construction and destruction.
A parent may own a child `AdtMachine`; transitions keep ownership local.

Version 0.2 removes the previous builder/event/task runtime and its headers.
Consumers must migrate before upgrading; there is no old-engine mode or shim.


## Busy-refusing compute worker (optional workers component)

`AsyncWorker<Request, Result>` reuses one existing solver thread. It accepts a
request only when the original stopped/busy/pending conditions allow it;
pending work is never overwritten. Compute, publish and notify run outside
the handoff mutex, in this order: compute(request), publish(result), idle,
notify(result) when publication requests notification. Notification sees the
same result object and its existing recipient identity. The product retains
snapshot/admission rules, backend operations, result admission and actual wake.

The stored Compute callable is an initial value. The start caller copies it once before each new thread launch, then moves
that copy into the thread, so mutable lambda captures hold that run's backend-entry locals.
Repeated start on an already-started worker does not reset them. This may
allocate once in start before launch; there is no per-request callable copy. Compute
or publish exceptions retain uncaught thread behavior; no failure result is
invented. Pre-start submission and start clearing pending preserve the original
NMPC edge behavior; normal consumers start before submitting.

The lifecycle owner serializes start/close/join, never from a worker callback.
`close()` closes admission and discards unclaimed work; `join()` requires close
first and waits for claimed work. Products may perform their original recipient
invalidation between them. `stop()` performs both. join/stop return whether a
thread was joined, for the original conditional backend exit in UGV. Stop/join
latency is governed by the existing solver; the library adds no timeout.


## Owner scheduling (standard C++17)

All installed headers and exported targets are independent of ROS and native host APIs.
`owner_schedule.hpp` provides `QueuedTurn` and `DeadlineArm<Time>`.
The caller retains its original owner/lock and queue; only turn requests coalesce,
not input samples or FIFO events. Call `beginTurn()` first in the queued callback.
`cancel(remove)` removes the callback without adding a join or restart contract.

`DeadlineArm::arm(value, install)` suppresses equal deadlines before calling the
lazy installation function. A changed deadline is recorded only after installation
returns. Earlier and later values both replace the old arm; `consumed()` clears it
before notifying the owner. `cancel(remove)` invokes removal only when armed.
`update(optional_value, install, remove)` combines those operations. Each boundary
maps cancellation to its own API; there is no negative-time sentinel in this library.
Clocks, due-now decisions, rounding and business phases remain with their owners.

`owner_loop.hpp` provides `OwnerWake`, `OwnerLoop` and `projectOwnerDeadline`.
The loop borrows a queue with `dispatchOne()` and `empty()`, then executes exactly
one callback, work and demand per turn. Remaining callbacks continue before waiting.
A revision captured before dispatch is checked under the wait mutex, so an input
or completion arriving during work or before sleep is retained. Idle waiting uses
a steady deadline or notification, without polling or creating a thread. Link the
existing optional `workers` target for standard thread support when using this wait.

`projectOwnerDeadline` preserves the existing double domain/steady deadline
projection. A future external-clock deadline waits for a clock notification;
a separate steady deadline still wakes. Clock reads stay lazy and the existing
ceil-to-nanoseconds conversion is retained. Separate typed `DeadlineArm` instances
must be used for obligations from different clock domains.

ROS callback, timer, delivered-clock and shutdown wiring lives in `ros1-utils`:
`ros1_utils/queued_owner.h` and `ros1_utils/adaptive_owner_loop.h`. There are no
SDK ROS components or compatibility headers. Native modules directly use
`DeadlineArm<int64_t>::update(next, arm_callback, cancel_callback)` at their
existing step/start/stop boundaries; the host continues to own its queue and timer.
