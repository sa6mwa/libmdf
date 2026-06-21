#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
JUDGE="$ROOT/build/parityjudge-cgo"
STAMP=$(cksum "$ROOT/src/mdf.c" "$ROOT/src/cmdf_fonts.c" "$ROOT/src/cmdf_fonts.h" "$ROOT/src/render.c" "$ROOT/src/render_"*.c "$ROOT/src/mdf_internal.h" "$ROOT/include/libmdf/mdf.h" "$ROOT/src/html_embedded/"*.h | cksum | awk '{print $1}')

(cd "$ROOT/parityjudge" && go build -tags "libmdf_$STAMP" -o "$JUDGE" .)

run_case() {
  printf '%s\n' "parityjudge option case: $1" >&2
  case "$1" in
    ansi_table_wire)
      "$JUDGE" -mode ansi -compare-libmdf \
        -suite "$ROOT/testdata/table-corpus" \
        -chunks 4096 \
        -widths "20 100" \
        -themes default \
        -borings false \
        -osc8s false \
        -table-buffers full \
        -table-wires "ascii space"
      ;;
    ansi_table_boring)
      "$JUDGE" -mode ansi -compare-libmdf \
        -suite "$ROOT/testdata/table-corpus" \
        -chunks 4096 \
        -widths "20 100" \
        -themes default \
        -borings true \
        -osc8s false \
        -table-buffers full \
        -table-wires line
      ;;
    ansi_table_row_trace)
      "$JUDGE" -mode ansi -compare-libmdf -trace-compare \
        -suite "$ROOT/testdata/table-corpus" \
        -chunks 1 \
        -widths "20 100" \
        -themes default \
        -borings false \
        -osc8s false \
        -table-buffers row \
        -table-wires line
      ;;
    ansi_regressions)
      "$JUDGE" -mode ansi -compare-libmdf \
        -suite "$ROOT/testdata/parity-regressions" \
        -chunks "1 4096" \
        -widths "20 100" \
        -themes all \
        -borings false \
        -osc8s false \
        -table-buffers full \
        -table-wires line
      ;;
    html_regressions)
      "$JUDGE" -mode html -compare-libmdf \
        -suite "$ROOT/testdata/parity-html-regressions" \
        -chunks 4096 \
        -themes all \
        -borings "false true" \
        -table-buffers full \
        -table-wires line
      ;;
    *)
      printf '%s\n' "unknown parityjudge option case: $1" >&2
      exit 2
      ;;
  esac
}

if [ "$#" -gt 0 ]; then
  for case_name in "$@"; do
    run_case "$case_name"
  done
else
  run_case ansi_table_wire
  run_case ansi_table_boring
  run_case ansi_table_row_trace
  run_case ansi_regressions
  run_case html_regressions
fi
