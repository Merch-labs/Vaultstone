#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGES_DIR="${ROOT_DIR}/.deps/local-libcxx/packages"
EXTRACT_DIR="${ROOT_DIR}/.deps/local-libcxx/root"

mkdir -p "${PACKAGES_DIR}" "${EXTRACT_DIR}"

packages=(
  "libc++-18-dev"
  "libc++1-18"
  "libc++abi-18-dev"
  "libc++abi1-18"
  "libunwind-18-dev"
  "libunwind-18"
)

(
  cd "${PACKAGES_DIR}"
  apt download "${packages[@]}"
)

rm -rf "${EXTRACT_DIR}"
mkdir -p "${EXTRACT_DIR}"

for archive in "${PACKAGES_DIR}"/*.deb; do
  dpkg-deb -x "${archive}" "${EXTRACT_DIR}"
done

printf 'Local libc++ toolchain extracted to %s\n' "${EXTRACT_DIR}"
printf 'Headers: %s\n' "${EXTRACT_DIR}/usr/include/c++/v1"
printf 'Libraries: %s\n' "${EXTRACT_DIR}/usr/lib/llvm-18/lib"
