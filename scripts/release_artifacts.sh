#!/usr/bin/env bash
# The artifact inventory is data, not a second build graph.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
source "$root/scripts/release_targets.sh"
version=$("$root/scripts/version.sh")
scope=${1:-complete}
case "$scope" in
  binary) ;;
  complete) [[ -z ${LIBMDF_TARGETS:-} ]] || { printf 'Complete inventory requires the full declared target matrix\n' >&2; exit 1; };;
  *) printf 'usage: release_artifacts.sh [complete|binary]\n' >&2; exit 2;;
esac
for target in "${release_targets[@]}"; do
  printf '%s\n' "libmdf-$version-$target.tar.gz" "cmdf-$version-$target.tar.gz"
done
if [[ $scope == complete ]]; then
  printf '%s\n' "libmdf-$version.tar.gz" "libmdf-lua-$version.tar.gz" "libmdf-$version-1.rockspec" "libmdf-$version-1.src.rock"
fi
