#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RENDER=${LIBMDF_GOLDEN_RENDER:-"$ROOT/build/debug/golden_render"}
MANIFEST_CMD="$ROOT/scripts/libmdf_golden_manifest.sh"
GOLDEN_DIR="$ROOT/testdata/goldens/libmdf"
TMP=${TMPDIR:-/tmp}/libmdf-golden-update.$$

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TMP"

if [ ! -x "$RENDER" ]; then
  "$ROOT/scripts/build.sh" debug
fi

mkdir -p "$GOLDEN_DIR"
"$MANIFEST_CMD" > "$TMP/MANIFEST.txt"

while read -r name input args; do
  case "$name" in
    ''|'#'*) continue ;;
  esac
  if [ ! -f "$ROOT/$input" ]; then
    printf '%s\n' "missing golden input: $input" >&2
    exit 1
  fi
  # shellcheck disable=SC2086
  if ! "$RENDER" $args "$ROOT/$input" > "$TMP/$name.golden"; then
    printf '%s\n' "failed to render golden: $name" >&2
    exit 1
  fi
done < "$TMP/MANIFEST.txt"

find "$GOLDEN_DIR" -maxdepth 1 -type f -name '*.golden' -delete
cp "$TMP/MANIFEST.txt" "$GOLDEN_DIR/MANIFEST.txt"
cp "$TMP"/*.golden "$GOLDEN_DIR/"
