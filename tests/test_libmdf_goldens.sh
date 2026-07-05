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
