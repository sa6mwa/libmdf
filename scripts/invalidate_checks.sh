#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[[ ! -L $root/build ]] || { printf 'Cannot invalidate borrowed build state\n' >&2; exit 1; }
for directory in "$root/build/debug/checks" "$root/build/fuzz/checks" "$root/build/package-checks"; do
  [[ ! -L $directory ]] || { printf 'Cannot invalidate borrowed checks: %s\n' "$directory" >&2; exit 1; }
  if [[ -d $directory ]]; then find "$directory" -name '*.pass' -type f -delete; fi
done
