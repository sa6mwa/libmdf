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
TMP=${TMPDIR:-/tmp}/libmdf-cmdf-goldens.$$

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TMP"

run_case() {
  name=$1
  shift
  actual="$TMP/$name"
  expected="$GOLDEN_DIR/$name.golden"
  if [ ! -f "$expected" ]; then
    printf '%s\n' "missing golden: $expected" >&2
    exit 1
  fi
  "$CMDF" "$@" "$INPUT" > "$actual"
  diff -u "$expected" "$actual"
}

run_ansi_case() {
  name=$1
  shift
  run_case "$name" --osc8 on "$@"
}

run_ansi_case comprehensive.ansi.styled.w80 -w 80
run_ansi_case comprehensive.ansi.boring.w80 --boring -w 80
run_ansi_case comprehensive.ansi.boring.w80.margin-l4-r6 --boring -w 80 --margin-left 4 --margin-right 6
run_ansi_case comprehensive.ansi.styled.w40.margin-l2-r3 -w 40 --margin-left 2 --margin-right 3
run_case comprehensive.html.default --html
run_case comprehensive.html.w42 --html -w 42
run_case comprehensive.deck.default --deck
run_case comprehensive.deck.w42 --deck -w 42
run_case comprehensive.deck.slide-numbers.fade --deck --slide-numbers -x fade
run_case comprehensive.deck.boring --deck --boring
