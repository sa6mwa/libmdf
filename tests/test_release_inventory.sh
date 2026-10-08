#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-release-inventory"
rm -rf -- "$work"
mkdir -p "$work/dist"
export LIBMDF_DIST_DIR="$work/dist"
unset LIBMDF_TARGETS
version=$("$root/scripts/version.sh")
inventory=$("$root/scripts/release_artifacts.sh" complete)
while IFS= read -r name; do printf 'fixture\n' > "$work/dist/$name"; done <<< "$inventory"
"$root/scripts/package_checksums.sh" complete
(cd "$work/dist" && sha256sum -c "libmdf-$version-CHECKSUMS")
cp "$work/dist/libmdf-$version-CHECKSUMS" "$work/first.manifest"
rm "$work/dist/libmdf-lua-$version.tar.gz"
if "$root/scripts/package_checksums.sh" complete > "$work/missing.log" 2>&1; then exit 1; fi
cmp "$work/first.manifest" "$work/dist/libmdf-$version-CHECKSUMS"
if LIBMDF_TARGETS=x86_64-linux-gnu "$root/scripts/package_checksums.sh" complete > "$work/partial.log" 2>&1; then exit 1; fi
if LIBMDF_TARGETS='x86_64-linux-gnu x86_64-linux-gnu' "$root/scripts/release_artifacts.sh" binary > "$work/duplicate.log" 2>&1; then exit 1; fi
