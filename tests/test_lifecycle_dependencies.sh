#!/usr/bin/env bash
# Prove observable shared-only Lua support and public-header archive invalidation.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-lifecycle-dependencies"
mkdir -p "$work/shared" "$work/source"
cmake --preset debug -S "$root" -B "$work/shared" -DLIBMDF_BUILD_STATIC=OFF -DLIBMDF_BUILD_HOST_TESTS=OFF > "$work/shared.log" 2>&1
cmake --build "$work/shared" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" \
  --target lua-sdk libmdf_lua_core lua test_destination_policy >> "$work/shared.log" 2>&1
ctest --test-dir "$work/shared" -R '^lua_(destination_policy|runtime)$' \
  --output-on-failure --stop-on-failure --no-tests=error -j1 >> "$work/shared.log" 2>&1
cmake --preset debug -S "$root" -B "$work/layout" -DLIBMDF_BUILD_HOST_TESTS=OFF \
  -DCMAKE_INSTALL_LIBDIR=lib64 -DCMAKE_INSTALL_INCLUDEDIR=api/include \
  -DLIBMDF_INSTALL_BINARY=ON -DLIBMDF_CMDF_STATIC_RUNTIME=ON > "$work/layout.log" 2>&1
cmake --build "$work/layout" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" \
  --target libmdf_lua_core lua lua-cli test_destination_policy cmdf >> "$work/layout.log" 2>&1
ctest --test-dir "$work/layout" -R '^lua_(smoke|destination_policy|cli_destination_policy|runtime)$' \
  --output-on-failure --stop-on-failure --no-tests=error -j1 >> "$work/layout.log" 2>&1
cmake --install "$work/layout" --component cli --prefix "$work/cli-install" >> "$work/layout.log" 2>&1
test -x "$work/cli-install/bin/cmdf.lua"
# A local source copy lets the test edit a public input without changing the
# checkout while a verification operation owns it. Release excludes test inputs.
cp "$root/CMakeLists.txt" "$root/CMakePresets.json" "$root/LICENSE" "$root/README.md" "$work/source/"
cp -R "$root/include" "$root/src" "$root/cmake" "$root/scripts" "$work/source/"
mkdir -p "$work/dist"
cmake --preset x86_64-linux-gnu-release -S "$work/source" -B "$work/release" \
  -DLIBMDF_DIST_DIR="$work/dist" > "$work/header.log" 2>&1
cmake --build "$work/release" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" --target package-sdk >> "$work/header.log" 2>&1
archive="$work/dist/libmdf-0.0.0-x86_64-linux-gnu.tar.gz"
cp "$archive" "$work/before.tar.gz"
printf '\n#define LIBMDF_UMBRELLA_REBUILD_PROBE 1\n' >> "$work/source/include/libmdf.h"
cmake --build "$work/release" --target package-sdk >> "$work/header.log" 2>&1
if cmp -s "$work/before.tar.gz" "$archive"; then printf 'Public umbrella-header edit did not refresh its SDK archive\n' >&2; exit 1; fi
tar -xOzf "$archive" libmdf-0.0.0-x86_64-linux-gnu/include/libmdf.h > "$work/shipped-header.h"
grep -Fxq '#define LIBMDF_UMBRELLA_REBUILD_PROBE 1' "$work/shipped-header.h"
