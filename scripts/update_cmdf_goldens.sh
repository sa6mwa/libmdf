#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CMDF=${LIBMDF_CMDF:-"$ROOT/build/debug/cmdf"}
INPUT="$ROOT/testdata/deck-corpus/comprehensive.md"
GOLDEN_DIR="$ROOT/testdata/goldens/cmdf"

if [ ! -x "$CMDF" ]; then
  "$ROOT/scripts/build.sh" debug
fi

mkdir -p "$GOLDEN_DIR"

write_case() {
  name=$1
  shift
  "$CMDF" "$@" "$INPUT" > "$GOLDEN_DIR/$name.golden"
}

write_case comprehensive.ansi.styled.w80 -w 80
write_case comprehensive.ansi.boring.w80 --boring -w 80
write_case comprehensive.ansi.boring.w80.margin-l4-r6 --boring -w 80 --margin-left 4 --margin-right 6
write_case comprehensive.ansi.styled.w40.margin-l2-r3 -w 40 --margin-left 2 --margin-right 3
write_case comprehensive.html.default --html
write_case comprehensive.html.w42 --html -w 42
write_case comprehensive.deck.default --deck
write_case comprehensive.deck.w42 --deck -w 42
write_case comprehensive.deck.slide-numbers.fade --deck --slide-numbers -x fade
write_case comprehensive.deck.boring --deck --boring
