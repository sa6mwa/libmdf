#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-ansi}
JUDGE=${LIBMDF_PARITYJUDGE:-$ROOT/build/debug/parityjudge}
PROFILE=${LIBMDF_STREAM_PARITY_PROFILE:-full}

case "$PROFILE" in
  quick)
    DEFAULT_CHUNKS="1 4096"
    DEFAULT_WIDTHS="20 100"
    DEFAULT_THEMES="default"
    DEFAULT_BORINGS="false"
    DEFAULT_OSC8S="false"
    DEFAULT_TABLE_BUFFERS="full row"
    DEFAULT_TABLE_WIRES="line"
    ;;
  full|exhaustive|release)
    DEFAULT_CHUNKS="1 2 3 7 31 4096"
    DEFAULT_WIDTHS="20 40 80 100 120"
    DEFAULT_THEMES="default"
    DEFAULT_BORINGS="false"
    DEFAULT_OSC8S="false"
    DEFAULT_TABLE_BUFFERS="full row"
    DEFAULT_TABLE_WIRES="line ascii space"
    ;;
  *)
    printf '%s\n' "unknown LIBMDF_STREAM_PARITY_PROFILE: $PROFILE" >&2
    exit 2
    ;;
esac

CHUNKS=${LIBMDF_PARITY_CHUNKS:-$DEFAULT_CHUNKS}
WIDTHS=${LIBMDF_PARITY_WIDTHS:-$DEFAULT_WIDTHS}
THEMES=${LIBMDF_STREAM_PARITY_THEMES:-$DEFAULT_THEMES}
BORINGS=${LIBMDF_STREAM_PARITY_BORINGS:-$DEFAULT_BORINGS}
OSC8S=${LIBMDF_STREAM_PARITY_OSC8S:-$DEFAULT_OSC8S}
TABLE_BUFFERS=${LIBMDF_STREAM_PARITY_TABLE_BUFFERS:-$DEFAULT_TABLE_BUFFERS}
TABLE_WIRES=${LIBMDF_STREAM_PARITY_TABLE_WIRES:-$DEFAULT_TABLE_WIRES}
DEFAULT_EXCLUDES="$ROOT/testdata/chart-corpus $ROOT/testdata/deck-corpus"
DEFAULT_CASE_EXCLUDES=
# Deliberate project contract exceptions; see scripts/parity.sh. Trace
# exceptions are width-specific and protected by exact libmdf ANSI goldens.
while read -r parity_kind parity_path parity_width extra; do
  case "$parity_kind" in
    ''|'#'*) continue ;;
    ansi|trace)
      if [ -n "${extra:-}" ] || ! printf '%s\n' "$parity_width" | grep -Eq '^[1-9][0-9]*$'; then
        printf '%s\n' "invalid ANSI/trace parity exception: $parity_kind $parity_path $parity_width${extra:+ $extra}" >&2
        exit 2
      fi
      ;;
    html)
      if [ -n "${parity_width:-}" ] || [ -n "${extra:-}" ]; then
        printf '%s\n' "invalid HTML parity exception: $parity_kind $parity_path${parity_width:+ $parity_width}${extra:+ $extra}" >&2
        exit 2
      fi
      continue
      ;;
    *)
      printf '%s\n' "invalid parity exception kind: $parity_kind" >&2
      exit 2
      ;;
  esac
  if [ "$parity_kind" = trace ]; then
    DEFAULT_CASE_EXCLUDES="${DEFAULT_CASE_EXCLUDES}${DEFAULT_CASE_EXCLUDES:+ }$ROOT/$parity_path@$parity_width"
  fi
done < "$ROOT/testdata/goldens/libmdf/PARITY_EXCLUSIONS.txt"
EXCLUDES=${LIBMDF_STREAM_PARITY_EXCLUDES:-$DEFAULT_EXCLUDES}
CASE_EXCLUDES=${LIBMDF_STREAM_PARITY_CASE_EXCLUDES:-$DEFAULT_CASE_EXCLUDES}
test -x "$JUDGE" || { printf "Missing parity runner; run make build-debug\n" >&2; exit 1; }

"$JUDGE" -mode ansi -compare-libmdf -trace-compare -suite "$ROOT/testdata" -exclude "$EXCLUDES" -exclude-cases "$CASE_EXCLUDES" -chunks "$CHUNKS" -widths "$WIDTHS" \
  -themes "$THEMES" -borings "$BORINGS" -osc8s "$OSC8S" -table-buffers "$TABLE_BUFFERS" -table-wires "$TABLE_WIRES"
