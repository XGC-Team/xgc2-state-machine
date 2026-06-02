#!/usr/bin/env bash

set -euo pipefail

dpkg -s libxgc2-state-machine-dev >/dev/null

multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
test -f /usr/include/state_machine/state_machine.hpp
test -f "/usr/lib/${multiarch}/libxgc2_state_machine.so"
test -f "/usr/lib/${multiarch}/cmake/xgc2_state_machine/xgc2_state_machineConfig.cmake"

ldd "/usr/lib/${multiarch}/libxgc2_state_machine.so" | tee /tmp/xgc2-state-machine-ldd.txt
if grep -q "not found" /tmp/xgc2-state-machine-ldd.txt; then
  exit 1
fi

probe_dir="${XGC2_STATE_MACHINE_SMOKE_DIR:-$(mktemp -d -t xgc2-state-machine-smoke-XXXXXX)}"
mkdir -p "${probe_dir}"

cat > "${probe_dir}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(xgc2_state_machine_link_probe LANGUAGES CXX)

find_package(xgc2_state_machine REQUIRED CONFIG)
xgc2_state_machine_require()

add_executable(link_probe link_probe.cpp)
target_compile_features(link_probe PRIVATE cxx_std_17)
target_link_libraries(link_probe PRIVATE xgc2_state_machine::state_machine)
CMAKE

cat > "${probe_dir}/link_probe.cpp" <<'CPP'
#include <state_machine/state_machine.hpp>

#include <memory>

class ReadyState final : public state_machine::State {
public:
  std::string name() const override { return "Ready"; }

  state_machine::ActionResult onEnter(state_machine::StateContext&) override
  {
    entered = true;
    return {};
  }

  bool entered{false};
};

int main()
{
  auto ready = std::make_unique<ReadyState>();
  auto* ready_ptr = ready.get();

  state_machine::StateMachine machine("probe");
  if (!machine.addState({1}, std::move(ready)).ok()) {
    return 1;
  }
  if (!machine.setInitialState(state_machine::kDefaultRegion, 1).ok()) {
    return 1;
  }
  if (!machine.start().ok()) {
    return 1;
  }
  if (!ready_ptr->entered) {
    return 1;
  }
  return machine.currentState() == 1 ? 0 : 1;
}
CPP

cmake -S "${probe_dir}" -B "${probe_dir}/build"
cmake --build "${probe_dir}/build" -- -j2
"${probe_dir}/build/link_probe"

echo "libxgc2-state-machine-dev installed smoke test passed."
