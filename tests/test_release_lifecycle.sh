#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MAKEFILE="$ROOT/Makefile"

fail() {
  printf '%s\n' "FAIL: $1" >&2
  exit 1
}

help=$(make --no-print-directory -C "$ROOT" help)
printf '%s\n' "$help" | grep -F 'make prerelease             Run the release proof graph without an initial clean' >/dev/null ||
  fail "help must describe prerelease as the unclean release proof graph"
printf '%s\n' "$help" | grep -F 'make lifecycle-version-contract Verify lightweight-tag release version handling' >/dev/null ||
  fail "help must describe lifecycle-version-contract"
printf '%s\n' "$help" | grep -F 'make release                Run the clean final release gate' >/dev/null ||
  fail "help must describe release as the clean final gate"

pipeline=$(awk '
  /^release-pipeline:/ { active = 1; next }
  active && /^[^[:space:]#]/ { exit }
  active { print }
' "$MAKEFILE")
printf '%s\n' "$pipeline" | grep -F '$(MAKE) format' >/dev/null || fail "pipeline must format first"
printf '%s\n' "$pipeline" | grep -F '$(MAKE) test-hardening' >/dev/null || fail "pipeline must run ordinary tests"
printf '%s\n' "$pipeline" | grep -F '$(MAKE) package-source-smoke' >/dev/null || fail "pipeline must smoke-test source artifacts"
printf '%s\n' "$pipeline" | grep -F '$(MAKE) release-matrix' >/dev/null || fail "pipeline must run release matrix"
tests_line=$(printf '%s\n' "$pipeline" | nl -ba | grep -F '$(MAKE) test-hardening' | awk '{ print $1 }')
matrix_line=$(printf '%s\n' "$pipeline" | nl -ba | grep -F '$(MAKE) release-matrix' | awk '{ print $1 }')
[ "$tests_line" -lt "$matrix_line" ] ||
  fail "pipeline must run ordinary tests before the release matrix"

release=$(awk '
  /^release:/ { active = 1; next }
  active && /^[^[:space:]#]/ { exit }
  active { print }
' "$MAKEFILE")
contract_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) lifecycle-version-contract' | awk '{ print $1 }')
clean_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) clean' | awk '{ print $1 }')
pipeline_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) release-pipeline' | awk '{ print $1 }')
[ -n "$contract_line" ] && [ -n "$clean_line" ] && [ -n "$pipeline_line" ] ||
  fail "release must invoke version contract, clean, and shared pipeline"
[ "$contract_line" -lt "$clean_line" ] && [ "$clean_line" -lt "$pipeline_line" ] ||
  fail "release order must be version contract, clean, then shared pipeline"
[ "$(printf '%s\n' "$release" | grep -F '$(MAKE)' | wc -l | tr -d '[:space:]')" = 3 ] ||
  fail "release must not duplicate or bypass the shared pipeline"

prerelease=$(awk '/^prerelease:/ { print; exit }' "$MAKEFILE")
[ "$prerelease" = "prerelease: release-pipeline" ] ||
  fail "prerelease must use the shared release pipeline"

prerelease_hardening=$(awk '/^prerelease-hardening:/ { print; exit }' "$MAKEFILE")
[ "$prerelease_hardening" = "prerelease-hardening: prerelease" ] ||
  fail "prerelease-hardening must be an alias when no additional tier exists"

contract="$ROOT/scripts/lifecycle_version_contract.sh"
grep -F 'PREFERRED_TEST_TAG=v99.99.99' "$contract" >/dev/null ||
  fail "version contract must prefer the reserved test tag"
grep -F 'show-ref --verify --quiet "refs/tags/$PREFERRED_TEST_TAG"' "$contract" >/dev/null ||
  fail "version contract must reject an existing reserved test tag"
if grep -F 'tag -d "$PREFERRED_TEST_TAG"' "$contract" >/dev/null; then
  fail "version contract must never delete a reserved test tag"
fi
if grep -F 'tag "$PREFERRED_TEST_TAG"' "$contract" >/dev/null; then
  fail "version contract must never create the reserved test tag in the real worktree"
fi
make --no-print-directory -C "$ROOT" lifecycle-version-contract >/dev/null
if git -C "$ROOT" show-ref --verify --quiet refs/tags/v99.99.99; then
  fail "lifecycle version contract must leave no reserved tag in the real worktree"
fi

reserved_repo="$ROOT/build/test-reserved-lifecycle-tag"
rm -rf "$reserved_repo"
mkdir -p "$reserved_repo/scripts"
cp "$ROOT/scripts/lifecycle_version_contract.sh" "$ROOT/scripts/version.sh" \
  "$ROOT/scripts/release_version.sh" "$reserved_repo/scripts/"
cat >"$reserved_repo/Makefile" <<'EOF'
print-release-version:
	@scripts/release_version.sh
EOF
git -C "$reserved_repo" init -q
git -C "$reserved_repo" config user.email test@example.invalid
git -C "$reserved_repo" config user.name libmdf-test
git -C "$reserved_repo" add Makefile scripts
git -C "$reserved_repo" commit -q -m initial
git -C "$reserved_repo" -c tag.gpgSign=false tag v99.99.99
if sh "$reserved_repo/scripts/lifecycle_version_contract.sh" >/dev/null 2>&1; then
  fail "version contract must reject a pre-existing reserved test tag"
fi
[ "$(git -C "$reserved_repo" cat-file -t refs/tags/v99.99.99)" = commit ] ||
  fail "version contract must preserve a pre-existing reserved test tag"
if sh "$reserved_repo/scripts/version.sh" >/dev/null 2>&1; then
  fail "reserved test tag must reject release version resolution"
fi

for parity_script in "$ROOT/scripts/parity.sh" "$ROOT/scripts/stream_parity.sh"; do
  grep -F '"$ROOT/src/unicode_classify.c"' "$parity_script" >/dev/null ||
    fail "$(basename "$parity_script") cache key must include unicode_classify.c"
  grep -F '"$ROOT/src/unicode_classify.h"' "$parity_script" >/dev/null ||
    fail "$(basename "$parity_script") cache key must include unicode_classify.h"
done
