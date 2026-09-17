#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="$ROOT/build/cmdf-static"
OUT="$OUT_DIR/cmdf"

verify_static_cmdf() {
  bin=$1
  desc=$(file -b "$bin" 2>/dev/null || true)
  case "$desc" in
    *ELF*)
      if command -v readelf >/dev/null 2>&1; then
        if readelf -l "$bin" 2>/dev/null | grep -q 'Requesting program interpreter'; then
          printf '%s\n' "$bin is dynamically linked; cmdf install build must be static" >&2
          exit 1
        fi
        dyn="$OUT_DIR/readelf-dynamic.txt"
        if readelf -d "$bin" > "$dyn" 2>/dev/null && grep -q '(NEEDED)' "$dyn"; then
          printf '%s\n' "$bin has dynamic dependencies; cmdf install build must be static" >&2
          exit 1
        fi
        rm -f "$dyn"
      fi
      ;;
  esac
}

build_preset() {
  preset=$1
  "$ROOT/scripts/configure_preset.sh" "$preset" cmdf-static
  cmake --build --preset "$preset" --target cmdf
  mkdir -p "$OUT_DIR"
  cp "$ROOT/build/$preset/cmdf" "$OUT"
}

printf '%s\n' 'building static cmdf with the pinned Bootlin musl toolchain'
build_preset x86_64-linux-musl-release

verify_static_cmdf "$OUT"
printf '%s\n' "$OUT"
