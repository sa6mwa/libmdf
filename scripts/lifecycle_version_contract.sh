#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PREFERRED_TEST_TAG=v99.99.99
CONTRACT_TEST_TAG=v0.0.1
TEST_REPO=

fail() {
  printf '%s\n' "lifecycle version contract failed: $1" >&2
  exit 1
}

cleanup() {
  if [ -n "$TEST_REPO" ] && [ -d "$TEST_REPO" ]; then
    rm -rf "$TEST_REPO"
  fi
}

trap cleanup EXIT HUP INT TERM

git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 ||
  fail "must run from a git worktree"

# This ref is a test-only sentinel, never a release version.  Refuse a
# collision without changing it: a lifecycle check must not delete, replace,
# or hide a user-created tag, even if that tag is itself invalid state.
if git -C "$ROOT" show-ref --verify --quiet "refs/tags/$PREFERRED_TEST_TAG"; then
  fail "$PREFERRED_TEST_TAG is reserved for the lifecycle test and must not exist"
fi

expected=$(
  git -C "$ROOT" tag --points-at HEAD --list 'v[0-9]*.[0-9]*.[0-9]*' |
    while IFS= read -r candidate; do
      version=${candidate#v}
      if printf '%s\n' "$version" | grep -Eq '^[0-9]+[.][0-9]+[.][0-9]+$' &&
         [ "$(git -C "$ROOT" cat-file -t "refs/tags/$candidate" 2>/dev/null || true)" = "commit" ]; then
        printf '%s\n' "$version"
      fi
    done |
    awk -F. '
      NF == 3 {
        major = $1 + 0
        minor = $2 + 0
        patch = $3 + 0
        if (!seen ||
            major > best_major ||
            (major == best_major && minor > best_minor) ||
            (major == best_major && minor == best_minor && patch > best_patch)) {
          seen = 1
          best = $0
          best_major = major
          best_minor = minor
          best_patch = patch
        }
      }
      END {
        if (seen) {
          print best
        }
      }'
)

if [ -z "$expected" ]; then
  expected=0.0.0
fi

actual=$(sh "$ROOT/scripts/version.sh")
[ "$actual" = "$expected" ] ||
  fail "version.sh resolved $actual; expected $expected"

actual=$(make --no-print-directory -C "$ROOT" print-release-version)
[ "$actual" = "$expected" ] ||
  fail "make print-release-version resolved $actual; expected $expected"

if [ "$expected" != "0.0.0" ]; then
  exit 0
fi

TEST_REPO=$(mktemp -d "${TMPDIR:-/tmp}/libmdf-lifecycle-version.XXXXXX") ||
  fail "could not create isolated lifecycle version repository"
mkdir -p "$TEST_REPO/scripts" || fail "could not initialize isolated lifecycle version repository"
cp "$ROOT/scripts/version.sh" "$ROOT/scripts/release_version.sh" "$TEST_REPO/scripts/" ||
  fail "could not copy version scripts to isolated lifecycle version repository"
printf '%s\n' 'print-release-version:' '	@sh scripts/release_version.sh' >"$TEST_REPO/Makefile"
git -C "$TEST_REPO" init -q || fail "could not initialize isolated lifecycle version repository"
git -C "$TEST_REPO" config user.email lifecycle@example.invalid ||
  fail "could not configure isolated lifecycle version repository"
git -C "$TEST_REPO" config user.name libmdf-lifecycle ||
  fail "could not configure isolated lifecycle version repository"
git -C "$TEST_REPO" add Makefile scripts || fail "could not stage isolated lifecycle version repository"
git -C "$TEST_REPO" commit -q -m lifecycle-version-contract ||
  fail "could not commit isolated lifecycle version repository"
git -C "$TEST_REPO" -c tag.gpgSign=false tag "$CONTRACT_TEST_TAG" ||
  fail "could not create isolated lifecycle test tag"
[ "$(git -C "$TEST_REPO" cat-file -t "refs/tags/$CONTRACT_TEST_TAG")" = "commit" ] ||
  fail "temporary tag must be lightweight"

expected=${CONTRACT_TEST_TAG#v}

actual=$(sh "$TEST_REPO/scripts/version.sh")
[ "$actual" = "$expected" ] ||
  fail "version.sh did not select temporary lightweight tag"

actual=$(make --no-print-directory -C "$TEST_REPO" print-release-version)
[ "$actual" = "$expected" ] ||
  fail "make print-release-version did not select temporary lightweight tag"
