#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TARGETS=${LIBMDF_TARGETS:-"x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"}

for target in $TARGETS; do
  preset="$target-release"
  echo "==> $preset"
  "$ROOT/scripts/configure_preset.sh" "$preset" release
  "$ROOT/scripts/with_target_env.sh" "$preset" cmake --build --preset "$preset"
done
