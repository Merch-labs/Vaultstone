#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_TAG="backupper-build:debian12"

docker build -f "${ROOT_DIR}/docker/Dockerfile.debian12-build" -t "${IMAGE_TAG}" "${ROOT_DIR}"

docker run --rm \
  -v "${ROOT_DIR}:/workspace" \
  -w /workspace \
  "${IMAGE_TAG}" \
  bash -lc '
    cmake -S . -B build-debian12 \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++
    cmake --build build-debian12
  '

echo "Built Debian 12 compatible plugin at:"
echo "  ${ROOT_DIR}/build-debian12/endstone_endstone_backupper.so"
