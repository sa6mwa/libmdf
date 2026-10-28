#!/usr/bin/env bash
# Check ordinary editor discovery using the normal native producer's inputs.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[[ $# -ge 2 ]] || { printf 'usage: test_clangd.sh <clangd> <source>...\n' >&2; exit 2; }
clangd=$1
shift
work="$root/build/debug/clangd-check"
[[ -s $root/build/debug/compile_commands.json ]] || {
  printf 'Run make build-debug before checking editor discovery\n' >&2; exit 1;
}
mkdir -p "$work"
for source in "$@"; do
  log="$work/${source//\//_}.log"
  # This verifies compilation/discovery. Disable refactoring-action self-tests,
  # which exercise clangd itself rather than the project's compiler settings.
  if ! "$clangd" --check="$root/$source" --tweaks= --log=info > "$log" 2>&1; then
    cat "$log" >&2
    exit 1
  fi
  printf 'clangd: %s passed\n' "$source"
done
