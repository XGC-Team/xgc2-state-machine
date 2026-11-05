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
