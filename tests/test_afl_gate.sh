#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/test-afl-gate.XXXXXX")
eval "$("$root/scripts/cpkt-aflpp.sh" env)"
eval "$("$root/scripts/bootlin_x86_runtime.sh")"
for tool in afl-fuzz afl-showmap afl-tmin afl-gotcpu afl-analyze afl-cc; do
  executable="$CPKT_AFLPP_ROOT/bin/$tool"
  cmake -DREADELF="$LIBMDF_BOOTLIN_READELF" -DEXECUTABLE="$executable" \
    -DINTERPRETER="$LIBMDF_BOOTLIN_INTERPRETER" -DRUNTIME_DIR="$LIBMDF_BOOTLIN_RUNTIME_DIR" \
    -P "$root/tests/assert_bootlin_runtime.cmake"
  LD_TRACE_LOADED_OBJECTS=1 "$executable" > "$work/$tool.loaded"
  if grep -E '=> /(lib|lib64|usr/lib|usr/lib64)/|not found' "$work/$tool.loaded"; then
    printf 'AFL++ resolved host or missing libraries: %s\n' "$executable" >&2
    exit 1
  fi
done
mkdir -p "$work/seeds" "$work/crashing" "$work/clean" "$work/error"
printf A > "$work/seeds/seed"
"$CC" -Wall -Wextra -Werror "$root/tests/afl_gate_fixture.c" \
  "-Wl,--dynamic-linker,$LIBMDF_BOOTLIN_INTERPRETER" \
  "-Wl,--disable-new-dtags,-rpath,$LIBMDF_BOOTLIN_RUNTIME_RPATH" -o "$work/fixture"
gate="$root/scripts/run_afl_gate.sh"
afl="$CPKT_AFLPP_ROOT/bin/afl-fuzz"
if sh "$gate" "$afl" 5 "$work/seeds" "$work/crashing" "$work/fixture" >"$work/crashing.log" 2>&1; then
  printf 'Crash gate unexpectedly passed: %s\n' "$work" >&2
  exit 1
fi
grep -q 'AFL++ discovered crashes; retained reproducers:' "$work/crashing.log"
grep -q 'Time limit was reached' "$work/crashing.log"
test -n "$(find "$work/crashing" -path '*/crashes/id:*' -type f -print)"
sh "$gate" "$afl" 1 "$work/seeds" "$work/clean" "$work/fixture" safe >"$work/clean.log" 2>&1
if sh "$gate" "$afl" 1 "$work/missing-seeds" "$work/error" "$work/fixture" >"$work/error.log" 2>&1; then
  printf 'AFL++ startup failure unexpectedly passed\n' >&2
  exit 1
fi
printf 'AFL++ crash, clean completion, and startup failure checks passed: %s\n' "$work"
