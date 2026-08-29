#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE="$ROOT/build/test-darwin-linker-route"
OSXCROSS_ROOT=${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}
CPKT_OSXCROSS_HOST=${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}
LD="$OSXCROSS_ROOT/bin/$CPKT_OSXCROSS_HOST-ld"
TOOLCHAIN="$ROOT/cmake/toolchains/arm64-apple-darwin.cmake"

if grep -F -- '-fuse-ld=' "$TOOLCHAIN" >/dev/null; then
  printf '%s\n' 'Darwin toolchain must not pass an absolute linker through -fuse-ld' >&2
  exit 1
fi
if ! grep -F -- 'set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${CMAKE_LINKER}")' "$TOOLCHAIN" >/dev/null ||
   ! grep -F -- 'set(CMAKE_SHARED_LINKER_FLAGS_INIT "--ld-path=${CMAKE_LINKER}")' "$TOOLCHAIN" >/dev/null ||
   ! grep -F -- 'set(CMAKE_MODULE_LINKER_FLAGS_INIT "--ld-path=${CMAKE_LINKER}")' "$TOOLCHAIN" >/dev/null; then
  printf '%s\n' 'Darwin toolchain must pass the configured target linker through --ld-path' >&2
  exit 1
fi

rm -rf "$BASE"
mkdir -p "$BASE/osxcross/bin"

"$ROOT/scripts/with_target_env.sh" arm64-apple-darwin sh -c '
  case ":$PATH:" in
    *":$OSXCROSS_ROOT/bin:"*) ;;
    *) printf "%s\n" "osxcross bin was not added to PATH" >&2; exit 1 ;;
  esac
  case "$PATH" in
    "$OSXCROSS_ROOT/bin":*|"$OSXCROSS_ROOT/bin") ;;
    *) printf "%s\n" "osxcross bin was not first in PATH" >&2; exit 1 ;;
  esac
'

OSXCROSS_ROOT="$BASE/osxcross" CPKT_OSXCROSS_HOST="$CPKT_OSXCROSS_HOST" \
  "$ROOT/scripts/with_target_env.sh" arm64-apple-darwin sh -c '
    case "$PATH" in
      "$OSXCROSS_ROOT/bin":*|"$OSXCROSS_ROOT/bin") ;;
      *) printf "%s\n" "custom osxcross bin was not first in PATH" >&2; exit 1 ;;
    esac
  '

mkdir -p "$BASE/host" "$BASE/later-osxcross/bin"
OSXCROSS_ROOT="$BASE/later-osxcross" CPKT_OSXCROSS_HOST="$CPKT_OSXCROSS_HOST" \
  PATH="$BASE/host:$BASE/later-osxcross/bin:$PATH" \
  "$ROOT/scripts/with_target_env.sh" arm64-apple-darwin sh -c '
    case "$PATH" in
      "$OSXCROSS_ROOT/bin":*) ;;
      *) printf "%s\n" "existing osxcross bin was not moved to the front of PATH" >&2; exit 1 ;;
    esac
    case "$PATH" in
      *":$OSXCROSS_ROOT/bin:"*|*":$OSXCROSS_ROOT/bin")
        printf "%s\n" "osxcross bin remained duplicated later in PATH" >&2
        exit 1
        ;;
    esac
  '

if [ ! -x "$LD" ]; then
  printf '%s\n' "SKIP: osxcross linker unavailable at $LD"
  exit 0
fi

rm -rf "$ROOT/build/arm64-apple-darwin-release"
"$ROOT/scripts/configure_preset.sh" arm64-apple-darwin-release release >/dev/null

if ! grep -F -- "--ld-path=$LD" "$ROOT/build/arm64-apple-darwin-release/build.ninja" >/dev/null; then
  printf '%s\n' "Darwin build graph does not route links through $LD" >&2
  exit 1
fi

if ! awk '
  /^build cmdf:/ { in_cmdf = 1; next }
  /^build / { in_cmdf = 0 }
  in_cmdf && index($0, "--ld-path='"$LD"'") { found = 1 }
  END { exit found ? 0 : 1 }
' "$ROOT/build/arm64-apple-darwin-release/build.ninja"; then
  printf '%s\n' "cmdf link rule does not route through $LD" >&2
  exit 1
fi

# test_pager exercises Darwin PTY and window-resize APIs. Its compile edge
# must retain the Darwin extension selector rather than relying on the
# library targets' private definitions.
if ! awk '
  /^build CMakeFiles\/test_pager.dir\/tests\/test_pager.c.o:/ { in_pager = 1; next }
  /^build / { in_pager = 0 }
  in_pager && index($0, "-D_DARWIN_C_SOURCE") { found = 1 }
  END { exit found ? 0 : 1 }
' "$ROOT/build/arm64-apple-darwin-release/build.ninja"; then
  printf '%s\n' 'Darwin pager test must compile with _DARWIN_C_SOURCE' >&2
  exit 1
fi

cmake --build "$ROOT/build/arm64-apple-darwin-release" --target test_pager >/dev/null

if grep -F 'install_name_tool' "$ROOT/build/arm64-apple-darwin-release/cmake_install.cmake" >/dev/null; then
  printf '%s\n' 'Darwin install script mutates final Mach-O artifacts with install_name_tool' >&2
  exit 1
fi
