#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE="$ROOT/build/test-darwin-linker-route"
OSXCROSS_ROOT=${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}
CPKT_OSXCROSS_HOST=${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}
LD="$OSXCROSS_ROOT/bin/$CPKT_OSXCROSS_HOST-ld"

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

"$ROOT/scripts/configure_preset.sh" arm64-apple-darwin-release package >/dev/null

if ! grep -F -- "-fuse-ld=$LD" "$ROOT/build/arm64-apple-darwin-release/build.ninja" >/dev/null; then
  printf '%s\n' "Darwin build graph does not route links through $LD" >&2
  exit 1
fi

if ! awk '
  /^build cmdf:/ { in_cmdf = 1; next }
  /^build / { in_cmdf = 0 }
  in_cmdf && index($0, "-fuse-ld='"$LD"'") { found = 1 }
  END { exit found ? 0 : 1 }
' "$ROOT/build/arm64-apple-darwin-release/build.ninja"; then
  printf '%s\n' "cmdf link rule does not route through $LD" >&2
  exit 1
fi

if grep -F 'install_name_tool' "$ROOT/build/arm64-apple-darwin-release/cmake_install.cmake" >/dev/null; then
  printf '%s\n' 'Darwin install script mutates final Mach-O artifacts with install_name_tool' >&2
  exit 1
fi
