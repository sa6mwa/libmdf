#!/usr/bin/env sh
set -eu

if [ "$#" -ne 3 ]; then
  printf '%s\n' "usage: $0 /path/to/golden_render /path/to/incremental_decision_parity /path/to/cmdf" >&2
  exit 2
fi

RENDER=$1
INCREMENTAL=$2
CMDF=$3
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
INPUT="$ROOT/tests/golden-fixtures/pending-emphasis-wrap.md"
EXPECTED="$ROOT/testdata/goldens/streaming/pending-emphasis-wrap.ansi.boring.w12.golden"
EXPECTED_TRACE="$ROOT/testdata/goldens/streaming/pending-emphasis-wrap.ansi.boring.w12.trace.golden"
TMP=${TMPDIR:-/tmp}/libmdf-pending-emphasis-wrap.$$

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TMP"

# The checked-in output and trace come from trunk. The direct lifecycle runner
# compares every sink write and trace event across all fragment schedules.
"$RENDER" --ansi --boring -w 12 --margin-left 0 --margin-right 0 "$INPUT" > "$TMP/libmdf.out"
cmp "$EXPECTED" "$TMP/libmdf.out"
"$INCREMENTAL" --ansi --boring -w 12 --margin-left 0 --margin-right 0 "$INPUT"

"$CMDF" --boring -w 12 --simulate-chunk 1 --trace-writes "$TMP/cmdf.trace" \
  "$INPUT" > "$TMP/cmdf.out"
"$CMDF" --incremental --boring -w 12 --simulate-chunk 1 \
  --trace-writes "$TMP/cmdf.incremental.trace" "$INPUT" > "$TMP/cmdf.incremental.out"
cmp "$EXPECTED" "$TMP/cmdf.out"
cmp "$EXPECTED" "$TMP/cmdf.incremental.out"
cmp "$EXPECTED_TRACE" "$TMP/cmdf.trace"
cmp "$EXPECTED_TRACE" "$TMP/cmdf.incremental.trace"
