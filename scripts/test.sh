#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PRESET=${1:-debug}
"$ROOT/scripts/configure_preset.sh" "$PRESET" dev
cmake --build --preset "$PRESET"
ctest --preset "$PRESET"
