#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
preset=${1:-debug}
shift "$(( $# > 0 ? 1 : 0 ))"
cd "$root"
"$root/scripts/with_target_env.sh" "$preset" cmake --preset "$preset" -DLIBMDF_DIST_DIR="${LIBMDF_DIST_DIR:-$root/dist}"
"$root/scripts/with_target_env.sh" "$preset" cmake --build --preset "$preset" "$@"
