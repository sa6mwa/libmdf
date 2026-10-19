#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
source "$root/scripts/release_targets.sh"
version=$("$root/scripts/version.sh")
dist=${LIBMDF_DIST_DIR:-$root/dist}
workspace=${LIBMDF_PACKAGE_WORK_DIR:-$root/build/package}
"$root/scripts/require_build_path.sh" "$workspace"
[[ ! -L $dist ]] || { printf 'Package output must use owned directories\n' >&2; exit 1; }
for target in "${release_targets[@]}"; do
  preset="$target-release"
  "$root/scripts/build.sh" "$preset" --target package-sdk package-cli
  for product in libmdf cmdf; do test -f "$dist/$product-$version-$target.tar.gz"; done
done
"$root/scripts/package_checksums.sh" binary
