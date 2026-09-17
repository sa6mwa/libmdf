#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
  printf '%s\n' "usage: $0 /path/to/incremental_decision_parity" >&2
  exit 2
fi

RUNNER=$1
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST_CMD="$ROOT/scripts/libmdf_golden_manifest.sh"
GOLDEN_DIR="$ROOT/testdata/goldens/libmdf"
TMP=${TMPDIR:-/tmp}/libmdf-incremental-decision-goldens.$$

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TMP"
"$MANIFEST_CMD" > "$TMP/MANIFEST.txt"

while read -r name input args; do
  case "$name" in
    ''|'#'*) continue ;;
    *.ansi.*) ;;
    *) continue ;;
  esac
  if [ ! -f "$GOLDEN_DIR/$name.golden" ] || [ ! -f "$ROOT/$input" ]; then
    printf '%s\n' "missing ANSI decision-parity fixture: $name" >&2
    exit 1
  fi
  # The synchronous render path is the golden-pinned pre-incremental oracle.
  # Each schedule calls flush after every feed; the one-byte schedule exercises
  # every possible input boundary.
  # shellcheck disable=SC2086
  "$RUNNER" $args "$ROOT/$input"
done < "$TMP/MANIFEST.txt"
