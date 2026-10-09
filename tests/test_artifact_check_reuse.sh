#!/usr/bin/env bash
# The real artifact-check graph must rerun changed executable export assertions.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-artifact-check-reuse"
rm -rf -- "$work"
mkdir -p "$work/source/scripts" "$work/source/tests" "$work/source/cmake/toolchains" "$work/dist" "$work/inventory"
cat > "$work/source/scripts/version.sh" <<'SCRIPT'
#!/bin/sh
printf '0.0.0\n'
SCRIPT
cat > "$work/source/scripts/release_artifacts.sh" <<'SCRIPT'
#!/bin/sh
printf '%s\n' libmdf-0.0.0-x86_64-linux-gnu.tar.gz cmdf-0.0.0-x86_64-linux-gnu.tar.gz
SCRIPT
cat > "$work/source/scripts/package-verify.sh" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
printf 'run\n' >> "$LIBMDF_DIST_DIR/runs"
grep -Fxq 'set(LIBMDF_ABI_VERSION 5)' "$root/CMakeLists.txt"
grep -Fxq '# toolchain-valid' "$root/cmake/toolchains/included.cmake"
if [[ $1 == complete ]]; then test ! -f "$LIBMDF_DIST_DIR/libmdf-9.9.9-x86_64-linux-gnu.tar.gz"; fi
bash "$root/tests/test_shared_exports.sh"
SCRIPT
printf '#!/bin/sh\nexit 0\n' > "$work/source/tests/test_shared_exports.sh"
printf 'set(LIBMDF_ABI_VERSION 5)\n' > "$work/source/CMakeLists.txt"
printf '# toolchain-valid\n' > "$work/source/cmake/toolchains/included.cmake"
chmod +x "$work/source/scripts/"*.sh
touch "$work/dist/libmdf-0.0.0-x86_64-linux-gnu.tar.gz" "$work/dist/cmdf-0.0.0-x86_64-linux-gnu.tar.gz" "$work/inventory/binary-CHECKSUMS"
cmake -S "$root/cmake/package-checks" -B "$work/build" -G Ninja \
  -DLIBMDF_SOURCE_DIR="$work/source" -DLIBMDF_DIST_DIR="$work/dist" \
  -DLIBMDF_PACKAGE_WORK_DIR="$work/inventory" -DLIBMDF_ARTIFACT_SCOPE=binary > "$work/configure.log"
cmake --build "$work/build" --target verify > "$work/first.log"
cmake --build "$work/build" --target verify > "$work/warm.log"
[[ $(wc -l < "$work/dist/runs") == 1 ]]
sleep 1
printf 'set(LIBMDF_ABI_VERSION 6)\n' > "$work/source/CMakeLists.txt"
if cmake --build "$work/build" --target verify > "$work/abi.log" 2>&1; then
  printf 'Updated ABI expectations left a stale artifact success record\n' >&2; exit 1;
fi
test ! -f "$work/build/artifacts.pass"
printf 'set(LIBMDF_ABI_VERSION 5)\n' > "$work/source/CMakeLists.txt"
cmake --build "$work/build" --target verify > "$work/abi-restored.log"
sleep 1
printf '#!/bin/sh\nexit 1\n' > "$work/source/tests/test_shared_exports.sh"
if cmake --build "$work/build" --target verify > "$work/changed.log" 2>&1; then
  printf 'Updated export assertions did not invalidate artifact verification\n' >&2; exit 1;
fi
[[ $(wc -l < "$work/dist/runs") == 4 ]]
test ! -f "$work/build/artifacts.pass"
printf '#!/bin/sh\nexit 0\n' > "$work/source/tests/test_shared_exports.sh"
cmake --build "$work/build" --target verify > "$work/toolchain-before.log"
printf '# invalid toolchain\n' > "$work/source/cmake/toolchains/included.cmake"
if cmake --build "$work/build" --target verify > "$work/toolchain-changed.log" 2>&1; then
  printf 'Updated consumer toolchain left a stale artifact success record\n' >&2; exit 1;
fi
test ! -f "$work/build/artifacts.pass"
printf '# toolchain-valid\n' > "$work/source/cmake/toolchains/included.cmake"
mkdir -p "$work/source/build/debug/lua-runtime/bin"
touch "$work/source/build/debug/lua-runtime/bin/lua" "$work/dist/libmdf-0.0.0-CHECKSUMS"
cmake -S "$root/cmake/package-checks" -B "$work/complete" -G Ninja \
  -DLIBMDF_SOURCE_DIR="$work/source" -DLIBMDF_DIST_DIR="$work/dist" \
  -DLIBMDF_ARTIFACT_SCOPE=complete > "$work/complete-configure.log"
cmake --build "$work/complete" --target verify > "$work/complete-first.log"
cp -p "$work/dist/libmdf-0.0.0-x86_64-linux-gnu.tar.gz" "$work/dist/libmdf-9.9.9-x86_64-linux-gnu.tar.gz"
if cmake --build "$work/complete" --target verify > "$work/membership.log" 2>&1; then
  printf 'Added release-looking filename left a stale complete success record\n' >&2; exit 1;
fi
test ! -f "$work/complete/artifacts.pass"
