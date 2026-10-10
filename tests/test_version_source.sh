#!/usr/bin/env bash
set -euo pipefail
unset LIBMDF_RELEASE_VERSION LIBMDF_TARGETS LIBMDF_DIST_DIR LIBMDF_NATIVE_RELEASE_BUILD LIBMDF_PACKAGE_WORK_DIR
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-version-source"
rm -rf -- "$work"
mkdir -p "$work/repo/scripts" "$work/extract"
repo="$work/repo"
cp "$root/scripts/"{version.sh,package_source.sh,archive.sh,lifecycle_version_contract.sh} "$repo/scripts/"
git -C "$repo" init -q
git -C "$repo" config user.email test@example.invalid
git -C "$repo" config user.name 'libmdf test'
git -C "$repo" config core.hooksPath /dev/null
git -C "$repo" config commit.gpgSign false
git -C "$repo" config tag.gpgSign false
git -C "$repo" add scripts
git -C "$repo" commit -qm 'test(lifecycle): create source fixture'
[[ $("$repo/scripts/version.sh") == 0.0.0 ]]
printf '7.8.9\n' > "$repo/VERSION"
[[ $("$repo/scripts/version.sh") == 0.0.0 ]]
rm "$repo/VERSION"
[[ $(LIBMDF_RELEASE_VERSION=3.4.5 "$repo/scripts/version.sh") == 3.4.5 ]]
git -C "$repo" tag -a v9.9.9 -m annotated
[[ $("$repo/scripts/version.sh") == 0.0.0 ]]
if "$repo/scripts/lifecycle_version_contract.sh" > "$work/annotated.log" 2>&1; then exit 1; fi
git -C "$repo" tag v2.3.4
[[ $("$repo/scripts/version.sh") == 2.3.4 ]]
if "$repo/scripts/lifecycle_version_contract.sh" > "$work/unsigned.log" 2>&1; then exit 1; fi
grep -q 'verified signature' "$work/unsigned.log"
"$repo/scripts/package_source.sh" > "$work/package.log"
tar -C "$work/extract" -xzf "$repo/dist/libmdf-2.3.4.tar.gz"
# An enclosing repository must never own the extracted tree's source version.
[[ $("$work/extract/libmdf-2.3.4/scripts/version.sh") == 2.3.4 ]]
cp "$repo/dist/libmdf-2.3.4.tar.gz" "$work/first.tar.gz"
find "$repo/build/package-source" -exec touch -t 203001010101 {} +
"$repo/scripts/package_source.sh" >> "$work/package.log"
cmp "$work/first.tar.gz" "$repo/dist/libmdf-2.3.4.tar.gz"
git -C "$repo" tag v2.10.0
if "$repo/scripts/version.sh" > "$work/ambiguous.log" 2>&1; then printf 'Ambiguous tags accepted\n' >&2; exit 1; fi
grep -q 'ambiguous lightweight release tags' "$work/ambiguous.log"
git -C "$repo" tag -d v2.10.0 >/dev/null
if LIBMDF_RELEASE_VERSION=2.3.4 "$repo/scripts/lifecycle_version_contract.sh" > "$work/override.log" 2>&1; then exit 1; fi
if LIBMDF_RELEASE_VERSION=invalid "$repo/scripts/version.sh" > "$work/invalid.log" 2>&1; then exit 1; fi
git -C "$repo" symbolic-ref refs/tags/v8.8.8 refs/tags/v2.3.4
[[ $("$repo/scripts/version.sh") == 2.3.4 ]]
printf '\n# dirty\n' >> "$repo/scripts/version.sh"
touch "$repo/build/preserve.marker"
if "$repo/scripts/lifecycle_version_contract.sh" > "$work/dirty.log" 2>&1; then exit 1; fi
test -f "$repo/build/preserve.marker"
if "$repo/scripts/package_source.sh" > "$work/dirty-source.log" 2>&1; then exit 1; fi
