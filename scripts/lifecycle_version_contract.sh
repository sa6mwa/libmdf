#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RESERVED_TEST_TAG=v99.99.99
created_tag=0

fail() {
  printf '%s\n' "lifecycle version contract failed: $1" >&2
  exit 1
}

cleanup() {
  if [ "$created_tag" -eq 1 ]; then
    git -C "$ROOT" tag -d "$RESERVED_TEST_TAG" >/dev/null
  fi
}

trap cleanup EXIT HUP INT TERM

git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 ||
  fail "must run from a git worktree"

# This impossible version is lifecycle-owned test state. Remove stale residue
# from an interrupted invocation before examining exact release tags.
if git -C "$ROOT" show-ref --verify --quiet "refs/tags/$RESERVED_TEST_TAG"; then
  git -C "$ROOT" tag -d "$RESERVED_TEST_TAG" >/dev/null
fi

expected=$(sh "$ROOT/scripts/version.sh")
actual=$(make --no-print-directory -C "$ROOT" print-release-version)
[ "$actual" = "$expected" ] ||
  fail "make print-release-version resolved $actual; expected $expected"

if [ "$expected" != "0.0.0" ]; then
  exit 0
fi

git -C "$ROOT" -c tag.gpgSign=false tag "$RESERVED_TEST_TAG" ||
  fail "could not create temporary lightweight tag"
created_tag=1
[ "$(git -C "$ROOT" cat-file -t "refs/tags/$RESERVED_TEST_TAG")" = "commit" ] ||
  fail "temporary tag must be lightweight"

actual=$(sh "$ROOT/scripts/version.sh")
[ "$actual" = "${RESERVED_TEST_TAG#v}" ] ||
  fail "version.sh did not select the active-worktree temporary tag"
actual=$(make --no-print-directory -C "$ROOT" print-release-version)
[ "$actual" = "${RESERVED_TEST_TAG#v}" ] ||
  fail "make print-release-version did not select the active-worktree temporary tag"
