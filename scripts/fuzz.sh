#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-standard}
CACHE="$ROOT/build/fuzz/CMakeCache.txt"
SEED_CORPUS="$ROOT/fuzz/corpus"
WORK_CORPUS="$ROOT/build/fuzz/corpus"

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

if [ -f "$CACHE" ] && ! grep -Eq '^CMAKE_C_COMPILER:FILEPATH=.*clang([^/]*)?$' "$CACHE"; then
  rm -rf "$ROOT/build/fuzz"
fi

"$ROOT/scripts/configure_preset.sh" fuzz fuzz
cmake --build --preset fuzz
mkdir -p "$ROOT/build/fuzz/artifacts"
rm -rf "$WORK_CORPUS"
mkdir -p "$WORK_CORPUS"
cp "$SEED_CORPUS"/* "$WORK_CORPUS"/
"$ROOT/build/fuzz/fuzz_smoke" \
  -max_total_time="$MAX_TOTAL_TIME" \
  -artifact_prefix="$ROOT/build/fuzz/artifacts/" \
  "$WORK_CORPUS"
