#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=$("$root/scripts/version.sh")
dist=${LIBMDF_DIST_DIR:-$root/dist}
archive="$dist/libmdf-$version.tar.gz"
work="$root/build/package-source-smoke"
[[ -f $archive && ! -L $root/build ]] || { printf 'Missing source archive or unsafe source scratch: %s\n' "$archive" >&2; exit 1; }
rm -rf -- "$work"
mkdir -p "$work/source"
tar -C "$work/source" -xzf "$archive"
src="$work/source/libmdf-$version"
test "$(cat "$src/VERSION")" = "$version"
(cd "$src" && find . -type f | sed 's#^./##' | sort) > "$work/manifest.actual"
sort "$src/RELEASE_MANIFEST" > "$work/manifest.expected"
cmp "$work/manifest.actual" "$work/manifest.expected"
(cd "$src" && cmake --preset x86_64-linux-gnu-release -B "$work/build" \
  -DLIBMDF_BUILD_TESTS=ON -DLIBMDF_BUILD_HOST_TESTS=OFF -DLIBMDF_BUILD_LUA_TESTS=OFF \
  -DLIBMDF_DIST_DIR="$dist")
cmake --build "$work/build"
ctest --test-dir "$work/build" --output-on-failure --stop-on-failure --no-tests=error -j1 -LE memory
