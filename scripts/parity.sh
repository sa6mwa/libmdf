#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-ansi}
BUILD="$ROOT/build/debug"
JUDGE="$ROOT/build/parityjudge-cgo"
PROFILE=${LIBMDF_PARITY_PROFILE:-full}

case "$PROFILE" in
  quick)
    DEFAULT_CHUNKS="1 4096"
    DEFAULT_WIDTHS="20 80 120"
    DEFAULT_THEMES="default"
    DEFAULT_BORINGS="false"
    DEFAULT_OSC8S="false"
    DEFAULT_TABLE_BUFFERS="full row"
    DEFAULT_TABLE_WIRES="line"
    ;;
  full|exhaustive|release)
    DEFAULT_CHUNKS="1 2 3 7 31 4096"
    DEFAULT_WIDTHS="20 40 80 100 120"
    DEFAULT_THEMES="all"
    DEFAULT_BORINGS="false true"
    DEFAULT_OSC8S="false"
    DEFAULT_TABLE_BUFFERS="full row"
    DEFAULT_TABLE_WIRES="line ascii space"
    ;;
  *)
    printf '%s\n' "unknown LIBMDF_PARITY_PROFILE: $PROFILE" >&2
    exit 2
    ;;
esac

CHUNKS=${LIBMDF_PARITY_CHUNKS:-$DEFAULT_CHUNKS}
WIDTHS=${LIBMDF_PARITY_WIDTHS:-$DEFAULT_WIDTHS}
THEMES=${LIBMDF_PARITY_THEMES:-$DEFAULT_THEMES}
BORINGS=${LIBMDF_PARITY_BORINGS:-$DEFAULT_BORINGS}
OSC8S=${LIBMDF_PARITY_OSC8S:-$DEFAULT_OSC8S}
TABLE_BUFFERS=${LIBMDF_PARITY_TABLE_BUFFERS:-$DEFAULT_TABLE_BUFFERS}
TABLE_WIRES=${LIBMDF_PARITY_TABLE_WIRES:-$DEFAULT_TABLE_WIRES}
# Go mdf splits URL scheme delimiters at narrow widths. libmdf intentionally
# keeps :// atomic; the affected corpus documents are covered by libmdf ANSI
# goldens instead of Go parity.
EXCLUDES=${LIBMDF_PARITY_EXCLUDES:-"$ROOT/testdata/chart-corpus $ROOT/testdata/deck-corpus $ROOT/testdata/centaur-post.md $ROOT/testdata/frontmatter2.md $ROOT/testdata/future/linkify.md $ROOT/testdata/future/reflinks.md"}
STAMP=$(cksum "$ROOT/src/mdf.c" "$ROOT/src/html_fonts.c" "$ROOT/src/render.c" "$ROOT/src/render_"*.c "$ROOT/src/mdf_internal.h" "$ROOT/include/libmdf/mdf.h" "$ROOT/src/html_embedded/"*.h | cksum | awk '{print $1}')
child_pid=

cleanup_child() {
  if [ -n "$child_pid" ]; then
    kill -TERM "$child_pid" 2>/dev/null || true
  fi
}

run_child() {
  "$@" &
  child_pid=$!
  if wait "$child_pid"; then
    status=0
  else
    status=$?
  fi
  child_pid=
  return "$status"
}

trap 'cleanup_child; exit 130' INT
trap 'cleanup_child; exit 143' TERM HUP

(cd "$ROOT/parityjudge" && go build -tags "libmdf_$STAMP" -o "$JUDGE" .)

if [ "$MODE" = "ansi" ] || [ "$MODE" = "html" ]; then
  if [ "$MODE" = "ansi" ]; then
    run_child "$JUDGE" -mode ansi -compare-libmdf -suite "$ROOT/testdata" -exclude "$EXCLUDES" -chunks "$CHUNKS" -widths "$WIDTHS" \
      -themes "$THEMES" -borings "$BORINGS" -osc8s "$OSC8S" -table-buffers "$TABLE_BUFFERS" -table-wires "$TABLE_WIRES"
  else
    run_child "$JUDGE" -mode html -compare-libmdf -suite "$ROOT/testdata" -exclude "$EXCLUDES" -chunks "$CHUNKS" \
      -themes "$THEMES" -borings "$BORINGS" -table-buffers "$TABLE_BUFFERS" -table-wires "$TABLE_WIRES"
  fi
  exit $?
fi

printf '%s\n' "unsupported parity mode: $MODE" >&2
exit 2
