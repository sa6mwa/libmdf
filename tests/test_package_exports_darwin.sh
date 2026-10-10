#!/usr/bin/env bash
# Exercise the actual extracted Darwin SDK export gate with fixture target tools.
set -euo pipefail
unset LIBMDF_RELEASE_VERSION LIBMDF_DIST_DIR LIBMDF_PACKAGE_WORK_DIR LIBMDF_TARGETS
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-package-exports-darwin"
rm -rf -- "$work"
source="$work/source"
mkdir -p "$source/scripts" "$source/tests" "$source/dist" "$source/build/package" "$work/tools"
cp "$root/scripts/"{package-verify.sh,discover_target_tools.sh,version.sh} "$source/scripts/"
cp "$root/tests/test_shared_exports.sh" "$source/tests/"
printf 'set(LIBMDF_ABI_VERSION 5)\n' > "$source/CMakeLists.txt"
name=libmdf-0.0.0-arm64-apple-darwin
printf '#!/bin/sh\nprintf "%s\\n"\n' "$name.tar.gz" > "$source/scripts/release_artifacts.sh"
for script in verify_package_consumers verify_release_privacy; do printf '#!/bin/sh\nexit 0\n' > "$source/scripts/$script.sh"; done
chmod +x "$source/scripts/"*.sh "$source/tests/test_shared_exports.sh"
package="$work/stage/$name"
mkdir -p "$package/include/libmdf" "$package/lib/pkgconfig" "$package/lib/cmake/libmdf" "$package/share/libmdf" "$package/share/doc/libmdf"
touch "$package/include/libmdf.h" "$package/lib/libmdf.a" "$package/lib/libmdf.dylib"
touch "$package/lib/cmake/libmdf/"{libmdfConfig.cmake,libmdfConfigVersion.cmake}
touch "$package/share/doc/libmdf/"{LICENSE,OFL.txt}
printf '#define LIBMDF_VERSION "0.0.0"\n' > "$package/include/libmdf/mdf.h"
printf 'Version: 0.0.0\n' > "$package/lib/pkgconfig/libmdf.pc"
printf 'target_id=arm64-apple-darwin\nversion=0.0.0\n' > "$package/share/libmdf/package-metadata.txt"
tar -C "$work/stage" -czf "$source/dist/$name.tar.gz" "$name"
(cd "$source/dist" && sha256sum "$name.tar.gz") > "$source/build/package/binary-CHECKSUMS"
cat > "$work/tools/otool" <<'TOOL'
#!/bin/sh
case "$1" in -hv) printf 'ARM64\n';; -L) printf '%s:\n@rpath/libmdf.5.dylib\n' "$2";; *) exit 2;; esac
TOOL
cat > "$work/tools/nm" <<'TOOL'
#!/bin/sh
test "$1" = -g && test "$2" = -U && test "$3" = -j || exit 2
test "${TEST_NM_FAIL:-0}" = 0 || exit 17
cat "$TEST_EXPORTS"
TOOL
chmod +x "$work/tools/"*
export LIBMDF_NM="$work/tools/nm" LIBMDF_OTOOL="$work/tools/otool" TEST_EXPORTS="$work/exports"
cp "$root/cmake/libmdf.exports.darwin" "$TEST_EXPORTS"
verify() { "$source/scripts/package-verify.sh" binary > "$work/$1.log" 2>&1; }
verify valid
sed -i '/^_mdf_feed$/d' "$TEST_EXPORTS"
if verify missing; then printf 'Missing Darwin public export accepted\n' >&2; exit 1; fi
grep -q 'export allowlist mismatch' "$work/missing.log"
cp "$root/cmake/libmdf.exports.darwin" "$TEST_EXPORTS"
printf '_private_helper\n' >> "$TEST_EXPORTS"
if verify extra; then printf 'Private Darwin export accepted\n' >&2; exit 1; fi
grep -q 'export allowlist mismatch' "$work/extra.log"
cp "$root/cmake/libmdf.exports.darwin" "$TEST_EXPORTS"
if TEST_NM_FAIL=1 verify tool-failed; then printf 'Failed Darwin nm accepted\n' >&2; exit 1; fi
