#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=${1:-$("$root/scripts/version.sh")}
stage=${2:-$root/build/lua-release/stage}
"$root/scripts/require_build_path.sh" "$stage"
rm -rf -- "$stage"
dest="$stage/libmdf-lua-$version"
mkdir -p "$dest/lua"
cp "$root/LICENSE" "$root/README.md" "$root/libmdf-scm-1.rockspec.in" "$dest/"
cp "$root/lua/libmdf.lua" "$root/lua/libmdf_core.c" "$dest/lua/"
printf '%s\n' "$version" > "$dest/VERSION"
(cd "$dest" && find . -type f ! -name RELEASE_MANIFEST | sed 's#^./##' | sort > RELEASE_MANIFEST)
printf '%s\n' "$dest"
