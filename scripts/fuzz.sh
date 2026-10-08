#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
mode=${1:-standard}
case "$mode" in
  smoke) "$root/scripts/build.sh" fuzz --target check-fuzz; exit;;
  standard) seconds=${LIBMDF_FUZZ_SECONDS:-15};;
  long) seconds=${LIBMDF_FUZZ_LONG_SECONDS:-60};;
  *) printf 'usage: fuzz.sh [smoke|standard|long]\n' >&2; exit 2;;
esac
"$root/scripts/build.sh" fuzz
"$root/scripts/run_fuzz.sh" "$root/build/fuzz/fuzz_smoke" "$root/build/fuzz" "$mode" "$seconds"
