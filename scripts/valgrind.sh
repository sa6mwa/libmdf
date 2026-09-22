#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v valgrind >/dev/null 2>&1; then
  printf '%s\n' 'valgrind is required for make valgrind; install it with the host OS package manager' >&2
  exit 1
fi

"$ROOT/scripts/configure_preset.sh" valgrind dev
cmake --build --preset valgrind --target test_public_api
valgrind \
  --leak-check=full \
  --show-leak-kinds=all \
  --track-origins=yes \
  --error-exitcode=1 \
  "$ROOT/build/valgrind/test_public_api"
