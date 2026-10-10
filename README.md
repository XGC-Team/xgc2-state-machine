# XGC2 State Machine

C++17 deterministic, event-driven state machine library.

## Install

```bash
sudo apt update
sudo apt install libxgc2-state-machine-dev
```

## Use

```cmake
find_package(xgc2_state_machine REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE xgc2_state_machine::state_machine)
```

Include `<state_machine/state_machine.hpp>`. Configure with `StateMachine::builder()`,
then call `start()`, `postEvent()`, `update()` and `stop()`.
`update()` runs on the owner thread; `postEvent()` accepts cross-thread input.
Check returned `Status` and `Result` values.
