#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PRESET=${1:-debug}
case "$PRESET" in
  *-release) PROFILE=release ;;
  fuzz) PROFILE=fuzz ;;
  *) PROFILE=dev ;;
esac
"$ROOT/scripts/configure_preset.sh" "$PRESET" "$PROFILE"
"$ROOT/scripts/with_target_env.sh" "$PRESET" cmake --build --preset "$PRESET"
