#!/usr/bin/env bash
# Test real preflight and sequencing in a disposable, locally owned repository.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-release-lifecycle"
rm -rf -- "$work"
repo="$work/repo"
mkdir -p "$repo/scripts" "$repo/fixture/bin" "$repo/record"
cp "$root/scripts/"{release.sh,version.sh,lifecycle_version_contract.sh,clean.sh} "$repo/scripts/"
printf 'build/\ndist/\n.cache/\nrecord/\n' > "$repo/.gitignore"
cat > "$repo/fixture/bin/make" <<'MAKE'
#!/usr/bin/env bash
set -euo pipefail
phase=${@: -1}
printf '%s\n' "$phase" >> "$TEST_PHASE_LOG"
if [[ ${TEST_FAIL_PHASE:-} == "$phase" ]]; then exit 17; fi
if [[ $phase == package && -n ${LIBMDF_NATIVE_RELEASE_BUILD:-} ]]; then
  printf '%s\n' "$LIBMDF_NATIVE_RELEASE_BUILD" > "$TEST_NATIVE_RECORD"
fi
MAKE
chmod +x "$repo/fixture/bin/make"
git -C "$repo" init -q
git -C "$repo" config user.email test@example.invalid
git -C "$repo" config user.name 'libmdf test'
git -C "$repo" config core.hooksPath /dev/null
git -C "$repo" config commit.gpgSign false
git -C "$repo" config tag.gpgSign false
git -C "$repo" add .
git -C "$repo" commit -qm 'test(lifecycle): create release sequencing fixture'
export TEST_PHASE_LOG="$repo/record/phases" TEST_NATIVE_RECORD="$repo/record/native" PATH="$repo/fixture/bin:$PATH"
unset LIBMDF_RELEASE_VERSION LIBMDF_TARGETS LIBMDF_NATIVE_RELEASE_BUILD
mkdir -p "$repo/build" "$repo/dist"
touch "$repo/build/preserve" "$repo/dist/preserve"
if "$repo/scripts/release.sh" final > "$repo/record/untagged.log" 2>&1; then exit 1; fi
test -f "$repo/build/preserve"; test -f "$repo/dist/preserve"; test ! -e "$TEST_PHASE_LOG"
git -C "$repo" tag v1.2.3
printf 'dirty\n' >> "$repo/.gitignore"
if "$repo/scripts/release.sh" final > "$repo/record/dirty.log" 2>&1; then exit 1; fi
test -f "$repo/build/preserve"; test ! -e "$TEST_PHASE_LOG"
git -C "$repo" checkout -- .gitignore
"$repo/scripts/release.sh" final > "$repo/record/final.log" 2>&1
test ! -e "$repo/build/preserve"; test ! -e "$repo/dist/preserve"
printf '%s\n' format-check test-hardening package-source-smoke package release-lua-artifacts package-checksums verify-release-archives > "$repo/record/expected"
cmp "$repo/record/expected" "$TEST_PHASE_LOG"
grep -Fxq "$repo/build/package-source-smoke/build" "$TEST_NATIVE_RECORD"
: > "$TEST_PHASE_LOG"
mkdir -p "$repo/build"; touch "$repo/build/warm"
"$repo/scripts/release.sh" prepare > "$repo/record/prepare.log" 2>&1
test -f "$repo/build/warm"
printf '%s\n' format-check test-hardening package release-lua-artifacts package-verify verify-lua-artifacts > "$repo/record/expected"
cmp "$repo/record/expected" "$TEST_PHASE_LOG"
: > "$TEST_PHASE_LOG"
if TEST_FAIL_PHASE=package "$repo/scripts/release.sh" prepare > "$repo/record/failed.log" 2>&1; then exit 1; fi
printf '%s\n' format-check test-hardening package > "$repo/record/expected"
cmp "$repo/record/expected" "$TEST_PHASE_LOG"
if LIBMDF_RELEASE_VERSION=1.2.3 "$repo/scripts/release.sh" final > "$repo/record/override.log" 2>&1; then exit 1; fi
test -f "$repo/build/warm"
