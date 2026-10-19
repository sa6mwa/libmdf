#!/usr/bin/env bash
# Validate a selected build subdirectory before any staging or inventory writes.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[[ $# == 1 ]] || { printf 'usage: require_build_path.sh <owned-build-subdirectory>\n' >&2; exit 2; }
path=$1
case "$path" in "$root"/build/*) ;; *) printf 'Expected a subdirectory under owned build: %s\n' "$path" >&2; exit 2;; esac
case "/${path#"$root"/build/}/" in
  *'/../'*|*'/./'*|*'//'*) printf 'Expected a canonical build subdirectory: %s\n' "$path" >&2; exit 2;;
esac
ancestor=$path
while [[ $ancestor != "$root" ]]; do
  [[ ! -L $ancestor ]] || { printf 'Build subdirectory cannot use a symlink: %s\n' "$ancestor" >&2; exit 1; }
  ancestor=${ancestor%/*}
done
