#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/test-cmdf-install.XXXXXX")
eval "$("$root/scripts/bootlin_x86_runtime.sh")"
for mode in OFF ON; do
  build="$work/$mode"
  cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/x86_64-linux-gnu.cmake" \
    -DCMAKE_INSTALL_PREFIX="$build/install" \
    -DLIBMDF_BUILD_TESTS=OFF -DLIBMDF_BUILD_EXAMPLES=OFF \
    -DLIBMDF_CMDF_STATIC_RUNTIME="$mode" >"$work/$mode.log" 2>&1
  cmake --build "$build" >>"$work/$mode.log" 2>&1
  cmake --install "$build" >>"$work/$mode.log" 2>&1
  test -f "$build/install/lib/libmdf.a"
  if [[ "$mode" == OFF ]]; then
    cmake -DREADELF="$LIBMDF_BOOTLIN_READELF" -DEXECUTABLE="$build/cmdf" \
      -DINTERPRETER="$LIBMDF_BOOTLIN_INTERPRETER" -DRUNTIME_DIR="$LIBMDF_BOOTLIN_RUNTIME_DIR" \
      -P "$root/tests/assert_bootlin_runtime.cmake"
    test ! -e "$build/install/bin/cmdf"
  else
    mv "$build/install" "$work/relocated"
    executable="$work/relocated/bin/cmdf"
    "$LIBMDF_BOOTLIN_READELF" -l -d "$executable" >"$work/installed.elf"
    if grep -E 'INTERP|RPATH|RUNPATH|NEEDED' "$work/installed.elf"; then
      printf 'Installed Linux cmdf must be static\n' >&2
      exit 1
    fi
    "$executable" --version
    printf '# portable\n' | "$executable" -b | grep -q portable
  fi
done
