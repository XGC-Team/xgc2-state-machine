#!/usr/bin/env bash
set -euo pipefail

dpkg -s libxgc2-state-machine-dev >/dev/null
multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
test -f /usr/include/state_machine/adt_machine.hpp
test ! -e /usr/include/state_machine/state_machine.hpp
test ! -d /usr/include/state_machine/runtime
test -f "/usr/lib/${multiarch}/cmake/xgc2_state_machine/xgc2_state_machineConfig.cmake"
if compgen -G "/usr/lib/${multiarch}/libxgc2_state_machine.so*" >/dev/null; then
  echo "Retired state-machine library remains installed" >&2
  exit 1
fi

probe_dir="${XGC2_STATE_MACHINE_SMOKE_DIR:-$(mktemp -d -t xgc2-state-machine-smoke-XXXXXX)}"
mkdir -p "${probe_dir}"
cat > "${probe_dir}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.10)
project(xgc2_state_machine_installed_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 11)
find_package(xgc2_state_machine 0.2.0 REQUIRED CONFIG)
get_target_property(kind xgc2_state_machine::state_machine TYPE)
if(NOT kind STREQUAL "INTERFACE_LIBRARY")
  message(FATAL_ERROR "The kernel must be header-only")
endif()
add_executable(probe probe.cpp)
target_link_libraries(probe PRIVATE xgc2_state_machine::state_machine)
CMAKE
cat > "${probe_dir}/probe.cpp" <<'CPP'
#include <state_machine/adt_machine.hpp>
struct Cold {};
struct Hot { int value; };
struct Heat {};
struct Spec {
  using State = std::variant<Cold, Hot>;
  using Time = int;
  static state_machine::adt::Transition<Cold, Hot> handle(Cold&, const Heat&) {
    return state_machine::adt::Enter{Hot{42}};
  }
  static state_machine::adt::Transition<Cold, Hot> handle(Hot&, const Heat&) {
    return state_machine::adt::Stay{};
  }
};
int main() {
  static_assert(__cplusplus >= 201703L, "C++17 must propagate from the target");
  state_machine::adt::AdtMachine<Spec> machine(Cold{}, 0);
  machine.apply(machine.dispatch(Heat{}), 1);
  return std::get<Hot>(machine.state()).value == 42 && machine.entered_at() == 1 ? 0 : 1;
}
CPP
mkdir -p "${probe_dir}/build"
(cd "${probe_dir}/build" && cmake "${probe_dir}")
cmake --build "${probe_dir}/build" -- -j2
"${probe_dir}/build/probe"
echo "Header-only installed contract passed."
