#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=$("$root/scripts/version.sh")
name="libmdf-$version"
dist=${LIBMDF_DIST_DIR:-$root/dist}
build="$root/build/package-source"
stage="$build/$name"
[[ ! -L $root/build && ! -L $dist ]] || { printf 'Source staging must use owned directories\n' >&2; exit 1; }
owner=$(git -C "$root" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -n $owner && $(cd "$owner" && pwd -P) == "$root" ]]; then
  git -C "$root" diff --quiet HEAD -- || { printf 'Source packaging requires committed tracked inputs\n' >&2; exit 1; }
  rm -rf -- "$build"
  mkdir -p "$build" "$dist"
  git -C "$root" archive --format=tar --prefix="$name/" HEAD | tar -C "$build" -xf -
  { git -C "$root" ls-tree -r --name-only HEAD; printf '%s\n' RELEASE_MANIFEST VERSION; } | sort -u > "$stage/RELEASE_MANIFEST"
else
  [[ -f $root/RELEASE_MANIFEST ]] || { printf 'Extracted source requires RELEASE_MANIFEST\n' >&2; exit 1; }
  # Validate the complete input list before changing staging.
  while IFS= read -r path; do
    case "$path" in ''|/*|..|../*|*/../*|*/..|build/*|dist/*|.git/*) printf 'Invalid source manifest path: %s\n' "$path" >&2; exit 1;; esac
    [[ -f $root/$path && ! -L $root/$path ]] || { printf 'Missing source manifest input: %s\n' "$path" >&2; exit 1; }
  done < "$root/RELEASE_MANIFEST"
  rm -rf -- "$build"
  mkdir -p "$stage" "$dist"
  tar -C "$root" -cf - -T "$root/RELEASE_MANIFEST" | tar -C "$stage" -xf -
fi
printf '%s\n' "$version" > "$stage/VERSION"
"$root/scripts/archive.sh" "$build" "$name" "$dist/$name.tar.gz"
printf '%s\n' "$dist/$name.tar.gz"
