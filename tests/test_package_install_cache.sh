#!/usr/bin/env bash
# Exercise the actual package entrypoint with an isolated native Release producer.
set -euo pipefail
unset LIBMDF_RELEASE_VERSION LIBMDF_TARGETS LIBMDF_DIST_DIR LIBMDF_PACKAGE_WORK_DIR
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-package-native"
source="$work/source"
mkdir -p "$source/tests"
cp "$root/"{CMakeLists.txt,CMakePresets.json,LICENSE,README.md} "$source/"
cp -R "$root/"{src,include,cmake,scripts,assets} "$source/"
cp -R "$root/"{tests,benchmarks} "$source/"
version=$("$source/scripts/version.sh")
build="$source/build/x86_64-linux-gnu-release"
export LIBMDF_TARGETS=x86_64-linux-gnu LIBMDF_DIST_DIR="$work/dist" LIBMDF_PACKAGE_WORK_DIR="$source/build/staging"
mkdir -p "$work/dist"
printf 'keep sibling\n' > "$work/dist/unrelated-artifact.txt"
printf 'keep stale candidate\n' > "$work/dist/libmdf-9.9.9-armhf-linux-gnu.tar.gz"
"$source/scripts/package.sh" > "$work/package.log" 2>&1
grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$source" "$build/CMakeCache.txt"
for binary in libmdf.a "libmdf.so.$version" cmdf; do cp -p "$build/$binary" "$work/warm-$binary"; done
sdk="libmdf-$version-x86_64-linux-gnu"
cp "$work/dist/$sdk.tar.gz" "$work/first.tar.gz"
find "$build/package" -exec touch -t 203001010101 {} +
(umask 077; "$root/scripts/archive.sh" "$build/package" "$sdk" "$work/changed-timestamps.tar.gz")
cmp "$work/first.tar.gz" "$work/changed-timestamps.tar.gz"
(umask 077; "$source/scripts/package.sh" >> "$work/package.log" 2>&1)
for binary in libmdf.a "libmdf.so.$version" cmdf; do
  test ! "$build/$binary" -nt "$work/warm-$binary" || { printf 'Warm packaging rebuilt %s\n' "$binary" >&2; exit 1; }
done
cmp "$work/first.tar.gz" "$work/dist/$sdk.tar.gz"
grep -Fxq 'keep sibling' "$work/dist/unrelated-artifact.txt"
grep -Fxq 'keep stale candidate' "$work/dist/libmdf-9.9.9-armhf-linux-gnu.tar.gz"
test ! -e "$work/dist/libmdf-$version-CHECKSUMS"
"$source/scripts/package-verify.sh" binary > "$work/verify.log" 2>&1
cli="cmdf-$version-x86_64-linux-gnu"
cp "$work/dist/$cli.tar.gz" "$work/valid-cli.tar.gz"
for notice in LICENSE OFL.txt; do
  rm -rf "$work/notice-test"
  mkdir -p "$work/notice-test"
  tar -C "$work/notice-test" -xzf "$work/valid-cli.tar.gz"
  rm "$work/notice-test/$cli/share/doc/cmdf/$notice"
  "$root/scripts/archive.sh" "$work/notice-test" "$cli" "$work/dist/$cli.tar.gz"
  "$source/scripts/package_checksums.sh" binary > "$work/notice-$notice-checksums.log"
  if "$source/scripts/package-verify.sh" binary > "$work/notice-$notice.log" 2>&1; then
    printf 'CLI verification accepted missing %s\n' "$notice" >&2; exit 1;
  fi
  grep -Fq "Missing mandatory CLI payload: share/doc/cmdf/$notice" "$work/notice-$notice.log"
done
cp "$work/valid-cli.tar.gz" "$work/dist/$cli.tar.gz"
"$source/scripts/package_checksums.sh" binary > "$work/restored-checksums.log"
if LIBMDF_TARGETS=unsupported "$source/scripts/package.sh" > "$work/invalid.log" 2>&1; then exit 1; fi
test -f "$work/dist/unrelated-artifact.txt"
