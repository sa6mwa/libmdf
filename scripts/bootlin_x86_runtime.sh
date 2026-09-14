#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 0 ]]; then
  printf 'usage: %s\n' "$0" >&2
  exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
resolver="$root/scripts/cpkt-toolchains.sh"
description=$("$resolver" ensure x86_64-linux-gnu)

value() {
  local key=$1
  sed -n "s/^${key}=//p" <<<"$description" | tail -1
}

cc=$(value cc)
readelf=$(value readelf)
sysroot=$(value sysroot)
[[ -x "$cc" && -x "$readelf" && -d "$sysroot" ]] || {
  printf '%s\n' 'Bootlin runtime metadata is incomplete' >&2
  exit 1
}

loader=$(find "$sysroot/lib" -maxdepth 1 -type f \( -name 'ld-linux*.so*' -o -name 'ld-musl-*.so*' \) -print | sort | head -n 1)
[[ -n "$loader" && -f "$loader" ]] || {
  printf 'Bootlin sysroot has no ELF interpreter: %s\n' "$sysroot" >&2
  exit 1
}

libgcc=$("$cc" -print-file-name=libgcc_s.so.1)
[[ -f "$libgcc" ]] || {
  printf '%s\n' 'Bootlin compiler did not locate libgcc_s.so.1' >&2
  exit 1
}
runtime_dirs="$sysroot/lib:$sysroot/usr/lib:$(dirname -- "$libgcc")"

printf 'export LIBMDF_BOOTLIN_CC=%q\n' "$cc"
printf 'export LIBMDF_BOOTLIN_READELF=%q\n' "$readelf"
printf 'export LIBMDF_BOOTLIN_INTERPRETER=%q\n' "$loader"
printf 'export LIBMDF_BOOTLIN_RUNTIME_DIR=%q\n' "$sysroot/lib"
printf 'export LIBMDF_BOOTLIN_RUNTIME_RPATH=%q\n' "$runtime_dirs"
