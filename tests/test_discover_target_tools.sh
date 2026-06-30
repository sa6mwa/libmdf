#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE="$ROOT/build/test-discover-target-tools"
HELPER="$ROOT/scripts/discover_target_tools.sh"

rm -rf "$BASE"
mkdir -p "$BASE/root/build/preset" "$BASE/bin" "$BASE/path"

make_tool() {
  path=$1
  mkdir -p "$(dirname "$path")"
  printf '#!/usr/bin/env sh\nexit 0\n' > "$path"
  chmod +x "$path"
}

expect_path() {
  want=$1
  shift
  got=$("$@" 2>/dev/null)
  if [ "$got" != "$want" ]; then
    printf '%s\n' "expected $want, got ${got:-unset}" >&2
    exit 1
  fi
}

make_tool "$BASE/explicit/otool"
LIBMDF_OTOOL="$BASE/explicit/otool" expect_path "$BASE/explicit/otool" \
  "$HELPER" --root "$BASE/root" --target arm64-apple-darwin otool

make_tool "$BASE/cache/arm64-apple-darwin25-otool"
cat > "$BASE/root/build/preset/CMakeCache.txt" <<EOF
CMAKE_OTOOL:FILEPATH=$BASE/cache/arm64-apple-darwin25-otool
EOF
expect_path "$BASE/cache/arm64-apple-darwin25-otool" \
  "$HELPER" --root "$BASE/root" --target arm64-apple-darwin --preset preset otool

make_tool "$BASE/sibling/arm64-apple-darwin25-clang"
make_tool "$BASE/sibling/arm64-apple-darwin25-strip"
cat > "$BASE/root/build/preset/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$BASE/sibling/arm64-apple-darwin25-clang
EOF
expect_path "$BASE/sibling/arm64-apple-darwin25-strip" \
  "$HELPER" --root "$BASE/root" --target arm64-apple-darwin --preset preset strip

rm -f "$BASE/sibling/arm64-apple-darwin25-strip"
make_tool "$BASE/sibling/strip"
expect_path "$BASE/sibling/strip" \
  "$HELPER" --root "$BASE/root" --target arm64-apple-darwin --preset preset strip

rm -f "$BASE/sibling/strip"
make_tool "$BASE/osxcross/bin/arm64-apple-darwin25-install_name_tool"
OSXCROSS_ROOT="$BASE/osxcross" expect_path "$BASE/osxcross/bin/arm64-apple-darwin25-install_name_tool" \
  "$HELPER" --root "$BASE/root" --target arm64-apple-darwin --preset preset install_name_tool

rm -f "$BASE/osxcross/bin/arm64-apple-darwin25-install_name_tool"
make_tool "$BASE/path/arm64-apple-darwin25-otool"
OSXCROSS_ROOT="$BASE/no-osxcross" PATH="$BASE/path:$PATH" expect_path "$BASE/path/arm64-apple-darwin25-otool" \
  "$HELPER" --root "$BASE/root" --target arm64-apple-darwin --preset preset otool

rm -f "$BASE/path/arm64-apple-darwin25-otool"
make_tool "$BASE/path/otool"
if OSXCROSS_ROOT="$BASE/no-osxcross" PATH="$BASE/path:$PATH" "$HELPER" --root "$BASE/root" --target arm64-apple-darwin --preset preset otool >/dev/null 2>&1; then
  printf '%s\n' 'unprefixed host otool was selected for cross-Darwin target' >&2
  exit 1
fi

make_tool "$BASE/path/readelf"
PATH="$BASE/path:$PATH" expect_path "$BASE/path/readelf" \
  "$HELPER" --root "$BASE/root" --target x86_64-linux-gnu readelf
