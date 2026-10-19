#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
scope=${1:-binary}
case "$scope" in binary|complete|lua) ;; *) printf 'usage: verify_packages.sh [binary|complete|lua]\n' >&2; exit 2;; esac
build="$root/build/package-checks/$scope"
cmake -S "$root/cmake/package-checks" -B "$build" -G Ninja \
  -DLIBMDF_SOURCE_DIR="$root" -DLIBMDF_DIST_DIR="${LIBMDF_DIST_DIR:-$root/dist}" \
  -DLIBMDF_PACKAGE_WORK_DIR="${LIBMDF_PACKAGE_WORK_DIR:-$root/build/package}" \
  -DLIBMDF_TEST_TARGETS="${LIBMDF_TARGETS:-}" -DLIBMDF_ARTIFACT_SCOPE="$scope"
cmake --build "$build" --target verify
