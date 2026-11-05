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


## Existing owner scheduling edges

The optional `ros1` component exports `xgc2_state_machine::ros1` for
`ros1_queued_owner.hpp`: `Ros1QueuedTurn` preserves the existing coalesced
callback on the caller's queue; `Ros1DomainDeadline<Time>` preserves one-shot
timer bookkeeping. This light edge requires roscpp and does not include the
UGV Clock/PollManager implementation. The default state target stays ROS-free.
Repeated find_package calls retain identical imported target properties.

QueuedTurn request/beginTurn/cancel use the caller's original synchronization.
beginTurn is the first action of the queued callback at that same boundary;
cancel occurs before owner teardown/joining its producers. No mutex or lifetime
gate is introduced. DomainDeadline arm(deadline, delay) takes the original
absolute value and a lazy callable returning ros::Duration. The callable runs
only for a changed arm, after the original equality comparison. The product
retains its due branch and clock/rounding rules, and cancels before teardown.

The `ros1_loop` component exports `xgc2_state_machine::ros1_loop`, adding
rosgraph_msgs for the existing UGV `Ros1AdaptiveLoop` in
`ros1_adaptive_loop.hpp`. This owner uses one callback, work, demand and a
latched steady wait. Demand supplies the original double domain/wall absolute
deadlines (infinity means absent); delivered simulation-clock and shutdown
wake semantics are retained. Attach before producers, destroy after joining.
It creates no worker thread, spinner or separate scheduling service.

`SyncRuntimeTickAdapter` owns only the native optional nanosecond arm.
arm(next,wake_at) replaces, deduplicates or cancels; consumed clears a delivered
one-shot. The module converts its deadline and retains host step reasons and
set_period_ns lifecycle/phase. No host ABI/header dependency is added.
