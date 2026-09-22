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

recipe() {
  awk -v target="$1" '
    $0 ~ "^" target ":" { active = 1; next }
    active && /^[^[:space:]#]/ { exit }
    active { print }
  ' "$MAKEFILE"
}

pipeline=$(recipe release-pipeline)
printf '%s\n' "$pipeline" | grep -F '$(MAKE) format' >/dev/null || fail "pipeline must format first"
printf '%s\n' "$pipeline" | grep -F '$(MAKE) test-hardening' >/dev/null || fail "pipeline must run ordinary tests"
printf '%s\n' "$pipeline" | grep -F '$(MAKE) release-matrix' >/dev/null || fail "pipeline must run release matrix"
if printf '%s\n' "$pipeline" | grep -F 'package-source-smoke' >/dev/null; then
  fail "ordinary prerelease pipeline must not reconstruct source archives"
fi

release=$(recipe release)
contract_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) lifecycle-version-contract' | awk '{ print $1 }')
clean_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) clean' | awk '{ print $1 }')
pipeline_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) release-pipeline' | awk '{ print $1 }')
source_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) package-source-smoke' | awk '{ print $1 }')
checksums_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) package-checksums' | awk '{ print $1 }')
verify_line=$(printf '%s\n' "$release" | nl -ba | grep -F '$(MAKE) package-verify' | awk '{ print $1 }')
[ -n "$contract_line" ] && [ -n "$clean_line" ] && [ -n "$pipeline_line" ] && [ -n "$source_line" ] && [ -n "$checksums_line" ] && [ -n "$verify_line" ] ||
  fail "release must run version contract, clean, ordinary proof, source reconstruction, checksums, and verification"
[ "$contract_line" -lt "$clean_line" ] && [ "$clean_line" -lt "$pipeline_line" ] &&
  [ "$pipeline_line" -lt "$source_line" ] && [ "$source_line" -lt "$checksums_line" ] &&
  [ "$checksums_line" -lt "$verify_line" ] ||
  fail "release must reserve source reconstruction for the final clean stage"

[ "$(awk '/^prerelease:/ { print; exit }' "$MAKEFILE")" = "prerelease: release-pipeline" ] ||
  fail "prerelease must use the binary-only shared release pipeline"
[ "$(awk '/^prerelease-hardening:/ { print; exit }' "$MAKEFILE")" = "prerelease-hardening: prerelease" ] ||
  fail "prerelease-hardening must be an alias when no additional tier exists"

worktree_root=$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null || true)
if [ "$worktree_root" = "$ROOT" ]; then
  make --no-print-directory -C "$ROOT" lifecycle-version-contract >/dev/null
  if git -C "$ROOT" show-ref --verify --quiet refs/tags/v99.99.99; then
    fail "lifecycle version contract must clean its reserved active-worktree tag"
  fi
else
  [ -f "$ROOT/VERSION" ] || fail "source archive must provide VERSION"
  expected=$(sed -n '1p' "$ROOT/VERSION" | tr -d '[:space:]')
  [ "$(sh "$ROOT/scripts/version.sh")" = "$expected" ] ||
    fail "source archive version.sh must use injected VERSION"
  [ "$(make --no-print-directory -C "$ROOT" print-release-version)" = "$expected" ] ||
    fail "source archive Make version must use injected VERSION"
  if contract_output=$(make --no-print-directory -C "$ROOT" lifecycle-version-contract 2>&1); then
    fail "source archive must reject the tag-mutating version contract"
  fi
  printf '%s\n' "$contract_output" | grep -F 'must run from the git worktree root' >/dev/null ||
    fail "source archive version contract must reject parent git worktrees"
fi

for parity_script in "$ROOT/scripts/parity.sh" "$ROOT/scripts/stream_parity.sh"; do
  grep -F '"$ROOT/src/unicode_classify.c"' "$parity_script" >/dev/null ||
    fail "$(basename "$parity_script") cache key must include unicode_classify.c"
  grep -F '"$ROOT/src/unicode_classify.h"' "$parity_script" >/dev/null ||
    fail "$(basename "$parity_script") cache key must include unicode_classify.h"
done
