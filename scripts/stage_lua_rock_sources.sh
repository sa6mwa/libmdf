#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=${1:-$("$root/scripts/version.sh")}
stage=${2:-$root/build/lua-release/stage}
case "$stage" in "$root"/build/*) ;; *) printf 'Lua staging must be under the owned build directory\n' >&2; exit 2;; esac
[[ ! -L $root/build && ! -L $stage ]] || { printf 'Lua staging cannot be a symlink\n' >&2; exit 1; }
rm -rf -- "$stage"
dest="$stage/libmdf-lua-$version"
mkdir -p "$dest/lua"
cp "$root/LICENSE" "$root/README.md" "$root/libmdf-scm-1.rockspec.in" "$dest/"
cp "$root/lua/libmdf.lua" "$root/lua/libmdf_core.c" "$dest/lua/"
printf '%s\n' "$version" > "$dest/VERSION"
(cd "$dest" && find . -type f ! -name RELEASE_MANIFEST | sed 's#^./##' | sort > RELEASE_MANIFEST)
printf '%s\n' "$dest"
