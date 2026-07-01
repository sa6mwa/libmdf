#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CMDF=${CMDF:-"$ROOT/build/cmdf-static/cmdf"}
CORPUS=${CORPUS:-"$ROOT/testdata/chart-corpus"}
OUT=${OUT:-"$ROOT/build/chart-corpus-preview"}

if [ ! -x "$CMDF" ]; then
  echo "render_chart_corpus: cmdf not found: $CMDF" >&2
  echo "run make build, or set CMDF=/path/to/cmdf" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/ansi-color" "$OUT/ansi-boring" "$OUT/ansi-narrow" "$OUT/html"

for src in "$CORPUS"/*.md; do
  name=$(basename "$src" .md)
  if [ "$name" = "README" ]; then
    continue
  fi
  "$CMDF" --width 96 "$src" > "$OUT/ansi-color/$name.ansi"
  "$CMDF" --boring --width 96 "$src" > "$OUT/ansi-boring/$name.txt"
  "$CMDF" --boring --width 32 "$src" > "$OUT/ansi-narrow/$name.txt"
  "$CMDF" --html "$src" > "$OUT/html/$name.html"
done

cat > "$OUT/README.txt" <<EOF
Chart preview output generated from:
  $CORPUS

ANSI with theme colors:
  $OUT/ansi-color

ANSI boring, width 96:
  $OUT/ansi-boring

ANSI boring, width 32:
  $OUT/ansi-narrow

HTML:
  $OUT/html
EOF

printf '%s\n' "$OUT"
