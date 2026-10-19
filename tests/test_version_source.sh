#!/usr/bin/env bash
set -euo pipefail
unset LIBMDF_RELEASE_VERSION LIBMDF_TARGETS LIBMDF_DIST_DIR LIBMDF_PACKAGE_WORK_DIR
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-version-source"
rm -rf -- "$work"
mkdir -p "$work/repo/scripts" "$work/extract"
repo="$work/repo"
cp "$root/scripts/"{version.sh,package_source.sh,archive.sh,lifecycle_version_contract.sh,verify_source_archive.sh,require_build_path.sh} "$repo/scripts/"
git -C "$repo" init -q -b trunk
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
"$repo/scripts/verify_source_archive.sh" > "$work/verify-source.log" 2>&1
LIBMDF_DIST_DIR="$repo/dist" "$work/extract/libmdf-2.3.4/scripts/verify_source_archive.sh" > "$work/verify-extracted.log" 2>&1
for defect in content missing extra mode version manifest; do
  rm -rf "$work/corrupt"
  mkdir -p "$work/corrupt"
  tar -C "$work/corrupt" -xzf "$work/first.tar.gz"
  source="$work/corrupt/libmdf-2.3.4"
  case "$defect" in
    content) printf '\n# changed input\n' >> "$source/scripts/archive.sh";;
    missing) rm "$source/scripts/archive.sh";;
    extra) printf 'unexpected\n' > "$source/extra.txt";;
    mode) chmod -x "$source/scripts/archive.sh";;
    version) printf '9.9.9\n' > "$source/VERSION";;
    manifest)
      sed '/scripts\/archive.sh/d' "$source/RELEASE_MANIFEST" > "$work/manifest-edited"
      mv "$work/manifest-edited" "$source/RELEASE_MANIFEST";;
  esac
  "$root/scripts/archive.sh" "$work/corrupt" libmdf-2.3.4 "$repo/dist/libmdf-2.3.4.tar.gz"
  if "$repo/scripts/verify_source_archive.sh" > "$work/verify-$defect.log" 2>&1; then
    printf 'Source verification accepted %s defect\n' "$defect" >&2; exit 1;
  fi
done
cp "$work/first.tar.gz" "$repo/dist/libmdf-2.3.4.tar.gz"
"$repo/scripts/verify_source_archive.sh" >> "$work/verify-source.log" 2>&1
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
