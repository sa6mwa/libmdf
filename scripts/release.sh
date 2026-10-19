#!/usr/bin/env bash
# The local proof sequence. Publication remains an explicit engineer operation.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
mode=${1:-prepare}
case "$mode" in
  final)
    "$root/scripts/lifecycle_version_contract.sh"
    make --no-print-directory -C "$root" clean;;
  prepare) ;;
  *) printf 'usage: release.sh [prepare|final]\n' >&2; exit 2;;
esac
make --no-print-directory -C "$root" format-check
make --no-print-directory -C "$root" test-hardening
if [[ $mode == final ]]; then
  make --no-print-directory -C "$root" test-release
  make --no-print-directory -C "$root" package-source
fi
make --no-print-directory -C "$root" package
make --no-print-directory -C "$root" release-lua-artifacts
if [[ $mode == final ]]; then
  make --no-print-directory -C "$root" package-checksums
  make --no-print-directory -C "$root" verify-release-archives
else
  make --no-print-directory -C "$root" package-verify
  make --no-print-directory -C "$root" verify-lua-artifacts
fi
