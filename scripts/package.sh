#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sh "$ROOT/scripts/version.sh")
TARGETS=${LIBMDF_TARGETS:-"x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"}
mkdir -p "$ROOT/dist"
rm -f \
  "$ROOT"/dist/libmdf-"$VERSION"-*.tar.gz \
  "$ROOT"/dist/cmdf-"$VERSION"-*.tar.gz \
  "$ROOT"/dist/libmdf-lua-"$VERSION".tar.gz \
  "$ROOT"/dist/libmdf-"$VERSION"-1.rockspec \
  "$ROOT"/dist/libmdf-"$VERSION"-1.src.rock \
  "$ROOT/dist/libmdf-$VERSION-CHECKSUMS"
find "$ROOT/dist" -maxdepth 1 -type f \( \
  -name 'libmdf-*.tar.gz' -o \
  -name 'cmdf-*.tar.gz' -o \
  -name 'libmdf-lua-*.tar.gz' -o \
  -name 'libmdf-*-1.rockspec' -o \
  -name 'libmdf-*-1.src.rock' -o \
  -name 'libmdf-*-CHECKSUMS' \
\) ! \( \
  -name "libmdf-$VERSION.tar.gz" \
\) -exec rm -f {} +

strip_tool_for_preset() {
  preset=$1
  target=${preset%-release}
  "$ROOT/scripts/discover_target_tools.sh" --root "$ROOT" --target "$target" --preset "$preset" strip 2>/dev/null || true
}

strip_release_file() {
  strip_tool=$1
  mode=$2
  file=$3
  if [ -z "$strip_tool" ] || [ ! -x "$strip_tool" ] || [ ! -f "$file" ]; then
    return 0
  fi
  if [ "$mode" = "binary" ]; then
    "$strip_tool" -s "$file" 2>/dev/null || "$strip_tool" -S "$file" 2>/dev/null || true
  else
    "$strip_tool" -S "$file" 2>/dev/null || true
  fi
}

strip_release_tree() {
  strip_tool=$1
  root=$2
  if [ -z "$strip_tool" ] || [ ! -x "$strip_tool" ] || [ ! -d "$root" ]; then
    return 0
  fi
  find "$root" -type f \( -name '*.a' -o -name '*.so' -o -name '*.so.*' -o -name '*.dylib' -o -name '*.dylib.*' \) |
  while IFS= read -r file; do
    strip_release_file "$strip_tool" library "$file"
  done
}

for target in $TARGETS; do
  preset="$target-release"
  lib_root="$ROOT/build/install/$target"
  lib_name="libmdf-$VERSION-$target"
  install="$lib_root/$lib_name"
  cmdf_root="$ROOT/build/cmdf-package/$target"
  cmdf_name="cmdf-$VERSION-$target"
  cmdf_install="$cmdf_root/$cmdf_name"
  rm -rf "$lib_root"
  rm -f "$ROOT/build/$preset/cmdf"
  "$ROOT/scripts/configure_preset.sh" "$preset" package -DCMAKE_INSTALL_PREFIX="$install"
  "$ROOT/scripts/with_target_env.sh" "$preset" cmake --build --preset "$preset"
  test -x "$ROOT/build/$preset/cmdf"
  "$ROOT/scripts/with_target_env.sh" "$preset" cmake --install "$ROOT/build/$preset"
  case "$target" in
    *-apple-darwin)
      strip_tool=
      ;;
    *)
      strip_tool=$(strip_tool_for_preset "$preset")
      ;;
  esac
  strip_release_tree "$strip_tool" "$install"
  tar -C "$lib_root" -czf "$ROOT/dist/$lib_name.tar.gz" "$lib_name"
  rm -rf "$cmdf_root"
  mkdir -p "$cmdf_install/bin" "$cmdf_install/share/doc/cmdf"
  cp "$ROOT/build/$preset/cmdf" "$cmdf_install/bin/cmdf"
  strip_release_file "$strip_tool" binary "$cmdf_install/bin/cmdf"
  "$ROOT/scripts/generate_cmdf_lua.sh" "$cmdf_install/bin/cmdf.lua"
  cp "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/src/html_embedded/OFL.txt" "$cmdf_install/share/doc/cmdf/"
  tar -C "$cmdf_root" -czf "$ROOT/dist/$cmdf_name.tar.gz" "$cmdf_name"
done
"$ROOT/scripts/package_checksums.sh"
