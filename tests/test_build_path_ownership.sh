#!/usr/bin/env bash
# Invalid selected paths must preserve non-staging data and start no producer.
set -euo pipefail
unset LIBMDF_RELEASE_VERSION LIBMDF_DIST_DIR
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work="$root/build/test-build-path-ownership"
rm -rf -- "$work"
source="$work/source"
mkdir -p "$source/scripts" "$source/lua" "$source/build" "$source/sentinel" "$source/dist" "$work/borrowed/stage"
cp "$root/scripts/"{require_build_path.sh,stage_lua_rock_sources.sh,package.sh,package_checksums.sh,release_targets.sh,release_artifacts.sh,version.sh} "$source/scripts/"
for file in LICENSE README.md libmdf-scm-1.rockspec.in lua/libmdf.lua lua/libmdf_core.c; do printf 'fixture\n' > "$source/$file"; done
printf 'preserve\n' > "$source/sentinel/marker"
printf 'preserve\n' > "$work/borrowed/stage/marker"
printf 'preserve\n' > "$source/build/marker"
ln -s "$work/borrowed" "$source/build/borrowed"
for path in "$source/build/../sentinel" "$source/build/" "$source/build/borrowed/stage"; do
  if bash "$source/scripts/stage_lua_rock_sources.sh" 0.0.0 "$path" > "$work/stage.log" 2>&1; then
    printf 'Unsafe Lua staging path accepted: %s\n' "$path" >&2; exit 1;
  fi
  test -f "$source/sentinel/marker"; test -f "$work/borrowed/stage/marker"; test -f "$source/build/marker"
done
bash "$source/scripts/stage_lua_rock_sources.sh" 0.0.0 "$source/build/normal staging/stage" > "$work/valid-stage.log"
cmp "$source/LICENSE" "$source/build/normal staging/stage/libmdf-lua-0.0.0/LICENSE"
cat > "$source/scripts/build.sh" <<'SCRIPT'
#!/usr/bin/env bash
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
printf 'producer\n' >> "$root/producer.log"
SCRIPT
chmod +x "$source/scripts/build.sh"
export LIBMDF_TARGETS=x86_64-linux-gnu
for product in libmdf cmdf; do printf 'fixture\n' > "$source/dist/$product-0.0.0-x86_64-linux-gnu.tar.gz"; done
for path in "$source/build/../sentinel" "$source/build/" "$source/build/borrowed/stage"; do
  if LIBMDF_PACKAGE_WORK_DIR="$path" "$source/scripts/package.sh" > "$work/package.log" 2>&1; then
    printf 'Unsafe binary inventory path accepted: %s\n' "$path" >&2; exit 1;
  fi
  test ! -f "$source/producer.log"
  if LIBMDF_PACKAGE_WORK_DIR="$path" "$source/scripts/package_checksums.sh" binary > "$work/checksums.log" 2>&1; then
    printf 'Unsafe direct checksum path accepted: %s\n' "$path" >&2; exit 1;
  fi
  test ! -f "$source/sentinel/binary-CHECKSUMS"; test ! -f "$work/borrowed/stage/binary-CHECKSUMS"
done
LIBMDF_PACKAGE_WORK_DIR="$source/build/normal inventory" "$source/scripts/package.sh" > "$work/valid-package.log"
(cd "$source/dist" && sha256sum -c "$source/build/normal inventory/binary-CHECKSUMS")
