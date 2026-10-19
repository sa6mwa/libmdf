#!/usr/bin/env bash
# Exercise the actual package entrypoint with an isolated native Release producer.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-package-native"
mkdir -p "$work"
version=$("$root/scripts/version.sh")
export LIBMDF_TARGETS=x86_64-linux-gnu LIBMDF_DIST_DIR="$work/dist" LIBMDF_PACKAGE_WORK_DIR="$work/staging"
export LIBMDF_NATIVE_RELEASE_BUILD="$work/build"
cmake --preset x86_64-linux-gnu-release -S "$root" -B "$work/build" -DLIBMDF_DIST_DIR="$work/dist" > "$work/configure.log" 2>&1
cmake --build "$work/build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" > "$work/build.log" 2>&1
mkdir -p "$work/dist"
printf 'keep sibling\n' > "$work/dist/unrelated-artifact.txt"
printf 'keep stale candidate\n' > "$work/dist/libmdf-9.9.9-armhf-linux-gnu.tar.gz"
"$root/scripts/package.sh" > "$work/package.log" 2>&1
sdk="libmdf-$version-x86_64-linux-gnu"
cp "$work/dist/$sdk.tar.gz" "$work/first.tar.gz"
find "$work/build/package" -exec touch -t 203001010101 {} +
(umask 077; "$root/scripts/archive.sh" "$work/build/package" "$sdk" "$work/changed-timestamps.tar.gz")
cmp "$work/first.tar.gz" "$work/changed-timestamps.tar.gz"
(umask 077; "$root/scripts/package.sh" >> "$work/package.log" 2>&1)
cmp "$work/first.tar.gz" "$work/dist/$sdk.tar.gz"
grep -Fxq 'keep sibling' "$work/dist/unrelated-artifact.txt"
grep -Fxq 'keep stale candidate' "$work/dist/libmdf-9.9.9-armhf-linux-gnu.tar.gz"
test ! -e "$work/dist/libmdf-$version-CHECKSUMS"
"$root/scripts/package-verify.sh" binary > "$work/verify.log" 2>&1
cli="cmdf-$version-x86_64-linux-gnu"
cp "$work/dist/$cli.tar.gz" "$work/valid-cli.tar.gz"
for notice in LICENSE OFL.txt; do
  rm -rf "$work/notice-test"
  mkdir -p "$work/notice-test"
  tar -C "$work/notice-test" -xzf "$work/valid-cli.tar.gz"
  rm "$work/notice-test/$cli/share/doc/cmdf/$notice"
  "$root/scripts/archive.sh" "$work/notice-test" "$cli" "$work/dist/$cli.tar.gz"
  "$root/scripts/package_checksums.sh" binary > "$work/notice-$notice-checksums.log"
  if "$root/scripts/package-verify.sh" binary > "$work/notice-$notice.log" 2>&1; then
    printf 'CLI verification accepted missing %s\n' "$notice" >&2; exit 1;
  fi
  grep -Fq "Missing mandatory CLI payload: share/doc/cmdf/$notice" "$work/notice-$notice.log"
done
cp "$work/valid-cli.tar.gz" "$work/dist/$cli.tar.gz"
"$root/scripts/package_checksums.sh" binary > "$work/restored-checksums.log"
if LIBMDF_TARGETS=unsupported "$root/scripts/package.sh" > "$work/invalid.log" 2>&1; then exit 1; fi
test -f "$work/dist/unrelated-artifact.txt"
