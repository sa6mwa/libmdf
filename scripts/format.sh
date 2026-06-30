#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v clang-format >/dev/null 2>&1; then
  printf '%s\n' 'clang-format is required for make format' >&2
  exit 1
fi

find "$ROOT/include" "$ROOT/src" "$ROOT/examples" "$ROOT/tests" "$ROOT/fuzz" "$ROOT/lua" \
  -type f \( -name '*.c' -o -name '*.h' \) \
  ! -path "$ROOT/src/html_embedded/*" \
  -exec clang-format -i {} +
