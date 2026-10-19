#!/usr/bin/env bash
# Read-only final release preflight. Reject invalid identity before any producer.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
fail() { printf 'Release preflight: %s\n' "$*" >&2; exit 1; }
owner=$(git -C "$root" rev-parse --show-toplevel 2>/dev/null) || fail 'an owning Git checkout is required'
[[ $(cd "$owner" && pwd -P) == "$root" ]] || fail 'the source root must own its Git checkout'
if default_ref=$(git -C "$root" symbolic-ref --quiet refs/remotes/origin/HEAD); then
  [[ $default_ref == refs/remotes/origin/* ]] || fail 'origin/HEAD must identify the remote default branch'
  default_branch=${default_ref#refs/remotes/origin/}
elif git -C "$root" show-ref --verify --quiet refs/heads/trunk; then
  default_branch=trunk
else
  fail 'unable to resolve the default branch; set origin/HEAD or provide the declared local trunk branch'
fi
branch_ref=$(git -C "$root" symbolic-ref --quiet HEAD) || fail 'HEAD must be attached to the resolved default branch for final release'
[[ $branch_ref == "refs/heads/$default_branch" ]] || fail "final release requires HEAD attached to the resolved default branch $default_branch"
[[ -z ${LIBMDF_RELEASE_VERSION:-} ]] || fail 'unset the candidate LIBMDF_RELEASE_VERSION override'
[[ -z ${LIBMDF_TARGETS:-} ]] || fail 'final release requires the complete declared target matrix; unset LIBMDF_TARGETS'
[[ ${LIBMDF_DIST_DIR:-$root/dist} == "$root/dist" ]] || fail 'final release artifacts must use the owned dist directory; unset LIBMDF_DIST_DIR'
[[ ${LIBMDF_PACKAGE_WORK_DIR:-$root/build/package} == "$root/build/package" ]] || fail 'final release staging must use owned build/package; unset LIBMDF_PACKAGE_WORK_DIR'
[[ -z $(git -C "$root" status --porcelain=v1 -uall) ]] || fail 'commit the source changes before final release'
version=$("$root/scripts/version.sh")
tag="refs/tags/v$version"
[[ $(git -C "$root" cat-file -t "$tag" 2>/dev/null) == commit ]] || fail "an exact lightweight vX.Y.Z tag on HEAD is required (resolved $version)"
[[ $(git -C "$root" rev-parse "$tag") == $(git -C "$root" rev-parse HEAD) ]] || fail 'the release tag must point to HEAD'
if git -C "$root" symbolic-ref -q "$tag" >/dev/null 2>&1; then fail 'symbolic tags cannot identify a release'; fi
git -C "$root" verify-commit HEAD || fail 'the final amended commit must have a verified signature before release'
printf 'Release preflight passed: v%s at %s on %s\n' "$version" "$(git -C "$root" rev-parse HEAD)" "$default_branch"
