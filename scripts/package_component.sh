#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
build=$1 component=$2 archive=$3 target_os=$4
name=$(basename "$archive" .tar.gz)
parent="$build/package"
[[ ! -L $build && ! -L $parent ]] || { printf 'Component staging must be owned: %s\n' "$parent" >&2; exit 1; }
rm -rf -- "$parent/$name"
mkdir -p "$parent" "$(dirname -- "$archive")"
strip_args=()
[[ $target_os == Darwin ]] || strip_args=(--strip)
cmake --install "$build" --component "$component" --prefix "$parent/$name" "${strip_args[@]}"
"$root/scripts/archive.sh" "$parent" "$name" "$archive"
