#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

bash -n .xgc2/scripts/*.sh

nested_git="$(
  find . \
    -path ./.git -prune -o \
    -path ./.ci -prune -o \
    -path ./build -prune -o \
    -name .git -print
)"
if [[ -n "${nested_git}" ]]; then
  echo "Nested .git directory found." >&2
  echo "${nested_git}" >&2
  exit 1
fi

if git ls-files | grep -E '(^|/)(build|devel|install|\.catkin_tools|\.ci)(/|$)' >/dev/null; then
  echo "Generated build artifacts are tracked." >&2
  git ls-files | grep -E '(^|/)(build|devel|install|\.catkin_tools|\.ci)(/|$)' >&2
  exit 1
fi

required_files=(
  .clang-format
  .clang-tidy
  README.md
  CMakeLists.txt
  cmake/xgc2_state_machineConfig.cmake.in
  include/state_machine/state_machine.hpp
  include/state_machine/runtime/event_dispatcher.hpp
  include/state_machine/runtime/event_post.hpp
  include/state_machine/runtime/event_time.hpp
  src/state_machine.cpp
  test/state_machine_runtime_test.cpp
  .github/workflows/ci.yml
  .xgc2/product.yml
  .xgc2/scripts/build_deb.sh
  .xgc2/scripts/check_cpp_quality.sh
  .xgc2/scripts/check_package_compliance.sh
  .xgc2/scripts/smoke_test_installed.sh
)

for file in "${required_files[@]}"; do
  if [[ ! -f "${file}" ]]; then
    echo "Missing required file: ${file}" >&2
    exit 1
  fi
done

for removed_file in package.xml .github/workflows/build-debs.yml; do
  if [[ -e "${removed_file}" ]]; then
    echo "master system package branch must not keep ROS/catkin file: ${removed_file}" >&2
    exit 1
  fi
done

if grep -R "find_package(catkin\\|catkin_package\\|catkin_add_gtest" CMakeLists.txt cmake 2>/dev/null; then
  echo "master system package branch must not use catkin." >&2
  exit 1
fi

echo "libxgc2-state-machine-dev package compliance checks passed."
