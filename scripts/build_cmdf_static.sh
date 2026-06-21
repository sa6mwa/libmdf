#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="$ROOT/build/cmdf-static"
OUT="$OUT_DIR/cmdf"

configure_common="-DCMAKE_BUILD_TYPE=Release -DLIBMDF_BUILD_STATIC=ON -DLIBMDF_BUILD_SHARED=OFF -DLIBMDF_BUILD_BINARY=ON -DLIBMDF_BUILD_TESTS=OFF -DLIBMDF_BUILD_EXAMPLES=OFF -DLIBMDF_BUILD_FUZZERS=OFF -DLIBMDF_INSTALL=OFF -DLIBMDF_INSTALL_BINARY=OFF -DLIBMDF_CMDF_STATIC_RUNTIME=ON"

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

build_cc() {
  cc=$1
  build_dir=$2
  mkdir -p "$build_dir"
  CC=$cc cmake -S "$ROOT" -B "$build_dir" -G Ninja $configure_common
  cmake --build "$build_dir" --target cmdf
  mkdir -p "$OUT_DIR"
  cp "$build_dir/cmdf" "$OUT"
}

if command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
  printf '%s\n' 'building static cmdf with x86_64-linux-musl-gcc'
  build_preset x86_64-linux-musl-release
elif command -v musl-gcc >/dev/null 2>&1; then
  printf '%s\n' 'building static cmdf with musl-gcc'
  build_cc musl-gcc "$ROOT/build/cmdf-static-musl"
elif command -v x86_64-linux-gnu-gcc >/dev/null 2>&1; then
  printf '%s\n' 'building static cmdf with x86_64-linux-gnu-gcc'
  build_preset x86_64-linux-gnu-release
else
  printf '%s\n' "building static cmdf with ${CC:-cc}"
  build_cc "${CC:-cc}" "$ROOT/build/cmdf-static-cc"
fi

verify_static_cmdf "$OUT"
printf '%s\n' "$OUT"
