#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LOG="$ROOT/build/test-configure-profiles.log"
mkdir -p "$ROOT/build"
: > "$LOG"

unset CC CFLAGS CPPFLAGS LDFLAGS

preset_schema=$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$ROOT/CMakePresets.json" | sed -n '1p')
if [ "$preset_schema" != "2" ]; then
  printf '%s\n' "CMakePresets.json must use schema version 2 for CMake 3.20 compatibility, got ${preset_schema:-unset}" >&2
  exit 1
fi

cleanup() {
  "$ROOT/scripts/configure_preset.sh" debug dev >/dev/null 2>&1 || true
}
trap cleanup EXIT INT HUP TERM

cache_value() {
  cache=$1
  key=$2
  sed -n "s/^$key:BOOL=//p" "$cache" | sed -n '1p'
}

expect_bool() {
  cache=$1
  key=$2
  want=$3
  got=$(cache_value "$cache" "$key")
  if [ "$got" != "$want" ]; then
    printf '%s\n' "$cache: expected $key=$want, got ${got:-unset}" >&2
    exit 1
  fi
}

poison_cache() {
  preset=$1
  if [ "$preset" != "debug" ]; then
    rm -rf "$ROOT/build/$preset"
  fi
  cmake --preset "$preset" \
    -DLIBMDF_BUILD_STATIC=OFF \
    -DLIBMDF_BUILD_SHARED=ON \
    -DLIBMDF_BUILD_BINARY=OFF \
    -DLIBMDF_BUILD_TESTS=OFF \
    -DLIBMDF_BUILD_EXAMPLES=OFF \
    -DLIBMDF_BUILD_FUZZERS=OFF \
    -DLIBMDF_INSTALL=OFF \
    -DLIBMDF_INSTALL_BINARY=OFF \
    -DLIBMDF_CMDF_STATIC_RUNTIME=ON >>"$LOG" 2>&1
}

verify_profile() {
  preset=$1
  profile=$2
  static=$3
  shared=$4
  binary=$5
  tests=$6
  examples=$7
  fuzzers=$8
  install=$9
  install_binary=${10}
  static_runtime=${11}
  cache="$ROOT/build/$preset/CMakeCache.txt"

  poison_cache "$preset"
  "$ROOT/scripts/configure_preset.sh" "$preset" "$profile" >>"$LOG" 2>&1

  expect_bool "$cache" LIBMDF_BUILD_STATIC "$static"
  expect_bool "$cache" LIBMDF_BUILD_SHARED "$shared"
  expect_bool "$cache" LIBMDF_BUILD_BINARY "$binary"
  expect_bool "$cache" LIBMDF_BUILD_TESTS "$tests"
  expect_bool "$cache" LIBMDF_BUILD_EXAMPLES "$examples"
  expect_bool "$cache" LIBMDF_BUILD_FUZZERS "$fuzzers"
  expect_bool "$cache" LIBMDF_INSTALL "$install"
  expect_bool "$cache" LIBMDF_INSTALL_BINARY "$install_binary"
  expect_bool "$cache" LIBMDF_CMDF_STATIC_RUNTIME "$static_runtime"
}

verify_profile debug dev ON ON ON ON ON OFF ON ON OFF
verify_profile debug lua ON ON ON ON ON OFF ON OFF OFF
verify_profile x86_64-linux-gnu-release release ON ON ON ON ON OFF ON ON OFF
verify_profile x86_64-linux-gnu-release package ON ON ON OFF OFF OFF ON OFF ON
verify_profile x86_64-linux-gnu-release cmdf-static ON OFF ON OFF OFF OFF OFF OFF ON
verify_profile fuzz fuzz ON OFF OFF OFF OFF ON OFF OFF OFF
