#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "$#" -lt 2 ]; then
  printf '%s\n' "usage: $0 <preset> <dev|release|package|cmdf-static|lua|fuzz> [cmake-args...]" >&2
  exit 2
fi

preset=$1
profile=$2
shift 2

common='-DLIBMDF_BUILD_STATIC=ON -DLIBMDF_BUILD_SHARED=ON'
with_target_env="$ROOT/scripts/with_target_env.sh"
cache="$ROOT/build/$preset/CMakeCache.txt"

cache_value() {
  key=$1
  sed -n "s/^$key:[^=]*=//p" "$cache" | sed -n '1p'
}

case "$preset" in
  debug|debug-lua)
    expected_toolchain="$ROOT/cmake/toolchains/x86_64-linux-gnu.cmake"
    ;;
  fuzz)
    expected_toolchain="$ROOT/cmake/toolchains/x86_64-linux-gnu-afl.cmake"
    ;;
  *-release)
    expected_toolchain="$ROOT/cmake/toolchains/${preset%-release}.cmake"
    ;;
  *)
    expected_toolchain=
    ;;
esac

if [ -n "$expected_toolchain" ] && [ -f "$cache" ]; then
  cached_toolchain=$(cache_value CMAKE_TOOLCHAIN_FILE)
  if [ "$cached_toolchain" != "$expected_toolchain" ]; then
    rm -rf "$ROOT/build/$preset"
  fi
fi

case "$profile" in
  dev)
    exec "$with_target_env" "$preset" cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=ON \
      -DLIBMDF_BUILD_EXAMPLES=ON \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=ON \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  release)
    exec "$with_target_env" "$preset" cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=ON \
      -DLIBMDF_BUILD_EXAMPLES=ON \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=ON \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  package)
    exec "$with_target_env" "$preset" cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=OFF \
      -DLIBMDF_BUILD_EXAMPLES=OFF \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=ON \
      "$@"
    ;;
  cmdf-static)
    exec "$with_target_env" "$preset" cmake --preset "$preset" \
      -DLIBMDF_BUILD_STATIC=ON \
      -DLIBMDF_BUILD_SHARED=OFF \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=OFF \
      -DLIBMDF_BUILD_EXAMPLES=OFF \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=OFF \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=ON \
      "$@"
    ;;
  lua)
    exec "$with_target_env" "$preset" cmake --preset "$preset" \
      $common \
      -DLIBMDF_BUILD_BINARY=ON \
      -DLIBMDF_BUILD_TESTS=ON \
      -DLIBMDF_BUILD_EXAMPLES=ON \
      -DLIBMDF_BUILD_FUZZERS=OFF \
      -DLIBMDF_INSTALL=ON \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  fuzz)
    if [ -f "$cache" ]; then
      cached_compiler=$(cache_value CMAKE_C_COMPILER)
      case "$cached_compiler" in
        */cpkt-afl-gcc) ;;
        *) rm -rf "$ROOT/build/fuzz" ;;
      esac
    fi
    exec "$with_target_env" "$preset" cmake --preset "$preset" \
      -DLIBMDF_BUILD_STATIC=ON \
      -DLIBMDF_BUILD_SHARED=OFF \
      -DLIBMDF_BUILD_BINARY=OFF \
      -DLIBMDF_BUILD_TESTS=OFF \
      -DLIBMDF_BUILD_EXAMPLES=OFF \
      -DLIBMDF_BUILD_FUZZERS=ON \
      -DLIBMDF_INSTALL=OFF \
      -DLIBMDF_INSTALL_BINARY=OFF \
      -DLIBMDF_CMDF_STATIC_RUNTIME=OFF \
      "$@"
    ;;
  *)
    printf '%s\n' "unknown configure profile: $profile" >&2
    exit 2
    ;;
esac
