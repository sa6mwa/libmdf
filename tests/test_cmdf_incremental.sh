#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
  printf '%s\n' "usage: $0 /path/to/cmdf" >&2
  exit 2
fi

CMDF=$1
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
INPUT="$ROOT/testdata/deck-corpus/comprehensive.md"
GOLDEN_DIR="$ROOT/testdata/goldens/cmdf"
TMP=${TMPDIR:-/tmp}/libmdf-cmdf-incremental.$$

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TMP"

if "$CMDF" --incremental --html "$INPUT" > /dev/null 2> "$TMP/html.err"; then
  printf '%s\n' '--incremental unexpectedly accepted HTML without a title' >&2
  exit 1
fi
grep -F -q -- '--incremental --html requires --title' "$TMP/html.err"

if "$CMDF" --incremental --deck --title 'deck title' "$INPUT" > /dev/null 2> "$TMP/deck.err"; then
  printf '%s\n' '--incremental unexpectedly accepted deck output' >&2
  exit 1
fi
grep -F -q -- '--incremental is not supported for deck output' "$TMP/deck.err"

run_case() {
  name=$1
  shift
  expected="$GOLDEN_DIR/$name.golden"
  if [ ! -f "$expected" ]; then
    printf '%s\n' "missing golden: $expected" >&2
    exit 1
  fi
  for chunk in 1 3 7 4096; do
    baseline="$TMP/$name.$chunk.baseline"
    incremental="$TMP/$name.$chunk.incremental"
    baseline_trace="$baseline.trace"
    incremental_trace="$incremental.trace"
    "$CMDF" --osc8 on "$@" --simulate-chunk "$chunk" \
      --trace-writes "$baseline_trace" "$INPUT" > "$baseline"
    "$CMDF" --incremental --osc8 on "$@" --simulate-chunk "$chunk" \
      --trace-writes "$incremental_trace" "$INPUT" > "$incremental"
    diff -u "$expected" "$baseline"
    cmp "$baseline" "$incremental"
    cmp "$baseline_trace" "$incremental_trace"
  done
}

# The baseline is cmdf's ordinary source renderer. The incremental feature
# driver must preserve its exact output and exact ANSI decision trace for each
# source-read schedule, including one byte at a time.
run_case comprehensive.ansi.styled.w80 -w 80
run_case comprehensive.ansi.boring.w80 --boring -w 80
run_case comprehensive.ansi.boring.w80.margin-l4-r6 --boring -w 80 --margin-left 4 --margin-right 6
run_case comprehensive.ansi.styled.w40.margin-l2-r3 -w 40 --margin-left 2 --margin-right 3

run_html_case() {
  name=$1
  shift
  expected="$GOLDEN_DIR/$name.golden"
  if [ ! -f "$expected" ]; then
    printf '%s\n' "missing golden: $expected" >&2
    exit 1
  fi
  for chunk in 1 3 7 4096; do
    baseline="$TMP/$name.$chunk.baseline"
    incremental="$TMP/$name.$chunk.incremental"
    "$CMDF" --html --title 'libmdf Deck Mode' --simulate-chunk "$chunk" \
      "$@" "$INPUT" > "$baseline"
    "$CMDF" --incremental --html --title 'libmdf Deck Mode' --simulate-chunk "$chunk" \
      "$@" "$INPUT" > "$incremental"
    diff -u "$expected" "$baseline"
    cmp "$baseline" "$incremental"
  done
}

# HTML streaming must use an explicit title, then match the ordinary source
# renderer byte-for-byte at every bounded source-read schedule.
run_html_case comprehensive.html.default
run_html_case comprehensive.html.w42 -w 42

run_boundary_case() {
  name=$1
  expected=$2
  input=$3
  shift 3
  baseline="$TMP/$name.baseline"
  incremental="$TMP/$name.incremental"
  baseline_trace="$baseline.trace"
  incremental_trace="$incremental.trace"

  printf '%b' "$input" > "$TMP/$name.md"
  "$CMDF" --boring "$@" --simulate-chunk 1 --trace-writes "$baseline_trace" \
    "$TMP/$name.md" > "$baseline"
  "$CMDF" --incremental --boring "$@" --simulate-chunk 1 \
    --trace-writes "$incremental_trace" "$TMP/$name.md" > "$incremental"
  printf '%b' "$expected" > "$TMP/$name.expected"
  cmp "$TMP/$name.expected" "$baseline"
  cmp "$baseline" "$incremental"
  cmp "$baseline_trace" "$incremental_trace"
}

# These are boundary-only decisions: output and exact decision trace must
# remain identical when cmdf drives the public feed/flush lifecycle.
run_boundary_case trailing-tab 'hello\n' 'hello\t\n'
run_boundary_case unfinished-inline 'hello *[\n' '*hello *[\n\n'
run_boundary_case unfinished-inline-space 'hello*\n' 'hello* '
run_boundary_case pending-tab-wrap 'helloabcde\two\nrld\n' 'helloabcde\tworld\n' -w 12
run_boundary_case empty-heading '# \n' '#  '
run_boundary_case trailing-inline-space-eof 'b \n' 'b_ '
run_boundary_case trailing-inline-space-newline 'hello  world\n' 'hello_ \nworld'

run_source_boundary_case() {
  name=$1
  input=$2
  shift 2
  one="$TMP/$name.one"
  whole="$TMP/$name.whole"
  one_trace="$one.trace"
  whole_trace="$whole.trace"

  printf '%b' "$input" > "$TMP/$name.md"
  "$CMDF" --boring "$@" --simulate-chunk 1 --trace-writes "$one_trace" \
    "$TMP/$name.md" > "$one"
  "$CMDF" --boring "$@" --simulate-chunk 4096 --trace-writes "$whole_trace" \
    "$TMP/$name.md" > "$whole"
  cmp "$one" "$whole"
  cmp "$one_trace" "$whole_trace"
}

run_source_boundary_case deferred-list '1. abc\n`' -w 5
