#!/usr/bin/env bash
set -euo pipefail

DEB_DIR="${DEB_DIR:-${PWD}/debs}"
APT_REPO_HOST="${APT_REPO_HOST:-}"
APT_REPO_PORT="${APT_REPO_PORT:-22}"
APT_REPO_USER="${APT_REPO_USER:-aptdeploy}"
APT_REPO_DISTRIBUTION="${APT_REPO_DISTRIBUTION:-focal}"
APT_REPO_SSH_KEY="${APT_REPO_SSH_KEY:-}"
APT_REPO_KNOWN_HOSTS="${APT_REPO_KNOWN_HOSTS:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deb-dir)
      DEB_DIR="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "${APT_REPO_HOST}" || -z "${APT_REPO_SSH_KEY}" || -z "${APT_REPO_KNOWN_HOSTS}" ]]; then
  echo "APT_REPO_HOST, APT_REPO_SSH_KEY and APT_REPO_KNOWN_HOSTS are required" >&2
  exit 1
fi

if ! compgen -G "${DEB_DIR}/*.deb" >/dev/null; then
  echo "no .deb files found in ${DEB_DIR}" >&2
  exit 1
fi

tmp_dir="$(mktemp -d)"
cleanup() {
  rm -rf "${tmp_dir}"
}
trap cleanup EXIT

key_file="${tmp_dir}/apt-repo-key"
known_hosts_file="${tmp_dir}/known_hosts"
printf '%s\n' "${APT_REPO_SSH_KEY}" > "${key_file}"
printf '%s\n' "${APT_REPO_KNOWN_HOSTS}" > "${known_hosts_file}"
chmod 0600 "${key_file}" "${known_hosts_file}"

ssh_args=(
  -i "${key_file}"
  -p "${APT_REPO_PORT}"
  -o IdentitiesOnly=yes
  -o StrictHostKeyChecking=yes
  -o "UserKnownHostsFile=${known_hosts_file}"
)

tar -C "${DEB_DIR}" -cf - . |
  ssh "${ssh_args[@]}" "${APT_REPO_USER}@${APT_REPO_HOST}" "publish ${APT_REPO_DISTRIBUTION}"

echo "published ${DEB_DIR}/*.deb to ${APT_REPO_HOST}:${APT_REPO_PORT} distribution ${APT_REPO_DISTRIBUTION}"
