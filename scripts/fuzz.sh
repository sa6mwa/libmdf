#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-standard}
SEED_CORPUS="$ROOT/fuzz/corpus"
OUTPUT="$ROOT/build/fuzz/afl-output"

case "$MODE" in
  smoke)
    MAX_TOTAL_TIME=${LIBMDF_FUZZ_SMOKE_SECONDS:-3}
    ;;
  standard)
    MAX_TOTAL_TIME=${LIBMDF_FUZZ_SECONDS:-15}
    ;;
  long)
    MAX_TOTAL_TIME=${LIBMDF_FUZZ_LONG_SECONDS:-60}
    ;;
  *)
    printf '%s\n' "usage: $0 {smoke|standard|long}" >&2
    exit 2
    ;;
esac

"$ROOT/scripts/configure_preset.sh" fuzz fuzz
cmake --build --preset fuzz
eval "$("$ROOT/scripts/bootlin_x86_runtime.sh")"
cmake \
  -DREADELF="$LIBMDF_BOOTLIN_READELF" \
  -DEXECUTABLE="$ROOT/build/fuzz/fuzz_smoke" \
  -DINTERPRETER="$LIBMDF_BOOTLIN_INTERPRETER" \
  -DRUNTIME_DIR="$LIBMDF_BOOTLIN_RUNTIME_DIR" \
  -P "$ROOT/tests/assert_bootlin_runtime.cmake"
rm -rf "$OUTPUT"
mkdir -p "$OUTPUT"
eval "$("$ROOT/scripts/cpkt-aflpp.sh" env)"
sh "$ROOT/scripts/run_afl_gate.sh" "$CPKT_AFLPP_ROOT/bin/afl-fuzz" \
  "$MAX_TOTAL_TIME" "$SEED_CORPUS" "$OUTPUT" "$ROOT/build/fuzz/fuzz_smoke" @@
