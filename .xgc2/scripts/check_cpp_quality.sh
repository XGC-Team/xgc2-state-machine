#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${XGC2_STATE_MACHINE_QUALITY_BUILD_DIR:-${repo_root}/.ci/cpp-quality}"

cd "${repo_root}"

sources=(
  include/state_machine/state_machine.hpp
  include/state_machine/runtime/event_dispatcher.hpp
  src/state_machine.cpp
  test/state_machine_runtime_test.cpp
)

for tool in cmake ctest clang-format clang-tidy cppcheck; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Missing required C++ quality tool: ${tool}" >&2
    exit 1
  fi
done

clang-format --dry-run --Werror "${sources[@]}"

rm -rf "${build_dir}"
cmake -S "${repo_root}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Werror"
cmake --build "${build_dir}" -- -j"$(nproc)"
(cd "${build_dir}" && ctest --output-on-failure)

clang-tidy --quiet -p "${build_dir}" "${sources[@]}" 2>&1 | sed -E '/^[0-9]+ warnings generated\.$/d'

cppcheck \
  --enable=warning,performance,portability,style \
  --error-exitcode=1 \
  --std=c++17 \
  --inline-suppr \
  --suppress=assertWithSideEffect:test/state_machine_runtime_test.cpp \
  -I include \
  src test

echo "C++ quality checks passed."
