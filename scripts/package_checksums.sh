#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=$("$root/scripts/version.sh")
dist=${LIBMDF_DIST_DIR:-$root/dist}
scope=${1:-complete}
inventory=$("$root/scripts/release_artifacts.sh" "$scope")
mapfile -t artifacts <<< "$inventory"
case "$scope" in
  binary)
    manifest="${LIBMDF_PACKAGE_WORK_DIR:-$root/build/package}/binary-CHECKSUMS";;
  complete)
    [[ -z ${LIBMDF_TARGETS:-} ]] || { printf 'Complete manifest requires the full declared target matrix\n' >&2; exit 1; }
    manifest="$dist/libmdf-$version-CHECKSUMS"
    ;;
  *) printf 'usage: package_checksums.sh [complete|binary]\n' >&2; exit 2;;
esac
for artifact in "${artifacts[@]}"; do
  [[ -f $dist/$artifact ]] || { printf 'Missing required %s artifact: %s\n' "$scope" "$dist/$artifact" >&2; exit 1; }
done
mkdir -p "$(dirname -- "$manifest")"
(cd "$dist" && sha256sum "${artifacts[@]}") > "$manifest.tmp"
if [[ -f $manifest ]] && cmp -s "$manifest.tmp" "$manifest"; then
  rm "$manifest.tmp"
else
  mv "$manifest.tmp" "$manifest"
fi
printf '%s\n' "$manifest"
