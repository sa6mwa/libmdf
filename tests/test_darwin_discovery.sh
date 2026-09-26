#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$ROOT/build/test-darwin-discovery"
rm -rf "$WORK"
mkdir -p "$WORK/osxcross/bin" "$WORK/cache/roots"
export OSXCROSS_ROOT="$WORK/osxcross" CPKT_TOOLCHAIN_CACHE="$WORK/cache"
unset CPKT_OSXCROSS_HOST
RESOLVER="$ROOT/scripts/cpkt-toolchains.sh"

tool() {
  printf '#!/bin/sh\nexit 0\n' > "$1"
  chmod +x "$1"
}

# An already provisioned host MIG fixture keeps discovery entirely offline.
MIG="$WORK/cache/roots/host-mig-puredarwin-88753c478c97b9a08bcdb66cecc68ba5881ff3af-x86_64-linux-gnu"
mkdir -p "$MIG/bin" "$MIG/libexec"
tool "$MIG/bin/mig"
tool "$MIG/bin/mig-upstream"
tool "$MIG/libexec/migcom"
touch "$MIG/TOOLCHAIN"

collection() {
  local prefix=$1 version=$2 name
  for name in clang clang++ ld ar ranlib strip nm otool install_name_tool; do
    tool "$OSXCROSS_ROOT/bin/$prefix-$name"
  done
  mkdir -p "$OSXCROSS_ROOT/SDK/MacOSX$version.sdk"
  printf '#!/bin/sh\nprintf '\''export OSXCROSS_SDK="%%s"\\n'\'' '\''%s'\''\n' \
    "$OSXCROSS_ROOT/SDK/MacOSX$version.sdk" > "$OSXCROSS_ROOT/bin/$prefix-osxcross-conf"
  chmod +x "$OSXCROSS_ROOT/bin/$prefix-osxcross-conf"
}

expect_prefix() {
  local actual
  actual=$("$RESOLVER" discover arm64-apple-darwin | sed -n 's/^prefix=//p')
  [[ "$actual" == "$1" ]] || { printf 'expected %s, got %s\n' "$1" "$actual" >&2; exit 1; }
}

collection arm64-apple-darwin25.4 26.4
collection arm64-apple-darwin25.10 26.10
# Incomplete newer collections and unrelated Darwin generations are ignored.
tool "$OSXCROSS_ROOT/bin/arm64-apple-darwin25.11-clang"
collection arm64-apple-darwin26.1 27.1
expect_prefix arm64-apple-darwin25.10
CPKT_OSXCROSS_HOST=arm64-apple-darwin25.4 expect_prefix arm64-apple-darwin25.4
if CPKT_OSXCROSS_HOST=arm64-apple-darwin25.11 "$RESOLVER" env arm64-apple-darwin > "$WORK/incomplete.log" 2>&1; then
  printf '%s\n' 'incomplete pinned Darwin collection was accepted' >&2
  exit 1
fi

cat > "$WORK/assert.cmake" <<'EOF'
include("${TOOLCHAIN}")
foreach(name CMAKE_C_COMPILER CMAKE_AR CMAKE_LINKER CMAKE_RANLIB CMAKE_STRIP CMAKE_OTOOL)
  if(NOT "${${name}}" MATCHES "arm64-apple-darwin25\\.10-")
    message(FATAL_ERROR "${name} did not use the selected collection: ${${name}}")
  endif()
endforeach()
if(NOT CMAKE_OSX_SYSROOT STREQUAL "$ENV{OSXCROSS_ROOT}/SDK/MacOSX26.10.sdk")
  message(FATAL_ERROR "SDK did not come from the selected compiler collection: ${CMAKE_OSX_SYSROOT}")
endif()
EOF
cmake -DTOOLCHAIN="$ROOT/cmake/toolchains/arm64-apple-darwin.cmake" -P "$WORK/assert.cmake"

# No configured build is needed to discover the selected collection's tools.
"$ROOT/scripts/with_target_env.sh" arm64-apple-darwin sh -c '
  test -z "${CPKT_OSXCROSS_HOST:-}"
  case "$PATH" in "$OSXCROSS_ROOT/bin":*) ;; *) exit 1 ;; esac
'
actual=$("$ROOT/scripts/discover_target_tools.sh" --root "$WORK/no-builds" --target arm64-apple-darwin otool)
# The helper uses its own repository resolver, independent of the build root.
[[ "$actual" == "$OSXCROSS_ROOT/bin/arm64-apple-darwin25.10-otool" ]]

rm "$OSXCROSS_ROOT/bin/arm64-apple-darwin25.10-osxcross-conf"
if cmake -DTOOLCHAIN="$ROOT/cmake/toolchains/arm64-apple-darwin.cmake" -P "$WORK/assert.cmake" > "$WORK/no-sdk.log" 2>&1; then
  printf '%s\n' 'missing osxcross SDK metadata was accepted' >&2
  exit 1
fi
