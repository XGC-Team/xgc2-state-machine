#!/usr/bin/env bash
set -euo pipefail

dpkg -s libxgc2-state-machine-dev >/dev/null
multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
test -f /usr/include/state_machine/adt_machine.hpp
test ! -e /usr/include/state_machine/state_machine.hpp
test ! -d /usr/include/state_machine/runtime
test ! -e /usr/include/state_machine/ros1_adaptive_loop.hpp
test ! -e /usr/include/state_machine/ros1_queued_owner.hpp
test ! -e /usr/include/state_machine/sync_runtime_tick_adapter.hpp
test -f "/usr/lib/${multiarch}/cmake/xgc2_state_machine/xgc2_state_machineConfig.cmake"
if compgen -G "/usr/lib/${multiarch}/libxgc2_state_machine.so*" >/dev/null; then
  echo "Retired state-machine library remains installed" >&2
  exit 1
fi

probe_dir="${XGC2_STATE_MACHINE_SMOKE_DIR:-$(mktemp -d -t xgc2-state-machine-smoke-XXXXXX)}"
mkdir -p "${probe_dir}"
# Compile every installed public header alone and together; never a selected subset.
mapfile -t public_headers < <(find /usr/include/state_machine -type f \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.hh' -o -name '*.hxx' \) | sort)
test "${#public_headers[@]}" -gt 0
: > "${probe_dir}/surface.cpp"
for header in "${public_headers[@]}"; do
  relative="${header#/usr/include/}"
  printf '#include <%s>\n' "${relative}" > "${probe_dir}/one-header.cpp"
  "${CXX:-c++}" -std=c++17 -pthread -fsyntax-only "${probe_dir}/one-header.cpp"
  printf '#include <%s>\n' "${relative}" >> "${probe_dir}/surface.cpp"
done
"${CXX:-c++}" -std=c++17 -pthread -fsyntax-only "${probe_dir}/surface.cpp"
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
find_package(xgc2_state_machine 0.2.0 REQUIRED CONFIG COMPONENTS workers)
foreach(target xgc2_state_machine::state_machine xgc2_state_machine::workers)
  foreach(property INTERFACE_INCLUDE_DIRECTORIES INTERFACE_LINK_LIBRARIES)
    get_target_property(value ${target} ${property})
    if("${value}" MATCHES "roscpp|rosgraph|/opt/ros|catkin")
      message(FATAL_ERROR "ROS dependency in installed ${target}: ${value}")
    endif()
  endforeach()
endforeach()
if(TARGET xgc2_state_machine::ros1 OR TARGET xgc2_state_machine::ros1_loop)
  message(FATAL_ERROR "Retired ROS target remains exported")
endif()
add_executable(surface surface.cpp)
target_link_libraries(surface PRIVATE xgc2_state_machine::workers)
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
cat >> "${probe_dir}/surface.cpp" <<'CPP'
#include <future>
struct Queue {
  int calls = 0;
  void dispatchOne() { ++calls; }
  bool empty() const { return true; }
};
int main() {
  static_assert(__cplusplus >= 201703L, "C++17 must propagate from workers");
  state_machine::SignalSlot<int> slot;
  slot.replace(7);
  if (slot.take() != 7) return 1;
  state_machine::NextWake<int> next;
  next.at(3); next.at(5);
  if (next.deadline() != 3) return 2;
  int posts = 0;
  state_machine::QueuedTurn turn;
  turn.request([&] { ++posts; }); turn.request([&] { ++posts; });
  turn.beginTurn(); turn.request([&] { ++posts; });
  if (posts != 2) return 3;
  int installs = 0, cancellations = 0;
  state_machine::DeadlineArm<int> arm;
  const auto install = [&](int) { ++installs; };
  const auto cancel = [&] { ++cancellations; };
  arm.update(10, install, cancel); arm.update(10, install, cancel);
  arm.consumed(); arm.update(10, install, cancel);
  arm.update(std::nullopt, install, cancel); arm.update(std::nullopt, install, cancel);
  if (installs != 2 || cancellations != 1) return 4;
  Queue queue;
  state_machine::OwnerWake wake;
  state_machine::OwnerLoop loop(wake);
  loop.run(queue, [&] { wake.stop(); }, [] { return state_machine::OwnerWake::Deadline{}; },
           [](const auto& deadline) { return deadline; }, [] { return true; });
  if (queue.calls != 1) return 5;
  std::promise<int> ordered_result;
  auto ordered_ready = ordered_result.get_future();
  const auto execute = [&](int value) { ordered_result.set_value(value); };
  state_machine::OrderedWorker<int, decltype(execute)> ordered(execute);
  ordered.start(); ordered.post(6);
  const auto ordered_status = ordered_ready.wait_for(std::chrono::seconds(1));
  ordered.stop();
  if (ordered_status != std::future_status::ready || ordered_ready.get() != 6) return 6;
  std::promise<int> completion;
  auto completed = completion.get_future();
  state_machine::AsyncWorker<int, int> worker(
      [](int request) { return request + 1; }, [](int) { return true; },
      [&](int result) { completion.set_value(result); });
  worker.start();
  if (!worker.trySubmit(7)) return 7;
  const auto status = completed.wait_for(std::chrono::seconds(1));
  worker.stop();
  return status == std::future_status::ready && completed.get() == 8 ? 0 : 8;
}
CPP
mkdir -p "${probe_dir}/build"
(cd "${probe_dir}/build" && cmake "${probe_dir}")
cmake --build "${probe_dir}/build" -- -j2
"${probe_dir}/build/probe"
"${probe_dir}/build/surface"
echo "Complete public surface and core/workers installed contracts passed."
