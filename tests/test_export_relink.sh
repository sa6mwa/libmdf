#!/usr/bin/env bash
# A list-only edit must change actual shared exports without recompiling C.
set -euo pipefail
unset LIBMDF_RELEASE_VERSION
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-export-relink"
rm -rf -- "$work"
mkdir -p "$work/source"
cp -R "$root/"{src,include,cmake,scripts} "$work/source/"
cp "$root/CMakeLists.txt" "$work/source/"
source="$work/source" build="$work/build"
cmake -S "$source" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/x86_64-linux-gnu.cmake" \
  -DLIBMDF_BUILD_STATIC=OFF -DLIBMDF_BUILD_SHARED=ON -DLIBMDF_BUILD_BINARY=OFF \
  -DLIBMDF_BUILD_TESTS=OFF -DLIBMDF_BUILD_HOST_TESTS=OFF -DLIBMDF_BUILD_LUA_TESTS=OFF \
  -DLIBMDF_BUILD_EXAMPLES=OFF -DLIBMDF_INSTALL=OFF > "$work/configure.log"
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" --target mdf_shared > "$work/first.log"
nm=$("$root/scripts/discover_target_tools.sh" --target x86_64-linux-gnu --cache "$build/CMakeCache.txt" nm)
exports() { "$nm" -D --defined-only "$build/libmdf.so" > "$work/exports"; }
exports
grep -q ' mdf_feed@@' "$work/exports"
find "$build/CMakeFiles/mdf_shared.dir" -name '*.o' -printf '%P %T@\n' | sort > "$work/objects.before"
cp "$source/cmake/libmdf.exports.map" "$work/original.map"
sed -i '/    mdf_feed;/d' "$source/cmake/libmdf.exports.map"
cmake --build "$build" --target mdf_shared > "$work/changed.log"
exports
if grep -q ' mdf_feed@@' "$work/exports"; then printf 'Export-list edit did not relink the shared library\n' >&2; exit 1; fi
cp "$work/original.map" "$source/cmake/libmdf.exports.map"
cmake --build "$build" --target mdf_shared > "$work/restored.log"
bash "$root/tests/test_shared_exports.sh" "$nm" "$build/libmdf.so"
find "$build/CMakeFiles/mdf_shared.dir" -name '*.o' -printf '%P %T@\n' | sort > "$work/objects.after"
cmp "$work/objects.before" "$work/objects.after"
