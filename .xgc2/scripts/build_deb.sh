#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

package_name="libxgc2-state-machine-dev"
package_distribution="${PACKAGE_DISTRIBUTION:-${APT_REPO_DISTRIBUTION:-}}"
build_dir="${XGC2_STATE_MACHINE_BUILD_DIR:-${repo_root}/.ci/build}"
stage_dir="${XGC2_STATE_MACHINE_STAGE_DIR:-${repo_root}/.ci/stage}"
output_dir="${XGC2_STATE_MACHINE_DEB_OUTPUT_DIR:-${repo_root}/.ci/debs}"
pkg_root="${repo_root}/.ci/pkg/${package_name}"
arch="$(dpkg --print-architecture)"
multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

product_version() {
  awk -F': *' '/^version:[[:space:]]*/ {print $2; exit}' "${repo_root}/.xgc2/product.yml"
}

package_base_version="${PACKAGE_BASE_VERSION:-$(product_version)}"
if [[ -z "${package_base_version}" ]]; then
  echo "package version is missing; set PACKAGE_BASE_VERSION or .xgc2/product.yml version" >&2
  exit 1
fi

if [[ -z "${package_distribution}" && -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  package_distribution="${VERSION_CODENAME:-${UBUNTU_CODENAME:-}}"
fi

if [[ -n "${PACKAGE_VERSION:-}" ]]; then
  version="${PACKAGE_VERSION}"
else
  if [[ -z "${package_distribution}" ]]; then
    echo "PACKAGE_DISTRIBUTION or VERSION_CODENAME is required for binary Debian package versioning" >&2
    exit 1
  fi
  version="${package_base_version}~${package_distribution}"
fi

if [[ -n "${package_distribution}" && "${ALLOW_UNSCOPED_BINARY_DEB_VERSION:-0}" != "1" ]]; then
  case "${version}" in
    *"~${package_distribution}"*|*"+"${package_distribution}*) ;;
    *)
      echo "binary Debian package version '${version}' must include distribution suffix '${package_distribution}'" >&2
      echo "set ALLOW_UNSCOPED_BINARY_DEB_VERSION=1 only for a deliberately distro-neutral artifact" >&2
      exit 1
      ;;
  esac
fi

rm -rf "${build_dir}" "${stage_dir}" "${output_dir}" "${pkg_root}"
mkdir -p "${output_dir}"

cmake -S "${repo_root}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG"
cmake --build "${build_dir}" -- -j"$(nproc)"
(cd "${build_dir}" && ctest --output-on-failure)
DESTDIR="${stage_dir}" cmake --install "${build_dir}"

mkdir -p \
  "${pkg_root}/DEBIAN" \
  "${pkg_root}/usr/share/doc/${package_name}"

cp -a "${stage_dir}/usr" "${pkg_root}/"

cat > "${pkg_root}/DEBIAN/control" <<EOF
Package: ${package_name}
Version: ${version}
Section: libdevel
Priority: optional
Architecture: ${arch}
Maintainer: XGC2 <apt@example.com>
Depends: libc6, libgcc-s1, libstdc++6
Conflicts: ros-noetic-xgc2-state-machine
Replaces: ros-noetic-xgc2-state-machine
Description: XGC2 deterministic C++ state machine development library
 Header, shared library, and CMake package configuration for the deterministic
 event-driven state machine runtime used by XGC2 controllers and planners.
EOF

cat > "${pkg_root}/DEBIAN/postinst" <<'SH'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig
fi
SH
cat > "${pkg_root}/DEBIAN/postrm" <<'SH'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig
fi
SH
chmod 0755 "${pkg_root}/DEBIAN/postinst" "${pkg_root}/DEBIAN/postrm"

cp -a "${repo_root}/README.md" "${pkg_root}/usr/share/doc/${package_name}/"

test -f "${pkg_root}/usr/include/state_machine/state_machine.hpp"
test -f "${pkg_root}/usr/include/state_machine/runtime/event_dispatcher.hpp"
test -f "${pkg_root}/usr/include/state_machine/runtime/event_post.hpp"
test -f "${pkg_root}/usr/include/state_machine/runtime/event_time.hpp"
test -f "${pkg_root}/usr/lib/${multiarch}/libxgc2_state_machine.so"
test -f "${pkg_root}/usr/lib/${multiarch}/cmake/xgc2_state_machine/xgc2_state_machineConfig.cmake"

find "${pkg_root}" -type d -exec chmod 0755 {} +
find "${pkg_root}" -type f -exec chmod 0644 {} +
chmod 0755 "${pkg_root}/DEBIAN" "${pkg_root}/DEBIAN/postinst" "${pkg_root}/DEBIAN/postrm"
find "${pkg_root}/usr/lib/${multiarch}" -maxdepth 1 -type f -name 'libxgc2_state_machine.so*' -exec chmod 0755 {} +
find "${pkg_root}/usr/lib/${multiarch}" -maxdepth 1 -type f -name 'libxgc2_state_machine.so*' \
  -exec strip --strip-unneeded {} + 2>/dev/null || true

fakeroot dpkg-deb --build "${pkg_root}" "${output_dir}/${package_name}_${version}_${arch}.deb" >/dev/null
dpkg-deb -I "${output_dir}/${package_name}_${version}_${arch}.deb"
echo "Debian artifacts written to ${output_dir}"
