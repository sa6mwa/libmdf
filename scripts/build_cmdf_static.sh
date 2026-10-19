#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
"$root/scripts/build.sh" x86_64-linux-musl-release --target cmdf
binary="$root/build/x86_64-linux-musl-release/cmdf"
readelf=$("$root/scripts/discover_target_tools.sh" --target x86_64-linux-musl --preset x86_64-linux-musl-release readelf)
"$readelf" -l "$binary" > "$root/build/x86_64-linux-musl-release/cmdf-program-headers.txt"
"$readelf" -d "$binary" > "$root/build/x86_64-linux-musl-release/cmdf-dynamic.txt"
if grep -Eq 'Requesting program interpreter|\(NEEDED\)' "$root/build/x86_64-linux-musl-release/cmdf-program-headers.txt" "$root/build/x86_64-linux-musl-release/cmdf-dynamic.txt"; then
  printf 'cmdf install binary must be static: %s\n' "$binary" >&2; exit 1;
fi
mkdir -p "$root/build/cmdf-static"
cp "$binary" "$root/build/cmdf-static/cmdf"
printf '%s\n' "$root/build/cmdf-static/cmdf"
