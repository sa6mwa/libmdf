#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output="$root/build/parityjudge-suite-test"
eval "$("$root/scripts/bootlin_x86_runtime.sh")"
mkdir -p "$(dirname -- "$output")"

link_flags="-linkmode=external -extldflags=-Wl,--dynamic-linker,${LIBMDF_BOOTLIN_INTERPRETER},--disable-new-dtags,-rpath,${LIBMDF_BOOTLIN_RUNTIME_RPATH}"
(
  cd "$root/parityjudge"
  CC="$LIBMDF_BOOTLIN_CC" CGO_ENABLED=1 go test -c -ldflags "$link_flags" -o "$output" .
)

cmake \
  -DREADELF="$LIBMDF_BOOTLIN_READELF" \
  -DEXECUTABLE="$output" \
  -DINTERPRETER="$LIBMDF_BOOTLIN_INTERPRETER" \
  -DRUNTIME_DIR="$LIBMDF_BOOTLIN_RUNTIME_DIR" \
  -P "$root/tests/assert_bootlin_runtime.cmake"
exec "$output"
