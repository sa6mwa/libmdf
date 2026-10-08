#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
for dir in build dist .cache; do
  [[ ! -L $root/$dir ]] || { printf 'Refusing to clean a borrowed directory: %s\n' "$root/$dir" >&2; exit 1; }
done
rm -rf -- "$root/build" "$root/dist" "$root/.cache"
