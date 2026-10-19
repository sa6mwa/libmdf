#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
command -v clang-format >/dev/null || { printf 'clang-format is required\n' >&2; exit 1; }
case "${1:-}" in '') args=(-i);; --check) args=(--dry-run --Werror);; *) printf 'usage: format.sh [--check]\n' >&2; exit 2;; esac
find "$root/include" "$root/src" "$root/examples" "$root/tests" "$root/fuzz" "$root/lua" "$root/benchmarks" \
  -type f \( -name '*.c' -o -name '*.h' \) ! -path "$root/src/html_embedded/*" \
  -exec clang-format "${args[@]}" {} +
