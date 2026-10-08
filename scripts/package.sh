#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
source "$root/scripts/release_targets.sh"
version=$("$root/scripts/version.sh")
dist=${LIBMDF_DIST_DIR:-$root/dist}
workspace=${LIBMDF_PACKAGE_WORK_DIR:-$root/build/package}
case "$workspace" in "$root"/build/*) ;; *) printf 'Package work must stay under build\n' >&2; exit 2;; esac
[[ ! -L $root/build && ! -L $dist && ! -L $workspace ]] || { printf 'Package output must use owned directories\n' >&2; exit 1; }
for target in "${release_targets[@]}"; do
  preset="$target-release"
  if [[ $target == x86_64-linux-gnu && -n ${LIBMDF_NATIVE_RELEASE_BUILD:-} ]]; then
    build=$LIBMDF_NATIVE_RELEASE_BUILD
    test -f "$build/CMakeCache.txt"; test -x "$build/cmdf"
    grep -Fxq 'CMAKE_BUILD_TYPE:STRING=Release' "$build/CMakeCache.txt"
    grep -Fxq 'LIBMDF_TARGET_ID:STRING=x86_64-linux-gnu' "$build/CMakeCache.txt"
    # This producer belongs to the same final run or an explicitly prepared lane.
    # Do not configure it or change any of its inputs here.
    "$root/scripts/with_target_env.sh" "$preset" cmake --build "$build" --target package-sdk package-cli
  else
    "$root/scripts/build.sh" "$preset" --target package-sdk package-cli
  fi
  for product in libmdf cmdf; do test -f "$dist/$product-$version-$target.tar.gz"; done
done
"$root/scripts/package_checksums.sh" binary
