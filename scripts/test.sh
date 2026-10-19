#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
preset=${1:-debug}
shift "$(( $# > 0 ? 1 : 0 ))"
[[ $preset == debug ]] || { printf 'The native test entrypoint uses debug; select other configured trees through CTest\n' >&2; exit 2; }
if [[ $# == 0 ]]; then
  rm -f "$root/build/$preset/checks/native.pass"
  "$root/scripts/build.sh" "$preset" --target check-native
else
  "$root/scripts/build.sh" "$preset"
  ctest --test-dir "$root/build/$preset" --output-on-failure --stop-on-failure --no-tests=error -j1 -LE 'memory|hardening|full-parity|quick-parity' "$@"
fi
