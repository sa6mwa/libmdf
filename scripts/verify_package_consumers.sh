#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
sdk=$1 target=$2 work=$3
version=$("$root/scripts/version.sh")
mkdir -p "$work"
configure_consumers() {
  local prefix=$1 output=$2
  PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR= \
    "$root/scripts/with_target_env.sh" "$target" cmake -S "$root/tests/package-consumer" -B "$output" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/$target.cmake" \
    -Dlibmdf_DIR="$prefix/lib/cmake/libmdf"
  "$root/scripts/with_target_env.sh" "$target" cmake --build "$output" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
  case "$(uname -s):$(uname -m):$target" in
    Linux:x86_64:x86_64-linux-*|Darwin:arm64:arm64-apple-darwin)
      for consumer in static shared pkgconfig; do
        actual=$("$output/consumer_$consumer")
        [[ $actual == "$version" ]] || { printf 'Consumer version mismatch: %s\n' "$actual" >&2; exit 1; }
      done;;
    *) printf 'Consumer execution deferred for %s; target link proof passed\n' "$target";;
  esac
}
configure_consumers "$sdk" "$work/original"
mv "$sdk" "$work/relocated SDK"
configure_consumers "$work/relocated SDK" "$work/relocated"
