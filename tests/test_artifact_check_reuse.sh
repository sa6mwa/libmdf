#!/usr/bin/env bash
# The real artifact-check graph must rerun changed executable export assertions.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-artifact-check-reuse"
rm -rf -- "$work"
mkdir -p "$work/source/scripts" "$work/source/tests" "$work/dist" "$work/inventory"
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
bash "$root/tests/test_shared_exports.sh"
SCRIPT
printf '#!/bin/sh\nexit 0\n' > "$work/source/tests/test_shared_exports.sh"
chmod +x "$work/source/scripts/"*.sh
touch "$work/dist/libmdf-0.0.0-x86_64-linux-gnu.tar.gz" "$work/dist/cmdf-0.0.0-x86_64-linux-gnu.tar.gz" "$work/inventory/binary-CHECKSUMS"
cmake -S "$root/cmake/package-checks" -B "$work/build" -G Ninja \
  -DLIBMDF_SOURCE_DIR="$work/source" -DLIBMDF_DIST_DIR="$work/dist" \
  -DLIBMDF_PACKAGE_WORK_DIR="$work/inventory" -DLIBMDF_ARTIFACT_SCOPE=binary > "$work/configure.log"
cmake --build "$work/build" --target verify > "$work/first.log"
cmake --build "$work/build" --target verify > "$work/warm.log"
[[ $(wc -l < "$work/dist/runs") == 1 ]]
sleep 1
printf '#!/bin/sh\nexit 1\n' > "$work/source/tests/test_shared_exports.sh"
if cmake --build "$work/build" --target verify > "$work/changed.log" 2>&1; then
  printf 'Updated export assertions did not invalidate artifact verification\n' >&2; exit 1;
fi
[[ $(wc -l < "$work/dist/runs") == 2 ]]
test ! -f "$work/build/artifacts.pass"
