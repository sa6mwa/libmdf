#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
  printf '%s\n' "usage: $0 /path/to/golden_render" >&2
  exit 2
fi

RENDER=$1
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST_CMD="$ROOT/scripts/libmdf_golden_manifest.sh"
GOLDEN_DIR="$ROOT/testdata/goldens/libmdf"
PARITY_EXCLUSIONS="$GOLDEN_DIR/PARITY_EXCLUSIONS.txt"
TMP=${TMPDIR:-/tmp}/libmdf-goldens.$$
seen=

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TMP"
"$MANIFEST_CMD" > "$TMP/MANIFEST.txt"

if ! cmp -s "$TMP/MANIFEST.txt" "$GOLDEN_DIR/MANIFEST.txt"; then
  printf '%s\n' "stale golden manifest: run make golden-update" >&2
  diff -u "$GOLDEN_DIR/MANIFEST.txt" "$TMP/MANIFEST.txt" >&2 || true
  exit 1
fi

if [ ! -f "$PARITY_EXCLUSIONS" ]; then
  printf '%s\n' "missing parity exclusions: $PARITY_EXCLUSIONS" >&2
  exit 1
fi
while read -r parity_kind parity_path parity_width extra; do
  case "$parity_kind" in
    ''|'#'*) continue ;;
    ansi|trace)
      if [ -n "${extra:-}" ] || ! printf '%s\n' "$parity_width" | grep -Eq '^[1-9][0-9]*$'; then
        printf '%s\n' "invalid ANSI/trace parity exception: $parity_kind $parity_path $parity_width${extra:+ $extra}" >&2
        exit 1
      fi
      golden_count=$(grep -F -c " $parity_path " "$TMP/MANIFEST.txt" || true)
      if [ "$golden_count" -lt 4 ]; then
        printf '%s\n' "parity exception lacks four ANSI goldens: $parity_path" >&2
        exit 1
      fi
      if ! grep -F " $parity_path --ansi -w $parity_width --margin-left 0 --margin-right 0" "$TMP/MANIFEST.txt" >/dev/null ||
         ! grep -F " $parity_path --ansi --boring -w $parity_width --margin-left 0 --margin-right 0" "$TMP/MANIFEST.txt" >/dev/null; then
        printf '%s\n' "parity exception lacks styled and boring goldens at width $parity_width: $parity_path" >&2
        exit 1
      fi
      ;;
    html)
      if [ -n "${parity_width:-}" ] || [ -n "${extra:-}" ]; then
        printf '%s\n' "invalid HTML parity exception: $parity_kind $parity_path${parity_width:+ $parity_width}${extra:+ $extra}" >&2
        exit 1
      fi
      golden_count=$(grep -F -c " $parity_path --html" "$TMP/MANIFEST.txt" || true)
      if [ "$golden_count" -lt 2 ]; then
        printf '%s\n' "HTML parity exception lacks two HTML goldens: $parity_path" >&2
        exit 1
      fi
      ;;
    *)
      printf '%s\n' "invalid parity exception kind: $parity_kind" >&2
      exit 1
      ;;
  esac
done < "$PARITY_EXCLUSIONS"

while read -r name input args; do
  case "$name" in
    ''|'#'*) continue ;;
  esac
  expected="$GOLDEN_DIR/$name.golden"
  actual="$TMP/$name"
  seen="$seen $name.golden"
  if [ ! -f "$ROOT/$input" ]; then
    printf '%s\n' "missing golden input: $input" >&2
    exit 1
  fi
  if [ ! -f "$expected" ]; then
    printf '%s\n' "missing golden: $expected" >&2
    exit 1
  fi
  # shellcheck disable=SC2086
  "$RENDER" $args "$ROOT/$input" > "$actual"
  diff -u "$expected" "$actual"
done < "$TMP/MANIFEST.txt"

for golden in "$GOLDEN_DIR"/*.golden; do
  [ -e "$golden" ] || continue
  base=${golden##*/}
  case " $seen " in
    *" $base "*) ;;
    *)
      printf '%s\n' "unlisted golden: $golden" >&2
      exit 1
      ;;
  esac
done
