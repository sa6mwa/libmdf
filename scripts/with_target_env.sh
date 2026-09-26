#!/usr/bin/env sh
set -eu

if [ "$#" -lt 2 ]; then
  printf '%s\n' "usage: $0 <target-or-preset> <command> [args...]" >&2
  exit 2
fi

target=$1
shift
target=${target%-release}

case "$target" in
  *-apple-darwin)
    OSXCROSS_ROOT=${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}
    export OSXCROSS_ROOT
    path_tail=$(printf '%s\n' "$PATH" | awk -v drop="$OSXCROSS_ROOT/bin" '
      BEGIN { RS = ":"; ORS = "" }
      $0 != drop {
        if (seen) {
          printf ":%s", $0
        } else {
          printf "%s", $0
          seen = 1
        }
      }
    ')
    PATH="$OSXCROSS_ROOT/bin${path_tail:+:$path_tail}"
    export PATH
    ;;
esac

exec "$@"
