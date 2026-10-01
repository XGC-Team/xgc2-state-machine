#!/usr/bin/env bash
# Differential test of the runtime against an older revision of itself.
#
#   test/compare_revisions.sh <git revision> [seeds [steps [equivalence test flags...]]]
#
# Builds state_machine_equivalence_test twice with ${CXX:-c++}: against the library
# sources at <git revision> and against this working tree. It then runs the random
# scenarios seed:1 .. seed:<seeds> (default 100 seeds of 1000 steps) through both and
# compares the hash of every observable after every step, byte for byte. The first
# difference is reported with its seed and step; replay it with
#   state_machine_equivalence_test <flags> --steps N --dump seed:S
# on each build and diff the text.
#
# Without flags only scenarios that are defined in every revision run (see struct
# Restrictions in the test). --full lifts them: rules registered interleaved, many
# tasks, internal events posted from every callback. The last of these is undefined
# behaviour before the revision "Keep per-tick internal events at stable
# addresses", so use --full only against that revision or a later one.
#
# CXX names the compiler and CXXFLAGS adds flags for both builds, e.g.
#   CXX=g++-9 CXXFLAGS="-Wl,-rpath,/opt/gcc9/lib" test/compare_revisions.sh 6f31e52
set -euo pipefail

if [ $# -lt 1 ]; then
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit 2
fi

revision=$1
seeds=${2:-100}
steps=${3:-1000}
shift $(($# < 3 ? $# : 3))
flags=("$@")

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx=${CXX:-c++}
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

mkdir -p "${work}/old"
git -C "${repo_root}" archive "${revision}" | tar -x -C "${work}/old"

build() {
    local tree=$1 out=$2
    # shellcheck disable=SC2086 # CXXFLAGS is a list of words
    "${cxx}" -std=c++17 -O1 -pthread ${CXXFLAGS:-} -I"${tree}/include" -I"${repo_root}/test" \
        "${repo_root}/test/state_machine_equivalence_test.cpp" "${tree}/src/state_machine.cpp" -o "${out}"
}
build "${work}/old" "${work}/old_test"
build "${repo_root}" "${work}/new_test"

status=0
for ((seed = 1; seed <= seeds; ++seed)); do
    "${work}/old_test" "${flags[@]}" --steps "${steps}" --hashes --dump "seed:${seed}" >"${work}/old.out"
    "${work}/new_test" "${flags[@]}" --steps "${steps}" --hashes --dump "seed:${seed}" >"${work}/new.out"
    if ! cmp -s "${work}/old.out" "${work}/new.out"; then
        first=$(diff "${work}/old.out" "${work}/new.out" | sed -n '2p' | cut -d' ' -f2)
        echo "DIFFERENT: seed ${seed}, first differing step ${first:-?} (flags: ${flags[*]:-none})"
        status=1
        break
    fi
done
if [ "${status}" -eq 0 ]; then
    echo "identical: ${seeds} seeds x ${steps} steps against ${revision} (flags: ${flags[*]:-none})"
fi
exit "${status}"
