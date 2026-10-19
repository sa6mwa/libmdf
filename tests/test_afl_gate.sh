#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
fixture=$1 build=$2
mkdir -p "$build/fixtures"
work=$(mktemp -d "$build/fixtures/afl-gate.XXXXXXXX")
af_environment=$("$root/scripts/cpkt-aflpp.sh" env)
eval "$af_environment"
mkdir -p "$work/seeds" "$work/crashing" "$work/clean" "$work/error"
printf A > "$work/seeds/seed"
gate="$root/scripts/run_afl_gate.sh"
af="$CPKT_AFLPP_ROOT/bin/afl-fuzz"
if "$gate" "$af" 2 "$work/seeds" "$work/crashing" "$fixture" > "$work/crashing.log" 2>&1; then
  printf 'Crash gate unexpectedly passed: %s\n' "$work" >&2; exit 1;
fi
grep -q 'AFL++ discovered crashes; retained reproducers:' "$work/crashing.log"
test -n "$(find "$work/crashing" -path '*/crashes/id:*' -type f -print)"
"$gate" "$af" 1 "$work/seeds" "$work/clean" "$fixture" safe > "$work/clean.log" 2>&1
if "$gate" "$af" 1 "$work/missing-seeds" "$work/error" "$fixture" > "$work/error.log" 2>&1; then
  printf 'AFL++ startup failure unexpectedly passed\n' >&2; exit 1;
fi
printf 'AFL++ crash, completion and startup failure proof: %s\n' "$work"
