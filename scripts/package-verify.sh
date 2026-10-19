#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
version=$("$root/scripts/version.sh")
dist=${LIBMDF_DIST_DIR:-$root/dist}
scope=${1:-binary}
expected=$("$root/scripts/release_artifacts.sh" "$scope")
case "$scope" in
  binary) manifest="${LIBMDF_PACKAGE_WORK_DIR:-$root/build/package}/binary-CHECKSUMS";;
  complete) manifest="$dist/libmdf-$version-CHECKSUMS";;
esac
test -f "$manifest"
work="$root/build/package-verify/$scope"
rm -rf -- "$work"
mkdir -p "$work"
printf '%s\n' "$expected" | sort > "$work/expected"
awk '{print $2}' "$manifest" | sort > "$work/actual"
cmp "$work/expected" "$work/actual"
(cd "$dist" && sha256sum -c "$manifest")
abi=$(sed -n 's/^set(LIBMDF_ABI_VERSION \([0-9]*\))$/\1/p' "$root/CMakeLists.txt")
mapfile -t artifacts <<< "$expected"
for artifact in "${artifacts[@]}"; do
  case "$artifact" in
    libmdf-"$version"-*-linux-*.tar.gz|libmdf-"$version"-*-apple-darwin.tar.gz) kind=sdk; target=${artifact#libmdf-$version-};;
    cmdf-"$version"-*.tar.gz) kind=cli; target=${artifact#cmdf-$version-};;
    *) continue;;
  esac
  target=${target%.tar.gz}
  dir="$work/$artifact"
  mkdir -p "$dir"
  tar -C "$dir" -xzf "$dist/$artifact"
  name=${artifact%.tar.gz}
  package="$dir/$name"
  test -d "$package"
  [[ $(find "$dir" -mindepth 1 -maxdepth 1 | wc -l) == 1 ]]
  while IFS= read -r file; do
    case "$kind:$file" in
      sdk:include/libmdf.h|sdk:include/libmdf/mdf.h|sdk:lib/libmdf.a|sdk:lib/libmdf.so|sdk:lib/libmdf.so.*|sdk:lib/libmdf*.dylib|sdk:lib/cmake/libmdf/libmdfConfig.cmake|sdk:lib/cmake/libmdf/libmdfConfigVersion.cmake|sdk:lib/cmake/libmdf/libmdfTargets.cmake|sdk:lib/cmake/libmdf/libmdfTargets-release.cmake|sdk:lib/pkgconfig/libmdf.pc|sdk:share/libmdf/package-metadata.txt|sdk:share/doc/libmdf/LICENSE|sdk:share/doc/libmdf/README.md|sdk:share/doc/libmdf/OFL.txt|cli:bin/cmdf|cli:bin/cmdf.lua|cli:share/doc/cmdf/LICENSE|cli:share/doc/cmdf/README.md|cli:share/doc/cmdf/OFL.txt) ;;
      *) printf 'Unexpected %s payload: %s\n' "$kind" "$file" >&2; exit 1;;
    esac
  done < <(cd "$package" && find . \( -type f -o -type l \) | sed 's#^./##')
  if [[ $kind == sdk ]]; then
    for required in include/libmdf.h include/libmdf/mdf.h lib/libmdf.a lib/pkgconfig/libmdf.pc lib/cmake/libmdf/libmdfConfig.cmake lib/cmake/libmdf/libmdfConfigVersion.cmake share/libmdf/package-metadata.txt share/doc/libmdf/LICENSE share/doc/libmdf/OFL.txt; do test -f "$package/$required"; done
    grep -Fxq "#define LIBMDF_VERSION \"$version\"" "$package/include/libmdf/mdf.h"
    grep -Fxq "Version: $version" "$package/lib/pkgconfig/libmdf.pc"
    grep -Fxq "target_id=$target" "$package/share/libmdf/package-metadata.txt"
    grep -Fxq "version=$version" "$package/share/libmdf/package-metadata.txt"
    binary="$package/lib/libmdf.so"
    [[ $target != *-apple-darwin ]] || binary="$package/lib/libmdf.dylib"
    test -f "$binary"
  else
    binary="$package/bin/cmdf"
    test -x "$binary"; test -x "$package/bin/cmdf.lua"
    for required in LICENSE README.md OFL.txt; do
      test -f "$package/share/doc/cmdf/$required" || {
        printf 'Missing mandatory CLI payload: share/doc/cmdf/%s\n' "$required" >&2; exit 1;
      }
    done
    grep -a -q 'JetBrains Mono' "$binary"
    grep -a -Fq 'require("libmdf")' "$package/bin/cmdf.lua"
  fi
  if [[ $target == *-linux-* ]]; then
    readelf=$("$root/scripts/discover_target_tools.sh" --target "$target" --preset "$target-release" readelf)
    "$readelf" -h "$binary" > "$dir/header"
    case "$target" in x86_64-*) machine='Advanced Micro Devices X86-64';; aarch64-*) machine=AArch64;; armhf-*) machine=ARM;; esac
    grep -Eq "Machine:[[:space:]]+$machine$" "$dir/header"
    "$readelf" -d "$binary" > "$dir/dynamic"
    if [[ $kind == cli ]]; then
      "$readelf" -l "$binary" > "$dir/program"
      if grep -Eq '\(NEEDED\)|Requesting program interpreter' "$dir/dynamic" "$dir/program"; then printf 'Linux CLI must be static: %s\n' "$artifact" >&2; exit 1; fi
    else
      grep -Fq "[libmdf.so.$abi]" "$dir/dynamic"
      nm=$("$root/scripts/discover_target_tools.sh" --target "$target" --preset "$target-release" nm)
      "$root/tests/test_shared_exports.sh" "$nm" "$binary"
    fi
  else
    otool=$("$root/scripts/discover_target_tools.sh" --target "$target" --preset "$target-release" otool)
    "$otool" -hv "$binary" > "$dir/header"
    grep -Eq 'ARM64|arm64' "$dir/header"
    "$otool" -L "$binary" > "$dir/dynamic"
    if [[ $kind == sdk ]]; then
      grep -Fq "@rpath/libmdf.$abi.dylib" "$dir/dynamic"
      nm=$("$root/scripts/discover_target_tools.sh" --target "$target" --preset "$target-release" nm)
      "$root/tests/test_shared_exports.sh" "$nm" "$binary" darwin
    elif sed '1d' "$dir/dynamic" | grep -q libmdf; then printf 'Darwin CLI must embed libmdf: %s\n' "$artifact" >&2; exit 1; fi
  fi
  if [[ $kind == sdk ]]; then
    "$root/scripts/verify_package_consumers.sh" "$package" "$target" "$dir/consumers"
  else
    case "$(uname -s):$(uname -m):$target" in
      Linux:x86_64:x86_64-linux-*|Darwin:arm64:arm64-apple-darwin)
        "$binary" --version | grep -Fq "$version"
        printf '# Package\n' | "$binary" --ascii | grep -Fxq '# Package';;
    esac
  fi
done
if [[ $scope == complete ]]; then
  "$root/scripts/verify_source_archive.sh"
  "$root/scripts/verify_lua_artifacts.sh"
fi
LIBMDF_MANIFEST="$manifest" LIBMDF_PRIVACY_SCOPE="$scope" "$root/scripts/verify_release_privacy.sh"
