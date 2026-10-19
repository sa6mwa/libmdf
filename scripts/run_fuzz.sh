#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
executable=$1 build=$2 mode=$3 seconds=$4
[[ $seconds =~ ^[1-9][0-9]*$ ]] || { printf 'Fuzz duration must be positive\n' >&2; exit 2; }
af_environment=$("$root/scripts/cpkt-aflpp.sh" env)
eval "$af_environment"
mkdir -p "$build/runs"
output=$(mktemp -d "$build/runs/$mode.XXXXXXXX")
printf 'Fuzz run and reproducers: %s\n' "$output"
"$root/scripts/run_afl_gate.sh" "$CPKT_AFLPP_ROOT/bin/afl-fuzz" "$seconds" "$root/fuzz/corpus" "$output" "$executable" @@
