#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
valid_version() { printf '%s\n' "$1" | grep -Eq '^[0-9]+[.][0-9]+[.][0-9]+$'; }
fail() { printf 'libmdf version: %s\n' "$*" >&2; exit 1; }
owner=$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null || true)
if [ -n "$owner" ] && [ "$(CDPATH= cd -- "$owner" && pwd -P)" = "$ROOT" ]; then
  if [ -n "${LIBMDF_RELEASE_VERSION:-}" ]; then
    valid_version "$LIBMDF_RELEASE_VERSION" || fail 'LIBMDF_RELEASE_VERSION must be X.Y.Z'
    printf '%s\n' "$LIBMDF_RELEASE_VERSION"
    exit 0
  fi
  tags=$(git -C "$ROOT" tag --points-at HEAD --list 'v[0-9]*.[0-9]*.[0-9]*')
  version=
  for tag in $tags; do
    valid_version "${tag#v}" || continue
    [ "$(git -C "$ROOT" cat-file -t "refs/tags/$tag")" = commit ] || continue
    if git -C "$ROOT" symbolic-ref -q "refs/tags/$tag" >/dev/null 2>&1; then continue; fi
    [ -z "$version" ] || fail "ambiguous lightweight release tags on HEAD: v$version and $tag"
    version=${tag#v}
  done
  printf '%s\n' "${version:-0.0.0}"
elif [ -f "$ROOT/VERSION" ]; then
  [ -f "$ROOT/RELEASE_MANIFEST" ] || fail 'extracted source VERSION requires RELEASE_MANIFEST'
  version=$(cat "$ROOT/VERSION")
  valid_version "$version" || fail 'source VERSION must be X.Y.Z'
  printf '%s\n' "$version"
else
  printf '%s\n' '0.0.0'
fi
