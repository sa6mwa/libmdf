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
INPUT_HEX="$ROOT/tests/golden-fixtures/narrow-quote-url.md.hex"
EXPECTED_HEX="$ROOT/testdata/goldens/libmdf/narrow-quote-url.ansi.boring.w8.margin-l0-r0.golden.hex"
TMP=${TMPDIR:-/tmp}/libmdf-narrow-quote-url-streaming.$$

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TMP"

decode_hex() {
  awk '
    {
      gsub(/[[:space:]]/, "");
      data = data $0;
    }
    END {
      digits = "0123456789abcdef";
      for (i = 1; i <= length(data); i += 2) {
        high = index(digits, tolower(substr(data, i, 1))) - 1;
        low = index(digits, tolower(substr(data, i + 1, 1))) - 1;
        if (high < 0 || low < 0) exit 1;
        printf "%c", high * 16 + low;
      }
    }
  ' "$1"
}

decode_hex "$INPUT_HEX" > "$TMP/input.md"
INPUT="$TMP/input.md"

assert_golden() {
  tr -d '\n' < "$EXPECTED_HEX" > "$TMP/expected.hex"
  od -An -tx1 -v "$1" | tr -d ' \n' > "$TMP/actual.hex"
  cmp "$TMP/expected.hex" "$TMP/actual.hex"
}

# This fixture is pinned to the accepted pre-incremental libmdf behavior.  Go
# mdf independently establishes the termination requirement.  The timeout is
# part of the regression: a narrow quote plus a wrapped URL must always make
# progress rather than repeatedly emitting the quote continuation prefix.
timeout 5 "$RENDER" --ansi --boring -w 8 --margin-left 0 --margin-right 0 "$INPUT" > "$TMP/libmdf.out"
assert_golden "$TMP/libmdf.out"

# The direct public lifecycle must preserve the ordinary renderer's exact
# decision-emission sequence for all tested feed/flush fragment schedules.
timeout 5 "$INCREMENTAL" --ansi --boring -w 8 --margin-left 0 --margin-right 0 "$INPUT"

# cmdf exercises both source drivers.  Their output must match the library
# golden, and the ANSI sink-write trace must remain identical.
timeout 5 "$CMDF" --ansi on --boring -w 8 --simulate-chunk 1 --trace-writes "$TMP/cmdf.trace" \
  "$INPUT" > "$TMP/cmdf.out"
timeout 5 "$CMDF" --ansi on --incremental --boring -w 8 --simulate-chunk 1 \
  --trace-writes "$TMP/cmdf.incremental.trace" "$INPUT" > "$TMP/cmdf.incremental.out"
assert_golden "$TMP/cmdf.out"
cmp "$TMP/cmdf.out" "$TMP/cmdf.incremental.out"
cmp "$TMP/cmdf.trace" "$TMP/cmdf.incremental.trace"
