#!/usr/bin/env bash
# Test real preflight and sequencing in a disposable, locally owned repository.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-release-lifecycle"
rm -rf -- "$work"
repo="$work/repo"
mkdir -p "$repo/scripts" "$repo/fixture/bin" "$repo/record"
cp "$root/scripts/"{release.sh,version.sh,lifecycle_version_contract.sh,clean.sh} "$repo/scripts/"
printf 'build/\ndist/\n.cache\nrecord/\n' > "$repo/.gitignore"
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
unset LIBMDF_RELEASE_VERSION LIBMDF_TARGETS LIBMDF_NATIVE_RELEASE_BUILD LIBMDF_DIST_DIR LIBMDF_PACKAGE_WORK_DIR
mkdir -p "$repo/build/.hidden" "$repo/dist/.hidden" "$repo/.cache/.hidden"
touch "$repo/build/preserve" "$repo/dist/preserve" "$repo/.cache/.hidden/preserve"
if "$repo/scripts/release.sh" final > "$repo/record/untagged.log" 2>&1; then exit 1; fi
test -f "$repo/build/preserve"; test -f "$repo/dist/preserve"; test ! -e "$TEST_PHASE_LOG"
git -C "$repo" tag v1.2.3
printf 'dirty\n' >> "$repo/.gitignore"
if "$repo/scripts/release.sh" final > "$repo/record/dirty.log" 2>&1; then exit 1; fi
test -f "$repo/build/preserve"; test ! -e "$TEST_PHASE_LOG"
git -C "$repo" checkout -- .gitignore
if "$repo/scripts/release.sh" final > "$repo/record/unsigned.log" 2>&1; then exit 1; fi
grep -q 'verified signature' "$repo/record/unsigned.log"
test -f "$repo/build/preserve"; test -f "$repo/dist/preserve"; test ! -e "$TEST_PHASE_LOG"
# Real signatures exercise Git's verifier without changing account configuration.
ssh-keygen -q -t ed25519 -N '' -f "$repo/record/signing-key"
printf 'test@example.invalid %s\n' "$(cat "$repo/record/signing-key.pub")" > "$repo/record/allowed-signers"
git -C "$repo" config gpg.format ssh
git -C "$repo" config gpg.ssh.program ssh-keygen
git -C "$repo" config user.signingKey "$repo/record/signing-key"
git -C "$repo" config gpg.ssh.allowedSignersFile "$repo/record/allowed-signers"
git -C "$repo" commit --amend --no-edit -S -q
# A signature-only amendment invalidates the old final tag.
if "$repo/scripts/release.sh" final > "$repo/record/stale-tag.log" 2>&1; then exit 1; fi
test -f "$repo/build/preserve"; test ! -e "$TEST_PHASE_LOG"
git -C "$repo" tag -d v1.2.3 >/dev/null
git -C "$repo" tag v1.2.3
: > "$repo/record/allowed-signers"
if "$repo/scripts/release.sh" final > "$repo/record/untrusted.log" 2>&1; then exit 1; fi
grep -q 'verified signature' "$repo/record/untrusted.log"
test -f "$repo/build/preserve"; test ! -e "$TEST_PHASE_LOG"
printf 'test@example.invalid %s\n' "$(cat "$repo/record/signing-key.pub")" > "$repo/record/allowed-signers"
if LIBMDF_DIST_DIR="$work/redirected" "$repo/scripts/release.sh" final > "$repo/record/dist.log" 2>&1; then exit 1; fi
if LIBMDF_PACKAGE_WORK_DIR="$work/redirected" "$repo/scripts/release.sh" final > "$repo/record/staging.log" 2>&1; then exit 1; fi
grep -q 'owned dist directory' "$repo/record/dist.log"
grep -q 'owned build/package' "$repo/record/staging.log"
if LIBMDF_TARGETS=x86_64-linux-gnu "$repo/scripts/release.sh" final > "$repo/record/partial.log" 2>&1; then exit 1; fi
grep -q 'complete declared target matrix' "$repo/record/partial.log"
test ! -e "$work/redirected"; test -f "$repo/build/preserve"; test ! -e "$TEST_PHASE_LOG"
# A borrowed top-level workspace is refused before any owned directory is removed.
mkdir -p "$work/borrowed"
touch "$work/borrowed/preserve"
mv "$repo/.cache" "$repo/record/local-cache"
ln -s "$work/borrowed" "$repo/.cache"
if "$repo/scripts/release.sh" final > "$repo/record/borrowed.log" 2>&1; then exit 1; fi
grep -q 'Refusing to clean a borrowed directory' "$repo/record/borrowed.log"
test -f "$work/borrowed/preserve"; test -f "$repo/build/preserve"; test ! -e "$TEST_PHASE_LOG"
rm "$repo/.cache"
mv "$repo/record/local-cache" "$repo/.cache"
"$repo/scripts/release.sh" final > "$repo/record/final.log" 2>&1
test ! -e "$repo/build/preserve"; test ! -e "$repo/dist/preserve"
test ! -e "$repo/build/.hidden"; test ! -e "$repo/dist/.hidden"; test ! -e "$repo/.cache"
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
: > "$TEST_PHASE_LOG"
if TEST_FAIL_PHASE=package "$repo/scripts/release.sh" final > "$repo/record/final-failed.log" 2>&1; then exit 1; fi
printf '%s\n' format-check test-hardening package-source-smoke package > "$repo/record/expected"
cmp "$repo/record/expected" "$TEST_PHASE_LOG"
