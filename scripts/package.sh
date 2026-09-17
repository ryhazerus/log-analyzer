#!/usr/bin/env bash
# Packages a built binary plus deploy files into dist/podlogs-<version>-<target>.tar.gz
#   scripts/package.sh out/podlogs linux-amd64 v1.2.3
set -euo pipefail
bin="$1"
target="$2"
version="$3"
here="$(cd "$(dirname "$0")/.." && pwd)"

name="podlogs-${version}-${target}"
stage="${here}/dist/${name}"
rm -rf "${stage}"
mkdir -p "${stage}"
install -m 0755 "${bin}" "${stage}/podlogs"
install -m 0644 "${here}/deploy/podlogs.json" "${stage}/podlogs.json"
install -m 0644 "${here}/deploy/systemd/podlogs.service" "${stage}/podlogs.service"
install -m 0644 "${here}/deploy/systemd/podlogs-user.service" "${stage}/podlogs-user.service"
install -m 0644 "${here}/deploy/quadlet/podlogs.container" "${stage}/podlogs.container"
install -m 0644 "${here}/README.md" "${stage}/README.md"
tar -C "${here}/dist" -czf "${here}/dist/${name}.tar.gz" "${name}"
rm -rf "${stage}"
echo "created dist/${name}.tar.gz"
