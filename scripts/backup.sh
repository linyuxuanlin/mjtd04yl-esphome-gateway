#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
profile="${1:-mac}"
backup_dir="${2:-${project_dir}/backups}"

case "${profile}" in
  mac)
    compose_files=("-f" "${project_dir}/compose.yaml" "-f" "${project_dir}/compose.mac.yaml")
    ;;
  debian)
    compose_files=("-f" "${project_dir}/compose.yaml" "-f" "${project_dir}/compose.debian.yaml")
    ;;
  *)
    echo "usage: $0 [mac|debian] [backup-directory]" >&2
    exit 2
    ;;
esac

mkdir -p "${backup_dir}"
timestamp="$(date +%Y%m%d-%H%M%S)"
archive="${backup_dir}/homeassistant-${timestamp}.tar.gz"

container_id="$(docker compose "${compose_files[@]}" ps -q homeassistant)"
was_running=0
if [[ -n "${container_id}" ]] && [[ "$(docker inspect -f '{{.State.Running}}' "${container_id}")" == "true" ]]; then
  was_running=1
fi

restart_if_needed() {
  if [[ "${was_running}" == "1" ]]; then
    docker compose "${compose_files[@]}" start homeassistant >/dev/null || true
  fi
}

if [[ "${was_running}" == "1" ]]; then
  trap restart_if_needed EXIT
  docker compose "${compose_files[@]}" stop homeassistant
fi

tar -czf "${archive}" -C "${project_dir}" homeassistant

if [[ "${was_running}" == "1" ]]; then
  docker compose "${compose_files[@]}" start homeassistant >/dev/null
  trap - EXIT
fi

echo "backup=${archive}"
