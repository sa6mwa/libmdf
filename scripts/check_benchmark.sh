#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --baseline|--rounds|--warmups|--repeat|--allowance|--attempts)
      option=${1#--}; value=$2; shift
      [[ $option != baseline || $value == /* ]] || value="$root/$value"
      args+=("-D$option=$value");;
    --no-lua) args+=(-Dno_lua=ON);;
    *) printf 'Unknown benchmark gate option: %s\n' "$1" >&2; exit 2;;
  esac
  shift
done
"$root/scripts/build.sh" debug
cmake "${args[@]}" -P "$root/cmake/CheckBenchmark.cmake"
