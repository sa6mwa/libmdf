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
STAMP=$(cksum "$ROOT/src/mdf.c" "$ROOT/src/cmdf_fonts.c" "$ROOT/src/cmdf_fonts.h" "$ROOT/src/render.c" "$ROOT/src/render_"*.c "$ROOT/src/mdf_internal.h" "$ROOT/include/libmdf/mdf.h" "$ROOT/src/html_embedded/"*.h | cksum | awk '{print $1}')
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
    run_child "$JUDGE" -mode ansi -compare-libmdf -suite "$ROOT/testdata" -chunks "$CHUNKS" -widths "$WIDTHS" \
      -themes "$THEMES" -borings "$BORINGS" -osc8s "$OSC8S" -table-buffers "$TABLE_BUFFERS" -table-wires "$TABLE_WIRES"
  else
    run_child "$JUDGE" -mode html -compare-libmdf -suite "$ROOT/testdata" -chunks "$CHUNKS" \
      -themes "$THEMES" -borings "$BORINGS" -table-buffers "$TABLE_BUFFERS" -table-wires "$TABLE_WIRES"
  fi
  exit $?
fi

if [ ! -x "$BUILD/cmdf" ]; then
  "$ROOT/scripts/build.sh" debug
fi

fail=0
for md in $(find "$ROOT/testdata" -type f -name '*.md' | sort); do
  for chunk in $CHUNKS; do
    if [ "$MODE" = "ansi" ]; then
      run_widths=$WIDTHS
    else
      run_widths=0
    fi
    for width in $run_widths; do
      go_out="$ROOT/build/parity-go.out"
      c_out="$ROOT/build/parity-c.out"
      if [ "$MODE" = "html" ]; then
        run_child "$JUDGE" -mode html -chunk "$chunk" < "$md" > "$go_out"
        run_child "$BUILD/cmdf" -H -S "$chunk" "$md" > "$c_out"
      elif [ "$MODE" = "tokens" ]; then
        run_child "$JUDGE" -mode "$MODE" -chunk "$chunk" < "$md" > "$go_out"
        run_child "$ROOT/build/debug/test_token_dump" "$chunk" "$md" > "$c_out"
      else
        run_child "$JUDGE" -mode ansi -chunk "$chunk" -width "$width" < "$md" > "$go_out"
        run_child "$BUILD/cmdf" -w "$width" -S "$chunk" "$md" > "$c_out"
      fi
      if ! cmp -s "$go_out" "$c_out"; then
        if [ "$MODE" = "ansi" ]; then
          echo "parity mismatch: mode=$MODE chunk=$chunk width=$width file=${md#$ROOT/}" >&2
        else
          echo "parity mismatch: mode=$MODE chunk=$chunk file=${md#$ROOT/}" >&2
        fi
        fail=1
        break
      fi
    done
    if [ "$fail" != "0" ]; then
      break
    fi
  done
done

exit "$fail"
