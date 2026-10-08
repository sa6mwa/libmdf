#!/usr/bin/env bash
# A deterministic tar.gz producer; staging is already owned by the caller.
set -euo pipefail
[[ $# == 3 ]] || { printf 'usage: archive.sh <staging-parent> <root-name> <output>\n' >&2; exit 2; }
parent=$1 name=$2 output=$3
[[ -d $parent/$name && $name != */* && $name != . && $name != .. ]] || { printf 'Invalid archive root: %s/%s\n' "$parent" "$name" >&2; exit 2; }
tar_tool=${LIBMDF_TAR:-tar}
"$tar_tool" --version | grep -q 'GNU tar' || { printf 'GNU tar is required (set LIBMDF_TAR to gtar on macOS)\n' >&2; exit 1; }
umask 022
"$tar_tool" --sort=name --format=posix --pax-option=delete=atime,delete=ctime \
  --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=u+rwX,go+rX,go-w \
  -C "$parent" -cf - "$name" | gzip -n > "$output"
